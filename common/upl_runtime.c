/*-------------------------------------------------------------------------
 *
 * uplpgsql_runtime.c
 *		C runtime helper functions called from JIT'd code.
 *
 *		These functions bridge the gap between LLVM-generated native code
 *		and PostgreSQL's C runtime.  They are called from JIT'd functions
 *		when an operation requires PostgreSQL infrastructure that cannot
 *		be expressed in LLVM IR (SPI calls, memory management, error
 *		handling, etc.).
 *
 *		Symbol visibility:
 *		  All functions are marked UPLPGSQL_RT_EXPORT which expands to
 *		  __attribute__((visibility("default"))).  This is necessary
 *		  because the extension builds with -fvisibility=hidden.  OrcJIT's
 *		  process symbol search generator finds these symbols at link time.
 *
 *		Function patterns:
 *		  Most helpers follow a consistent pattern:
 *		    1. Receive UPLpgSQL_exec_state *estate as first argument
 *		    2. Extract plstate = estate->uplpgsql_estate
 *		    3. Call the corresponding exec_* function from upl_exec.c
 *		    4. Copy return values (retval, retisnull) back to JIT estate
 *
 *		  Some thin wrappers (e.g. exec_stmt_perform, exec_stmt_raise)
 *		  are bypassed entirely by the compiler — uplpgsql_compile.c
 *		  embeds the exec_* function pointer directly.  The runtime
 *		  helpers remain for:
 *		    - Functions that need result value transfer (exec_return)
 *		    - Functions with non-trivial bridging logic (eval_expr copies)
 *		    - Cursor management (open/fetch/close portal lifecycle)
 *		    - Exception handling (subtransaction management steps)
 *
 *		Categories:
 *		  - Expression evaluation: eval_expr, eval_bool, eval_int
 *		  - Variable management: assign_expr, assign_int, assign_null,
 *		    init_var, set_found, assign_var_datum, copy_assign_var_datum,
 *		    get_recfield
 *		  - Statement execution: exec_return, exec_perform, exec_sql, etc.
 *		  - Cursor management: open_query_cursor, fetch_cursor_row,
 *		    close_portal, open_forc_cursor, open_dynfors_cursor
 *		  - Exception handling: exception_push_frame, exception_arm,
 *		    exception_try_exit, exception_catch, exception_set_handler_vars,
 *		    exception_handler_done, exception_rethrow
 *		  - Error helpers: int_overflow, div_zero (in uplpgsql_expr.c)
 *
 *
 * Copyright (c) 2003-2014, Jonah H. Harris <jonah.harris@gmail.com>
 * Copyright (c) 2014-2026, NEXTGRES, LLC. <oss@nextgres.com>
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain a
 * copy of the License in LICENSE or at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 *-------------------------------------------------------------------------
 */
#include "upl_common.h"

#include "access/xact.h"
#include "catalog/pg_type_d.h"
#include "utils/array.h"
#include "utils/expandeddatum.h"
#include "utils/expandedrecord.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/portal.h"

/*
 * Per-cursor context for batch prefetching in FOR-query loops.
 *
 * Allocated by open_query_cursor, freed by close_portal.  Each nested
 * FOR-query loop gets its own context, so batch state doesn't bleed
 * between nesting levels.
 */
typedef struct UPLpgSQL_cursor_ctx
{
	Portal			portal;
	SPITupleTable  *batch_tuptab;		/* current batch of fetched rows */
	uint64			batch_count;		/* number of rows in current batch */
	uint64			batch_idx;			/* index of next row to process */
	uint64			batch_tupdesc_id;	/* previous tupdesc id for fast assign */
	bool			batch_tupdescs_match; /* can use expanded_record_set_tuple? */
} UPLpgSQL_cursor_ctx;

/*
 * uplpgsql_rt_exec_block_protected - Execute a block with exception handling.
 *
 * Delegates the entire block (including its exception handlers) to the
 * interpreter's exec_stmt_block, which manages subtransactions via
 * PG_TRY/PG_CATCH.  This cannot be compiled to LLVM IR because longjmp
 * is incompatible with LLVM's control flow model.
 */
UPLPGSQL_RT_EXPORT int32
uplpgsql_rt_exec_block_protected(UPLpgSQL_exec_state *estate,
								 UPLpgSQL_stmt_block *stmt)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	int32		rc;

	rc = exec_stmt_block(plstate, stmt);

	/*
	 * If the block returned via RETURN, copy retval/retisnull back to
	 * JIT state so the caller can see the result.
	 */
	if (rc == UPLPGSQL_RC_RETURN)
	{
		estate->retval = plstate->retval;
		estate->retisnull = plstate->retisnull;
	}

	return rc;
}

/*
 * uplpgsql_rt_eval_expr - Evaluate a UPLpgSQL_expr and return its Datum value.
 *
 * Wraps exec_eval_expr with datum copy for safety, since exec_eval_expr
 * returns values that may live in the eval_tuptable.
 */
UPLPGSQL_RT_EXPORT Datum
uplpgsql_rt_eval_expr(UPLpgSQL_exec_state *estate,
					  UPLpgSQL_expr *expr, bool *isNull)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	Datum		result;
	Oid			rettype;
	int32		rettypmod;

	result = exec_eval_expr(plstate, expr, isNull, &rettype, &rettypmod);

	/*
	 * exec_eval_expr returns a value that may be in the eval_tuptable
	 * and could be freed on the next call. Copy pass-by-reference values.
	 */
	if (!*isNull)
	{
		int16	typlen;
		bool	typbyval;

		get_typlenbyval(rettype, &typlen, &typbyval);
		if (!typbyval)
			result = datumCopy(result, typbyval, typlen);
	}

	exec_eval_cleanup(plstate);

	return result;
}

/*
 * uplpgsql_rt_eval_bool - Evaluate expression and coerce to bool.
 *                          NULL is treated as false.
 */
UPLPGSQL_RT_EXPORT bool
uplpgsql_rt_eval_bool(UPLpgSQL_exec_state *estate, UPLpgSQL_expr *expr)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	bool		isnull;
	bool		result;

	result = exec_eval_boolean(plstate, expr, &isnull);
	exec_eval_cleanup(plstate);

	if (isnull)
		return false;

	return result;
}

/*
 * uplpgsql_rt_eval_int - Evaluate expression and coerce to int32.
 */
UPLPGSQL_RT_EXPORT int32
uplpgsql_rt_eval_int(UPLpgSQL_exec_state *estate, UPLpgSQL_expr *expr)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	bool		isnull;
	int32		result;

	result = exec_eval_integer(plstate, expr, &isnull);
	exec_eval_cleanup(plstate);

	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("NULL value not allowed for integer expression")));

	return result;
}

