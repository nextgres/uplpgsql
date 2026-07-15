/*-------------------------------------------------------------------------
 *
 * uplpgsql_handler.c
 *		PostgreSQL language handler entry points for uplpgsql.
 *
 *		This file is the main entry point for all uplpgsql execution.
 *		It contains:
 *
 *		_PG_init()              — Module initialization: sets up LLVM, cache,
 *		                          GUCs, and transaction callbacks.
 *
 *		uplpgsql_call_handler() — Main execution entry for functions and triggers.
 *		                          Compiles the function (get AST via forked parser),
 *		                          checks the JIT cache, optionally JIT-compiles,
 *		                          then executes via native code or interpreter.
 *
 *		uplpgsql_inline_handler() — Executes DO blocks (always interpreted since
 *		                            they run once and JIT overhead would dominate).
 *
 *		uplpgsql_validator()    — CREATE FUNCTION validation via test-compilation.
 *
 *		The call_handler implements the three-state cache flow:
 *		  1. Cache lookup: HIT → use JIT'd code, SKIP → use interpreter
 *		  2. Cache MISS → evaluate heuristic (uplpgsql_should_jit)
 *		  3. If should JIT → compile, cache result; else cache skip marker
 *		  4. JIT compilation failures are caught and fall back to interpreter
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

#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/event_trigger.h"
#include "commands/trigger.h"
#include "funcapi.h"
#include "executor/executor.h"
#include "nodes/parsenodes.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/plancache.h"
#include "utils/resowner.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

PG_MODULE_MAGIC_EXT(
					.name = "uplpgsql",
					.version = PG_VERSION
);

/*
 * Forked PL/pgSQL global variables (originally in pl_handler.c).
 *
 * These mirror the original PL/pgSQL GUCs but under the "uplpgsql." prefix.
 * They are referenced by the forked parser/executor code (upl_comp.c,
 * upl_exec.c, etc.) which was mechanically renamed from plpgsql_* to
 * uplpgsql_*.
 */
static const struct config_enum_entry variable_conflict_options[] = {
	{"error", UPLPGSQL_RESOLVE_ERROR, false},
	{"use_variable", UPLPGSQL_RESOLVE_VARIABLE, false},
	{"use_column", UPLPGSQL_RESOLVE_COLUMN, false},
	{NULL, 0, false}
};

int			uplpgsql_variable_conflict = UPLPGSQL_RESOLVE_ERROR;
bool		uplpgsql_print_strict_params = false;
bool		uplpgsql_check_asserts = true;
static char *uplpgsql_extra_warnings_string = NULL;
static char *uplpgsql_extra_errors_string = NULL;
int			uplpgsql_extra_warnings;
int			uplpgsql_extra_errors;

/* Plugin hook pointer */
UPLpgSQL_plugin **uplpgsql_plugin_ptr = NULL;

/*
 * uplpgsql-specific GUCs (not in standard PL/pgSQL).
 *
 * enable_jit_heuristic: When true, use cost/benefit heuristic to skip JIT for
 *                       functions unlikely to benefit.  When false (default),
 *                       JIT-compile every function.
 * log_compilation:      Log a message when a function is JIT compiled
 * dump_ir:              Dump the generated LLVM IR to the server log (debugging)
 */
static bool uplpgsql_log_compilation = false;
static bool uplpgsql_dump_ir = false;
bool		uplpgsql_enable_jit_heuristic = false;

/* Forward declarations for GUC hooks */
static bool uplpgsql_extra_checks_check_hook(char **newvalue, void **extra,
											 GucSource source);
static void uplpgsql_extra_warnings_assign_hook(const char *newvalue, void *extra);
static void uplpgsql_extra_errors_assign_hook(const char *newvalue, void *extra);

/*
 * Cache staleness check callback for upl_cache_lookup().
 *
 * Returns true if the cached lang_func pointer matches the current one,
 * meaning the JIT'd code's embedded AST pointers are still valid.
 */
static bool
uplpgsql_cache_check(void *cached_lang_func, void *current_lang_func)
{
	return (cached_lang_func == current_lang_func);
}

/* Function declarations */
PG_FUNCTION_INFO_V1(uplpgsql_call_handler);
PG_FUNCTION_INFO_V1(uplpgsql_inline_handler);
PG_FUNCTION_INFO_V1(uplpgsql_validator);