/*
 * uplpgsql_rt_assign_expr - Evaluate expression and assign to variable.
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_assign_expr(UPLpgSQL_exec_state *estate,
						int target_dno, UPLpgSQL_expr *expr)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;

	exec_assign_expr(plstate, plstate->datums[target_dno], expr);
}

/*
 * uplpgsql_rt_case_assign_test - Evaluate a simple CASE's test expression into
 * its temporary variable, retyping the variable to the expression's real type.
 *
 * The parser cannot know what type "CASE <expr> WHEN ..." tests, so it builds
 * the temporary as an INT4 placeholder and leaves a note that the runtime will
 * "fix this at runtime if needed" (upl_gram.y).  exec_stmt_case() does the
 * fixing.  The JIT used to call exec_assign_expr() directly, which skips it and
 * coerces the value to int4 instead:
 *
 *     DECLARE a text := 'x';
 *     CASE a WHEN 'x' THEN ... END CASE;
 *     ERROR:  invalid input syntax for type integer: "x"
 *
 * so a simple CASE worked only when the test expression really was an integer.
 * Mirror exec_stmt_case().
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_case_assign_test(UPLpgSQL_exec_state *estate, int t_varno,
							 UPLpgSQL_expr *t_expr)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	UPLpgSQL_var *t_var;
	Datum		t_val;
	bool		isnull;
	Oid			t_typoid;
	int32		t_typmod;

	t_val = exec_eval_expr(plstate, t_expr, &isnull, &t_typoid, &t_typmod);

	t_var = (UPLpgSQL_var *) plstate->datums[t_varno];

	/*
	 * When the expected datatype differs from the real one, change it.  This
	 * modifies an execution copy of the datum, so it does not affect the
	 * stored parse tree.
	 */
	if (t_var->datatype->typoid != t_typoid ||
		t_var->datatype->atttypmod != t_typmod)
		t_var->datatype = uplpgsql_build_datatype(t_typoid,
												  t_typmod,
												  plstate->func->fn_input_collation,
												  NULL);

	exec_assign_value(plstate, (UPLpgSQL_datum *) t_var, t_val, isnull,
					  t_typoid, t_typmod);

	exec_eval_cleanup(plstate);
}

/*
 * uplpgsql_rt_init_var - Initialize a variable with its default value.
 *
 * Must match exec_stmt_block()'s variable initialization logic exactly,
 * including domain constraint checking and error context setup.
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_init_var(UPLpgSQL_exec_state *estate, int dno)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	UPLpgSQL_datum	   *datum = plstate->datums[dno];

	/*
	 * Set error context so that constraint violations during init report
	 * "during statement block local variable initialization" just like
	 * the interpreter does in exec_stmt_block().
	 */
	plstate->err_text = gettext_noop("during statement block local variable initialization");
	plstate->err_var = (UPLpgSQL_variable *) datum;

	switch (datum->dtype)
	{
		case UPLPGSQL_DTYPE_VAR:
			{
				UPLpgSQL_var *var = (UPLpgSQL_var *) datum;

				/*
				 * Free any old value, in case re-entering block, and
				 * initialize to NULL.
				 */
				assign_simple_var(plstate, var, (Datum) 0, true, false);

				if (var->default_val == NULL)
				{
					/*
					 * If needed, give the datatype a chance to reject NULLs,
					 * by assigning a NULL to the variable.  We claim the value
					 * is of type UNKNOWN, not the var's datatype, else
					 * coercion will be skipped.
					 */
					if (var->datatype->typtype == TYPTYPE_DOMAIN)
						exec_assign_value(plstate,
										  (UPLpgSQL_datum *) var,
										  (Datum) 0,
										  true,
										  UNKNOWNOID,
										  -1);

					/* parser should have rejected NOT NULL */
					Assert(!var->notnull);
				}
				else
				{
					exec_assign_expr(plstate, (UPLpgSQL_datum *) var,
									 var->default_val);
				}
			}
			break;

		case UPLPGSQL_DTYPE_REC:
			{
				UPLpgSQL_rec *rec = (UPLpgSQL_rec *) datum;

				if (rec->default_val == NULL)
				{
					/*
					 * If needed, give the datatype a chance to reject NULLs,
					 * by assigning a NULL to the variable.
					 */
					exec_move_row(plstate, (UPLpgSQL_variable *) rec,
								  NULL, NULL);

					/* parser should have rejected NOT NULL */
					Assert(!rec->notnull);
				}
				else
				{
					exec_assign_expr(plstate, (UPLpgSQL_datum *) rec,
									 rec->default_val);
				}
			}
			break;

		default:
			elog(ERROR, "unrecognized dtype: %d", datum->dtype);
	}

	plstate->err_text = NULL;
	plstate->err_var = NULL;
}

/*
 * uplpgsql_rt_set_found - Set the FOUND variable.
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_set_found(UPLpgSQL_exec_state *estate, bool value)
{
	exec_set_found(estate->uplpgsql_estate, value);
}

/*
 * uplpgsql_rt_exec_return - Execute a RETURN statement.
 *
 * Delegates to exec_stmt_return which handles all edge cases:
 * retvarno, expression, VOID hack, composite types, etc.
 * The return value, type, and null flag are stored directly into
 * the PL/pgSQL estate by exec_stmt_return.
 */
UPLPGSQL_RT_EXPORT int32
uplpgsql_rt_exec_return(UPLpgSQL_exec_state *estate, UPLpgSQL_stmt_return *stmt)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	int			rc;

	rc = exec_stmt_return(plstate, stmt);

	/* Copy back to JIT state so the caller can see the result */
	estate->retval = plstate->retval;
	estate->retisnull = plstate->retisnull;

	return rc;
}

/*
 * uplpgsql_rt_assign_int - Assign an int32 value to a variable by dno.
 *
 * Lightweight assignment for loop counters: the value is pass-by-value,
 * never null, never freeable. Bypasses the full assign_simple_var path.
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_assign_int(UPLpgSQL_exec_state *estate, int dno, int32 value)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	UPLpgSQL_var *var = (UPLpgSQL_var *) plstate->datums[dno];

	/*
	 * As in uplpgsql_rt_assign_null: release the old value through
	 * assign_simple_var so a read/write expanded object is deleted rather than
	 * pfree'd header-first.  Only reached for pass-by-value int targets today
	 * (loop counters), where the old value cannot be expanded — but the raw
	 * free was latent breakage waiting for a caller with a wider target, and
	 * the detoast branch is inert here anyway (typlen != -1).
	 */
	assign_simple_var(plstate, var, Int32GetDatum(value), false, false);
}

/*
 * uplpgsql_rt_exec_perform - Execute a PERFORM statement.
 */
UPLPGSQL_RT_EXPORT int32
uplpgsql_rt_exec_perform(UPLpgSQL_exec_state *estate, UPLpgSQL_stmt_perform *stmt)
{
	return exec_stmt_perform(estate->uplpgsql_estate, stmt);
}

/*
 * uplpgsql_rt_exec_sql - Execute a static SQL statement.
 */
UPLPGSQL_RT_EXPORT int32
uplpgsql_rt_exec_sql(UPLpgSQL_exec_state *estate, UPLpgSQL_stmt_execsql *stmt)
{
	return exec_stmt_execsql(estate->uplpgsql_estate, stmt);
}

/*
 * uplpgsql_rt_exec_raise - Execute a RAISE statement.
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_exec_raise(UPLpgSQL_exec_state *estate, UPLpgSQL_stmt_raise *stmt)
{
	exec_stmt_raise(estate->uplpgsql_estate, stmt);
}

/*
 * uplpgsql_rt_assign_null - Set a variable to NULL.
 *
 * Used by CASE to clear the temporary search expression variable
 * after a match is found or at the end of the statement.
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_assign_null(UPLpgSQL_exec_state *estate, int dno)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	UPLpgSQL_var *var = (UPLpgSQL_var *) plstate->datums[dno];

	/*
	 * Go through assign_simple_var rather than pfree'ing var->value directly.
	 *
	 * A read/write expanded object (an expanded array or record) must be
	 * released with DeleteExpandedObject: a bare pfree frees only the
	 * ExpandedObjectHeader chunk, leaking the object's private context and
	 * corrupting the allocator.  This is reachable — CASE clears its temporary
	 * search variable through here, and that variable can hold an expanded
	 * value.  assign_simple_var also cancels any promise on the variable,
	 * which the open-coded version skipped.
	 */
	assign_simple_var(plstate, var, (Datum) 0, true, false);
}

/*
 * uplpgsql_rt_case_error - Raise "case not found" error.
 *
 * Called when no WHEN clause matches and there is no ELSE clause.
 * SQL2003 mandates this error.
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_case_error(UPLpgSQL_exec_state *estate, int lineno)
{
	ereport(ERROR,
			(errcode(ERRCODE_CASE_NOT_FOUND),
			 errmsg("case not found"),
			 errhint("CASE statement is missing ELSE part.")));
}

/*
 * uplpgsql_rt_assert_fail - Handle a failed ASSERT.
 *
 * Called from JIT'd code when the ASSERT condition evaluated to false/NULL.
 * Evaluates the optional message expression and raises ERROR.
 * Also checks uplpgsql_check_asserts GUC — if asserts are disabled,
 * this is a no-op (the JIT'd code evaluates the condition but we
 * swallow the failure).
 */
/*
 * uplpgsql_rt_exec_assert - Execute an ASSERT statement.
 *
 * Delegates to exec_stmt_assert which handles the GUC check,
 * condition evaluation, message evaluation, and error raising.
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_exec_assert(UPLpgSQL_exec_state *estate, UPLpgSQL_stmt_assert *stmt)
{
	exec_stmt_assert(estate->uplpgsql_estate, stmt);
}

/*
 * uplpgsql_rt_exec_open - Open a cursor.
 *
 * Handles all three OPEN variants (static query, dynamic query, explicit
 * cursor) via exec_stmt_open. The path selection is based on AST fields
 * known at compile time, but the SPI infrastructure (plan preparation,
 * parameter binding, portal creation) requires runtime C code.
 */
UPLPGSQL_RT_EXPORT int32
uplpgsql_rt_exec_open(UPLpgSQL_exec_state *estate, UPLpgSQL_stmt_open *stmt)
{
	return exec_stmt_open(estate->uplpgsql_estate, stmt);
}

/*
 * uplpgsql_rt_exec_fetch - Fetch from a cursor or move cursor position.
 *
 * Handles FETCH INTO target and MOVE variants. Sets FOUND and
 * eval_processed (ROW_COUNT) in the estate.
 */
UPLPGSQL_RT_EXPORT int32
uplpgsql_rt_exec_fetch(UPLpgSQL_exec_state *estate, UPLpgSQL_stmt_fetch *stmt)
{
	return exec_stmt_fetch(estate->uplpgsql_estate, stmt);
}

/*
 * uplpgsql_rt_exec_close - Close a cursor.
 */
UPLPGSQL_RT_EXPORT int32
uplpgsql_rt_exec_close(UPLpgSQL_exec_state *estate, UPLpgSQL_stmt_close *stmt)
{
	return exec_stmt_close(estate->uplpgsql_estate, stmt);
}

/*
 * uplpgsql_rt_open_query_cursor - Open a portal for a SELECT query.
 *
 * Used by FOR-query (FORS) loops.  Prepares the plan if needed,
 * opens a cursor via SPI, and returns a cursor context that includes
 * batch prefetching state.  Each nested FOR-query loop gets its own
 * context, so batch state doesn't bleed between nesting levels.
 */
UPLPGSQL_RT_EXPORT void *
uplpgsql_rt_open_query_cursor(UPLpgSQL_exec_state *estate,
							  UPLpgSQL_expr *query)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	UPLpgSQL_cursor_ctx *cctx;
	Portal		portal;

	exec_run_select(plstate, query, 0, &portal);
	PinPortal(portal);

	cctx = (UPLpgSQL_cursor_ctx *) palloc0(sizeof(UPLpgSQL_cursor_ctx));
	cctx->portal = portal;

	return (void *) cctx;
}

/*
 * uplpgsql_rt_fetch_cursor_row - Fetch the next row from a cursor into
 *                                the target variable.
 *
 * Returns true if a row was fetched, false if no more rows.
 * Used by FOR-query loop condition: fetch → branch on result.
 *
 * Implements batch prefetching (matching PL/pgSQL's exec_for_query):
 * first fetch grabs 10 rows, subsequent fetches grab 50.  This reduces
 * SPI cursor overhead by ~50x for large result sets.
 *
 * For RECORD-type targets, after the first row we use the fast
 * expanded_record_set_tuple() path instead of exec_move_row(), which
 * avoids tuple deconstruction overhead on subsequent rows.
 */
UPLPGSQL_RT_EXPORT bool
uplpgsql_rt_fetch_cursor_row(UPLpgSQL_exec_state *estate,
							 void *portal_ptr, int target_dno)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	UPLpgSQL_cursor_ctx *cctx = (UPLpgSQL_cursor_ctx *) portal_ptr;
	UPLpgSQL_variable *target;

	target = (UPLpgSQL_variable *) plstate->datums[target_dno];

	/*
	 * If we have rows remaining in the current batch, assign the next one.
	 */
	if (cctx->batch_idx < cctx->batch_count)
	{
		SPITupleTable *tuptab = cctx->batch_tuptab;
		uint64		idx = cctx->batch_idx++;

		/*
		 * Fast path for RECORD targets: use expanded_record_set_tuple()
		 * when the tupdesc hasn't changed since the first row.  This is
		 * the same optimization PL/pgSQL uses in exec_for_query.
		 */
		if (target->dtype == UPLPGSQL_DTYPE_REC)
		{
			UPLpgSQL_rec *rec = (UPLpgSQL_rec *) target;

			if (rec->erh &&
				rec->erh->er_tupdesc_id == cctx->batch_tupdesc_id &&
				cctx->batch_tupdescs_match)
			{
				expanded_record_set_tuple(rec->erh, tuptab->vals[idx],
										  true, !plstate->atomic);
				return true;
			}
		}

		exec_move_row(plstate, target, tuptab->vals[idx], tuptab->tupdesc);
		exec_eval_cleanup(plstate);

		/* Check if fast path is usable for subsequent rows */
		if (target->dtype == UPLPGSQL_DTYPE_REC)
		{
			UPLpgSQL_rec *rec = (UPLpgSQL_rec *) target;

			if (rec->erh)
			{
				if (cctx->batch_tupdescs_match)
				{
					cctx->batch_tupdescs_match =
						(rec->rectypeid == RECORDOID ||
						 rec->rectypeid == tuptab->tupdesc->tdtypeid);
				}
				cctx->batch_tupdesc_id = rec->erh->er_tupdesc_id;
			}
		}

		return true;
	}

	/*
	 * Current batch is exhausted.  Free it and fetch a new one.
	 */
	if (cctx->batch_tuptab)
	{
		SPI_freetuptable(cctx->batch_tuptab);
		cctx->batch_tuptab = NULL;
	}

	/*
	 * Fetch next batch: 10 rows for first fetch (to minimize startup
	 * overhead), 50 rows for subsequent fetches (matching PL/pgSQL).
	 *
	 * Like PL/pgSQL, in non-atomic contexts we don't prefetch to avoid
	 * dangling toast references if the user commits mid-loop.
	 */
	{
		long	fetch_count;

		if (!plstate->atomic)
			fetch_count = 1;
		else if (cctx->batch_count == 0)
			fetch_count = 10;	/* first fetch */
		else
			fetch_count = 50;	/* subsequent fetches */

		SPI_cursor_fetch(cctx->portal, true, fetch_count);
	}

	cctx->batch_tuptab = SPI_tuptable;
	cctx->batch_count = SPI_processed;
	cctx->batch_idx = 0;

	if (cctx->batch_count == 0)
	{
		/* No more rows — set target to NULL */
		exec_move_row(plstate, target, NULL,
					  cctx->batch_tuptab->tupdesc);
		exec_eval_cleanup(plstate);
		SPI_freetuptable(cctx->batch_tuptab);
		cctx->batch_tuptab = NULL;
		return false;
	}

	/* Assign first row of new batch */
	{
		SPITupleTable *tuptab = cctx->batch_tuptab;

		cctx->batch_idx = 1;

		exec_move_row(plstate, target, tuptab->vals[0], tuptab->tupdesc);
		exec_eval_cleanup(plstate);

		/* Initialize fast-path state for subsequent rows */
		if (target->dtype == UPLPGSQL_DTYPE_REC)
		{
			UPLpgSQL_rec *rec = (UPLpgSQL_rec *) target;

			if (rec->erh)
			{
				cctx->batch_tupdescs_match =
					(rec->rectypeid == RECORDOID ||
					 rec->rectypeid == tuptab->tupdesc->tdtypeid);
				cctx->batch_tupdesc_id = rec->erh->er_tupdesc_id;
			}
		}
	}

	return true;
}