/*
 * _PG_init - module load callback
 */
void
_PG_init(void)
{
	static bool inited = false;

	if (inited)
		return;

	/* Initialize LLVM (core engine) */
	upl_llvm_init();

	/* Initialize function cache (core engine) */
	upl_cache_init();

	/*
	 * Register forked PL/pgSQL GUCs under "uplpgsql." prefix
	 */
	DefineCustomEnumVariable("uplpgsql.variable_conflict",
							 "Sets handling of conflicts between UPL/pgSQL variable names and table column names.",
							 NULL,
							 &uplpgsql_variable_conflict,
							 UPLPGSQL_RESOLVE_ERROR,
							 variable_conflict_options,
							 PGC_SUSET, 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("uplpgsql.print_strict_params",
							 "Print information about parameters in the DETAIL part of the error messages generated on INTO ... STRICT failures.",
							 NULL,
							 &uplpgsql_print_strict_params,
							 false,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("uplpgsql.check_asserts",
							 "Perform checks given in ASSERT statements.",
							 NULL,
							 &uplpgsql_check_asserts,
							 true,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);

	DefineCustomStringVariable("uplpgsql.extra_warnings",
							   "List of programming constructs that should produce a warning.",
							   NULL,
							   &uplpgsql_extra_warnings_string,
							   "none",
							   PGC_USERSET, GUC_LIST_INPUT,
							   uplpgsql_extra_checks_check_hook,
							   uplpgsql_extra_warnings_assign_hook,
							   NULL);

	DefineCustomStringVariable("uplpgsql.extra_errors",
							   "List of programming constructs that should produce an error.",
							   NULL,
							   &uplpgsql_extra_errors_string,
							   "none",
							   PGC_USERSET, GUC_LIST_INPUT,
							   uplpgsql_extra_checks_check_hook,
							   uplpgsql_extra_errors_assign_hook,
							   NULL);

	MarkGUCPrefixReserved("uplpgsql");

	/* uplpgsql-specific GUCs */
	DefineCustomBoolVariable("uplpgsql.log_compilation",
							 "Log when functions are JIT compiled.",
							 NULL,
							 &uplpgsql_log_compilation,
							 false,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("uplpgsql.dump_ir",
							 "Dump LLVM IR to server log.",
							 NULL,
							 &uplpgsql_dump_ir,
							 false,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("uplpgsql.enable_jit_heuristic",
							 "Use cost/benefit heuristic to skip JIT for functions unlikely to benefit. "
							 "When off (default), all functions are JIT compiled.",
							 NULL,
							 &uplpgsql_enable_jit_heuristic,
							 false,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);

	/* Register transaction callbacks for cleanup */
	RegisterXactCallback(uplpgsql_xact_cb, NULL);
	RegisterSubXactCallback(uplpgsql_subxact_cb, NULL);

	/* Set up rendezvous point with optional instrumentation plugin */
	uplpgsql_plugin_ptr = (UPLpgSQL_plugin **)
		find_rendezvous_variable("UPLpgSQL_plugin");

	inited = true;
}

/*
 * GUC check/assign hooks for extra_warnings/extra_errors
 */
static bool
uplpgsql_extra_checks_check_hook(char **newvalue, void **extra, GucSource source)
{
	char	   *rawstring;
	List	   *elemlist;
	ListCell   *l;
	int			extrachecks = 0;
	int		   *myextra;

	if (pg_strcasecmp(*newvalue, "all") == 0)
		extrachecks = UPLPGSQL_XCHECK_ALL;
	else if (pg_strcasecmp(*newvalue, "none") == 0)
		extrachecks = UPLPGSQL_XCHECK_NONE;
	else
	{
		rawstring = pstrdup(*newvalue);
		if (!SplitIdentifierString(rawstring, ',', &elemlist))
		{
			GUC_check_errdetail("List syntax is invalid.");
			pfree(rawstring);
			list_free(elemlist);
			return false;
		}

		foreach(l, elemlist)
		{
			char	   *tok = (char *) lfirst(l);

			if (pg_strcasecmp(tok, "shadowed_variables") == 0)
				extrachecks |= UPLPGSQL_XCHECK_SHADOWVAR;
			else if (pg_strcasecmp(tok, "too_many_rows") == 0)
				extrachecks |= UPLPGSQL_XCHECK_TOOMANYROWS;
			else if (pg_strcasecmp(tok, "strict_multi_assignment") == 0)
				extrachecks |= UPLPGSQL_XCHECK_STRICTMULTIASSIGNMENT;
			else if (pg_strcasecmp(tok, "all") == 0 || pg_strcasecmp(tok, "none") == 0)
			{
				GUC_check_errdetail("Key word \"%s\" cannot be combined with other key words.", tok);
				pfree(rawstring);
				list_free(elemlist);
				return false;
			}
			else
			{
				GUC_check_errdetail("Unrecognized key word: \"%s\".", tok);
				pfree(rawstring);
				list_free(elemlist);
				return false;
			}
		}

		pfree(rawstring);
		list_free(elemlist);
	}

	myextra = (int *) guc_malloc(LOG, sizeof(int));
	if (!myextra)
		return false;
	*myextra = extrachecks;
	*extra = myextra;

	return true;
}

static void
uplpgsql_extra_warnings_assign_hook(const char *newvalue, void *extra)
{
	uplpgsql_extra_warnings = *((int *) extra);
}

static void
uplpgsql_extra_errors_assign_hook(const char *newvalue, void *extra)
{
	uplpgsql_extra_errors = *((int *) extra);
}

/*
 * uplpgsql_call_handler - main entry point for function/trigger execution
 *
 * For regular functions and triggers, we attempt JIT compilation on first
 * call and execute the native code. On subsequent calls, the cached JIT'd
 * function is used directly. Falls back to the interpreter for event
 * triggers (JIT support for those is future work).
 */
Datum
uplpgsql_call_handler(PG_FUNCTION_ARGS)
{
	bool				nonatomic;
	UPLpgSQL_function   *func;
	UPLpgSQL_func	   *jitfunc;
	UPLpgSQL_execstate *save_cur_estate;
	ResourceOwner		procedure_resowner;
	volatile Datum		retval = (Datum) 0;
	int					rc;
	bool				isTrigger;
	bool				isEventTrigger;

	nonatomic = fcinfo->context &&
		IsA(fcinfo->context, CallContext) &&
		!castNode(CallContext, fcinfo->context)->atomic;

	isTrigger = CALLED_AS_TRIGGER(fcinfo);
	isEventTrigger = CALLED_AS_EVENT_TRIGGER(fcinfo);

	/*
	 * Connect to SPI manager
	 */
	SPI_connect_ext(nonatomic ? SPI_OPT_NONATOMIC : 0);

	/*
	 * Compile the function (get AST via our forked parser).
	 */
	func = uplpgsql_compile(fcinfo, isTrigger || isEventTrigger);

	/* Must save and restore prior value of cur_estate */
	save_cur_estate = func->cur_estate;

	/* Mark the function as busy, so it can't be deleted from under us */
	func->cfunc.use_count++;

	/*
	 * If we'll need a procedure-lifespan resowner to execute any CALL or DO
	 * statements, create it now.  Since this resowner is not tied to any
	 * parent, failing to free it would result in process-lifespan leaks.
	 * Therefore, be very wary of adding any code between here and the PG_TRY
	 * block.
	 */
	procedure_resowner =
		(nonatomic && func->requires_procedure_resowner) ?
		ResourceOwnerCreate(NULL, "UPL/pgSQL procedure resources") : NULL;

	/*
	 * For regular functions and triggers, try the JIT path.
	 * Event triggers still use the interpreter.
	 */
	jitfunc = NULL;
	if (!isEventTrigger)
	{
		int			cache_status;
		UPL_func   *upl_jitfunc;

		cache_status = upl_cache_lookup(func->fn_oid,
										func->cfunc.fn_xmin,
										func->cfunc.fn_tid,
										func,
										uplpgsql_cache_check,
										&upl_jitfunc);

		if (cache_status == UPL_CACHE_MISS)
		{
			/*
			 * First call for this function version.  By default we JIT
			 * everything.  When uplpgsql.enable_jit_heuristic is on, run
			 * the cost/benefit scorer to decide.
			 */
			if (!uplpgsql_enable_jit_heuristic || uplpgsql_should_jit(func))
			{
				PG_TRY();
				{
					jitfunc = uplpgsql_compile_function(func);
					upl_cache_store(func->fn_oid,
									func->cfunc.fn_xmin,
									func->cfunc.fn_tid,
									func,
									(UPL_func *) jitfunc);
					elog(LOG, "uplpgsql: JIT compiled function %s (oid %u)",
						 func->fn_signature, func->fn_oid);
				}
				PG_CATCH();
				{
					EmitErrorReport();
					FlushErrorState();
					jitfunc = NULL;
					elog(LOG, "uplpgsql: JIT compilation failed for %s, "
						 "using interpreter",
						 func->fn_signature);
				}
				PG_END_TRY();
			}
			else
			{
				/* Heuristic says skip — cache that decision */
				upl_cache_store_skip(func->fn_oid,
									 func->cfunc.fn_xmin,
									 func->cfunc.fn_tid,
									 func);
				jitfunc = NULL;
				elog(LOG, "uplpgsql: skipping JIT for %s (heuristic)",
					 func->fn_signature);
			}
		}
		else if (cache_status == UPL_CACHE_SKIP)
		{
			jitfunc = NULL;
		}
		else
		{
			/* UPL_CACHE_HIT: cast back to PL/pgSQL-specific type */
			jitfunc = (UPLpgSQL_func *) upl_jitfunc;
		}
	}

	PG_TRY();
	{
		if (isTrigger)
		{
			if (jitfunc != NULL)
			{
				retval = PointerGetDatum(
					uplpgsql_exec_trigger_jit(func,
											  (TriggerData *) fcinfo->context,
											  jitfunc->jit_func));
			}
			else
			{
				retval = PointerGetDatum(
					uplpgsql_exec_trigger(func,
										  (TriggerData *) fcinfo->context));
			}
		}
		else if (isEventTrigger)
		{
			uplpgsql_exec_event_trigger(func,
										(EventTriggerData *) fcinfo->context);
			/* there's no return value in this case */
		}
		else
		{
			if (jitfunc != NULL)
			{
				/* Execute via JIT'd native code */
				retval = uplpgsql_exec_function_jit(func, fcinfo,
													NULL, NULL,
													procedure_resowner,
													!nonatomic,
													jitfunc->jit_func);
			}
			else
			{
				/* Fallback to interpreter */
				retval = uplpgsql_exec_function(func, fcinfo,
												NULL, NULL,
												procedure_resowner,
												!nonatomic);
			}
		}
	}
	PG_FINALLY();
	{
		/* Decrement use-count, restore cur_estate */
		func->cfunc.use_count--;
		func->cur_estate = save_cur_estate;

		/* Be sure to release the procedure resowner if any */
		if (procedure_resowner)
		{
			ReleaseAllPlanCacheRefsInOwner(procedure_resowner);
			ResourceOwnerDelete(procedure_resowner);
		}
	}
	PG_END_TRY();

	/*
	 * Disconnect from SPI manager
	 */
	if ((rc = SPI_finish()) != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed: %s", SPI_result_code_string(rc));

	return retval;
}

/*
 * uplpgsql_inline_handler - DO block execution
 *
 * Executes anonymous code blocks (DO $$ ... $$ LANGUAGE uplpgsql).
 * DO blocks are always interpreted (not JIT'd) since they run once and
 * compilation overhead would exceed any benefit.
 *
 * This mirrors plpgsql_inline_handler() — sets up a private EState and
 * resowner for simple-expression execution that survive COMMIT/ROLLBACK.
 */
Datum
uplpgsql_inline_handler(PG_FUNCTION_ARGS)
{
	LOCAL_FCINFO(fake_fcinfo, 0);
	InlineCodeBlock *codeblock = castNode(InlineCodeBlock,
										  DatumGetPointer(PG_GETARG_DATUM(0)));
	UPLpgSQL_function *func;
	FmgrInfo	flinfo;
	EState	   *simple_eval_estate;
	ResourceOwner simple_eval_resowner;
	Datum		retval;
	int			rc;

	/*
	 * Connect to SPI manager
	 */
	SPI_connect_ext(codeblock->atomic ? 0 : SPI_OPT_NONATOMIC);

	/* Compile the anonymous code block */
	func = uplpgsql_compile_inline(codeblock->source_text);

	/* Mark the function as busy, just pro forma */
	func->cfunc.use_count++;

	/*
	 * Set up a fake fcinfo with just enough info to satisfy
	 * uplpgsql_exec_function().
	 */
	MemSet(fake_fcinfo, 0, SizeForFunctionCallInfo(0));
	MemSet(&flinfo, 0, sizeof(flinfo));
	fake_fcinfo->flinfo = &flinfo;
	flinfo.fn_oid = InvalidOid;
	flinfo.fn_mcxt = CurrentMemoryContext;

	/*
	 * Create a private EState and resowner for simple-expression execution.
	 * These must survive any COMMIT/ROLLBACK the DO block executes.
	 */
	simple_eval_estate = CreateExecutorState();
	simple_eval_resowner =
		ResourceOwnerCreate(NULL, "UPL/pgSQL DO block simple expressions");

	/* Execute the DO block via the interpreter (not JIT'd) */
	PG_TRY();
	{
		retval = uplpgsql_exec_function(func, fake_fcinfo,
										simple_eval_estate,
										simple_eval_resowner,
										simple_eval_resowner,
										codeblock->atomic);
	}
	PG_CATCH();
	{
		/* Clean up on failure */
		uplpgsql_subxact_cb(SUBXACT_EVENT_ABORT_SUB,
							GetCurrentSubTransactionId(),
							0, NULL);

		FreeExecutorState(simple_eval_estate);
		ReleaseAllPlanCacheRefsInOwner(simple_eval_resowner);
		ResourceOwnerDelete(simple_eval_resowner);

		func->cfunc.use_count--;
		uplpgsql_free_function_memory(func);

		PG_RE_THROW();
	}
	PG_END_TRY();

	/* Clean up */
	FreeExecutorState(simple_eval_estate);
	ReleaseAllPlanCacheRefsInOwner(simple_eval_resowner);
	ResourceOwnerDelete(simple_eval_resowner);

	func->cfunc.use_count--;
	uplpgsql_free_function_memory(func);

	/*
	 * Disconnect from SPI manager
	 */
	if ((rc = SPI_finish()) != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed: %s", SPI_result_code_string(rc));

	return retval;
}

/*
 * uplpgsql_validator - CREATE FUNCTION validation
 *
 * Validates by test-compiling the function with PL/pgSQL's parser.
 * Mirrors uplpgsql_validator() logic since that function is not callable
 * as a C function from extensions.
 */
Datum
uplpgsql_validator(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	Form_pg_proc proc;
	bool		is_trigger = false;
	bool		is_event_trigger = false;

	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, funcoid))
		PG_RETURN_VOID();

	/* Get the function's pg_proc entry */
	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcoid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);

	if (proc->prorettype == TRIGGEROID)
		is_trigger = true;
	else if (proc->prorettype == EVENT_TRIGGEROID)
		is_event_trigger = true;

	ReleaseSysCache(tuple);

	/* Postpone body checks if !check_function_bodies */
	if (check_function_bodies)
	{
		LOCAL_FCINFO(fake_fcinfo, 0);
		FmgrInfo	flinfo;
		int			rc;
		TriggerData trigdata;
		EventTriggerData etrigdata;

		SPI_connect();

		MemSet(fake_fcinfo, 0, SizeForFunctionCallInfo(0));
		MemSet(&flinfo, 0, sizeof(flinfo));
		fake_fcinfo->flinfo = &flinfo;
		flinfo.fn_oid = funcoid;
		flinfo.fn_mcxt = CurrentMemoryContext;

		if (is_trigger)
		{
			MemSet(&trigdata, 0, sizeof(trigdata));
			trigdata.type = T_TriggerData;
			fake_fcinfo->context = (Node *) &trigdata;
		}
		else if (is_event_trigger)
		{
			MemSet(&etrigdata, 0, sizeof(etrigdata));
			etrigdata.type = T_EventTriggerData;
			fake_fcinfo->context = (Node *) &etrigdata;
		}

		/* Test-compile the function */
		uplpgsql_compile(fake_fcinfo, is_trigger || is_event_trigger);

		if ((rc = SPI_finish()) != SPI_OK_FINISH)
			elog(ERROR, "SPI_finish failed: %s", SPI_result_code_string(rc));
	}

	PG_RETURN_VOID();
}