/*
 * uplpgsql_rt_close_portal - Close and unpin a portal.
 *
 * Used at the end of FOR-query loops.  Frees any remaining batch
 * and the cursor context itself.
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_close_portal(UPLpgSQL_exec_state *estate, void *portal_ptr)
{
	UPLpgSQL_cursor_ctx *cctx = (UPLpgSQL_cursor_ctx *) portal_ptr;

	/* Free any remaining batch from prefetch */
	if (cctx->batch_tuptab)
	{
		SPI_freetuptable(cctx->batch_tuptab);
		cctx->batch_tuptab = NULL;
	}

	UnpinPortal(cctx->portal);
	SPI_cursor_close(cctx->portal);
	pfree(cctx);
}

/*
 * uplpgsql_rt_open_forc_cursor - Open the cursor for a FOR-cursor loop.
 *
 * Handles argument evaluation, plan preparation, SPI portal opening,
 * and cursor variable assignment.  Returns a cursor context wrapping
 * a pinned Portal.
 */
UPLPGSQL_RT_EXPORT void *
uplpgsql_rt_open_forc_cursor(UPLpgSQL_exec_state *estate,
							 UPLpgSQL_stmt_forc *stmt)
{
	UPLpgSQL_cursor_ctx *cctx;
	Portal		portal;

	portal = exec_open_forc_cursor(estate->uplpgsql_estate, stmt);

	cctx = (UPLpgSQL_cursor_ctx *) palloc0(sizeof(UPLpgSQL_cursor_ctx));
	cctx->portal = portal;

	return (void *) cctx;
}

/*
 * uplpgsql_rt_close_forc_cursor - Close the cursor for a FOR-cursor loop.
 *
 * Unpins and closes the portal, resets cursor variable if needed.
 * Frees any remaining batch and the cursor context.
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_close_forc_cursor(UPLpgSQL_exec_state *estate,
							  UPLpgSQL_stmt_forc *stmt,
							  void *portal_ptr)
{
	UPLpgSQL_cursor_ctx *cctx = (UPLpgSQL_cursor_ctx *) portal_ptr;

	/* Free any remaining batch from prefetch */
	if (cctx->batch_tuptab)
	{
		SPI_freetuptable(cctx->batch_tuptab);
		cctx->batch_tuptab = NULL;
	}

	exec_close_forc_cursor(estate->uplpgsql_estate, stmt, cctx->portal);
	pfree(cctx);
}

/*
 * uplpgsql_rt_exec_dynexecute - Execute a dynamic SQL statement.
 */
UPLPGSQL_RT_EXPORT int32
uplpgsql_rt_exec_dynexecute(UPLpgSQL_exec_state *estate,
							UPLpgSQL_stmt_dynexecute *stmt)
{
	return exec_stmt_dynexecute(estate->uplpgsql_estate, stmt);
}

/*
 * uplpgsql_rt_exec_call - Execute a CALL statement.
 */
UPLPGSQL_RT_EXPORT int32
uplpgsql_rt_exec_call(UPLpgSQL_exec_state *estate, UPLpgSQL_stmt_call *stmt)
{
	return exec_stmt_call(estate->uplpgsql_estate, stmt);
}

/*
 * uplpgsql_rt_exec_getdiag - Execute a GET DIAGNOSTICS statement.
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_exec_getdiag(UPLpgSQL_exec_state *estate,
						 UPLpgSQL_stmt_getdiag *stmt)
{
	exec_stmt_getdiag(estate->uplpgsql_estate, stmt);
}

/*
 * uplpgsql_rt_exec_return_next - Execute RETURN NEXT.
 */
UPLPGSQL_RT_EXPORT int32
uplpgsql_rt_exec_return_next(UPLpgSQL_exec_state *estate,
							 UPLpgSQL_stmt_return_next *stmt)
{
	return exec_stmt_return_next(estate->uplpgsql_estate, stmt);
}

/*
 * uplpgsql_rt_exec_return_query - Execute RETURN QUERY.
 */
UPLPGSQL_RT_EXPORT int32
uplpgsql_rt_exec_return_query(UPLpgSQL_exec_state *estate,
							  UPLpgSQL_stmt_return_query *stmt)
{
	return exec_stmt_return_query(estate->uplpgsql_estate, stmt);
}

/*
 * uplpgsql_rt_exec_commit - Execute COMMIT.
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_exec_commit(UPLpgSQL_exec_state *estate,
						UPLpgSQL_stmt_commit *stmt)
{
	exec_stmt_commit(estate->uplpgsql_estate, stmt);
}

/*
 * uplpgsql_rt_exec_rollback - Execute ROLLBACK.
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_exec_rollback(UPLpgSQL_exec_state *estate,
						  UPLpgSQL_stmt_rollback *stmt)
{
	exec_stmt_rollback(estate->uplpgsql_estate, stmt);
}

/*
 * uplpgsql_rt_exec_foreach_a - Execute a FOREACH array loop.
 *
 * For now, delegate the entire loop to the interpreter since the
 * array slicing/element extraction logic is deeply tied to executor
 * internals. The loop control flow stays in C.
 */
UPLPGSQL_RT_EXPORT int32
uplpgsql_rt_exec_foreach_a(UPLpgSQL_exec_state *estate,
						   UPLpgSQL_stmt_foreach_a *stmt)
{
	return exec_stmt_foreach_a(estate->uplpgsql_estate, stmt);
}

/*
 * uplpgsql_rt_array_get_element - Read a single element from an array variable.
 *
 * Bypasses SPI expression evaluation for arr[i] reads.  The JIT'd code
 * compiles the subscript index to native IR and passes it directly as
 * an integer.  This function loads the array Datum from the variable and
 * calls array_get_element() — skipping exec_eval_expr, ExprState setup,
 * and the Param callback chain.
 *
 * Parameters:
 *   estate     - JIT execution state (plstate lives inside)
 *   array_dno  - datum number of the array variable
 *   subscript  - 1-based array index (already evaluated by JIT'd code)
 *   isNull_out - output: whether the element is NULL
 *
 * Returns the element Datum (pass-by-value or pointer to palloc'd copy).
 */
UPLPGSQL_RT_EXPORT Datum
uplpgsql_rt_array_get_element(UPLpgSQL_exec_state *estate,
							  int array_dno, int subscript,
							  int typlen, int16 elmlen,
							  bool elmbyval, char elmalign,
							  bool *isNull_out)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	UPLpgSQL_var   *arrayvar;
	Datum			result;

	arrayvar = (UPLpgSQL_var *) plstate->datums[array_dno];

	/* NULL array → NULL element */
	if (arrayvar->isnull)
	{
		*isNull_out = true;
		return (Datum) 0;
	}

	result = array_get_element(arrayvar->value,
							   1, &subscript,
							   typlen,
							   elmlen, elmbyval, elmalign,
							   isNull_out);

	/* Copy pass-by-ref result so it survives beyond transient contexts */
	if (!*isNull_out && !elmbyval)
		result = datumCopy(result, elmbyval, elmlen);

	return result;
}

/*
 * uplpgsql_rt_array_set_element - Write a single element into an array variable.
 *
 * Bypasses SPI expression evaluation for arr[i] := val writes.  The JIT'd
 * code compiles both the subscript index and the value to native IR and
 * passes them directly.  This function calls array_set_element() and
 * stores the modified array back into the variable.
 *
 * Handles:
 *   - NULL array (creates empty array first)
 *   - Expanded array promotion (R/W form for efficient subsequent access)
 *   - freeval cleanup via assign_simple_var
 *
 * Parameters:
 *   estate    - JIT execution state
 *   array_dno - datum number of the array variable
 *   subscript - 1-based array index (already evaluated by JIT'd code)
 *   newvalue  - Datum value to store
 *   valisnull - whether the value is NULL
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_array_set_element(UPLpgSQL_exec_state *estate,
							  int array_dno, int subscript,
							  Datum newvalue, bool valisnull,
							  int typlen, Oid elemtype,
							  int16 elmlen, bool elmbyval,
							  char elmalign)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	UPLpgSQL_var   *arrayvar;
	Datum			arraydatum;

	arrayvar = (UPLpgSQL_var *) plstate->datums[array_dno];

	/* NULL array → start with empty array */
	if (arrayvar->isnull)
		arraydatum = PointerGetDatum(construct_empty_array(elemtype));
	else
		arraydatum = arrayvar->value;

	{
		Datum olddatum = arraydatum;

		arraydatum = array_set_element(arraydatum,
									   1, &subscript,
									   newvalue, valisnull,
									   typlen,
									   elmlen, elmbyval, elmalign);

		if (arraydatum == olddatum)
		{
			/*
			 * array_set_element modified the expanded array in-place and
			 * returned the same pointer.  Nothing more to do — the variable
			 * already points to the updated expanded object.
			 */
		}
		else
		{
			/*
			 * Got a new flat array back.  Promote to R/W expanded form so
			 * subsequent subscript ops can modify in-place.
			 */
			arraydatum = expand_array(arraydatum,
									  plstate->datum_context, NULL);
			assign_simple_var(plstate, arrayvar, arraydatum, false, true);
		}
	}
}

/*
 * uplpgsql_rt_open_dynfors_cursor - Open a portal for a dynamic FOR query.
 *
 * Evaluates the dynamic query string with USING parameters and returns
 * a pinned Portal for the JIT'd loop to iterate over.
 */
UPLPGSQL_RT_EXPORT void *
uplpgsql_rt_open_dynfors_cursor(UPLpgSQL_exec_state *estate,
								UPLpgSQL_stmt_dynfors *stmt)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	UPLpgSQL_cursor_ctx *cctx;
	Portal		portal;

	portal = exec_dynquery_with_params(plstate, stmt->query, stmt->params,
									   NULL, CURSOR_OPT_NO_SCROLL);
	PinPortal(portal);

	cctx = (UPLpgSQL_cursor_ctx *) palloc0(sizeof(UPLpgSQL_cursor_ctx));
	cctx->portal = portal;

	return (void *) cctx;
}


/* ================================================================
 * Exception handling runtime helpers
 *
 * PL/pgSQL exception handling uses PostgreSQL's subtransaction mechanism:
 * BEGIN a subtransaction before the try block, COMMIT on success, ROLLBACK
 * on error.  The standard PL/pgSQL interpreter implements this with
 * PG_TRY/PG_CATCH macros, which expand to sigsetjmp/longjmp.
 *
 * In JIT'd code, we can't use the PG_TRY macro directly (it's a C macro
 * that creates local variables).  Instead, we decompose the pattern into
 * discrete steps callable from LLVM IR:
 *
 *   1. push_frame  — allocate exception frame, save state, begin subtxn
 *   2. sigsetjmp   — emitted directly as LLVM IR (with returns_twice attr)
 *   3. arm         — install our frame in PG_exception_stack (try path)
 *   4. try_exit    — commit subtxn, restore state (normal completion)
 *   5. catch       — rollback subtxn, capture error, find handler (error path)
 *   6. set_handler_vars — set SQLSTATE/SQLERRM for the matching handler
 *   7. handler_done — restore cur_error, pop mcontext (after handler runs)
 *   8. rethrow     — re-raise error if no handler matched
 *
 * The sigsetjmp call is in LLVM IR rather than C because:
 *   - The jmpbuf must be in the same stack frame as the code that might
 *     longjmp back to it (the compiled function's frame)
 *   - LLVM's returns_twice attribute ensures correct codegen around setjmp
 *
 * Memory management:
 *   The exception frame is heap-allocated (in the parent memory context,
 *   not the subtransaction's) so it survives subtransaction abort.  The
 *   stmt_mcontext is pre-created before entering the subtransaction
 *   because creating memory contexts during error recovery is risky.
 * ================================================================
 */

/*
 * uplpgsql_rt_exception_push_frame - Allocate exception frame, save state,
 *                                     begin subtransaction.
 *
 * Returns a pointer to the heap-allocated frame containing the sigjmp_buf
 * that the JIT'd code will pass to sigsetjmp.
 *
 * MUST be called BEFORE sigsetjmp.  The subtransaction is started here
 * so that it exists when sigsetjmp captures the stack.
 */
UPLPGSQL_RT_EXPORT void *
uplpgsql_rt_exception_push_frame(UPLpgSQL_exec_state *estate,
								 UPLpgSQL_stmt_block *block)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	UPLpgSQL_exception_frame *frame;
	MemoryContext stmt_mcontext;

	/*
	 * Force the stmt_mcontext to exist before entering the subtransaction.
	 * This is critical: we need a place to store error data during catch,
	 * and creating memory contexts during error recovery is risky.
	 */
	stmt_mcontext = get_stmt_mcontext(plstate);

	/*
	 * Allocate the frame in the function's memory context (not the
	 * subtransaction's) so it survives subtransaction abort.
	 */
	frame = (UPLpgSQL_exception_frame *)
		MemoryContextAllocZero(CurrentMemoryContext,
							   sizeof(UPLpgSQL_exception_frame));

	/* Save state that we'll need to restore on catch */
	frame->oldcontext = CurrentMemoryContext;
	frame->oldowner = CurrentResourceOwner;
	frame->old_eval_econtext = plstate->eval_econtext;
	frame->saved_cur_error = plstate->cur_error;
	frame->saved_error_context_stack = error_context_stack;
	frame->stmt_mcontext = stmt_mcontext;

	/* Begin the subtransaction */
	BeginInternalSubTransaction(NULL);

	/* Switch back to function's memory context */
	MemoryContextSwitchTo(frame->oldcontext);

	return (void *) frame;
}

/*
 * uplpgsql_rt_exception_arm - Set PG_exception_stack to our frame's jmpbuf.
 *
 * Called AFTER sigsetjmp returns 0 (the try path).  This is the step that
 * makes ereport(ERROR) jump to our frame instead of the enclosing handler.
 *
 * Also creates a new eval_econtext belonging to the subtransaction.
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_exception_arm(UPLpgSQL_exec_state *estate, void *frame_ptr)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	UPLpgSQL_exception_frame *frame = (UPLpgSQL_exception_frame *) frame_ptr;

	/* Save the current PG exception stack and install ours */
	frame->saved_exception_stack = PG_exception_stack;
	PG_exception_stack = &frame->jmpbuf;


	/*
	 * Create a new eval_econtext belonging to the current subtransaction.
	 * If we try to use the outer one, ExprContext shutdown callbacks will
	 * fire at the wrong times during subxact abort.
	 */
	uplpgsql_create_econtext(plstate);
}

/*
 * uplpgsql_rt_exception_try_exit - Normal exit from try block.
 *
 * Commits the subtransaction, restores PG_exception_stack and eval_econtext.
 * If the block ended with RETURN, copies the return value out of the
 * subtransaction's eval_context first.
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_exception_try_exit(UPLpgSQL_exec_state *estate, void *frame_ptr)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	UPLpgSQL_exception_frame *frame = (UPLpgSQL_exception_frame *) frame_ptr;

	/* Restore PG_exception_stack and error_context_stack before committing */
	PG_exception_stack = frame->saved_exception_stack;
	error_context_stack = frame->saved_error_context_stack;

	/*
	 * If there's a return value, transfer it out of the subtransaction's
	 * memory before we commit (which destroys the subtransaction's context).
	 *
	 * exec_stmt_block guards this with "rc == PLPGSQL_RC_RETURN &&" as well.
	 * We do not have rc here, but the test is equivalent: retisnull is only
	 * cleared by a RETURN that produced a value, so a non-RETURN exit always
	 * has retisnull set and skips the transfer.  Kept deliberately, not by
	 * oversight — threading rc in for a check that cannot change the outcome
	 * would cost a load and an argument on every block exit.
	 */
	if (!plstate->retisset && !plstate->retisnull)
	{
		int16	resTypLen;
		bool	resTypByVal;

		get_typlenbyval(plstate->rettype, &resTypLen, &resTypByVal);
		plstate->retval = datumTransfer(plstate->retval,
										resTypByVal, resTypLen);
	}

	/* Copy retval back to JIT state */
	estate->retval = plstate->retval;
	estate->retisnull = plstate->retisnull;

	/* Commit the subtransaction */
	ReleaseCurrentSubTransaction();
	MemoryContextSwitchTo(frame->oldcontext);
	CurrentResourceOwner = frame->oldowner;

	/* Restore eval_econtext to the outer one */
	plstate->eval_econtext = frame->old_eval_econtext;

	/*
	 * Terminal exit for this block, so release the frame.  Upstream PL/pgSQL
	 * has nothing to free here — it uses a stack-local sigjmp_buf via PG_TRY —
	 * but the JIT decomposition has to heap-allocate it, and a loop body that
	 * enters an exception block would otherwise accumulate one frame per
	 * iteration for the whole call.
	 *
	 * Safe here: PG_exception_stack was restored above, so the sigjmp_buf
	 * inside the frame is no longer a longjmp target, and we have switched
	 * back to the context the frame was allocated in.
	 */
	pfree(frame);
}

/*
 * uplpgsql_rt_exception_catch - Handle a caught exception.
 *
 * Called when sigsetjmp returns non-zero (longjmp from ereport(ERROR)).
 * Rolls back the subtransaction, captures error data, restores state,
 * and finds the matching exception handler.
 *
 * Returns the 0-based index of the matching handler in exc_list,
 * or -1 if no handler matches (caller should rethrow).
 */
UPLPGSQL_RT_EXPORT int32
uplpgsql_rt_exception_catch(UPLpgSQL_exec_state *estate,
							UPLpgSQL_stmt_block *block,
							void *frame_ptr)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	UPLpgSQL_exception_frame *frame = (UPLpgSQL_exception_frame *) frame_ptr;
	ErrorData  *edata;
	ListCell   *e;
	int			handler_idx = 0;

	/* Restore PG_exception_stack and error_context_stack */
	PG_exception_stack = frame->saved_exception_stack;
	error_context_stack = frame->saved_error_context_stack;

	/* Save error info in our pre-allocated stmt_mcontext */
	MemoryContextSwitchTo(frame->stmt_mcontext);
	edata = CopyErrorData();
	FlushErrorState();

	/* Abort the subtransaction */
	RollbackAndReleaseCurrentSubTransaction();
	MemoryContextSwitchTo(frame->oldcontext);
	CurrentResourceOwner = frame->oldowner;

	/*
	 * Set up the stmt_mcontext stack as though we had restored our previous
	 * state and then done push_stmt_mcontext().
	 */
	plstate->stmt_mcontext_parent = frame->stmt_mcontext;
	plstate->stmt_mcontext = NULL;

	/* Delete any nested stmt_mcontexts created as children */
	MemoryContextDeleteChildren(frame->stmt_mcontext);

	/* Restore eval_econtext to the outer one */
	plstate->eval_econtext = frame->old_eval_econtext;

	/*
	 * Clean up eval state. The tuple table was thrown away during subxact
	 * abort, so don't try to free it.
	 */
	plstate->eval_tuptable = NULL;
	exec_eval_cleanup(plstate);

	/* Store the error data for use by handlers */
	estate->cur_error = edata;

	elog(DEBUG1, "uplpgsql: exception_catch: edata=%p sqlerrcode=%d",
		 edata, edata->sqlerrcode);

	/* Find matching handler */
	foreach(e, block->exceptions->exc_list)
	{
		UPLpgSQL_exception *exception = (UPLpgSQL_exception *) lfirst(e);

		if (exception_matches_conditions(edata, exception->conditions))
			return handler_idx;

		handler_idx++;
	}

	/* No match — caller should rethrow */
	return -1;
}

/*
 * uplpgsql_rt_exception_set_handler_vars - Set SQLSTATE and SQLERRM variables
 *                                           for the matching handler.
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_exception_set_handler_vars(UPLpgSQL_exec_state *estate,
									   UPLpgSQL_stmt_block *block,
									   int handler_idx)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	ErrorData  *edata = estate->cur_error;
	UPLpgSQL_var *state_var;
	UPLpgSQL_var *errm_var;

	state_var = (UPLpgSQL_var *)
		plstate->datums[block->exceptions->sqlstate_varno];
	errm_var = (UPLpgSQL_var *)
		plstate->datums[block->exceptions->sqlerrm_varno];

	assign_text_var(plstate, state_var,
					unpack_sql_state(edata->sqlerrcode));
	assign_text_var(plstate, errm_var, edata->message);

	/*
	 * Also record the code in the execstate and refresh the user-declared
	 * SQLSTATE variable, as exec_stmt_block's handler setup does.
	 *
	 * This is distinct from the block's own SQLSTATE above: that one is the
	 * variable the grammar creates for every EXCEPTION block, while
	 * estate->sqlstate_varno tracks a SQL/PSM-style declared SQLSTATE char(5)
	 * that the runtime keeps current (block entry, after FETCH, and here).
	 * The PL/pgSQL grammar never sets stmt_block->sqlstate_varno, so
	 * uplpgsql_set_sqlstate() is a no-op for this driver today and the
	 * omission was invisible — but the plumbing is shared with the SQL/PSM
	 * driver, where it is not.
	 */
	plstate->sqlerrcode = edata->sqlerrcode;
	uplpgsql_set_sqlstate(plstate);

	/* Set cur_error so GET STACKED DIAGNOSTICS works inside the handler */
	plstate->cur_error = edata;
}

/*
 * uplpgsql_rt_exception_handler_done - Clean up after a handler executes.
 *
 * Restores cur_error, pops stmt_mcontext, and frees the error data.
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_exception_handler_done(UPLpgSQL_exec_state *estate,
								   void *frame_ptr)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	UPLpgSQL_exception_frame *frame = (UPLpgSQL_exception_frame *) frame_ptr;

	/* Restore previous cur_error */
	plstate->cur_error = frame->saved_cur_error;
	estate->cur_error = frame->saved_cur_error;

	/* Copy retval back to JIT state (handler may have done RETURN) */
	estate->retval = plstate->retval;
	estate->retisnull = plstate->retisnull;

	/* Restore stmt_mcontext stack and release the error data */
	pop_stmt_mcontext(plstate);
	MemoryContextReset(frame->stmt_mcontext);

	/*
	 * Terminal exit for a handled block; release the frame.  The frame lives
	 * in the function's context, not in stmt_mcontext, so the reset above has
	 * not already freed it.
	 */
	pfree(frame);
}

/*
 * uplpgsql_rt_exception_rethrow - Re-throw an unhandled exception.
 *
 * Called when no WHEN clause matches.  Also cleans up cur_error first.
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_exception_rethrow(UPLpgSQL_exec_state *estate, void *frame_ptr)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	UPLpgSQL_exception_frame *frame = (UPLpgSQL_exception_frame *) frame_ptr;
	ErrorData  *edata = estate->cur_error;

	/* Restore previous cur_error */
	plstate->cur_error = frame->saved_cur_error;
	estate->cur_error = frame->saved_cur_error;

	/*
	 * Restore stmt_mcontext and rethrow.
	 *
	 * The interpreter does not pop here — it lets the outer PG_CATCH unwind do
	 * it.  Popping first is safe: pop_stmt_mcontext only restores the stack
	 * pointer, it does not reset the context, so edata (which lives there)
	 * stays valid across ReThrowError.  Done explicitly because the JIT has no
	 * enclosing PG_CATCH of its own to unwind for it.
	 */
	pop_stmt_mcontext(plstate);

	/*
	 * Terminal exit for an unhandled block.  Free the frame before
	 * ReThrowError longjmps out of here — nothing below this point returns.
	 * edata lives in stmt_mcontext, not in the frame, so it is unaffected.
	 */
	pfree(frame);

	ReThrowError(edata);
}

/*
 * uplpgsql_rt_assign_var_datum - Assign a Datum to a variable with proper
 * freeval cleanup for pass-by-reference types.
 *
 * This is the Tier 2 fmgr bypass store path.  For pass-by-value types,
 * direct GEP+store is sufficient.  For pass-by-reference types (text,
 * numeric, bytea, etc.), the old value may need to be pfree'd and the
 * new value is always freeable (it was palloc'd by the PG function).
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_assign_var_datum(UPLpgSQL_exec_state *estate,
							 int dno, Datum value, bool isnull)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	UPLpgSQL_var	   *var = (UPLpgSQL_var *) plstate->datums[dno];

	/*
	 * For pass-by-reference types, the value came from a PG function call
	 * (e.g. textcat()) and was palloc'd — it's freeable.  For NULL results,
	 * there's nothing to free.
	 *
	 * Note: callers must NOT pass constant Datums here (e.g. from T_Const
	 * nodes) because those live in function memory and aren't freeable.
	 * Constants should go through datumCopy first.
	 */
	assign_simple_var(plstate, var, value, isnull, !isnull);
}

/*
 * uplpgsql_rt_free_var_datum - Free a variable's old pass-by-ref value.
 *
 * Called BEFORE a Tier 2 fmgr bypass function call to prevent
 * use-after-free when palloc reuses the freed block for the new
 * function result.  After this call, the variable's value is cleared
 * (set to 0/NULL with freeval=false) so assign_simple_var won't
 * double-free.
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_free_var_datum(UPLpgSQL_exec_state *estate, int dno)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	UPLpgSQL_var	   *var = (UPLpgSQL_var *) plstate->datums[dno];

	/*
	 * Use assign_simple_var to free the old value and set to NULL.
	 * This handles expanded objects, regular varlena, etc.
	 */
	assign_simple_var(plstate, var, (Datum) 0, true, false);
}

/*
 * uplpgsql_rt_copy_assign_var_datum - Copy a Datum then assign it.
 *
 * For pass-by-reference assignments where the source Datum is NOT palloc'd
 * in the current context (e.g. constants from the plan, or another variable's
 * Datum).  We datumCopy to get a palloc'd copy, then assign with freeable=true.
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_copy_assign_var_datum(UPLpgSQL_exec_state *estate,
								  int dno, Datum value, bool isnull)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	UPLpgSQL_var	   *var = (UPLpgSQL_var *) plstate->datums[dno];

	if (!isnull)
	{
		Datum	copied;

		copied = datumCopy(value, var->datatype->typbyval,
						   var->datatype->typlen);
		assign_simple_var(plstate, var, copied, false, true);
	}
	else
		assign_simple_var(plstate, var, (Datum) 0, true, false);
}

/*
 * uplpgsql_rt_get_recfield - Extract a field value from a record variable.
 *
 * Used by inlined expressions that reference record fields (e.g. r.a).
 * Calls exec_eval_datum to properly resolve the field from the record's
 * expanded record header.
 *
 * Returns the Datum value.  If the field is NULL, returns (Datum) 0.
 * Caller is responsible for null handling if needed.
 */
UPLPGSQL_RT_EXPORT Datum
uplpgsql_rt_get_recfield(UPLpgSQL_exec_state *estate, int recfield_dno)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	UPLpgSQL_datum	   *datum = plstate->datums[recfield_dno];
	Oid					typeid;
	int32				typetypmod;
	Datum				value;
	bool				isnull;

	exec_eval_datum(plstate, datum, &typeid, &typetypmod, &value, &isnull);

	if (isnull)
		return (Datum) 0;

	return value;
}

/*
 * uplpgsql_rt_get_recfield_fast - Fast record field access with compile-time
 * field number.
 *
 * Phase 5d optimization: when the record's tuple descriptor is known at
 * compile time (from SPI_prepare of the FOR query), the field number is
 * resolved once during compilation and passed as a constant.  This skips
 * exec_eval_datum entirely, going straight to expanded_record_get_field.
 */
UPLPGSQL_RT_EXPORT Datum
uplpgsql_rt_get_recfield_fast(UPLpgSQL_exec_state *estate,
							  int rec_dno, int fnumber,
							  bool *isnull_out)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	UPLpgSQL_rec	   *rec;
	ExpandedRecordHeader *erh;

	rec = (UPLpgSQL_rec *) plstate->datums[rec_dno];
	erh = rec->erh;

	if (unlikely(erh == NULL))
	{
		*isnull_out = true;
		return (Datum) 0;
	}

	return expanded_record_get_field(erh, fnumber, isnull_out);
}

/* ----------------------------------------------------------------
 * Native local array helpers (Phase 7)
 *
 * These are called from JIT'd code for native arrays that passed
 * escape analysis.  Only used for the heap allocation path (arrays
 * larger than NATIVE_ARRAY_STACK_THRESHOLD = 4096 bytes) and for
 * bounds check errors.  Small arrays use LLVM alloca + memset
 * directly in generated IR, never calling these helpers at all.
 * ----------------------------------------------------------------
 */

/*
 * Allocate flat memory for a native local array on the heap.
 *
 * Called when the array's byte_size exceeds NATIVE_ARRAY_STACK_THRESHOLD
 * (4KB).  Allocated in datum_context so the memory:
 *   - Lives for the entire function call duration
 *   - Is automatically freed on context destruction (including
 *     when an exception causes longjmp out of the function)
 *   - Does not require explicit pfree on normal return
 */
UPLPGSQL_RT_EXPORT void *
uplpgsql_rt_native_array_alloc(UPLpgSQL_exec_state *estate, int64 byte_size)
{
	MemoryContext oldcxt;
	void *result;

	oldcxt = MemoryContextSwitchTo(estate->uplpgsql_estate->datum_context);
	result = palloc0(byte_size);
	MemoryContextSwitchTo(oldcxt);
	return result;
}

/*
 * Decompose a PG array Datum into flat native memory.
 *
 * Used when a native array variable is assigned from a non-native source
 * (e.g., SELECT INTO arr, arr := ARRAY[...]).  Extracts the element data
 * from the PG array header and copies it into freshly allocated flat memory.
 *
 * Returns the pointer to flat element data.  Stores the element count
 * into *out_nelems.
 *
 * Only supports fixed-size pass-by-value element types (int4, int8, float8)
 * which are the types eligible for native arrays.
 */
UPLPGSQL_RT_EXPORT void *
uplpgsql_rt_native_array_from_datum(UPLpgSQL_exec_state *estate,
									Datum array_datum, bool isnull,
									int elem_size, int *out_nelems,
									int *out_lb, bool **out_nulls)
{
	MemoryContext	oldcxt;
	ArrayType	   *arr;
	int				nelems;
	void		   *result;

	/*
	 * A NULL array reads as empty: len 0 with lower bound 1 makes every
	 * subscript out of range, which is what the caller turns into SQL NULL.
	 */
	*out_nulls = NULL;

	if (isnull)
	{
		*out_nelems = 0;
		*out_lb = 1;
		return NULL;
	}

	arr = DatumGetArrayTypeP(array_datum);

	/*
	 * The native form is a flat one-dimensional vector.  A multi-dimensional
	 * value cannot be held in it — and must not be flattened into it, or the
	 * dimensions are lost on the way back out — so report len -1, the "not a
	 * 1-D array" marker.  The caller treats that as "every single-subscript
	 * read is NULL", which is exactly what PostgreSQL says about a[i] on a
	 * 2-D array, and routes writes to the generic path, which raises "wrong
	 * number of array subscripts" as it should.
	 *
	 * ARR_NDIM == 0 (an empty array) is a genuine empty 1-D value: len 0.
	 */
	if (ARR_NDIM(arr) == 0)
	{
		*out_nelems = 0;
		*out_lb = 1;
		return NULL;
	}

	if (ARR_NDIM(arr) != 1)
	{
		*out_nelems = -1;
		*out_lb = 1;
		return NULL;
	}

	/*
	 * A non-standard lower bound is fine: report it and let the caller index
	 * relative to it.  to_datum() rebuilds with the same bound.
	 */
	nelems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));
	*out_nelems = nelems;
	*out_lb = ARR_LBOUND(arr)[0];

	if (nelems == 0)
		return NULL;

	oldcxt = MemoryContextSwitchTo(estate->uplpgsql_estate->datum_context);
	result = palloc(nelems * elem_size);

	/*
	 * An array with NULL elements is still perfectly native — it just needs a
	 * per-element flag alongside the values, because the flat vector has no
	 * way to say "absent" on its own.  This is not exotic: extending an array
	 * past its end (a[6] := 9 on a 3-element array) is defined to fill the gap
	 * with NULLs, so any array grown that way arrives here with a null bitmap.
	 *
	 * The values themselves are *packed*: PostgreSQL stores nothing at all for
	 * a NULL element, so the source advances only past the ones that are
	 * present, while the native vector keeps a slot for every element and
	 * marks the absent ones.  (A set bit in the bitmap means not-null.)
	 */
	if (ARR_HASNULL(arr))
	{
		uint8	   *bitmap = ARR_NULLBITMAP(arr);
		bool	   *nulls = (bool *) palloc(nelems * sizeof(bool));
		char	   *src = ARR_DATA_PTR(arr);
		int			i;

		for (i = 0; i < nelems; i++)
		{
			nulls[i] = ((bitmap[i / 8] & (1 << (i % 8))) == 0);

			if (nulls[i])
				memset((char *) result + (i * elem_size), 0, elem_size);
			else
			{
				memcpy((char *) result + (i * elem_size), src, elem_size);
				src += elem_size;
			}
		}

		*out_nulls = nulls;
		MemoryContextSwitchTo(oldcxt);
		return result;
	}

	MemoryContextSwitchTo(oldcxt);

	memcpy(result, ARR_DATA_PTR(arr), nelems * elem_size);

	return result;
}

/*
 * Marshal native flat array back to a PG array Datum and store it
 * in the variable.  Called after native array initialization (array_fill)
 * and after subscript writes, so the interpreter path (RETURN, RAISE,
 * SPI expressions) can see a valid PG array value.
 *
 * This is the native→PG direction (complement of from_datum above).
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_native_array_to_datum(UPLpgSQL_exec_state *estate,
								  int varno, void *data, int nelems,
								  int lb, bool *nulls,
								  Oid elemtype, int elem_size)
{
	UPLpgSQL_execstate *plstate = estate->uplpgsql_estate;
	UPLpgSQL_var	   *var;
	Datum			   *elems;
	ArrayType		   *arr;
	int				   i;
	int16			   typlen;
	bool			   typbyval;
	char			   typalign;

	if (data == NULL || nelems <= 0)
		return;

	var = (UPLpgSQL_var *) plstate->datums[varno];

	get_typlenbyvalalign(elemtype, &typlen, &typbyval, &typalign);

	/* Build Datum array from flat memory */
	elems = (Datum *) palloc(nelems * sizeof(Datum));
	for (i = 0; i < nelems; i++)
	{
		if (elem_size == 4)
			elems[i] = Int32GetDatum(((int32 *) data)[i]);
		else if (elemtype == FLOAT8OID)
			elems[i] = Float8GetDatum(((float8 *) data)[i]);
		else
			elems[i] = Int64GetDatum(((int64 *) data)[i]);
	}

	/*
	 * construct_md_array rather than construct_array: the latter always builds
	 * a lower bound of 1, which would silently rebase an array declared as
	 * '[2:3]={9,10}'.
	 */
	arr = construct_md_array(elems, nulls, 1, &nelems, &lb,
							 elemtype, typlen, typbyval, typalign);
	pfree(elems);

	/* Store into the variable */
	assign_simple_var(plstate, var,
					  PointerGetDatum(arr), false, true);
}

/*
 * Bounds check for native array subscript access.
 *
 * PL/pgSQL arrays are 1-based, so valid range is [1, length].
 * This is the cold error path — the inline bounds check in the JIT'd
 * code branches here only when the subscript is out of range.  The
 * LLVM IR marks this call as followed by LLVMBuildUnreachable, which
 * tells the optimizer this path never returns (it always ereport(ERROR)s).
 */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_native_array_bounds_check(int subscript, int length)
{
	if (unlikely(subscript < 1 || subscript > length))
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("array subscript %d out of range [1..%d]",
						subscript, length)));
}
