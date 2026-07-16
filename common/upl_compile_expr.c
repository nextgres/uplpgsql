/*-------------------------------------------------------------------------
 *
 * uplpgsql_expr.c
 *		Expression-level compilation: walk PG Expr trees and emit native
 *		LLVM IR, bypassing the ExprEvalStep interpreter entirely.
 *
 *		This is the heart of uplpgsql's performance optimization.  It
 *		implements a three-tier compilation strategy for PL/pgSQL expressions:
 *
 *		Tier 1 — Native LLVM instructions (zero function call overhead):
 *		  int4:   add, sub, mul (overflow-checked via llvm.sadd.with.overflow),
 *		          sdiv, srem (with division-by-zero and MIN/-1 checks), abs, neg
 *		  int8:   same as int4 but with 64-bit variants
 *		  int4/int8 cross-type: operands widened via sext, result is int8
 *		  float8: fadd, fsub, fmul, fdiv, fneg, fabs (via select on fcmp)
 *		  bool:   and, or, not via i1 logic ops
 *		  casts:  int→float (sitofp), float4→float8 (fpext), numeric const
 *		          (compile-time evaluation)
 *
 *		  Variable access is via GEP chains:
 *		    estate → plstate → datums[dno] → var.value
 *		  Record fields use RT_GET_RECFIELD runtime helper.
 *
 *		Tier 2 — Direct PG function pointer call (fmgr bypass):
 *		  At compile time, resolves function OID via fmgr_info() to get
 *		  the C function pointer.  In IR, allocates FunctionCallInfoBaseData
 *		  on the LLVM stack (in entry block to prevent loop stack growth),
 *		  fills in arguments, and calls the function directly.
 *
 *		  Pass-by-value results: stored directly via GEP.
 *		  Pass-by-reference results: stored via RT_ASSIGN_VAR_DATUM which
 *		  handles freeval cleanup, or RT_COPY_ASSIGN_VAR_DATUM for constants
 *		  and variable references that need datumCopy() first.
 *
 *		  Safety: only built-in (system catalog) functions are eligible.
 *		  User-defined functions are excluded because their pointers can
 *		  change via CREATE OR REPLACE.
 *
 *		  Strict functions: emit null-check branch chain with PHI merge.
 *
 *		Tier 3 — Runtime helper fallback:
 *		  uplpgsql_rt_assign_expr, uplpgsql_rt_eval_bool, etc.
 *		  When compile-time SPI_prepare succeeds but the expression can't
 *		  be inlined, the plan is freed so the runtime path can re-prepare
 *		  with exec_simple_check_plan (avoiding 30x+ SPI overhead).
 *
 *		Public API (called from uplpgsql_compile.c):
 *		  uplpgsql_try_compile_assign() — try to inline an assignment
 *		  uplpgsql_try_compile_bool()   — try to inline a boolean condition
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

#include <math.h>				/* INFINITY */

#include "utils/datum.h"		/* datumCopy */

/*
 * Convenience macros for accessing PL/pgSQL-specific lang_data fields.
 */
#define ctx_lang(ctx)			UPLPGSQL_LANG_DATA(ctx)
#define ctx_plstate(ctx)		(ctx_lang(ctx)->plstate_ref)
#define ctx_func(ctx)			(ctx_lang(ctx)->uplpgsql_func)
#define ctx_native_arrays(ctx)	(ctx_lang(ctx)->native_arrays)
#define ctx_num_native_arrays(ctx) (ctx_lang(ctx)->num_native_arrays)

#include "access/transam.h"
#include "catalog/pg_type_d.h"
#include "utils/memutils.h"
#include "executor/spi_priv.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "nodes/parsenodes.h"
#include "utils/builtins.h"
#include "utils/expandedrecord.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

/*
 * Struct offsets for GEP-based variable access.
 *
 * These are computed at C compile time via offsetof() and embedded as
 * LLVM i64 constants in the generated IR.  They allow the JIT'd code to
 * navigate the PL/pgSQL execution state structs without needing LLVM
 * struct type definitions — we treat everything as byte arrays and use
 * offset-based GEPs (similar to the pattern used by NVC's LLVM JIT).
 *
 * This approach is fragile to struct layout changes, which is why
 * PGXS header dependency tracking is important (see CLAUDE.md build notes).
 */
#define OFF_ESTATE_PLSTATE		offsetof(UPLpgSQL_exec_state, uplpgsql_estate)
#define OFF_EXECSTATE_DATUMS	offsetof(UPLpgSQL_execstate, datums)
#define OFF_VAR_VALUE			offsetof(UPLpgSQL_var, value)
#define OFF_VAR_ISNULL			offsetof(UPLpgSQL_var, isnull)
#define OFF_VAR_FREEVAL			offsetof(UPLpgSQL_var, freeval)

/* FunctionCallInfoBaseData offsets for fmgr bypass */
#define OFF_FCI_FLINFO			offsetof(FunctionCallInfoBaseData, flinfo)
#define OFF_FCI_CONTEXT			offsetof(FunctionCallInfoBaseData, context)
#define OFF_FCI_RESULTINFO		offsetof(FunctionCallInfoBaseData, resultinfo)
#define OFF_FCI_COLLATION		offsetof(FunctionCallInfoBaseData, fncollation)
#define OFF_FCI_ISNULL			offsetof(FunctionCallInfoBaseData, isnull)
#define OFF_FCI_NARGS			offsetof(FunctionCallInfoBaseData, nargs)
#define OFF_FCI_ARGS			offsetof(FunctionCallInfoBaseData, args)
#define SIZE_NULLABLE_DATUM		sizeof(NullableDatum)
#define OFF_ND_VALUE			offsetof(NullableDatum, value)
#define OFF_ND_ISNULL			offsetof(NullableDatum, isnull)

/* ExpandedRecordHeader offsets for inline record field access (Phase 5d) */
#define OFF_REC_ERH				offsetof(UPLpgSQL_rec, erh)
#define OFF_ERH_FLAGS			offsetof(ExpandedRecordHeader, flags)
#define OFF_ERH_DVALUES			offsetof(ExpandedRecordHeader, dvalues)
#define OFF_ERH_DNULLS			offsetof(ExpandedRecordHeader, dnulls)

/*
 * Type classification for the expression compiler.
 *
 * Used by uplpgsql_classify_expr() to recursively determine whether an
 * entire expression tree can be compiled to Tier 1 native instructions.
 * Only expressions where ALL nodes classify to a known type (not UNKNOWN)
 * are eligible for Tier 1 compilation.  UNKNOWN falls through to Tier 2
 * or Tier 3.
 */
typedef enum ExprTypeClass
{
	EXPR_TYPE_INT4,
	EXPR_TYPE_INT8,
	EXPR_TYPE_FLOAT8,
	EXPR_TYPE_BOOL,
	EXPR_TYPE_UNKNOWN		/* not natively inlineable */
} ExprTypeClass;

/* Forward declarations */
static Expr *uplpgsql_prepare_and_get_expr(UPLpgSQL_compile_ctx *ctx,
										   UPLpgSQL_expr *expr);
static ExprTypeClass uplpgsql_classify_expr(Expr *expr);
static LLVMValueRef uplpgsql_compile_expr_datum(UPLpgSQL_compile_ctx *ctx,
												Expr *expr,
												LLVMValueRef estate_ref,
												ExprTypeClass *result_type);
static LLVMValueRef uplpgsql_compile_expr_bool(UPLpgSQL_compile_ctx *ctx,
											   Expr *expr,
											   LLVMValueRef estate_ref);

/* Tier 2: fmgr bypass */
static bool uplpgsql_can_fmgr_compile(Expr *expr);
static LLVMValueRef uplpgsql_compile_expr_fmgr(UPLpgSQL_compile_ctx *ctx,
											    Expr *expr,
											    LLVMValueRef estate_ref);
static LLVMValueRef uplpgsql_compile_expr_fmgr_full(UPLpgSQL_compile_ctx *ctx,
													 Expr *expr,
													 LLVMValueRef estate_ref,
													 LLVMValueRef *isnull_out);

/* Helpers */
static inline LLVMBasicBlockRef
expr_append_block(UPLpgSQL_compile_ctx *ctx, const char *name)
{
	return LLVMAppendBasicBlockInContext(ctx->context, ctx->function, name);
}

/*
 * Look up whether a datum number corresponds to a native local array
 * (one that passed escape analysis in Phase 7).
 * Returns the UPLpgSQL_native_array metadata if found, NULL otherwise.
 * When NULL, the caller should use the standard PG array path
 * (RT_ARRAY_GET_ELEMENT / RT_ARRAY_SET_ELEMENT).
 */
static UPLpgSQL_native_array *
find_native_array(UPLpgSQL_compile_ctx *ctx, int dno)
{
	int		i;

	for (i = 0; i < ctx_num_native_arrays(ctx); i++)
	{
		if (ctx_native_arrays(ctx)[i].dno == dno)
			return &ctx_native_arrays(ctx)[i];
	}
	return NULL;
}

/*
 * Resolve array element type info at compile time and emit LLVM constants.
 * Avoids repeated get_element_type()/get_typlenbyvalalign() at runtime.
 */
typedef struct ArrayTypeInfo
{
	LLVMValueRef	typlen_val;		/* i32: array type length (-1 for varlena) */
	LLVMValueRef	elemtype_val;	/* i32: element type OID */
	LLVMValueRef	elmlen_val;		/* i16: element length */
	LLVMValueRef	elmbyval_val;	/* i1: element pass-by-value */
	LLVMValueRef	elmalign_val;	/* i8: element alignment */
} ArrayTypeInfo;

static void
resolve_array_type_info(UPLpgSQL_compile_ctx *ctx, int array_dno,
						ArrayTypeInfo *info)
{
	UPLpgSQL_var   *arrayvar;
	Oid				elemtype;
	int16			elmlen;
	bool			elmbyval;
	char			elmalign;

	arrayvar = (UPLpgSQL_var *) ctx_func(ctx)->datums[array_dno];
	elemtype = get_element_type(arrayvar->datatype->typoid);
	get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);

	info->typlen_val = LLVMConstInt(ctx->types[UPLPGSQL_INT32],
									arrayvar->datatype->typlen, true);
	info->elemtype_val = LLVMConstInt(ctx->types[UPLPGSQL_INT32],
									  elemtype, false);
	info->elmlen_val = LLVMConstInt(ctx->types[UPLPGSQL_INT16],
									elmlen, true);
	info->elmbyval_val = LLVMConstInt(ctx->types[UPLPGSQL_INT1],
									  elmbyval ? 1 : 0, false);
	info->elmalign_val = LLVMConstInt(ctx->types[UPLPGSQL_INT8],
									  elmalign, false);
}


/* ================================================================
 * Variable access via GEP
 * ================================================================
 */

/*
 * Emit IR to load a variable's Datum (i64) via struct offset GEPs.
 */
static LLVMValueRef
uplpgsql_emit_load_var_datum(UPLpgSQL_compile_ctx *ctx,
							LLVMValueRef estate_ref, int dno)
{
	LLVMBuilderRef	builder = ctx->builder;
	LLVMTypeRef		i8 = ctx->types[UPLPGSQL_INT8];
	LLVMTypeRef		i64 = ctx->types[UPLPGSQL_INT64];
	LLVMTypeRef		ptr = ctx->types[UPLPGSQL_PTR];
	LLVMValueRef	off, gep, plstate, datums, datum;

	/* estate->uplpgsql_estate */
	off = LLVMConstInt(i64, OFF_ESTATE_PLSTATE, false);
	gep = LLVMBuildGEP2(builder, i8, estate_ref, &off, 1, "plstate.ptr");
	plstate = LLVMBuildLoad2(builder, ptr, gep, "plstate");

	/* plstate->datums */
	off = LLVMConstInt(i64, OFF_EXECSTATE_DATUMS, false);
	gep = LLVMBuildGEP2(builder, i8, plstate, &off, 1, "datums.ptr");
	datums = LLVMBuildLoad2(builder, ptr, gep, "datums");

	/* datums[dno] */
	off = LLVMConstInt(i64, dno, false);
	gep = LLVMBuildGEP2(builder, ptr, datums, &off, 1, "datum.slot");
	datum = LLVMBuildLoad2(builder, ptr, gep, "datum");

	/* datum->value */
	off = LLVMConstInt(i64, OFF_VAR_VALUE, false);
	gep = LLVMBuildGEP2(builder, i8, datum, &off, 1, "value.ptr");
	return LLVMBuildLoad2(builder, i64, gep, "var.datum");
}

/*
 * Emit IR to load a Datum from a param, handling both simple variables
 * and RECFIELD datums.  For RECFIELD, emits a call to RT_GET_RECFIELD;
 * for plain vars, uses the fast GEP path.
 *
 * Phase 5d optimization: when the parent record has a compile-time erh
 * (installed by uplpgsql_compile_fors), we resolve the field number at
 * compile time and emit inline LLVM IR to GEP directly into
 * erh->dvalues[fnumber-1], with a slow-path fallback to RT_GET_RECFIELD.
 */
static LLVMValueRef
uplpgsql_emit_load_param_datum(UPLpgSQL_compile_ctx *ctx,
							   LLVMValueRef estate_ref, int dno)
{
	UPLpgSQL_datum *d = ctx_func(ctx)->datums[dno];
	UPLpgSQL_native_array *na;

	/*
	 * Whole-datum read of a native array (y := x, f(x), ...).  The live
	 * contents are in flat memory and the variable's Datum is stale, so
	 * marshal it back before the load.  This is the only path by which a
	 * native array is read as a whole Datum — subscript reads never reach
	 * here, they GEP into data_ptr via the T_SubscriptingRef case — so the
	 * cost falls only on genuine escapes.
	 */
	na = find_native_array(ctx, dno);
	if (na != NULL)
	{
		elog(DEBUG1, "uplpgsql: native array to_datum dno %d (whole-datum read)",
			 dno);
		uplpgsql_emit_sync_native_array(ctx, na);
	}

	/*
	 * Promise datums (TG_OP, TG_WHEN, etc.) need runtime resolution via
	 * exec_eval_datum → uplpgsql_fulfill_promise.  We can't load them
	 * with a direct GEP because the value isn't populated until first access.
	 */
	if (d->dtype == UPLPGSQL_DTYPE_PROMISE)
	{
		LLVMValueRef args[] = {
			estate_ref,
			LLVMConstInt(ctx->types[UPLPGSQL_INT32], dno, false)
		};
		return LLVMBuildCall2(ctx->builder,
							  ctx->rt_fntypes[RT_GET_RECFIELD],
							  ctx->rt_funcs[RT_GET_RECFIELD],
							  args, 2, "promise.datum");
	}

	if (d->dtype == UPLPGSQL_DTYPE_RECFIELD)
	{
		UPLpgSQL_recfield  *recfield = (UPLpgSQL_recfield *) d;
		UPLpgSQL_rec	   *rec;

		rec = (UPLpgSQL_rec *) ctx_func(ctx)->datums[recfield->recparentno];

		/*
		 * Fast path: parent record has a compile-time erh (Phase 5d).
		 * Resolve the field number now and inline the expanded record
		 * field access directly as LLVM IR.
		 *
		 * Inlines: load rec → load erh → check erh != NULL &&
		 * (flags & ER_FLAG_DVALUES_VALID) → GEP erh->dvalues[fnumber-1].
		 * Falls back to RT_GET_RECFIELD for the slow path.
		 */
		if (rec->erh != NULL)
		{
			ExpandedRecordFieldInfo finfo;

			if (expanded_record_lookup_field(rec->erh,
											 recfield->fieldname,
											 &finfo))
			{
				LLVMTypeRef i8 = ctx->types[UPLPGSQL_INT8];
				LLVMTypeRef i32 = ctx->types[UPLPGSQL_INT32];
				LLVMTypeRef i64 = ctx->types[UPLPGSQL_INT64];
				LLVMTypeRef ptr = ctx->types[UPLPGSQL_PTR];
				LLVMBuilderRef builder = ctx->builder;
				LLVMValueRef off, gep, plstate, datums, datum;
				LLVMValueRef erh_ptr, flags_val, dvalues_ptr, field_datum;
				LLVMValueRef cond_erh, cond_flags, cond;
				LLVMBasicBlockRef bb_fast, bb_slow, bb_merge;
				LLVMValueRef phi;
				int field_idx = finfo.fnumber - 1;  /* 0-based array index */

				/* Navigate: estate → plstate → datums[rec_dno] → rec */
				off = LLVMConstInt(i64, OFF_ESTATE_PLSTATE, false);
				gep = LLVMBuildGEP2(builder, i8, estate_ref, &off, 1, "rf.plstate.ptr");
				plstate = LLVMBuildLoad2(builder, ptr, gep, "rf.plstate");

				off = LLVMConstInt(i64, OFF_EXECSTATE_DATUMS, false);
				gep = LLVMBuildGEP2(builder, i8, plstate, &off, 1, "rf.datums.ptr");
				datums = LLVMBuildLoad2(builder, ptr, gep, "rf.datums");

				off = LLVMConstInt(i64, recfield->recparentno, false);
				gep = LLVMBuildGEP2(builder, ptr, datums, &off, 1, "rf.rec.slot");
				datum = LLVMBuildLoad2(builder, ptr, gep, "rf.rec");

				/* Load erh = rec->erh */
				off = LLVMConstInt(i64, OFF_REC_ERH, false);
				gep = LLVMBuildGEP2(builder, i8, datum, &off, 1, "rf.erh.ptr");
				erh_ptr = LLVMBuildLoad2(builder, ptr, gep, "rf.erh");

				/* Check erh != NULL */
				cond_erh = LLVMBuildICmp(builder, LLVMIntNE, erh_ptr,
					LLVMConstNull(ptr), "rf.erh.notnull");

				/* Check erh->flags & ER_FLAG_DVALUES_VALID (0x0004) */
				off = LLVMConstInt(i64, OFF_ERH_FLAGS, false);
				gep = LLVMBuildGEP2(builder, i8, erh_ptr, &off, 1, "rf.flags.ptr");
				flags_val = LLVMBuildLoad2(builder, i32, gep, "rf.flags");
				cond_flags = LLVMBuildICmp(builder, LLVMIntNE,
					LLVMBuildAnd(builder, flags_val,
						LLVMConstInt(i32, 0x0004, false), "rf.flags.masked"),
					LLVMConstInt(i32, 0, false), "rf.dvalues.valid");

				cond = LLVMBuildAnd(builder, cond_erh, cond_flags, "rf.fast.ok");

				/* Branch: fast inline path vs slow runtime call */
				bb_fast = LLVMAppendBasicBlockInContext(ctx->context,
					ctx->function, "rf.fast");
				bb_slow = LLVMAppendBasicBlockInContext(ctx->context,
					ctx->function, "rf.slow");
				bb_merge = LLVMAppendBasicBlockInContext(ctx->context,
					ctx->function, "rf.merge");
				LLVMBuildCondBr(builder, cond, bb_fast, bb_slow);

				/* Fast path: erh->dvalues[fnumber-1] */
				LLVMPositionBuilderAtEnd(builder, bb_fast);
				off = LLVMConstInt(i64, OFF_ERH_DVALUES, false);
				gep = LLVMBuildGEP2(builder, i8, erh_ptr, &off, 1, "rf.dvalues.ptr");
				dvalues_ptr = LLVMBuildLoad2(builder, ptr, gep, "rf.dvalues");
				off = LLVMConstInt(i64, field_idx, false);
				gep = LLVMBuildGEP2(builder, i64, dvalues_ptr, &off, 1, "rf.field.ptr");
				field_datum = LLVMBuildLoad2(builder, i64, gep, "rf.field.datum");
				LLVMBuildBr(builder, bb_merge);

				/* Slow path: call RT_GET_RECFIELD */
				LLVMPositionBuilderAtEnd(builder, bb_slow);
				{
					LLVMValueRef slow_args[] = {
						estate_ref,
						LLVMConstInt(i32, dno, false)
					};
					LLVMValueRef slow_result = LLVMBuildCall2(builder,
						ctx->rt_fntypes[RT_GET_RECFIELD],
						ctx->rt_funcs[RT_GET_RECFIELD],
						slow_args, 2, "rf.slow.datum");
					LLVMBuildBr(builder, bb_merge);

					/* Merge with PHI */
					LLVMPositionBuilderAtEnd(builder, bb_merge);
					phi = LLVMBuildPhi(builder, i64, "rf.datum");
					{
						LLVMValueRef vals[] = { field_datum, slow_result };
						LLVMBasicBlockRef bbs[] = { bb_fast, bb_slow };
						LLVMAddIncoming(phi, vals, bbs, 2);
					}
					return phi;
				}
			}
		}

		/* Slow path: call exec_eval_datum via RT_GET_RECFIELD */
		{
			LLVMValueRef args[] = {
				estate_ref,
				LLVMConstInt(ctx->types[UPLPGSQL_INT32], dno, false)
			};
			return LLVMBuildCall2(ctx->builder,
								  ctx->rt_fntypes[RT_GET_RECFIELD],
								  ctx->rt_funcs[RT_GET_RECFIELD],
								  args, 2, "recfield.datum");
		}
	}

	return uplpgsql_emit_load_var_datum(ctx, estate_ref, dno);
}

/*
 * Emit IR to check if a param is NULL.  For RECFIELD datums we
 * conservatively return false (not-null) — matching the existing Tier 1
 * behavior of not null-checking variable loads.
 */
static LLVMValueRef
uplpgsql_emit_load_param_isnull(UPLpgSQL_compile_ctx *ctx,
								LLVMValueRef estate_ref, int dno)
{
	UPLpgSQL_datum *d = ctx_func(ctx)->datums[dno];
	LLVMBuilderRef builder = ctx->builder;
	LLVMTypeRef i1 = ctx->types[UPLPGSQL_INT1];
	UPLpgSQL_native_array *na;

	if (d->dtype == UPLPGSQL_DTYPE_RECFIELD ||
		d->dtype == UPLPGSQL_DTYPE_PROMISE)
		return LLVMConstInt(i1, 0, false);

	/*
	 * A native array's variable carries a stale isnull — it is still the
	 * declared-NULL initial value if the array was only ever populated in
	 * flat memory.  Callers load isnull and the datum separately (and
	 * isnull first), so sync here as well as in the datum load; otherwise
	 * a live array reads back as NULL.
	 */
	na = find_native_array(ctx, dno);
	if (na != NULL)
		uplpgsql_emit_sync_native_array(ctx, na);

	/* For plain vars: load datum->isnull */
	{
		LLVMTypeRef i8 = ctx->types[UPLPGSQL_INT8];
		LLVMTypeRef i64 = ctx->types[UPLPGSQL_INT64];
		LLVMTypeRef ptr = ctx->types[UPLPGSQL_PTR];
		LLVMValueRef off, gep, plstate, datums, datum, isnull_raw;

		off = LLVMConstInt(i64, OFF_ESTATE_PLSTATE, false);
		gep = LLVMBuildGEP2(builder, i8, estate_ref, &off, 1, "plstate.ptr");
		plstate = LLVMBuildLoad2(builder, ptr, gep, "plstate");

		off = LLVMConstInt(i64, OFF_EXECSTATE_DATUMS, false);
		gep = LLVMBuildGEP2(builder, i8, plstate, &off, 1, "datums.ptr");
		datums = LLVMBuildLoad2(builder, ptr, gep, "datums");

		off = LLVMConstInt(i64, dno, false);
		gep = LLVMBuildGEP2(builder, ptr, datums, &off, 1, "datum.slot");
		datum = LLVMBuildLoad2(builder, ptr, gep, "datum");

		off = LLVMConstInt(i64, OFF_VAR_ISNULL, false);
		gep = LLVMBuildGEP2(builder, i8, datum, &off, 1, "isnull.ptr");
		isnull_raw = LLVMBuildLoad2(builder, i8, gep, "isnull.raw");
		return LLVMBuildTrunc(builder, isnull_raw, i1, "isnull");
	}
}

/*
 * Emit a test for "element idx of this native array is NULL".
 *
 * nulls_ptr holds either a per-element bool array or NULL when no element is
 * null, so the flags load has to be guarded.  Returns an i1.
 */
static LLVMValueRef
emit_native_array_elem_isnull(UPLpgSQL_compile_ctx *ctx,
							  UPLpgSQL_native_array *na, LLVMValueRef idx0)
{
	LLVMBuilderRef		builder = ctx->builder;
	LLVMTypeRef			i8 = ctx->types[UPLPGSQL_INT8];
	LLVMTypeRef			i1 = ctx->types[UPLPGSQL_INT1];
	LLVMValueRef		nulls, has_nulls, gep, flag, phi;
	LLVMValueRef		vals[2];
	LLVMBasicBlockRef	blocks[2];
	LLVMBasicBlockRef	load_bb, done_bb, from_bb;

	nulls = LLVMBuildLoad2(builder, ctx->types[UPLPGSQL_PTR],
						   na->nulls_ptr, "na.nulls");
	has_nulls = LLVMBuildICmp(builder, LLVMIntNE, nulls,
							  LLVMConstNull(ctx->types[UPLPGSQL_PTR]),
							  "na.has.nulls");

	load_bb = expr_append_block(ctx, "na.nulls.load");
	done_bb = expr_append_block(ctx, "na.nulls.done");

	from_bb = LLVMGetInsertBlock(builder);
	LLVMBuildCondBr(builder, has_nulls, load_bb, done_bb);

	LLVMPositionBuilderAtEnd(builder, load_bb);
	gep = LLVMBuildGEP2(builder, i8, nulls, &idx0, 1, "na.null.ptr");
	flag = LLVMBuildLoad2(builder, i8, gep, "na.null.raw");
	flag = LLVMBuildTrunc(builder, flag, i1, "na.null.flag");
	LLVMBuildBr(builder, done_bb);

	LLVMPositionBuilderAtEnd(builder, done_bb);
	phi = LLVMBuildPhi(builder, i1, "na.elem.isnull");
	vals[0] = LLVMConstInt(i1, 0, false);
	blocks[0] = from_bb;
	vals[1] = flag;
	blocks[1] = load_bb;
	LLVMAddIncoming(phi, vals, blocks, 2);

	return phi;
}

/*
 * OR together the isnull flags of every leaf of a Tier 1 expression.
 *
 * Every operator Tier 1 accepts is strict — int4/int8 arithmetic, the
 * comparison operators, and the numeric casts all return NULL if any input is
 * NULL — so an expression is NULL exactly when some leaf is.  That lets the
 * caller decide nullness once, up front, instead of threading an isnull flag
 * through each node.  uplpgsql_classify_expr() keeps the non-strict operators
 * (AND/OR/NOT) out of Tier 1 precisely so this holds.
 *
 * Constants are never null here: classify_expr() rejects a null Const.
 *
 * Returns an i1, or NULL if the expression has no nullable leaf at all (a
 * constant expression), letting the caller skip the check entirely.
 */
static LLVMValueRef
tier1_expr_any_null(UPLpgSQL_compile_ctx *ctx, Expr *expr,
					LLVMValueRef estate_ref)
{
	LLVMBuilderRef	builder = ctx->builder;

	if (expr == NULL)
		return NULL;

	switch (nodeTag(expr))
	{
		case T_Const:
			/* classify_expr() rejects null Consts, so this is never null */
			return NULL;

		case T_Param:
			{
				Param *p = (Param *) expr;

				return uplpgsql_emit_load_param_isnull(ctx, estate_ref,
													  p->paramid - 1);
			}

		case T_RelabelType:
			return tier1_expr_any_null(ctx, ((RelabelType *) expr)->arg,
									   estate_ref);

		case T_SubscriptingRef:
			{
				/*
				 * A native array read is NULL when the array or the subscript
				 * is NULL — and also whenever the subscript is out of range.
				 *
				 * PostgreSQL returns NULL for a[i] outside the array's bounds,
				 * for a[i] on a NULL or empty array, and for a single-subscript
				 * read of a multi-dimensional array.  All three are runtime
				 * facts, and this function already returns a runtime i1, so
				 * folding them in here is what makes them come out as SQL NULL:
				 * the caller stores NULL whenever this is true.
				 *
				 * It also means the value path below is reached only for a
				 * subscript that is definitely in range, so it needs no bounds
				 * check of its own.
				 *
				 *     len < 0     -> not a 1-D array (from_datum's marker)
				 *     i < lb      -> below the lower bound
				 *     i > lb+len-1-> past the end (len 0 catches NULL/empty)
				 */
				SubscriptingRef *s = (SubscriptingRef *) expr;
				LLVMValueRef	acc = NULL;
				ListCell	   *lc;
				UPLpgSQL_native_array *na = NULL;

				if (IsA(s->refexpr, Param) &&
					list_length(s->refupperindexpr) == 1)
					na = find_native_array(ctx,
										   ((Param *) s->refexpr)->paramid - 1);

				/*
				 * Do NOT ask whether the array variable itself is NULL when it
				 * is native.
				 *
				 * That question goes through uplpgsql_emit_load_param_isnull(),
				 * which treats reading a native array's variable as a
				 * whole-datum escape and marshals the entire array back out to
				 * its Datum first.  In a loop over a[i] that is a full array
				 * rebuild *per iteration* — it made a 1000-element read loop
				 * ~100x slower than the interpreter.
				 *
				 * It is also unnecessary: from_datum reports len 0 for a NULL
				 * array, so the range test below already answers NULL for it.
				 */
				if (na == NULL)
					acc = tier1_expr_any_null(ctx, s->refexpr, estate_ref);

				foreach(lc, s->refupperindexpr)
				{
					LLVMValueRef v = tier1_expr_any_null(ctx,
														 (Expr *) lfirst(lc),
														 estate_ref);
					if (v == NULL)
						continue;
					acc = (acc == NULL) ? v
						: LLVMBuildOr(builder, acc, v, "anynull");
				}

				if (na != NULL)
				{
					LLVMTypeRef		i32 = ctx->types[UPLPGSQL_INT32];
					ExprTypeClass	idx_class;
					LLVMValueRef	idx, len, lb, oob;

					idx = uplpgsql_compile_expr_datum(ctx,
						(Expr *) linitial(s->refupperindexpr), estate_ref,
						&idx_class);
					if (idx_class == EXPR_TYPE_INT8)
						idx = LLVMBuildTrunc(builder, idx, i32, "na.idx32");

					len = LLVMBuildLoad2(builder, i32, na->len_ptr, "na.len");
					lb = LLVMBuildLoad2(builder, i32, na->lb_ptr, "na.lb");

					/* len < 0: not a 1-D array, so a[i] is NULL */
					oob = LLVMBuildICmp(builder, LLVMIntSLT, len,
										LLVMConstInt(i32, 0, true),
										"na.notflat");
					/* i < lb */
					oob = LLVMBuildOr(builder, oob,
						LLVMBuildICmp(builder, LLVMIntSLT, idx, lb,
									  "na.below"),
						"na.oob");
					/* i > lb + len - 1 */
					oob = LLVMBuildOr(builder, oob,
						LLVMBuildICmp(builder, LLVMIntSGT, idx,
							LLVMBuildSub(builder,
								LLVMBuildAdd(builder, lb, len, "na.end"),
								LLVMConstInt(i32, 1, false), "na.last"),
							"na.above"),
						"na.oob2");

					/*
					 * ...and, when in range, the element may itself be NULL —
					 * PostgreSQL leaves NULLs behind when an assignment
					 * extends an array past its end.  Only meaningful for an
					 * in-range subscript, but idx0 is harmless otherwise
					 * because the flags load is guarded and the result is
					 * OR-ed with oob anyway.
					 */
					{
						LLVMValueRef idx0, elem_null;

						idx0 = LLVMBuildSub(builder, idx, lb, "na.idx0");
						elem_null = emit_native_array_elem_isnull(ctx, na,
																  idx0);
						oob = LLVMBuildOr(builder, oob, elem_null,
										  "na.oob.or.null");
					}

					acc = (acc == NULL) ? oob
						: LLVMBuildOr(builder, acc, oob, "anynull");
				}
				else if (IsA(s->refexpr, Param) &&
						 list_length(s->refupperindexpr) == 1 &&
						 s->reflowerindexpr == NIL)
				{
					/*
					 * Non-native array — a function parameter, or an element
					 * type with no native form.  The read goes through
					 * array_get_element, which reports a NULL element, an
					 * out-of-range subscript, and a dimension mismatch alike
					 * through its isNull output — exactly the cases
					 * PostgreSQL reads as SQL NULL.  Probe it here so the
					 * caller's NULL branch covers them; the value path
					 * re-reads the element, which has no side effects and
					 * only costs this already-slow path a second helper
					 * call.  Without the probe a NULL element read as a
					 * value — "x := a[2]" on a parameter array — came out
					 * as 0.
					 *
					 * A NULL subscript reaches the helper as garbage, but
					 * array_get_element answers any integer safely and the
					 * loop above has already folded the subscript's own
					 * nullness into acc, which wins regardless of what the
					 * probe reads.
					 */
					int				array_dno =
						((Param *) s->refexpr)->paramid - 1;
					LLVMTypeRef		i1 = ctx->types[UPLPGSQL_INT1];
					LLVMTypeRef		i32 = ctx->types[UPLPGSQL_INT32];
					ExprTypeClass	idx_class;
					LLVMValueRef	idx, isnull_ptr, elem_null;
					LLVMBasicBlockRef entry_bb;

					idx = uplpgsql_compile_expr_datum(ctx,
						(Expr *) linitial(s->refupperindexpr), estate_ref,
						&idx_class);
					if (idx_class == EXPR_TYPE_INT8)
						idx = LLVMBuildTrunc(builder, idx, i32, "sref.idx32");

					/* Alloca for isNull output (must be in entry block) */
					entry_bb = LLVMGetEntryBasicBlock(ctx->function);
					{
						LLVMBuilderRef tmp =
							LLVMCreateBuilderInContext(ctx->context);

						LLVMPositionBuilderBefore(tmp,
							LLVMGetFirstInstruction(entry_bb));
						isnull_ptr = LLVMBuildAlloca(tmp, i1,
													 "sref_probe_isnull");
						LLVMDisposeBuilder(tmp);
					}

					{
						ArrayTypeInfo ati;

						resolve_array_type_info(ctx, array_dno, &ati);
						{
							LLVMValueRef args[] = {
								estate_ref,
								LLVMConstInt(i32, array_dno, false),
								idx,
								ati.typlen_val,
								ati.elmlen_val,
								ati.elmbyval_val,
								ati.elmalign_val,
								isnull_ptr
							};

							LLVMBuildCall2(builder,
								ctx->rt_fntypes[RT_ARRAY_GET_ELEMENT],
								ctx->rt_funcs[RT_ARRAY_GET_ELEMENT],
								args, 8, "sref.probe");
						}
					}

					elem_null = LLVMBuildLoad2(builder, i1, isnull_ptr,
											   "sref.elemnull");
					acc = (acc == NULL) ? elem_null
						: LLVMBuildOr(builder, acc, elem_null, "anynull");
				}

				return acc;
			}

		case T_OpExpr:
		case T_FuncExpr:
			{
				List	   *args = IsA(expr, OpExpr) ? ((OpExpr *) expr)->args
													 : ((FuncExpr *) expr)->args;
				LLVMValueRef acc = NULL;
				ListCell   *lc;

				foreach(lc, args)
				{
					LLVMValueRef v = tier1_expr_any_null(ctx,
														 (Expr *) lfirst(lc),
														 estate_ref);
					if (v == NULL)
						continue;
					acc = (acc == NULL) ? v
						: LLVMBuildOr(builder, acc, v, "anynull");
				}
				return acc;
			}

		default:
			/*
			 * Unreachable: classify_expr() admits nothing else into Tier 1.
			 * Be conservative anyway and claim it might be null, which costs
			 * only the fallback path.
			 */
			return LLVMConstInt(ctx->types[UPLPGSQL_INT1], 1, false);
	}
}

/*
 * Compile a Tier 1 value expression, computing it only on the path where it
 * is not NULL.
 *
 * Every Tier 1 operator is strict, so the expression is NULL exactly when one
 * of its leaves is; *isnull_out receives that flag (from
 * tier1_expr_any_null(), or NULL when nothing in the expression can be NULL).
 * The arithmetic must not run on the NULL path: the division and overflow
 * checks would raise on whatever garbage a NULL variable's datum happens to
 * hold, where PostgreSQL never invokes the operator at all — "a[1] := 1 / y"
 * with y NULL stores a NULL element, it does not raise division_by_zero.
 *
 * The scalar assignment path already branches this way (see
 * uplpgsql_try_compile_assign); this is the same branch for value positions
 * that consume the result as an SSA value rather than a store, so the NULL
 * edge contributes a placeholder zero through a phi.  Every caller passes
 * *isnull_out alongside the value, and nothing looks at the value when the
 * flag is set.
 */
static LLVMValueRef
tier1_compile_value_guarded(UPLpgSQL_compile_ctx *ctx, Expr *val_expr,
							LLVMValueRef estate_ref,
							ExprTypeClass *val_class,
							LLVMValueRef *isnull_out)
{
	LLVMTypeRef			vty;
	LLVMValueRef		computed, phi;
	LLVMValueRef		vals[2];
	LLVMBasicBlockRef	blocks[2];
	LLVMBasicBlockRef	compute_bb, join_bb, from_bb;

	*isnull_out = tier1_expr_any_null(ctx, val_expr, estate_ref);
	if (*isnull_out == NULL)
	{
		/* Nothing nullable in it — compute unconditionally. */
		return uplpgsql_compile_expr_datum(ctx, val_expr, estate_ref,
										   val_class);
	}

	if (*val_class == EXPR_TYPE_FLOAT8)
		vty = ctx->types[UPLPGSQL_DOUBLE];
	else if (*val_class == EXPR_TYPE_INT8)
		vty = ctx->types[UPLPGSQL_INT64];
	else if (*val_class == EXPR_TYPE_BOOL)
		vty = ctx->types[UPLPGSQL_INT1];
	else
		vty = ctx->types[UPLPGSQL_INT32];

	compute_bb = expr_append_block(ctx, "t1.val.compute");
	join_bb = expr_append_block(ctx, "t1.val.done");

	from_bb = LLVMGetInsertBlock(ctx->builder);
	LLVMBuildCondBr(ctx->builder, *isnull_out, join_bb, compute_bb);

	LLVMPositionBuilderAtEnd(ctx->builder, compute_bb);
	computed = uplpgsql_compile_expr_datum(ctx, val_expr, estate_ref,
										   val_class);
	vals[1] = computed;
	blocks[1] = LLVMGetInsertBlock(ctx->builder);
	LLVMBuildBr(ctx->builder, join_bb);

	LLVMPositionBuilderAtEnd(ctx->builder, join_bb);
	phi = LLVMBuildPhi(ctx->builder, vty, "t1.val");
	vals[0] = LLVMConstNull(vty);
	blocks[0] = from_bb;
	LLVMAddIncoming(phi, vals, blocks, 2);

	return phi;
}

/*
 * Emit IR to store a Datum (i64) into a variable.
 * Sets value, isnull=false, freeval=false.
 */
static void
uplpgsql_emit_store_var_datum(UPLpgSQL_compile_ctx *ctx,
							 LLVMValueRef estate_ref, int dno,
							 LLVMValueRef datum_val)
{
	LLVMBuilderRef	builder = ctx->builder;
	LLVMTypeRef		i8 = ctx->types[UPLPGSQL_INT8];
	LLVMTypeRef		i64 = ctx->types[UPLPGSQL_INT64];
	LLVMTypeRef		ptr = ctx->types[UPLPGSQL_PTR];
	LLVMValueRef	off, gep, plstate, datums, datum;

	off = LLVMConstInt(i64, OFF_ESTATE_PLSTATE, false);
	gep = LLVMBuildGEP2(builder, i8, estate_ref, &off, 1, "st.plstate.ptr");
	plstate = LLVMBuildLoad2(builder, ptr, gep, "st.plstate");

	off = LLVMConstInt(i64, OFF_EXECSTATE_DATUMS, false);
	gep = LLVMBuildGEP2(builder, i8, plstate, &off, 1, "st.datums.ptr");
	datums = LLVMBuildLoad2(builder, ptr, gep, "st.datums");

	off = LLVMConstInt(i64, dno, false);
	gep = LLVMBuildGEP2(builder, ptr, datums, &off, 1, "st.datum.slot");
	datum = LLVMBuildLoad2(builder, ptr, gep, "st.datum");

	/* datum->value = datum_val */
	off = LLVMConstInt(i64, OFF_VAR_VALUE, false);
	gep = LLVMBuildGEP2(builder, i8, datum, &off, 1, "st.value.ptr");
	LLVMBuildStore(builder, datum_val, gep);

	/* datum->isnull = false */
	off = LLVMConstInt(i64, OFF_VAR_ISNULL, false);
	gep = LLVMBuildGEP2(builder, i8, datum, &off, 1, "st.isnull.ptr");
	LLVMBuildStore(builder, LLVMConstInt(i8, 0, false), gep);

	/* datum->freeval = false */
	off = LLVMConstInt(i64, OFF_VAR_FREEVAL, false);
	gep = LLVMBuildGEP2(builder, i8, datum, &off, 1, "st.freeval.ptr");
	LLVMBuildStore(builder, LLVMConstInt(i8, 0, false), gep);
}

/*
 * Emit IR to store a Datum plus a computed isnull (i1) into a variable.
 *
 * Like uplpgsql_emit_store_var_datum(), but takes nullness from a value
 * rather than assuming not-null.  Pass-by-value targets only.
 */
static void
uplpgsql_emit_store_var_datum_isnull(UPLpgSQL_compile_ctx *ctx,
									 LLVMValueRef estate_ref, int dno,
									 LLVMValueRef datum_val,
									 LLVMValueRef isnull_val)
{
	LLVMBuilderRef	builder = ctx->builder;
	LLVMTypeRef		i8 = ctx->types[UPLPGSQL_INT8];
	LLVMTypeRef		i64 = ctx->types[UPLPGSQL_INT64];
	LLVMTypeRef		ptr = ctx->types[UPLPGSQL_PTR];
	LLVMValueRef	off, gep, plstate, datums, datum;

	off = LLVMConstInt(i64, OFF_ESTATE_PLSTATE, false);
	gep = LLVMBuildGEP2(builder, i8, estate_ref, &off, 1, "si.plstate.ptr");
	plstate = LLVMBuildLoad2(builder, ptr, gep, "si.plstate");

	off = LLVMConstInt(i64, OFF_EXECSTATE_DATUMS, false);
	gep = LLVMBuildGEP2(builder, i8, plstate, &off, 1, "si.datums.ptr");
	datums = LLVMBuildLoad2(builder, ptr, gep, "si.datums");

	off = LLVMConstInt(i64, dno, false);
	gep = LLVMBuildGEP2(builder, ptr, datums, &off, 1, "si.datum.slot");
	datum = LLVMBuildLoad2(builder, ptr, gep, "si.datum");

	off = LLVMConstInt(i64, OFF_VAR_VALUE, false);
	gep = LLVMBuildGEP2(builder, i8, datum, &off, 1, "si.value.ptr");
	LLVMBuildStore(builder, datum_val, gep);

	off = LLVMConstInt(i64, OFF_VAR_ISNULL, false);
	gep = LLVMBuildGEP2(builder, i8, datum, &off, 1, "si.isnull.ptr");
	LLVMBuildStore(builder,
				   LLVMBuildZExt(builder, isnull_val, i8, "si.isnull.i8"),
				   gep);

	off = LLVMConstInt(i64, OFF_VAR_FREEVAL, false);
	gep = LLVMBuildGEP2(builder, i8, datum, &off, 1, "si.freeval.ptr");
	LLVMBuildStore(builder, LLVMConstInt(i8, 0, false), gep);
}

/*
 * Emit IR to set a variable to NULL: value = 0, isnull = true.
 *
 * The pass-by-value counterpart of uplpgsql_emit_store_var_datum(), which
 * always clears isnull.  Only valid for pass-by-value targets — a
 * pass-by-reference variable would need assign_simple_var() to release its
 * old value, which is why Tier 1 only ever assigns scalars.
 */
static void
uplpgsql_emit_store_var_null(UPLpgSQL_compile_ctx *ctx,
							 LLVMValueRef estate_ref, int dno)
{
	LLVMBuilderRef	builder = ctx->builder;
	LLVMTypeRef		i8 = ctx->types[UPLPGSQL_INT8];
	LLVMTypeRef		i64 = ctx->types[UPLPGSQL_INT64];
	LLVMTypeRef		ptr = ctx->types[UPLPGSQL_PTR];
	LLVMValueRef	off, gep, plstate, datums, datum;

	off = LLVMConstInt(i64, OFF_ESTATE_PLSTATE, false);
	gep = LLVMBuildGEP2(builder, i8, estate_ref, &off, 1, "sn.plstate.ptr");
	plstate = LLVMBuildLoad2(builder, ptr, gep, "sn.plstate");

	off = LLVMConstInt(i64, OFF_EXECSTATE_DATUMS, false);
	gep = LLVMBuildGEP2(builder, i8, plstate, &off, 1, "sn.datums.ptr");
	datums = LLVMBuildLoad2(builder, ptr, gep, "sn.datums");

	off = LLVMConstInt(i64, dno, false);
	gep = LLVMBuildGEP2(builder, ptr, datums, &off, 1, "sn.datum.slot");
	datum = LLVMBuildLoad2(builder, ptr, gep, "sn.datum");

	/* datum->value = 0 */
	off = LLVMConstInt(i64, OFF_VAR_VALUE, false);
	gep = LLVMBuildGEP2(builder, i8, datum, &off, 1, "sn.value.ptr");
	LLVMBuildStore(builder, LLVMConstInt(i64, 0, false), gep);

	/* datum->isnull = true */
	off = LLVMConstInt(i64, OFF_VAR_ISNULL, false);
	gep = LLVMBuildGEP2(builder, i8, datum, &off, 1, "sn.isnull.ptr");
	LLVMBuildStore(builder, LLVMConstInt(i8, 1, false), gep);

	/* datum->freeval = false */
	off = LLVMConstInt(i64, OFF_VAR_FREEVAL, false);
	gep = LLVMBuildGEP2(builder, i8, datum, &off, 1, "sn.freeval.ptr");
	LLVMBuildStore(builder, LLVMConstInt(i8, 0, false), gep);
}


/* ================================================================
 * Expression preparation and analysis
 * ================================================================
 */

/*
 * Prepare a PL/pgSQL expression via SPI and extract the Expr tree.
 * Returns NULL if the expression is not simple.
 *
 * The PL/pgSQL parser callbacks (uplpgsql_parser_setup) require
 * expr->func->cur_estate to be set for variable type resolution.
 * At JIT compile time, no execution state exists yet, so we create a
 * minimal temporary execstate backed by the function's datums array.
 */
static Expr *
uplpgsql_prepare_and_get_expr(UPLpgSQL_compile_ctx *ctx, UPLpgSQL_expr *expr)
{
	SPIPlanPtr		plan;
	SPIPrepareOptions options;
	List		   *plansources;
	CachedPlanSource *plansource;
	Query		   *query;
	TargetEntry	   *tle;
	UPLpgSQL_function *func = ctx_func(ctx);

	if (expr->plan == NULL)
	{
		UPLpgSQL_execstate fake_estate;
		UPLpgSQL_execstate *saved_estate;
		MemoryContext oldcxt;

		/*
		 * The parser callbacks in upl_comp.c access expr->func->cur_estate
		 * to resolve variable names and types.  At compile time cur_estate
		 * is NULL, so we provide a minimal fake estate with the function's
		 * datums array — that's all resolve_column_ref/make_datum_param need
		 * for scalar variables.
		 *
		 * For record fields or other complex datums, SPI_prepare_extended
		 * may throw an error (e.g. "record not assigned yet").  We catch
		 * that and return NULL to fall back to the runtime helper.
		 */
		memset(&fake_estate, 0, sizeof(fake_estate));
		fake_estate.ndatums = func->ndatums;
		fake_estate.datums = func->datums;

		saved_estate = func->cur_estate;
		func->cur_estate = &fake_estate;

		memset(&options, 0, sizeof(options));
		options.parserSetup = (ParserSetupHook) uplpgsql_parser_setup;
		options.parserSetupArg = expr;
		options.parseMode = expr->parseMode;
		options.cursorOptions = CURSOR_OPT_PARALLEL_OK;

		oldcxt = CurrentMemoryContext;

		PG_TRY();
		{
			plan = SPI_prepare_extended(expr->query, &options);
		}
		PG_CATCH();
		{
			/* Swallow the error and fall back to runtime helper */
			MemoryContextSwitchTo(oldcxt);
			FlushErrorState();
			func->cur_estate = saved_estate;
			return NULL;
		}
		PG_END_TRY();

		func->cur_estate = saved_estate;

		if (plan == NULL)
			return NULL;

		SPI_keepplan(plan);
		expr->plan = plan;
	}

	plansources = SPI_plan_get_plan_sources(expr->plan);
	if (list_length(plansources) != 1)
		return NULL;
	plansource = (CachedPlanSource *) linitial(plansources);

	if (list_length(plansource->query_list) != 1)
		return NULL;
	query = (Query *) linitial(plansource->query_list);

	if (!IsA(query, Query) ||
		query->commandType != CMD_SELECT ||
		query->rtable != NIL ||
		query->hasAggs ||
		query->hasWindowFuncs ||
		query->hasTargetSRFs ||
		query->hasSubLinks ||
		query->cteList ||
		query->jointree->fromlist ||
		query->jointree->quals ||
		query->groupClause ||
		query->havingQual)
		return NULL;

	if (list_length(query->targetList) != 1)
		return NULL;

	tle = (TargetEntry *) linitial(query->targetList);
	return tle->expr;
}

/*
 * Classify an expression's result type for native compilation.
 * Returns EXPR_TYPE_UNKNOWN if the expression cannot be natively compiled.
 */
static ExprTypeClass
uplpgsql_classify_expr(Expr *expr)
{
	if (expr == NULL)
		return EXPR_TYPE_UNKNOWN;

	switch (nodeTag(expr))
	{
		case T_Const:
			{
				Const *c = (Const *) expr;

				if (c->constisnull)
					return EXPR_TYPE_UNKNOWN;
				if (c->consttype == INT4OID)
					return EXPR_TYPE_INT4;
				if (c->consttype == INT8OID)
					return EXPR_TYPE_INT8;
				if (c->consttype == FLOAT8OID)
					return EXPR_TYPE_FLOAT8;
				if (c->consttype == BOOLOID)
					return EXPR_TYPE_BOOL;
				return EXPR_TYPE_UNKNOWN;
			}

		case T_Param:
			{
				Param *p = (Param *) expr;

				if (p->paramkind != PARAM_EXTERN)
					return EXPR_TYPE_UNKNOWN;
				if (p->paramtype == INT4OID)
					return EXPR_TYPE_INT4;
				if (p->paramtype == INT8OID)
					return EXPR_TYPE_INT8;
				if (p->paramtype == FLOAT8OID)
					return EXPR_TYPE_FLOAT8;
				if (p->paramtype == BOOLOID)
					return EXPR_TYPE_BOOL;
				return EXPR_TYPE_UNKNOWN;
			}

		case T_OpExpr:
			{
				OpExpr *op = (OpExpr *) expr;
				Oid		fid = op->opfuncid;

				/* int4 arithmetic */
				if (fid == F_INT4PL || fid == F_INT4MI ||
					fid == F_INT4MUL || fid == F_INT4DIV ||
					fid == F_INT4MOD || fid == F_INT4UM)
				{
					ListCell *lc;
					foreach(lc, op->args)
					{
						if (uplpgsql_classify_expr((Expr *) lfirst(lc)) != EXPR_TYPE_INT4)
							return EXPR_TYPE_UNKNOWN;
					}
					return EXPR_TYPE_INT4;
				}

				/* int8 arithmetic */
				if (fid == F_INT8PL || fid == F_INT8MI ||
					fid == F_INT8MUL || fid == F_INT8DIV ||
					fid == F_INT8MOD || fid == F_INT8UM)
				{
					ListCell *lc;
					foreach(lc, op->args)
					{
						if (uplpgsql_classify_expr((Expr *) lfirst(lc)) != EXPR_TYPE_INT8)
							return EXPR_TYPE_UNKNOWN;
					}
					return EXPR_TYPE_INT8;
				}

				/*
				 * float8 arithmetic.
				 *
				 * Compiled natively, but with PostgreSQL's semantics rather
				 * than the raw IEEE ones — overflow/underflow and division by
				 * zero raise instead of saturating.  See emit_float_addsub/
				 * _mul/_div, which mirror float8_pl/_mi/_mul/_div from
				 * utils/float.h.
				 */
				if (fid == F_FLOAT8PL || fid == F_FLOAT8MI ||
					fid == F_FLOAT8MUL || fid == F_FLOAT8DIV ||
					fid == F_FLOAT8UM)
				{
					ListCell *lc;
					foreach(lc, op->args)
					{
						if (uplpgsql_classify_expr((Expr *) lfirst(lc)) != EXPR_TYPE_FLOAT8)
							return EXPR_TYPE_UNKNOWN;
					}
					return EXPR_TYPE_FLOAT8;
				}

				/* int4 comparisons → bool */
				if (fid == F_INT4EQ || fid == F_INT4NE ||
					fid == F_INT4LT || fid == F_INT4LE ||
					fid == F_INT4GT || fid == F_INT4GE)
				{
					if (list_length(op->args) != 2)
						return EXPR_TYPE_UNKNOWN;
					if (uplpgsql_classify_expr(linitial(op->args)) != EXPR_TYPE_INT4 ||
						uplpgsql_classify_expr(lsecond(op->args)) != EXPR_TYPE_INT4)
						return EXPR_TYPE_UNKNOWN;
					return EXPR_TYPE_BOOL;
				}

				/* int8 comparisons → bool */
				if (fid == F_INT8EQ || fid == F_INT8NE ||
					fid == F_INT8LT || fid == F_INT8GT ||
					fid == F_INT8LE || fid == F_INT8GE)
				{
					if (list_length(op->args) != 2)
						return EXPR_TYPE_UNKNOWN;
					if (uplpgsql_classify_expr(linitial(op->args)) != EXPR_TYPE_INT8 ||
						uplpgsql_classify_expr(lsecond(op->args)) != EXPR_TYPE_INT8)
						return EXPR_TYPE_UNKNOWN;
					return EXPR_TYPE_BOOL;
				}

				/*
				 * float8 comparisons → bool.
				 *
				 * Compiled natively via emit_float_cmp(), which reproduces
				 * float8_eq/_ne/_lt/_le/_gt/_ge from utils/float.h: NaN sorts
				 * above every other value and NaN = NaN is true, neither of
				 * which LLVM's ordered predicates give on their own.
				 */
				if (fid == F_FLOAT8EQ || fid == F_FLOAT8NE ||
					fid == F_FLOAT8LT || fid == F_FLOAT8LE ||
					fid == F_FLOAT8GT || fid == F_FLOAT8GE)
				{
					if (list_length(op->args) != 2)
						return EXPR_TYPE_UNKNOWN;
					if (uplpgsql_classify_expr(linitial(op->args)) != EXPR_TYPE_FLOAT8 ||
						uplpgsql_classify_expr(lsecond(op->args)) != EXPR_TYPE_FLOAT8)
						return EXPR_TYPE_UNKNOWN;
					return EXPR_TYPE_BOOL;
				}

				/* bool equality/inequality → bool */
				if (fid == F_BOOLEQ || fid == F_BOOLNE)
				{
					if (list_length(op->args) != 2)
						return EXPR_TYPE_UNKNOWN;
					if (uplpgsql_classify_expr(linitial(op->args)) != EXPR_TYPE_BOOL ||
						uplpgsql_classify_expr(lsecond(op->args)) != EXPR_TYPE_BOOL)
						return EXPR_TYPE_UNKNOWN;
					return EXPR_TYPE_BOOL;
				}

				/* int4↔int8 cross-type comparisons → bool */
				if (fid == F_INT84EQ || fid == F_INT84NE ||
					fid == F_INT84LT || fid == F_INT84GT ||
					fid == F_INT84LE || fid == F_INT84GE)
				{
					if (list_length(op->args) != 2)
						return EXPR_TYPE_UNKNOWN;
					if (uplpgsql_classify_expr(linitial(op->args)) != EXPR_TYPE_INT8 ||
						uplpgsql_classify_expr(lsecond(op->args)) != EXPR_TYPE_INT4)
						return EXPR_TYPE_UNKNOWN;
					return EXPR_TYPE_BOOL;
				}
				if (fid == F_INT48EQ || fid == F_INT48NE ||
					fid == F_INT48LT || fid == F_INT48GT ||
					fid == F_INT48LE || fid == F_INT48GE)
				{
					if (list_length(op->args) != 2)
						return EXPR_TYPE_UNKNOWN;
					if (uplpgsql_classify_expr(linitial(op->args)) != EXPR_TYPE_INT4 ||
						uplpgsql_classify_expr(lsecond(op->args)) != EXPR_TYPE_INT8)
						return EXPR_TYPE_UNKNOWN;
					return EXPR_TYPE_BOOL;
				}

				/* int4↔int8 cross-type arithmetic → int8 */
				if (fid == F_INT84PL || fid == F_INT84MI ||
					fid == F_INT84MUL || fid == F_INT84DIV)
				{
					if (list_length(op->args) != 2)
						return EXPR_TYPE_UNKNOWN;
					if (uplpgsql_classify_expr(linitial(op->args)) != EXPR_TYPE_INT8 ||
						uplpgsql_classify_expr(lsecond(op->args)) != EXPR_TYPE_INT4)
						return EXPR_TYPE_UNKNOWN;
					return EXPR_TYPE_INT8;
				}
				if (fid == F_INT48PL || fid == F_INT48MI ||
					fid == F_INT48MUL || fid == F_INT48DIV)
				{
					if (list_length(op->args) != 2)
						return EXPR_TYPE_UNKNOWN;
					if (uplpgsql_classify_expr(linitial(op->args)) != EXPR_TYPE_INT4 ||
						uplpgsql_classify_expr(lsecond(op->args)) != EXPR_TYPE_INT8)
						return EXPR_TYPE_UNKNOWN;
					return EXPR_TYPE_INT8;
				}

				return EXPR_TYPE_UNKNOWN;
			}

		case T_FuncExpr:
			{
				FuncExpr *f = (FuncExpr *) expr;
				int nargs = list_length(f->args);

				/* Two-arg float8 math: pow(x,y) / power(x,y) */
				if (nargs == 2 &&
					(f->funcid == F_DPOW ||
					 f->funcid == F_POW_FLOAT8_FLOAT8 ||
					 f->funcid == F_POWER_FLOAT8_FLOAT8) &&
					uplpgsql_classify_expr(linitial(f->args)) == EXPR_TYPE_FLOAT8 &&
					uplpgsql_classify_expr(lsecond(f->args)) == EXPR_TYPE_FLOAT8)
					return EXPR_TYPE_FLOAT8;

				if (nargs != 1)
					return EXPR_TYPE_UNKNOWN;

				/* ABS functions */
				if (f->funcid == F_INT4ABS &&
					uplpgsql_classify_expr(linitial(f->args)) == EXPR_TYPE_INT4)
					return EXPR_TYPE_INT4;

				if (f->funcid == F_INT8ABS &&
					uplpgsql_classify_expr(linitial(f->args)) == EXPR_TYPE_INT8)
					return EXPR_TYPE_INT8;

				if (f->funcid == F_FLOAT8ABS &&
					uplpgsql_classify_expr(linitial(f->args)) == EXPR_TYPE_FLOAT8)
					return EXPR_TYPE_FLOAT8;

				/* Single-arg float8 math intrinsics */
				if ((f->funcid == F_DSQRT ||
					 f->funcid == F_SQRT_FLOAT8 ||
					 f->funcid == F_CEIL_FLOAT8 ||
					 f->funcid == F_CEILING_FLOAT8 ||
					 f->funcid == F_FLOOR_FLOAT8 ||
					 f->funcid == F_DEXP ||
					 f->funcid == F_EXP_FLOAT8 ||
					 f->funcid == F_DLOG1 ||
					 f->funcid == F_LN_FLOAT8 ||
					 f->funcid == F_SIN ||
					 f->funcid == F_COS) &&
					uplpgsql_classify_expr(linitial(f->args)) == EXPR_TYPE_FLOAT8)
					return EXPR_TYPE_FLOAT8;

				/*
				 * Cast functions that produce float8.  These let us inline
				 * expressions like "y::double precision / p_height" and
				 * "2.0 * zx" where the parser wraps constants or int
				 * variables in an explicit cast FuncExpr.
				 */
				if (f->funcid == F_FLOAT8_INT4 || f->funcid == F_FLOAT8_INT2)
				{
					ExprTypeClass argclass =
						uplpgsql_classify_expr(linitial(f->args));
					if (argclass == EXPR_TYPE_INT4)
						return EXPR_TYPE_FLOAT8;
				}

				if (f->funcid == F_FLOAT8_INT8)
				{
					ExprTypeClass argclass =
						uplpgsql_classify_expr(linitial(f->args));
					if (argclass == EXPR_TYPE_INT8)
						return EXPR_TYPE_FLOAT8;
				}

				if (f->funcid == F_FLOAT8_FLOAT4)
					return EXPR_TYPE_FLOAT8;

				/*
				 * numeric → float8: only for Const args (PL/pgSQL parses
				 * literal "2.0" as numeric, then wraps in float8(numeric)).
				 * We evaluate the cast at compile time.
				 */
				if (f->funcid == F_FLOAT8_NUMERIC &&
					IsA(linitial(f->args), Const))
					return EXPR_TYPE_FLOAT8;

				return EXPR_TYPE_UNKNOWN;
			}

		case T_RelabelType:
			return uplpgsql_classify_expr(((RelabelType *) expr)->arg);

		case T_BoolExpr:
			/*
			 * AND/OR/NOT are deliberately NOT Tier 1.
			 *
			 * They are the only non-strict operators here: NULL AND false is
			 * false, NULL OR true is true, and NOT NULL is NULL.  Tier 1's
			 * NULL handling (see tier1_expr_any_null) relies on every
			 * operator being strict, so three-valued logic cannot ride along
			 * with it.  Tier 2 implements 3VL explicitly.
			 */
			return EXPR_TYPE_UNKNOWN;

		case T_SubscriptingRef:
			{
				SubscriptingRef *sbsref = (SubscriptingRef *) expr;

				/*
				 * Classify array element reads: arr[i] where the subscript
				 * is int4 and the element type is a known Tier 1 type.
				 * Only single-dimension, non-slice, Param-based arrays.
				 */
				if (sbsref->refassgnexpr == NULL &&
					list_length(sbsref->refupperindexpr) == 1 &&
					sbsref->reflowerindexpr == NIL &&
					IsA(sbsref->refexpr, Param) &&
					((Param *) sbsref->refexpr)->paramkind == PARAM_EXTERN &&
					uplpgsql_classify_expr(linitial(sbsref->refupperindexpr)) == EXPR_TYPE_INT4)
				{
					Oid elemtype = sbsref->refrestype;

					if (elemtype == INT4OID)
						return EXPR_TYPE_INT4;
					if (elemtype == INT8OID)
						return EXPR_TYPE_INT8;
					if (elemtype == FLOAT8OID)
						return EXPR_TYPE_FLOAT8;
					if (elemtype == BOOLOID)
						return EXPR_TYPE_BOOL;
				}
				return EXPR_TYPE_UNKNOWN;
			}

		default:
			return EXPR_TYPE_UNKNOWN;
	}
}


/* ================================================================
 * Runtime error helpers (called from JIT'd code)
 * ================================================================
 */

UPLPGSQL_RT_EXPORT void
uplpgsql_rt_int_overflow(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("integer out of range")));
}

UPLPGSQL_RT_EXPORT void
uplpgsql_rt_div_zero(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_DIVISION_BY_ZERO),
			 errmsg("division by zero")));
}

/* Mirrors array_subscript_assign(); see emit_set_subscript_null_check(). */
UPLPGSQL_RT_EXPORT void
uplpgsql_rt_array_subscript_null(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
			 errmsg("array subscript in assignment must not be null")));
}

/*
 * Emit a call to a no-arg error runtime function, followed by unreachable.
 *
 * Uses LLVMGetNamedFunction to reuse an existing declaration rather than
 * creating duplicates (LLVM would mangle the name, breaking symbol lookup).
 */
static void
emit_error_call(UPLpgSQL_compile_ctx *ctx, const char *fn_name)
{
	LLVMTypeRef err_ft = LLVMFunctionType(ctx->types[UPLPGSQL_VOID],
										  NULL, 0, false);
	LLVMValueRef err_fn = LLVMGetNamedFunction(ctx->module, fn_name);

	if (err_fn == NULL)
		err_fn = LLVMAddFunction(ctx->module, fn_name, err_ft);

	LLVMBuildCall2(ctx->builder, err_ft, err_fn, NULL, 0, "");
	LLVMBuildUnreachable(ctx->builder);
}

/*
 * Emit the NULL test for the subscript of an array element write, raising
 * PostgreSQL's error on the NULL edge.
 *
 * A NULL subscript reads as NULL but *assigns* as an error — PostgreSQL's
 * array_subscript_assign() raises "array subscript in assignment must not be
 * null" — and the compiled write consumed it as a garbage index instead:
 * whatever the NULL variable's datum happened to hold went straight into the
 * range checks, so "a[i] := 99" with i NULL silently wrote a[0].
 *
 * The test must run BEFORE the subscript's value is computed.  When the
 * subscript is itself a native array read — a[b[j]] := ... — that read's
 * value path is a raw flat load with no bounds check of its own, on the
 * invariant that tier1_expr_any_null() has been consulted and was false.
 * This is that consultation for the subscript position: it folds "b[j] is
 * out of range" (which PostgreSQL reads as NULL) into the same NULL edge,
 * so the raw load runs only for an in-range, non-NULL subscript, and an
 * out-of-range one raises here rather than fetching a garbage index from
 * past the end of the flat buffer.
 */
static void
emit_set_subscript_null_check(UPLpgSQL_compile_ctx *ctx, Expr *idx_expr,
							  LLVMValueRef estate_ref)
{
	LLVMValueRef		idx_isnull;
	LLVMBasicBlockRef	null_bb, ok_bb;

	idx_isnull = tier1_expr_any_null(ctx, idx_expr, estate_ref);
	if (idx_isnull == NULL)
		return;					/* nothing in the subscript can be NULL */

	null_bb = expr_append_block(ctx, "na.set.idxnull");
	ok_bb = expr_append_block(ctx, "na.set.idxok");

	LLVMBuildCondBr(ctx->builder, idx_isnull, null_bb, ok_bb);

	LLVMPositionBuilderAtEnd(ctx->builder, null_bb);
	emit_error_call(ctx, "uplpgsql_rt_array_subscript_null");

	LLVMPositionBuilderAtEnd(ctx->builder, ok_bb);
}


/* ================================================================
 * float8 arithmetic and comparison with PostgreSQL semantics
 *
 * PostgreSQL's float8 operators are not the raw IEEE instructions.  They
 * raise on overflow and underflow rather than saturating to Infinity or 0,
 * they raise division_by_zero rather than returning Infinity, and they order
 * NaN above every other value with NaN = NaN true.  See float8_pl/_mi/_mul/
 * _div and float8_eq/_ne/_lt/_le/_gt/_ge in utils/float.h — the code below
 * mirrors those definitions exactly.
 *
 * The error paths call PostgreSQL's own float_overflow_error(),
 * float_underflow_error() and float_zero_divide_error(), which the JIT
 * resolves as process symbols, so the errors raised are identical to the
 * interpreter's rather than merely similar.
 * ================================================================
 */

/* isnan(v): true when v is unordered with itself */
static LLVMValueRef
emit_float_isnan(UPLpgSQL_compile_ctx *ctx, LLVMValueRef v, const char *name)
{
	return LLVMBuildFCmp(ctx->builder, LLVMRealUNO, v, v, name);
}

/* isinf(v): v == +Inf || v == -Inf */
static LLVMValueRef
emit_float_isinf(UPLpgSQL_compile_ctx *ctx, LLVMValueRef v, const char *name)
{
	LLVMBuilderRef	builder = ctx->builder;
	LLVMTypeRef		dbl = ctx->types[UPLPGSQL_DOUBLE];
	LLVMValueRef	pos, neg;

	pos = LLVMBuildFCmp(builder, LLVMRealOEQ, v,
						LLVMConstReal(dbl, INFINITY), "isinf.p");
	neg = LLVMBuildFCmp(builder, LLVMRealOEQ, v,
						LLVMConstReal(dbl, -INFINITY), "isinf.n");
	return LLVMBuildOr(builder, pos, neg, name);
}

/* v == 0.0 */
static LLVMValueRef
emit_float_iszero(UPLpgSQL_compile_ctx *ctx, LLVMValueRef v, const char *name)
{
	return LLVMBuildFCmp(ctx->builder, LLVMRealOEQ, v,
						 LLVMConstReal(ctx->types[UPLPGSQL_DOUBLE], 0.0),
						 name);
}

/*
 * Branch to a float error function when cond holds, else continue.
 */
static void
emit_float_error_if(UPLpgSQL_compile_ctx *ctx, LLVMValueRef cond,
					const char *fn_name, const char *tag)
{
	LLVMBasicBlockRef	err_bb, ok_bb;

	err_bb = expr_append_block(ctx, tag);
	ok_bb = expr_append_block(ctx, "f8.ok");

	LLVMBuildCondBr(ctx->builder, cond, err_bb, ok_bb);

	LLVMPositionBuilderAtEnd(ctx->builder, err_bb);
	emit_error_call(ctx, fn_name);

	LLVMPositionBuilderAtEnd(ctx->builder, ok_bb);
}

/*
 * float8_pl / float8_mi:
 *   result = val1 +/- val2;
 *   if (isinf(result) && !isinf(val1) && !isinf(val2)) overflow;
 */
static LLVMValueRef
emit_float_addsub(UPLpgSQL_compile_ctx *ctx, LLVMValueRef lhs,
				  LLVMValueRef rhs, bool is_add)
{
	LLVMBuilderRef	builder = ctx->builder;
	LLVMValueRef	result, cond;

	result = is_add ? LLVMBuildFAdd(builder, lhs, rhs, "f8.add")
					: LLVMBuildFSub(builder, lhs, rhs, "f8.sub");

	cond = LLVMBuildAnd(builder,
		emit_float_isinf(ctx, result, "f8.r.inf"),
		LLVMBuildAnd(builder,
			LLVMBuildNot(builder, emit_float_isinf(ctx, lhs, "f8.l.inf"),
						 "f8.l.fin"),
			LLVMBuildNot(builder, emit_float_isinf(ctx, rhs, "f8.r2.inf"),
						 "f8.r.fin"),
			"f8.both.fin"),
		"f8.ovf");

	emit_float_error_if(ctx, cond, "float_overflow_error", "f8.ovf.err");
	return result;
}

/*
 * float8_mul:
 *   result = val1 * val2;
 *   if (isinf(result) && !isinf(val1) && !isinf(val2)) overflow;
 *   if (result == 0.0 && val1 != 0.0 && val2 != 0.0) underflow;
 */
static LLVMValueRef
emit_float_mul(UPLpgSQL_compile_ctx *ctx, LLVMValueRef lhs, LLVMValueRef rhs)
{
	LLVMBuilderRef	builder = ctx->builder;
	LLVMValueRef	result, cond;

	result = LLVMBuildFMul(builder, lhs, rhs, "f8.mul");

	cond = LLVMBuildAnd(builder,
		emit_float_isinf(ctx, result, "f8.r.inf"),
		LLVMBuildAnd(builder,
			LLVMBuildNot(builder, emit_float_isinf(ctx, lhs, "f8.l.inf"),
						 "f8.l.fin"),
			LLVMBuildNot(builder, emit_float_isinf(ctx, rhs, "f8.r2.inf"),
						 "f8.r.fin"),
			"f8.both.fin"),
		"f8.ovf");
	emit_float_error_if(ctx, cond, "float_overflow_error", "f8.ovf.err");

	cond = LLVMBuildAnd(builder,
		emit_float_iszero(ctx, result, "f8.r.zero"),
		LLVMBuildAnd(builder,
			LLVMBuildNot(builder, emit_float_iszero(ctx, lhs, "f8.l.zero"),
						 "f8.l.nz"),
			LLVMBuildNot(builder, emit_float_iszero(ctx, rhs, "f8.r2.zero"),
						 "f8.r.nz"),
			"f8.both.nz"),
		"f8.unf");
	emit_float_error_if(ctx, cond, "float_underflow_error", "f8.unf.err");

	return result;
}

/*
 * float8_div:
 *   if (val2 == 0.0 && !isnan(val1)) division_by_zero;
 *   result = val1 / val2;
 *   if (isinf(result) && !isinf(val1)) overflow;
 *   if (result == 0.0 && val1 != 0.0 && !isinf(val2)) underflow;
 */
static LLVMValueRef
emit_float_div(UPLpgSQL_compile_ctx *ctx, LLVMValueRef lhs, LLVMValueRef rhs)
{
	LLVMBuilderRef	builder = ctx->builder;
	LLVMValueRef	result, cond;

	cond = LLVMBuildAnd(builder,
		emit_float_iszero(ctx, rhs, "f8.d.zero"),
		LLVMBuildNot(builder, emit_float_isnan(ctx, lhs, "f8.l.nan"),
					 "f8.l.notnan"),
		"f8.divzero");
	emit_float_error_if(ctx, cond, "float_zero_divide_error", "f8.div0.err");

	result = LLVMBuildFDiv(builder, lhs, rhs, "f8.div");

	cond = LLVMBuildAnd(builder,
		emit_float_isinf(ctx, result, "f8.r.inf"),
		LLVMBuildNot(builder, emit_float_isinf(ctx, lhs, "f8.l.inf"),
					 "f8.l.fin"),
		"f8.ovf");
	emit_float_error_if(ctx, cond, "float_overflow_error", "f8.ovf.err");

	cond = LLVMBuildAnd(builder,
		emit_float_iszero(ctx, result, "f8.r.zero"),
		LLVMBuildAnd(builder,
			LLVMBuildNot(builder, emit_float_iszero(ctx, lhs, "f8.l.zero"),
						 "f8.l.nz"),
			LLVMBuildNot(builder, emit_float_isinf(ctx, rhs, "f8.r2.inf"),
						 "f8.r.fin"),
			"f8.unf.rest"),
		"f8.unf");
	emit_float_error_if(ctx, cond, "float_underflow_error", "f8.unf.err");

	return result;
}

/*
 * float8_eq/_ne/_lt/_le/_gt/_ge — NaN sorts above every other value, and
 * NaN = NaN is true, so the ordered LLVM predicates alone are wrong.  Each
 * formula below is the literal transcription of the corresponding inline
 * function in utils/float.h.  All are branch-free.
 */
static LLVMValueRef
emit_float_cmp(UPLpgSQL_compile_ctx *ctx, Oid fid, LLVMValueRef lhs,
			   LLVMValueRef rhs)
{
	LLVMBuilderRef	builder = ctx->builder;
	LLVMValueRef	l_nan = emit_float_isnan(ctx, lhs, "f8.cmp.l.nan");
	LLVMValueRef	r_nan = emit_float_isnan(ctx, rhs, "f8.cmp.r.nan");
	LLVMValueRef	l_ok = LLVMBuildNot(builder, l_nan, "f8.cmp.l.ok");
	LLVMValueRef	r_ok = LLVMBuildNot(builder, r_nan, "f8.cmp.r.ok");

	if (fid == F_FLOAT8EQ)
		/* isnan(l) ? isnan(r) : !isnan(r) && l == r */
		return LLVMBuildSelect(builder, l_nan, r_nan,
			LLVMBuildAnd(builder, r_ok,
				LLVMBuildFCmp(builder, LLVMRealOEQ, lhs, rhs, "f8.oeq"),
				"f8.eq.rhs"),
			"f8.eq");

	if (fid == F_FLOAT8NE)
		/* isnan(l) ? !isnan(r) : isnan(r) || l != r */
		return LLVMBuildSelect(builder, l_nan, r_ok,
			LLVMBuildOr(builder, r_nan,
				LLVMBuildFCmp(builder, LLVMRealONE, lhs, rhs, "f8.one"),
				"f8.ne.rhs"),
			"f8.ne");

	if (fid == F_FLOAT8LT)
		/* !isnan(l) && (isnan(r) || l < r) */
		return LLVMBuildAnd(builder, l_ok,
			LLVMBuildOr(builder, r_nan,
				LLVMBuildFCmp(builder, LLVMRealOLT, lhs, rhs, "f8.olt"),
				"f8.lt.rhs"),
			"f8.lt");

	if (fid == F_FLOAT8LE)
		/* isnan(r) || (!isnan(l) && l <= r) */
		return LLVMBuildOr(builder, r_nan,
			LLVMBuildAnd(builder, l_ok,
				LLVMBuildFCmp(builder, LLVMRealOLE, lhs, rhs, "f8.ole"),
				"f8.le.rhs"),
			"f8.le");

	if (fid == F_FLOAT8GT)
		/* !isnan(r) && (isnan(l) || l > r) */
		return LLVMBuildAnd(builder, r_ok,
			LLVMBuildOr(builder, l_nan,
				LLVMBuildFCmp(builder, LLVMRealOGT, lhs, rhs, "f8.ogt"),
				"f8.gt.rhs"),
			"f8.gt");

	if (fid == F_FLOAT8GE)
		/* isnan(l) || (!isnan(r) && l >= r) */
		return LLVMBuildOr(builder, l_nan,
			LLVMBuildAnd(builder, r_ok,
				LLVMBuildFCmp(builder, LLVMRealOGE, lhs, rhs, "f8.oge"),
				"f8.ge.rhs"),
			"f8.ge");

	elog(ERROR, "uplpgsql: unhandled float8 comparison funcid %u", fid);
	return NULL;
}


/* ================================================================
 * Integer overflow-checked arithmetic (shared by int4 and int8)
 *
 * Uses LLVM overflow intrinsics: llvm.sadd.with.overflow.iN, etc.
 * ================================================================
 */

/*
 * Emit overflow-checked add/sub/mul for iN (N=32 or N=64).
 */
static LLVMValueRef
emit_int_arith_checked(UPLpgSQL_compile_ctx *ctx,
					   LLVMValueRef lhs, LLVMValueRef rhs,
					   const char *intrinsic_name,
					   LLVMTypeRef int_type)
{
	LLVMBuilderRef	builder = ctx->builder;
	LLVMTypeRef		ovf_types[] = { int_type };
	unsigned		ovf_id;
	LLVMValueRef	ovf_fn, ovf_args[2], ovf_result;
	LLVMValueRef	value, overflow;
	LLVMBasicBlockRef ovf_bb, ok_bb;

	ovf_id = LLVMLookupIntrinsicID(intrinsic_name, strlen(intrinsic_name));
	ovf_fn = LLVMGetIntrinsicDeclaration(ctx->module, ovf_id, ovf_types, 1);

	ovf_args[0] = lhs;
	ovf_args[1] = rhs;

	{
		LLVMTypeRef ovf_fn_type = LLVMIntrinsicGetType(ctx->context, ovf_id,
													   ovf_types, 1);
		ovf_result = LLVMBuildCall2(builder, ovf_fn_type, ovf_fn,
									ovf_args, 2, "ovf.result");
	}

	value = LLVMBuildExtractValue(builder, ovf_result, 0, "ovf.value");
	overflow = LLVMBuildExtractValue(builder, ovf_result, 1, "ovf.flag");

	ovf_bb = expr_append_block(ctx, "ovf.error");
	ok_bb = expr_append_block(ctx, "ovf.ok");

	LLVMBuildCondBr(builder, overflow, ovf_bb, ok_bb);

	LLVMPositionBuilderAtEnd(builder, ovf_bb);
	emit_error_call(ctx, "uplpgsql_rt_int_overflow");

	LLVMPositionBuilderAtEnd(builder, ok_bb);
	return value;
}

/*
 * Emit checked division or modulo for iN.
 * Checks: divisor != 0, and for division, NOT (dividend = MIN && divisor = -1).
 */
static LLVMValueRef
emit_int_divmod(UPLpgSQL_compile_ctx *ctx,
				LLVMValueRef lhs, LLVMValueRef rhs,
				LLVMTypeRef int_type, bool is_div,
				uint64 min_val)
{
	LLVMBuilderRef	builder = ctx->builder;
	LLVMValueRef	cmp, result;
	LLVMBasicBlockRef zero_bb, check_bb, ok_bb;

	zero_bb = expr_append_block(ctx, "div.zero");
	check_bb = expr_append_block(ctx, "div.check");
	ok_bb = expr_append_block(ctx, "div.ok");

	/* Check divisor != 0 */
	cmp = LLVMBuildICmp(builder, LLVMIntEQ, rhs,
						LLVMConstInt(int_type, 0, false), "div.iszero");
	LLVMBuildCondBr(builder, cmp, zero_bb, check_bb);

	LLVMPositionBuilderAtEnd(builder, zero_bb);
	emit_error_call(ctx, "uplpgsql_rt_div_zero");

	LLVMPositionBuilderAtEnd(builder, check_bb);

	if (is_div)
	{
		/* Check for MIN / -1 overflow */
		LLVMValueRef is_min, is_neg1, is_ovf;
		LLVMBasicBlockRef ovf_bb, div_bb;

		ovf_bb = expr_append_block(ctx, "div.ovf");
		div_bb = expr_append_block(ctx, "div.do");

		is_min = LLVMBuildICmp(builder, LLVMIntEQ, lhs,
			LLVMConstInt(int_type, min_val, false), "is.min");
		is_neg1 = LLVMBuildICmp(builder, LLVMIntEQ, rhs,
			LLVMConstInt(int_type, (uint64) -1, true), "is.neg1");
		is_ovf = LLVMBuildAnd(builder, is_min, is_neg1, "div.ovf.check");
		LLVMBuildCondBr(builder, is_ovf, ovf_bb, div_bb);

		LLVMPositionBuilderAtEnd(builder, ovf_bb);
		emit_error_call(ctx, "uplpgsql_rt_int_overflow");

		LLVMPositionBuilderAtEnd(builder, div_bb);
		result = LLVMBuildSDiv(builder, lhs, rhs, "div.result");
	}
	else
	{
		/*
		 * SREM traps on INT_MIN % -1: the implied quotient overflows, so the
		 * x86 idiv raises #DE (SIGFPE) and takes the backend down, even though
		 * the mathematical remainder is 0.  It is undefined behaviour in LLVM
		 * either way — arm64 happens to return 0, which is why this is easy to
		 * miss on Apple Silicon.  int4mod/int8mod special-case a divisor of -1
		 * and return 0.
		 *
		 * Do the same without a branch: take the remainder against a divisor
		 * that is never -1 (SREM by 1 is always 0 and cannot trap), then
		 * select 0 when the real divisor was -1.
		 */
		LLVMValueRef	is_neg1,
						safe_rhs,
						rem;

		is_neg1 = LLVMBuildICmp(builder, LLVMIntEQ, rhs,
							    LLVMConstInt(int_type, (uint64) -1, true),
							    "mod.isneg1");
		safe_rhs = LLVMBuildSelect(builder, is_neg1,
								   LLVMConstInt(int_type, 1, false), rhs,
								   "mod.safe.rhs");
		rem = LLVMBuildSRem(builder, lhs, safe_rhs, "mod.result");
		result = LLVMBuildSelect(builder, is_neg1,
								 LLVMConstInt(int_type, 0, false), rem,
								 "mod.val");
	}

	{
		LLVMBasicBlockRef result_bb = LLVMGetInsertBlock(builder);

		LLVMBuildBr(builder, ok_bb);
		LLVMPositionBuilderAtEnd(builder, ok_bb);

		/* Phi from single predecessor (the block where result was computed) */
		{
			LLVMValueRef phi;

			phi = LLVMBuildPhi(builder, int_type, "divmod.val");
			LLVMAddIncoming(phi, &result, &result_bb, 1);
			return phi;
		}
	}
}

/*
 * Emit checked unary minus for iN (errors on MIN value).
 */
static LLVMValueRef
emit_int_negate(UPLpgSQL_compile_ctx *ctx, LLVMValueRef arg,
				LLVMTypeRef int_type, uint64 min_val)
{
	LLVMBuilderRef	builder = ctx->builder;
	LLVMBasicBlockRef ovf_bb, ok_bb;
	LLVMValueRef cmp;

	ovf_bb = expr_append_block(ctx, "neg.ovf");
	ok_bb = expr_append_block(ctx, "neg.ok");

	cmp = LLVMBuildICmp(builder, LLVMIntEQ, arg,
						LLVMConstInt(int_type, min_val, false), "neg.ismin");
	LLVMBuildCondBr(builder, cmp, ovf_bb, ok_bb);

	LLVMPositionBuilderAtEnd(builder, ovf_bb);
	emit_error_call(ctx, "uplpgsql_rt_int_overflow");

	LLVMPositionBuilderAtEnd(builder, ok_bb);
	return LLVMBuildNeg(builder, arg, "neg.result");
}

/*
 * Emit abs for iN (errors on MIN value).
 */
static LLVMValueRef
emit_int_abs(UPLpgSQL_compile_ctx *ctx, LLVMValueRef arg,
			 LLVMTypeRef int_type, uint64 min_val)
{
	LLVMBuilderRef	builder = ctx->builder;
	LLVMBasicBlockRef ovf_bb, ok_bb;
	LLVMValueRef cmp, neg;

	ovf_bb = expr_append_block(ctx, "abs.ovf");
	ok_bb = expr_append_block(ctx, "abs.ok");

	cmp = LLVMBuildICmp(builder, LLVMIntEQ, arg,
						LLVMConstInt(int_type, min_val, false), "abs.ismin");
	LLVMBuildCondBr(builder, cmp, ovf_bb, ok_bb);

	LLVMPositionBuilderAtEnd(builder, ovf_bb);
	emit_error_call(ctx, "uplpgsql_rt_int_overflow");

	LLVMPositionBuilderAtEnd(builder, ok_bb);
	neg = LLVMBuildNeg(builder, arg, "abs.neg");
	cmp = LLVMBuildICmp(builder, LLVMIntSGE, arg,
						LLVMConstInt(int_type, 0, false), "abs.cmp");
	return LLVMBuildSelect(builder, cmp, arg, neg, "abs.val");
}


/* ================================================================
 * Expression compilation — unified Expr tree → LLVM IR
 * ================================================================
 */

/*
 * Compile an expression that produces a Datum-width result (int4/int8/float8).
 *
 * Returns an LLVMValueRef in the native type (i32, i64, or double).
 * Sets *result_type to the type class of the result.
 */
static LLVMValueRef
uplpgsql_compile_expr_datum(UPLpgSQL_compile_ctx *ctx, Expr *expr,
							LLVMValueRef estate_ref,
							ExprTypeClass *result_type)
{
	LLVMBuilderRef	builder = ctx->builder;
	LLVMTypeRef		i32 = ctx->types[UPLPGSQL_INT32];
	LLVMTypeRef		i64 = ctx->types[UPLPGSQL_INT64];
	LLVMTypeRef		dbl = ctx->types[UPLPGSQL_DOUBLE];

	*result_type = uplpgsql_classify_expr(expr);

	switch (nodeTag(expr))
	{
		case T_Const:
			{
				Const *c = (Const *) expr;

				switch (*result_type)
				{
					case EXPR_TYPE_INT4:
						return LLVMConstInt(i32, DatumGetInt32(c->constvalue), true);
					case EXPR_TYPE_INT8:
						return LLVMConstInt(i64, DatumGetInt64(c->constvalue), true);
					case EXPR_TYPE_FLOAT8:
						return LLVMConstReal(dbl, DatumGetFloat8(c->constvalue));
					default:
						elog(ERROR, "uplpgsql: unexpected const type in datum compilation");
						return NULL;
				}
			}

		case T_Param:
			{
				Param	   *p = (Param *) expr;
				int			dno = p->paramid - 1;
				LLVMValueRef datum;

				datum = uplpgsql_emit_load_param_datum(ctx, estate_ref, dno);

				switch (*result_type)
				{
					case EXPR_TYPE_INT4:
						return LLVMBuildTrunc(builder, datum, i32, "int4.val");
					case EXPR_TYPE_INT8:
						/* Datum is already i64 for int8 */
						return datum;
					case EXPR_TYPE_FLOAT8:
						/* Datum contains the double bits; bitcast i64 → double */
						return LLVMBuildBitCast(builder, datum, dbl, "float8.val");
					default:
						elog(ERROR, "uplpgsql: unexpected param type in datum compilation");
						return NULL;
				}
			}

		case T_OpExpr:
			{
				OpExpr		   *op = (OpExpr *) expr;
				Oid				fid = op->opfuncid;

				/*
				 * Binary operators
				 */
				if (list_length(op->args) == 2)
				{
					ExprTypeClass ltype, rtype;
					LLVMValueRef lhs, rhs;

					lhs = uplpgsql_compile_expr_datum(ctx,
						(Expr *) linitial(op->args), estate_ref, &ltype);
					rhs = uplpgsql_compile_expr_datum(ctx,
						(Expr *) lsecond(op->args), estate_ref, &rtype);

					/* --- int4 arithmetic --- */
					if (fid == F_INT4PL)
						return emit_int_arith_checked(ctx, lhs, rhs,
							"llvm.sadd.with.overflow.i32", i32);
					if (fid == F_INT4MI)
						return emit_int_arith_checked(ctx, lhs, rhs,
							"llvm.ssub.with.overflow.i32", i32);
					if (fid == F_INT4MUL)
						return emit_int_arith_checked(ctx, lhs, rhs,
							"llvm.smul.with.overflow.i32", i32);
					if (fid == F_INT4DIV)
						return emit_int_divmod(ctx, lhs, rhs, i32, true,
							(uint32) PG_INT32_MIN);
					if (fid == F_INT4MOD)
						return emit_int_divmod(ctx, lhs, rhs, i32, false, 0);

					/* --- int8 arithmetic --- */
					if (fid == F_INT8PL)
						return emit_int_arith_checked(ctx, lhs, rhs,
							"llvm.sadd.with.overflow.i64", i64);
					if (fid == F_INT8MI)
						return emit_int_arith_checked(ctx, lhs, rhs,
							"llvm.ssub.with.overflow.i64", i64);
					if (fid == F_INT8MUL)
						return emit_int_arith_checked(ctx, lhs, rhs,
							"llvm.smul.with.overflow.i64", i64);
					if (fid == F_INT8DIV)
						return emit_int_divmod(ctx, lhs, rhs, i64, true,
							(uint64) PG_INT64_MIN);
					if (fid == F_INT8MOD)
						return emit_int_divmod(ctx, lhs, rhs, i64, false, 0);

					/* --- int4↔int8 cross-type arithmetic (result is int8) --- */
					if (fid == F_INT84PL || fid == F_INT48PL)
					{
						/* Widen int4 operand to int8 */
						if (ltype == EXPR_TYPE_INT4)
							lhs = LLVMBuildSExt(builder, lhs, i64, "widen.l");
						if (rtype == EXPR_TYPE_INT4)
							rhs = LLVMBuildSExt(builder, rhs, i64, "widen.r");
						return emit_int_arith_checked(ctx, lhs, rhs,
							"llvm.sadd.with.overflow.i64", i64);
					}
					if (fid == F_INT84MI || fid == F_INT48MI)
					{
						if (ltype == EXPR_TYPE_INT4)
							lhs = LLVMBuildSExt(builder, lhs, i64, "widen.l");
						if (rtype == EXPR_TYPE_INT4)
							rhs = LLVMBuildSExt(builder, rhs, i64, "widen.r");
						return emit_int_arith_checked(ctx, lhs, rhs,
							"llvm.ssub.with.overflow.i64", i64);
					}
					if (fid == F_INT84MUL || fid == F_INT48MUL)
					{
						if (ltype == EXPR_TYPE_INT4)
							lhs = LLVMBuildSExt(builder, lhs, i64, "widen.l");
						if (rtype == EXPR_TYPE_INT4)
							rhs = LLVMBuildSExt(builder, rhs, i64, "widen.r");
						return emit_int_arith_checked(ctx, lhs, rhs,
							"llvm.smul.with.overflow.i64", i64);
					}
					if (fid == F_INT84DIV || fid == F_INT48DIV)
					{
						if (ltype == EXPR_TYPE_INT4)
							lhs = LLVMBuildSExt(builder, lhs, i64, "widen.l");
						if (rtype == EXPR_TYPE_INT4)
							rhs = LLVMBuildSExt(builder, rhs, i64, "widen.r");
						return emit_int_divmod(ctx, lhs, rhs, i64, true,
							(uint64) PG_INT64_MIN);
					}

					/* --- float8 arithmetic (PG semantics, see above) --- */
					if (fid == F_FLOAT8PL)
						return emit_float_addsub(ctx, lhs, rhs, true);
					if (fid == F_FLOAT8MI)
						return emit_float_addsub(ctx, lhs, rhs, false);
					if (fid == F_FLOAT8MUL)
						return emit_float_mul(ctx, lhs, rhs);
					if (fid == F_FLOAT8DIV)
						return emit_float_div(ctx, lhs, rhs);
				}

				/*
				 * Unary operators
				 */
				if (list_length(op->args) == 1)
				{
					ExprTypeClass atype;
					LLVMValueRef arg;

					arg = uplpgsql_compile_expr_datum(ctx,
						(Expr *) linitial(op->args), estate_ref, &atype);

					if (fid == F_INT4UM)
						return emit_int_negate(ctx, arg, i32,
							(uint32) PG_INT32_MIN);
					if (fid == F_INT8UM)
						return emit_int_negate(ctx, arg, i64,
							(uint64) PG_INT64_MIN);
					if (fid == F_FLOAT8UM)
						return LLVMBuildFNeg(builder, arg, "f8.neg");
				}

				elog(ERROR, "uplpgsql: unhandled OpExpr funcid %u in datum compilation",
					 fid);
				return NULL;
			}

		case T_FuncExpr:
			{
				FuncExpr   *f = (FuncExpr *) expr;
				ExprTypeClass atype;
				LLVMValueRef arg;

				/*
				 * numeric const → float8: evaluate the cast at compile time.
				 * Must be handled before the generic arg compilation below,
				 * because Const(numeric) can't be compiled as a datum.
				 * The classifier only allows this for Const args.
				 */
				if (f->funcid == F_FLOAT8_NUMERIC)
				{
					Const *c = (Const *) linitial(f->args);

					Assert(IsA(c, Const));
					return LLVMConstReal(dbl,
						DatumGetFloat8(OidFunctionCall1(F_FLOAT8_NUMERIC,
														c->constvalue)));
				}

				/* Two-arg: pow(x, y) → llvm.pow.f64 */
				if (f->funcid == F_DPOW ||
					f->funcid == F_POW_FLOAT8_FLOAT8 ||
					f->funcid == F_POWER_FLOAT8_FLOAT8)
				{
					LLVMValueRef lhs, rhs;
					ExprTypeClass ltype, rtype;

					lhs = uplpgsql_compile_expr_datum(ctx,
						(Expr *) linitial(f->args), estate_ref, &ltype);
					rhs = uplpgsql_compile_expr_datum(ctx,
						(Expr *) lsecond(f->args), estate_ref, &rtype);

					{
						unsigned	id;
						LLVMValueRef fn, args[2];
						LLVMTypeRef ovf_types[] = { dbl };

						id = LLVMLookupIntrinsicID("llvm.pow", 8);
						fn = LLVMGetIntrinsicDeclaration(ctx->module, id,
														 ovf_types, 1);
						args[0] = lhs;
						args[1] = rhs;
						return LLVMBuildCall2(builder,
							LLVMIntrinsicGetType(ctx->context, id,
												  ovf_types, 1),
							fn, args, 2, "pow");
					}
				}

				arg = uplpgsql_compile_expr_datum(ctx,
					(Expr *) linitial(f->args), estate_ref, &atype);

				if (f->funcid == F_INT4ABS)
					return emit_int_abs(ctx, arg, i32, (uint32) PG_INT32_MIN);
				if (f->funcid == F_INT8ABS)
					return emit_int_abs(ctx, arg, i64, (uint64) PG_INT64_MIN);
				if (f->funcid == F_FLOAT8ABS)
				{
					LLVMValueRef neg, cmp;

					neg = LLVMBuildFNeg(builder, arg, "fabs.neg");
					cmp = LLVMBuildFCmp(builder, LLVMRealOGE, arg,
						LLVMConstReal(dbl, 0.0), "fabs.cmp");
					return LLVMBuildSelect(builder, cmp, arg, neg, "fabs.val");
				}

				/* Single-arg float8 math → LLVM intrinsics */
				if (f->funcid == F_DSQRT || f->funcid == F_SQRT_FLOAT8 ||
					f->funcid == F_CEIL_FLOAT8 || f->funcid == F_CEILING_FLOAT8 ||
					f->funcid == F_FLOOR_FLOAT8 ||
					f->funcid == F_DEXP || f->funcid == F_EXP_FLOAT8 ||
					f->funcid == F_DLOG1 || f->funcid == F_LN_FLOAT8 ||
					f->funcid == F_SIN || f->funcid == F_COS)
				{
					const char *iname;
					unsigned	id;
					LLVMValueRef fn, iargs[1];
					LLVMTypeRef ovf_types[] = { dbl };

					if (f->funcid == F_DSQRT || f->funcid == F_SQRT_FLOAT8)
						iname = "llvm.sqrt";
					else if (f->funcid == F_CEIL_FLOAT8 ||
							 f->funcid == F_CEILING_FLOAT8)
						iname = "llvm.ceil";
					else if (f->funcid == F_FLOOR_FLOAT8)
						iname = "llvm.floor";
					else if (f->funcid == F_DEXP || f->funcid == F_EXP_FLOAT8)
						iname = "llvm.exp";
					else if (f->funcid == F_DLOG1 || f->funcid == F_LN_FLOAT8)
						iname = "llvm.log";
					else if (f->funcid == F_SIN)
						iname = "llvm.sin";
					else
						iname = "llvm.cos";

					id = LLVMLookupIntrinsicID(iname, strlen(iname));
					fn = LLVMGetIntrinsicDeclaration(ctx->module, id,
													  ovf_types, 1);
					iargs[0] = arg;
					return LLVMBuildCall2(builder,
						LLVMIntrinsicGetType(ctx->context, id,
											  ovf_types, 1),
						fn, iargs, 1, "math");
				}

				/* int4/int2 → float8: sitofp i32 → double */
				if (f->funcid == F_FLOAT8_INT4 || f->funcid == F_FLOAT8_INT2)
					return LLVMBuildSIToFP(builder, arg, dbl, "i4tod");

				/* int8 → float8: sitofp i64 → double */
				if (f->funcid == F_FLOAT8_INT8)
					return LLVMBuildSIToFP(builder, arg, dbl, "i8tod");

				/* float4 → float8: fpext float → double */
				if (f->funcid == F_FLOAT8_FLOAT4)
					return LLVMBuildFPExt(builder, arg, dbl, "f4tod");

				elog(ERROR, "uplpgsql: unhandled FuncExpr funcid %u in datum compilation",
					 f->funcid);
				return NULL;
			}

		case T_RelabelType:
			return uplpgsql_compile_expr_datum(ctx,
				((RelabelType *) expr)->arg, estate_ref, result_type);

		case T_SubscriptingRef:
			{
				SubscriptingRef *sbsref = (SubscriptingRef *) expr;
				Param		   *array_param = (Param *) sbsref->refexpr;
				Expr		   *idx_expr = (Expr *) linitial(sbsref->refupperindexpr);
				ExprTypeClass	idx_class;
				int				array_dno = array_param->paramid - 1;
				LLVMValueRef	idx_val;
				UPLpgSQL_native_array *na;

				/* Compile the subscript index to native i32 */
				idx_val = uplpgsql_compile_expr_datum(ctx, idx_expr,
													  estate_ref, &idx_class);

				na = find_native_array(ctx, array_dno);
				if (na != NULL)
				{
					/*
					 * Native array subscript read (expression context).
					 *
					 * No bounds check here: tier1_expr_any_null() folds the
					 * range test into the caller's NULL test, and the caller
					 * only branches here when the subscript is in range — so
					 * an out-of-range read has already become SQL NULL, which
					 * is what PostgreSQL returns for it.  Emitting a check
					 * that can never fire would only cost a branch, and the
					 * old one raised where PostgreSQL does not.
					 *
					 * Index relative to the array's lower bound, which need
					 * not be 1 ('[2:3]={9,10}').
					 */
					LLVMValueRef data, lb, idx0, gep, result;

					data = LLVMBuildLoad2(builder, ctx->types[UPLPGSQL_PTR],
										  na->data_ptr, "na.data");
					lb = LLVMBuildLoad2(builder, i32, na->lb_ptr, "na.lb");
					idx0 = LLVMBuildSub(builder, idx_val, lb, "idx0");
					gep = LLVMBuildGEP2(builder, na->llvm_elemtype,
										data, &idx0, 1, "na.elem_ptr");
					result = LLVMBuildLoad2(builder, na->llvm_elemtype,
											gep, "na.elem");
					return result;
				}

				/* Standard PG array path */
				{
					LLVMValueRef	isnull_ptr, elem_datum;
					LLVMBasicBlockRef entry_bb;

					/* Alloca for isNull output (must be in entry block) */
					entry_bb = LLVMGetEntryBasicBlock(ctx->function);
					{
						LLVMBuilderRef tmp = LLVMCreateBuilderInContext(ctx->context);

						LLVMPositionBuilderBefore(tmp,
							LLVMGetFirstInstruction(entry_bb));
						isnull_ptr = LLVMBuildAlloca(tmp,
							ctx->types[UPLPGSQL_INT1], "sref_isnull");
						LLVMDisposeBuilder(tmp);
					}

					/* Call RT_ARRAY_GET_ELEMENT with compile-time type info */
					{
						ArrayTypeInfo ati;

						resolve_array_type_info(ctx, array_dno, &ati);
						{
							LLVMValueRef args[] = {
								estate_ref,
								LLVMConstInt(ctx->types[UPLPGSQL_INT32],
											 array_dno, false),
								idx_val,
								ati.typlen_val,
								ati.elmlen_val,
								ati.elmbyval_val,
								ati.elmalign_val,
								isnull_ptr
							};
							elem_datum = LLVMBuildCall2(ctx->builder,
								ctx->rt_fntypes[RT_ARRAY_GET_ELEMENT],
								ctx->rt_funcs[RT_ARRAY_GET_ELEMENT],
								args, 8, "arr_elem");
						}
					}

					/*
					 * Convert Datum (i64) to native type.  The classifier
					 * already verified the element type is a known Tier 1 type.
					 */
					if (*result_type == EXPR_TYPE_INT4)
						return LLVMBuildTrunc(builder, elem_datum, i32, "arr.i4");
					if (*result_type == EXPR_TYPE_INT8)
						return elem_datum;
					if (*result_type == EXPR_TYPE_FLOAT8)
						return LLVMBuildBitCast(builder, elem_datum, dbl, "arr.f8");

					/* BOOL: trunc i64 → i1 */
					return LLVMBuildTrunc(builder, elem_datum,
										  ctx->types[UPLPGSQL_INT1], "arr.bool");
				}
			}

		default:
			elog(ERROR, "uplpgsql: unhandled node type %d in datum compilation",
				 (int) nodeTag(expr));
			return NULL;
	}
}

/*
 * Compile a boolean expression tree to LLVM IR.
 * Returns an LLVMValueRef of type i1.
 */
static LLVMValueRef
uplpgsql_compile_expr_bool(UPLpgSQL_compile_ctx *ctx, Expr *expr,
						   LLVMValueRef estate_ref)
{
	LLVMBuilderRef	builder = ctx->builder;
	LLVMTypeRef		i1 = ctx->types[UPLPGSQL_INT1];
	LLVMTypeRef		i64 = ctx->types[UPLPGSQL_INT64];

	switch (nodeTag(expr))
	{
		case T_Const:
			{
				Const *c = (Const *) expr;

				return LLVMConstInt(i1, DatumGetBool(c->constvalue) ? 1 : 0,
								   false);
			}

		case T_Param:
			{
				Param	   *p = (Param *) expr;
				int			dno = p->paramid - 1;
				LLVMValueRef datum;

				datum = uplpgsql_emit_load_param_datum(ctx, estate_ref, dno);
				return LLVMBuildTrunc(builder, datum, i1, "bool.val");
			}

		case T_OpExpr:
			{
				OpExpr	   *op = (OpExpr *) expr;
				Oid			fid = op->opfuncid;
				LLVMIntPredicate int_pred = 0;

				bool		is_float_cmp = false;
				bool		is_cross_cmp = false;

				/* Determine comparison predicate */
				/* int4 comparisons */
				if (fid == F_INT4EQ) int_pred = LLVMIntEQ;
				else if (fid == F_INT4NE) int_pred = LLVMIntNE;
				else if (fid == F_INT4LT) int_pred = LLVMIntSLT;
				else if (fid == F_INT4LE) int_pred = LLVMIntSLE;
				else if (fid == F_INT4GT) int_pred = LLVMIntSGT;
				else if (fid == F_INT4GE) int_pred = LLVMIntSGE;
				/* int8 comparisons */
				else if (fid == F_INT8EQ) int_pred = LLVMIntEQ;
				else if (fid == F_INT8NE) int_pred = LLVMIntNE;
				else if (fid == F_INT8LT) int_pred = LLVMIntSLT;
				else if (fid == F_INT8GT) int_pred = LLVMIntSGT;
				else if (fid == F_INT8LE) int_pred = LLVMIntSLE;
				else if (fid == F_INT8GE) int_pred = LLVMIntSGE;
				/* int4↔int8 cross-type comparisons */
				else if (fid == F_INT84EQ || fid == F_INT48EQ)
					{ int_pred = LLVMIntEQ; is_cross_cmp = true; }
				else if (fid == F_INT84NE || fid == F_INT48NE)
					{ int_pred = LLVMIntNE; is_cross_cmp = true; }
				else if (fid == F_INT84LT)
					{ int_pred = LLVMIntSLT; is_cross_cmp = true; }
				else if (fid == F_INT84GT)
					{ int_pred = LLVMIntSGT; is_cross_cmp = true; }
				else if (fid == F_INT84LE)
					{ int_pred = LLVMIntSLE; is_cross_cmp = true; }
				else if (fid == F_INT84GE)
					{ int_pred = LLVMIntSGE; is_cross_cmp = true; }
				else if (fid == F_INT48LT)
					{ int_pred = LLVMIntSLT; is_cross_cmp = true; }
				else if (fid == F_INT48GT)
					{ int_pred = LLVMIntSGT; is_cross_cmp = true; }
				else if (fid == F_INT48LE)
					{ int_pred = LLVMIntSLE; is_cross_cmp = true; }
				else if (fid == F_INT48GE)
					{ int_pred = LLVMIntSGE; is_cross_cmp = true; }
				/*
				 * float8 comparisons: emit_float_cmp() picks the form from
				 * the funcid, since none of them is a plain LLVM predicate —
				 * each needs NaN handling around it.
				 */
				else if (fid == F_FLOAT8EQ || fid == F_FLOAT8NE ||
						 fid == F_FLOAT8LT || fid == F_FLOAT8LE ||
						 fid == F_FLOAT8GT || fid == F_FLOAT8GE)
					is_float_cmp = true;
				/* bool comparisons */
				else if (fid == F_BOOLEQ) int_pred = LLVMIntEQ;
				else if (fid == F_BOOLNE) int_pred = LLVMIntNE;

				if (is_float_cmp)
				{
					ExprTypeClass lt, rt;
					LLVMValueRef lhs, rhs;

					lhs = uplpgsql_compile_expr_datum(ctx,
						(Expr *) linitial(op->args), estate_ref, &lt);
					rhs = uplpgsql_compile_expr_datum(ctx,
						(Expr *) lsecond(op->args), estate_ref, &rt);
					return emit_float_cmp(ctx, fid, lhs, rhs);
				}

				if (int_pred != 0)
				{
					/* Check if we're comparing booleans */
					if (fid == F_BOOLEQ || fid == F_BOOLNE)
					{
						LLVMValueRef lhs, rhs;

						lhs = uplpgsql_compile_expr_bool(ctx,
							(Expr *) linitial(op->args), estate_ref);
						rhs = uplpgsql_compile_expr_bool(ctx,
							(Expr *) lsecond(op->args), estate_ref);
						return LLVMBuildICmp(builder, int_pred, lhs, rhs,
											 "boolcmp.result");
					}

					/* Integer comparison */
					{
						ExprTypeClass lt, rt;
						LLVMValueRef lhs, rhs;

						lhs = uplpgsql_compile_expr_datum(ctx,
							(Expr *) linitial(op->args), estate_ref, &lt);
						rhs = uplpgsql_compile_expr_datum(ctx,
							(Expr *) lsecond(op->args), estate_ref, &rt);

						/* Widen for cross-type comparisons */
						if (is_cross_cmp)
						{
							if (lt == EXPR_TYPE_INT4)
								lhs = LLVMBuildSExt(builder, lhs, i64,
													"widen.l");
							if (rt == EXPR_TYPE_INT4)
								rhs = LLVMBuildSExt(builder, rhs, i64,
													"widen.r");
						}

						return LLVMBuildICmp(builder, int_pred, lhs, rhs,
											 "icmp.result");
					}
				}

				elog(ERROR, "uplpgsql: unhandled OpExpr funcid %u in bool compilation",
					 fid);
				return NULL;
			}

		case T_BoolExpr:
			{
				BoolExpr   *b = (BoolExpr *) expr;
				ListCell   *lc;

				if (b->boolop == NOT_EXPR)
				{
					LLVMValueRef arg = uplpgsql_compile_expr_bool(ctx,
						(Expr *) linitial(b->args), estate_ref);
					return LLVMBuildNot(builder, arg, "not.result");
				}
				else if (b->boolop == AND_EXPR)
				{
					LLVMValueRef result = LLVMConstInt(i1, 1, false);

					foreach(lc, b->args)
					{
						LLVMValueRef arg = uplpgsql_compile_expr_bool(ctx,
							(Expr *) lfirst(lc), estate_ref);
						result = LLVMBuildAnd(builder, result, arg,
											  "and.result");
					}
					return result;
				}
				else if (b->boolop == OR_EXPR)
				{
					LLVMValueRef result = LLVMConstInt(i1, 0, false);

					foreach(lc, b->args)
					{
						LLVMValueRef arg = uplpgsql_compile_expr_bool(ctx,
							(Expr *) lfirst(lc), estate_ref);
						result = LLVMBuildOr(builder, result, arg,
											 "or.result");
					}
					return result;
				}

				elog(ERROR, "uplpgsql: unhandled BoolExpr type %d",
					 (int) b->boolop);
				return NULL;
			}

		default:
			elog(ERROR, "uplpgsql: unhandled node type %d in bool compilation",
				 (int) nodeTag(expr));
			return NULL;
	}
}


/* ================================================================
 * Tier 2: fmgr bypass — direct PG function pointer calls
 *
 * For expressions that can't be compiled to native LLVM arithmetic
 * (Tier 1) but whose expression tree has resolvable function OIDs
 * and only Param/Const leaf nodes, we emit a direct call to the
 * PG function's C implementation, bypassing the ExprEvalStep
 * interpreter entirely.
 *
 * At compile time, we resolve the function OID via fmgr_info() to
 * get the C function pointer.  In LLVM IR, we:
 *   1. alloca a FunctionCallInfoBaseData on the stack
 *   2. zero it, fill in nargs, collation, isnull=false
 *   3. load argument Datums from variables (Param) or embed (Const)
 *   4. call the resolved C function pointer directly
 *   5. read back the result Datum
 * ================================================================
 */

/*
 * Check whether an expression tree can be compiled via fmgr bypass.
 *
 * All leaf nodes must be Params (variables) or Consts, and all
 * intermediate nodes must be OpExpr, FuncExpr, or BoolExpr with
 * resolvable function OIDs.  We also support RelabelType (type
 * coercion casts that don't change the Datum value).
 */
static bool
uplpgsql_can_fmgr_compile(Expr *expr)
{
	if (expr == NULL)
		return false;

	switch (nodeTag(expr))
	{
		case T_Const:
			return true;

		case T_Param:
			{
				Param *p = (Param *) expr;

				return (p->paramkind == PARAM_EXTERN);
			}

		case T_RelabelType:
			{
				RelabelType *r = (RelabelType *) expr;

				return uplpgsql_can_fmgr_compile(r->arg);
			}

		case T_OpExpr:
			{
				OpExpr	   *op = (OpExpr *) expr;
				ListCell   *lc;

				if (op->opfuncid == InvalidOid)
					return false;

				/*
				 * Only allow Tier 2 for built-in (system catalog) functions.
				 * User-defined functions (SQL, PL/pgSQL, etc.) use mutable
				 * FmgrInfo state and need CachedPlan invalidation when the
				 * function is replaced via CREATE OR REPLACE.  We cannot
				 * safely freeze their function pointers at compile time.
				 */
				if (op->opfuncid >= FirstNormalObjectId)
					return false;

				foreach(lc, op->args)
				{
					if (!uplpgsql_can_fmgr_compile((Expr *) lfirst(lc)))
						return false;
				}
				return true;
			}

		case T_FuncExpr:
			{
				FuncExpr   *f = (FuncExpr *) expr;
				ListCell   *lc;

				if (f->funcid == InvalidOid)
					return false;

				/* Same as T_OpExpr: only built-in functions are safe */
				if (f->funcid >= FirstNormalObjectId)
					return false;

				foreach(lc, f->args)
				{
					if (!uplpgsql_can_fmgr_compile((Expr *) lfirst(lc)))
						return false;
				}
				return true;
			}

		case T_BoolExpr:
			{
				BoolExpr   *b = (BoolExpr *) expr;
				ListCell   *lc;

				foreach(lc, b->args)
				{
					if (!uplpgsql_can_fmgr_compile((Expr *) lfirst(lc)))
						return false;
				}
				return true;
			}

		default:
			return false;
	}
}

/*
 * Does this fmgr-bypass expression allocate transient memory when it runs?
 *
 * True if any function or operator in the tree returns a pass-by-reference
 * type -- numeric, text, and friends palloc their result (and any by-ref
 * intermediate) in the current context.  The caller uses this to decide
 * whether the statement needs an allocation scope around it; a tree of only
 * by-value results (int and float arithmetic, comparisons, casts to int)
 * allocates nothing and needs no scope, so the common path pays nothing.
 *
 * The typbyval lookups are compile-time only.
 */
static bool
fmgr_expr_allocates(Expr *expr)
{
	if (expr == NULL)
		return false;

	switch (nodeTag(expr))
	{
		case T_OpExpr:
			{
				OpExpr	   *op = (OpExpr *) expr;
				ListCell   *lc;

				if (OidIsValid(op->opresulttype) &&
					!get_typbyval(op->opresulttype))
					return true;
				foreach(lc, op->args)
					if (fmgr_expr_allocates((Expr *) lfirst(lc)))
						return true;
				return false;
			}

		case T_FuncExpr:
			{
				FuncExpr   *f = (FuncExpr *) expr;
				ListCell   *lc;

				if (OidIsValid(f->funcresulttype) &&
					!get_typbyval(f->funcresulttype))
					return true;
				foreach(lc, f->args)
					if (fmgr_expr_allocates((Expr *) lfirst(lc)))
						return true;
				return false;
			}

		case T_RelabelType:
			return fmgr_expr_allocates(((RelabelType *) expr)->arg);

		case T_BoolExpr:
			{
				BoolExpr   *b = (BoolExpr *) expr;
				ListCell   *lc;

				foreach(lc, b->args)
					if (fmgr_expr_allocates((Expr *) lfirst(lc)))
						return true;
				return false;
			}

		default:
			/* Const, Param: a value, not a fresh allocation. */
			return false;
	}
}

/*
 * Emit IR to load a variable's Datum value for use as a function argument.
 * Returns the Datum as i64.
 */
static LLVMValueRef
fmgr_load_arg_datum(UPLpgSQL_compile_ctx *ctx, Expr *expr,
					LLVMValueRef estate_ref)
{
	LLVMTypeRef i64 = ctx->types[UPLPGSQL_INT64];

	switch (nodeTag(expr))
	{
		case T_Const:
			{
				Const *c = (Const *) expr;

				if (c->constisnull)
					return LLVMConstInt(i64, 0, false);

				/*
				 * Embed the Datum value as an i64 constant.
				 *
				 * For a pass-by-reference constant (text, numeric, ...) the
				 * Datum is a pointer into the cached plan's tree.  Embedding
				 * it raw freezes that pointer into the compiled code, which
				 * outlives any particular plan: the JIT cache invalidates on
				 * fn_xmin/fn_tid (the pg_proc row), so a plan discarded and
				 * rebuilt for an unrelated reason — a revalidation after DDL,
				 * say — would leave the compiled code pointing at freed
				 * memory.  Copy it somewhere that lasts as long as the code
				 * does instead.
				 *
				 * TopMemoryContext matches the lifetime of the compiled code,
				 * which is never released (LLJIT has no cheap per-function
				 * removal), so this is the same deliberate trade the code
				 * itself makes rather than a new leak.
				 */
				if (!c->constbyval)
				{
					MemoryContext	oldcxt;
					Datum			persistent;

					oldcxt = MemoryContextSwitchTo(TopMemoryContext);
					persistent = datumCopy(c->constvalue, false, c->constlen);
					MemoryContextSwitchTo(oldcxt);

					return LLVMConstInt(i64, (uint64) persistent, false);
				}

				return LLVMConstInt(i64, (uint64) c->constvalue, false);
			}

		case T_Param:
			{
				Param *p = (Param *) expr;
				int dno = p->paramid - 1;

				return uplpgsql_emit_load_param_datum(ctx, estate_ref, dno);
			}

		case T_RelabelType:
			{
				RelabelType *r = (RelabelType *) expr;

				return fmgr_load_arg_datum(ctx, r->arg, estate_ref);
			}

		default:
			/* For sub-expressions, recursively compile via fmgr */
			return uplpgsql_compile_expr_fmgr(ctx, expr, estate_ref);
	}
}

/*
 * Emit IR to check if a variable is NULL (for strict function handling).
 * Returns i1 (true if NULL).
 */
static LLVMValueRef
fmgr_load_arg_isnull(UPLpgSQL_compile_ctx *ctx, Expr *expr,
					  LLVMValueRef estate_ref)
{
	LLVMTypeRef i1 = ctx->types[UPLPGSQL_INT1];

	switch (nodeTag(expr))
	{
		case T_Const:
			{
				Const *c = (Const *) expr;

				return LLVMConstInt(i1, c->constisnull ? 1 : 0, false);
			}

		case T_Param:
			{
				Param *p = (Param *) expr;
				int dno = p->paramid - 1;

				return uplpgsql_emit_load_param_isnull(ctx, estate_ref, dno);
			}

		case T_RelabelType:
			return fmgr_load_arg_isnull(ctx, ((RelabelType *) expr)->arg,
										estate_ref);

		default:
			/* Sub-expressions: not null (function call results handled separately) */
			return LLVMConstInt(i1, 0, false);
	}
}

/*
 * Compile an expression via direct fmgr function call.
 *
 * Resolves the PG function OID to a C function pointer at compile time,
 * emits LLVM IR to allocate FunctionCallInfoBaseData on stack, fill in
 * arguments, and call the function directly.
 *
 * Returns the result as Datum (i64).
 */
static LLVMValueRef
uplpgsql_compile_expr_fmgr(UPLpgSQL_compile_ctx *ctx, Expr *expr,
						    LLVMValueRef estate_ref)
{
	return uplpgsql_compile_expr_fmgr_full(ctx, expr, estate_ref, NULL);
}

/*
 * Full variant that also returns the result's isnull flag (i1).
 * If isnull_out is NULL, isnull tracking is skipped (pass-by-value path).
 */
static LLVMValueRef
uplpgsql_compile_expr_fmgr_full(UPLpgSQL_compile_ctx *ctx, Expr *expr,
								LLVMValueRef estate_ref,
								LLVMValueRef *isnull_out)
{
	LLVMBuilderRef	builder = ctx->builder;
	LLVMTypeRef		i8 = ctx->types[UPLPGSQL_INT8];
	LLVMTypeRef		i16 = ctx->types[UPLPGSQL_INT16];
	LLVMTypeRef		i32 = ctx->types[UPLPGSQL_INT32];
	LLVMTypeRef		i64 = ctx->types[UPLPGSQL_INT64];
	LLVMTypeRef		ptr = ctx->types[UPLPGSQL_PTR];
	LLVMTypeRef		i1 = ctx->types[UPLPGSQL_INT1];

	if (isnull_out)
		*isnull_out = LLVMConstInt(i1, 0, false);  /* default: not null */

	switch (nodeTag(expr))
	{
		case T_Const:
			{
				Const *c = (Const *) expr;

				if (isnull_out && c->constisnull)
					*isnull_out = LLVMConstInt(i1, 1, false);
				return fmgr_load_arg_datum(ctx, expr, estate_ref);
			}

		case T_Param:
			{
				if (isnull_out)
					*isnull_out = fmgr_load_arg_isnull(ctx, expr, estate_ref);
				return fmgr_load_arg_datum(ctx, expr, estate_ref);
			}

		case T_RelabelType:
			return uplpgsql_compile_expr_fmgr_full(ctx,
				((RelabelType *) expr)->arg, estate_ref, isnull_out);

		case T_BoolExpr:
			{
				/*
				 * BoolExpr (AND/OR/NOT) with SQL's three-valued logic.
				 *
				 * These are the only non-strict operators we compile, so the
				 * result's nullness cannot be "null if any input is null":
				 *
				 *   NULL AND false = false     NULL OR true = true
				 *   NULL AND true  = NULL      NULL OR false = NULL
				 *   NOT NULL       = NULL
				 *
				 * AND is NULL when some input is NULL and none is definitely
				 * false; OR is NULL when some input is NULL and none is
				 * definitely true.  Both are computed branch-free: the value
				 * is the ordinary bitwise fold (a NULL input contributes its
				 * datum, which is 0 = false, and that is exactly what makes
				 * "false wins" for AND and lets a true input win for OR).
				 */
				BoolExpr   *b = (BoolExpr *) expr;
				ListCell   *lc;

				if (b->boolop == NOT_EXPR)
				{
					LLVMValueRef arg, arg_isnull = NULL, b1, notv;

					arg = uplpgsql_compile_expr_fmgr_full(ctx,
						(Expr *) linitial(b->args), estate_ref, &arg_isnull);
					if (arg == NULL)
						return NULL;
					/* NOT NULL is NULL: nullness passes straight through */
					if (isnull_out)
						*isnull_out = arg_isnull;
					b1 = LLVMBuildTrunc(builder, arg, i1, "fmgr.not.in");
					notv = LLVMBuildNot(builder, b1, "fmgr.not");
					return LLVMBuildZExt(builder, notv, i64, "fmgr.not.datum");
				}
				else
				{
					bool			is_and = (b->boolop == AND_EXPR);
					LLVMValueRef	result = LLVMConstInt(i1, is_and ? 1 : 0,
														  false);
					LLVMValueRef	any_null = LLVMConstInt(i1, 0, false);
					LLVMValueRef	decided = LLVMConstInt(i1, 0, false);

					foreach(lc, b->args)
					{
						LLVMValueRef arg, arg_isnull = NULL, b1, known;

						arg = uplpgsql_compile_expr_fmgr_full(ctx,
							(Expr *) lfirst(lc), estate_ref, &arg_isnull);
						if (arg == NULL)
							return NULL;

						b1 = LLVMBuildTrunc(builder, arg, i1,
											is_and ? "fmgr.and.in"
												   : "fmgr.or.in");
						result = is_and
							? LLVMBuildAnd(builder, result, b1, "fmgr.and")
							: LLVMBuildOr(builder, result, b1, "fmgr.or");

						any_null = LLVMBuildOr(builder, any_null, arg_isnull,
											   "fmgr.bool.anynull");

						/*
						 * "decided": this input settles the result on its own
						 * — a non-NULL false for AND, a non-NULL true for OR.
						 */
						known = LLVMBuildNot(builder, arg_isnull,
											 "fmgr.bool.notnull");
						if (is_and)
							known = LLVMBuildAnd(builder, known,
								LLVMBuildNot(builder, b1, "fmgr.bool.isfalse"),
								"fmgr.bool.decides");
						else
							known = LLVMBuildAnd(builder, known, b1,
												 "fmgr.bool.decides");
						decided = LLVMBuildOr(builder, decided, known,
											  "fmgr.bool.decided");
					}

					if (isnull_out)
						*isnull_out = LLVMBuildAnd(builder, any_null,
							LLVMBuildNot(builder, decided, "fmgr.bool.undec"),
							"fmgr.bool.isnull");

					return LLVMBuildZExt(builder, result, i64,
										 is_and ? "fmgr.and.datum"
												: "fmgr.or.datum");
				}
			}

		case T_OpExpr:
		case T_FuncExpr:
			{
				Oid			funcid;
				List	   *args;
				Oid			collation;
				int			nargs;
				FmgrInfo	finfo;
				PGFunction	fn_addr;
				LLVMValueRef fci_alloca;
				int			fci_size;
				LLVMValueRef off, gep;
				LLVMValueRef fn_ptr_val;
				LLVMValueRef call_result;
				LLVMTypeRef	fn_type;
				ListCell   *lc;
				int			argidx;
				LLVMValueRef *arg_isnulls;

				if (IsA(expr, OpExpr))
				{
					OpExpr *op = (OpExpr *) expr;

					funcid = op->opfuncid;
					args = op->args;
					collation = op->inputcollid;
				}
				else
				{
					FuncExpr *f = (FuncExpr *) expr;

					funcid = f->funcid;
					args = f->args;
					collation = f->inputcollid;
				}

				nargs = list_length(args);

				/*
				 * float8(numeric-constant) → fold to a float8 Datum at
				 * compile time, exactly as the Tier 1 float path does.
				 *
				 * This is not just an optimisation.  Left as a runtime call,
				 * numeric_float8 allocates transient memory on every
				 * invocation, and a JIT'd loop -- unlike the interpreter --
				 * never resets the context that memory lands in, so a numeric
				 * literal inside any fmgr-bypass expression (a comparison under
				 * AND/OR, an argument to a cast) leaks without bound.
				 * "t2 > 0.0001 AND ..." in a hot loop was enough to exhaust
				 * memory and have the backend killed.  Folding removes the call.
				 */
				if (IsA(expr, FuncExpr) && funcid == F_FLOAT8_NUMERIC &&
					IsA(linitial(args), Const))
				{
					Const *c = (Const *) linitial(args);

					if (c->constisnull)
					{
						if (isnull_out)
							*isnull_out = LLVMConstInt(i1, 1, false);
						return LLVMConstInt(i64, 0, false);
					}
					return LLVMConstInt(i64, (uint64)
						OidFunctionCall1(F_FLOAT8_NUMERIC, c->constvalue),
						false);
				}

				/*
				 * Polymorphic functions (e.g. textanycat) cannot be called
				 * directly via fmgr bypass because the FunctionCallInfo we
				 * build doesn't carry polymorphic type resolution info.
				 * Fall back to Tier 3 for these.
				 */
				{
					Oid		   *declared_argtypes;
					int			declared_nargs;
					Oid			rettype;
					bool		has_poly = false;

					rettype = get_func_signature(funcid,
												 &declared_argtypes,
												 &declared_nargs);
					if (IsPolymorphicType(rettype))
						has_poly = true;
					for (int i = 0; i < declared_nargs && !has_poly; i++)
					{
						if (IsPolymorphicType(declared_argtypes[i]))
							has_poly = true;
					}
					pfree(declared_argtypes);
					if (has_poly)
						return NULL;
				}

				/*
				 * Resolve the function OID to a C function pointer at compile
				 * time.  This is the key optimization: we embed the resolved
				 * pointer as an LLVM constant, so the JIT'd code calls the
				 * PG function directly without going through fmgr_info or
				 * the expression evaluator.
				 */
				fmgr_info(funcid, &finfo);
				fn_addr = finfo.fn_addr;

				elog(DEBUG1, "uplpgsql: fmgr bypass for funcid %u (%d args, strict=%d)",
					 funcid, nargs, finfo.fn_strict);

				/*
				 * Allocate FunctionCallInfoBaseData on the LLVM stack.
				 * Size = header + nargs * sizeof(NullableDatum).
				 *
				 * IMPORTANT: alloca must be in the entry block to avoid
				 * unbounded stack growth when this expression is inside a
				 * loop body (e.g. WHILE loop in Mandelbrot computation).
				 */
				fci_size = OFF_FCI_ARGS + SIZE_NULLABLE_DATUM * nargs;

				/*
				 * Allocate the FunctionCallInfoBaseData and set everything
				 * about it that does not change between calls -- the zero-fill,
				 * flinfo, nargs, and collation -- once, in the entry block.
				 *
				 * All of this is loop-invariant: emitting it at the call site
				 * re-ran a memset plus four stores on every iteration, for
				 * fields the callee never changes.  Only the argument slots and
				 * the isnull reset are genuinely per-call, and those stay
				 * below.  The alloca must be in the entry block regardless, to
				 * avoid unbounded stack growth in a loop; the init rides along.
				 *
				 * Initialising an fcinfo whose call is never reached (it sits
				 * in a branch not taken) is harmless -- it only writes the
				 * scratch slot.
				 */
				{
					LLVMBasicBlockRef entry_bb;
					LLVMValueRef	  first_instr;
					LLVMBasicBlockRef saved_bb;
					FmgrInfo		 *persistent_finfo;

					saved_bb = LLVMGetInsertBlock(builder);
					entry_bb = LLVMGetEntryBasicBlock(ctx->function);
					first_instr = LLVMGetFirstInstruction(entry_bb);
					if (first_instr)
						LLVMPositionBuilderBefore(builder, first_instr);
					else
						LLVMPositionBuilderAtEnd(builder, entry_bb);

					fci_alloca = LLVMBuildArrayAlloca(builder, i8,
						LLVMConstInt(i32, fci_size, false), "fci.alloca");
					LLVMSetAlignment(fci_alloca, 8);

					/* Zero the struct */
					LLVMBuildMemSet(builder, fci_alloca,
						LLVMConstInt(i8, 0, false),
						LLVMConstInt(i64, fci_size, false), 8);

					/*
					 * Set flinfo to point to a persistent FmgrInfo.  Some
					 * built-in functions dereference fcinfo->flinfo for fn_oid,
					 * fn_collation, etc.  Allocate in TopMemoryContext so it
					 * lives as long as the JIT'd code.
					 *
					 * The compiled code holds a raw pointer to this FmgrInfo
					 * for as long as it exists, so it must outlive any
					 * compilation context — hence TopMemoryContext, and hence
					 * never freed.
					 *
					 * That is deliberate, not an oversight: the JIT'd code
					 * itself is intentionally leaked on recompile (LLJIT has
					 * no cheap per-function removal, and an in-flight call may
					 * still be executing the old code), so freeing the FmgrInfo
					 * on recompile would reintroduce exactly the
					 * use-after-free that leaking the code avoids.  The
					 * FmgrInfo is a few dozen bytes against the kilobytes of
					 * machine code it accompanies.
					 */
					persistent_finfo = (FmgrInfo *)
						MemoryContextAllocZero(TopMemoryContext,
											   sizeof(FmgrInfo));
					memcpy(persistent_finfo, &finfo, sizeof(FmgrInfo));
					persistent_finfo->fn_mcxt = TopMemoryContext;

					off = LLVMConstInt(i64, OFF_FCI_FLINFO, false);
					gep = LLVMBuildGEP2(builder, i8, fci_alloca, &off, 1,
										"fci.flinfo.ptr");
					LLVMBuildStore(builder,
						LLVMConstIntToPtr(
							LLVMConstInt(i64,
										 (uintptr_t) persistent_finfo,
										 false),
							ptr),
						LLVMBuildBitCast(builder, gep,
							LLVMPointerType(ptr, 0),
							"fci.flinfo.typed"));

					/* Set nargs */
					off = LLVMConstInt(i64, OFF_FCI_NARGS, false);
					gep = LLVMBuildGEP2(builder, i8, fci_alloca, &off, 1,
										"fci.nargs.ptr");
					LLVMBuildStore(builder,
						LLVMConstInt(i16, nargs, false),
						LLVMBuildBitCast(builder, gep,
							LLVMPointerType(i16, 0), "fci.nargs.typed"));

					/* Set collation */
					if (collation != InvalidOid)
					{
						off = LLVMConstInt(i64, OFF_FCI_COLLATION, false);
						gep = LLVMBuildGEP2(builder, i8, fci_alloca, &off, 1,
											"fci.collation.ptr");
						LLVMBuildStore(builder,
							LLVMConstInt(i32, collation, false),
							LLVMBuildBitCast(builder, gep,
								LLVMPointerType(i32, 0), "fci.collation.typed"));
					}

					LLVMPositionBuilderAtEnd(builder, saved_bb);
				}

				/* Fill in argument values and isnull flags */
				arg_isnulls = (LLVMValueRef *)
					palloc(sizeof(LLVMValueRef) * (nargs > 0 ? nargs : 1));
				argidx = 0;
				foreach(lc, args)
				{
					Expr	   *arg_expr = (Expr *) lfirst(lc);
					LLVMValueRef arg_datum;
					LLVMValueRef arg_isnull;
					uint64		arg_off;

					/*
					 * Compile the argument once, taking its value and its
					 * nullness together.
					 *
					 * Going through _full() rather than the two loaders is
					 * what makes a nested call's NULL visible here: for a
					 * Const/Param/RelabelType it delegates to exactly the same
					 * loaders as before, but for a sub-expression it returns
					 * the isnull that expression actually computed.
					 * fmgr_load_arg_isnull() answers a constant "not null" for
					 * those, so abs(int4larger(NULL, NULL)) called the strict
					 * int4abs with a bogus not-null argument and produced 0
					 * where SQL says NULL.
					 *
					 * Keep each isnull for the STRICT check below, which needs
					 * them again; recomputing would compile every argument a
					 * second time.
					 */
					arg_datum = uplpgsql_compile_expr_fmgr_full(ctx, arg_expr,
																estate_ref,
																&arg_isnull);

					/*
					 * _full() returns NULL for anything it cannot compile.
					 * Storing that would dereference it inside LLVM; bail out
					 * instead and let the caller fall back to Tier 3.
					 */
					if (arg_datum == NULL || arg_isnull == NULL)
						return NULL;

					arg_isnulls[argidx] = arg_isnull;

					/* args[argidx].value */
					arg_off = OFF_FCI_ARGS + SIZE_NULLABLE_DATUM * argidx
							  + OFF_ND_VALUE;
					off = LLVMConstInt(i64, arg_off, false);
					gep = LLVMBuildGEP2(builder, i8, fci_alloca, &off, 1,
										"fci.arg.value.ptr");
					LLVMBuildStore(builder, arg_datum,
						LLVMBuildBitCast(builder, gep,
							LLVMPointerType(i64, 0), "fci.arg.value.typed"));

					/* args[argidx].isnull */
					arg_off = OFF_FCI_ARGS + SIZE_NULLABLE_DATUM * argidx
							  + OFF_ND_ISNULL;
					off = LLVMConstInt(i64, arg_off, false);
					gep = LLVMBuildGEP2(builder, i8, fci_alloca, &off, 1,
										"fci.arg.isnull.ptr");
					LLVMBuildStore(builder,
						LLVMBuildZExt(builder, arg_isnull, i8, "isnull.i8"),
						gep);

					argidx++;
				}

				/*
				 * Strict function null check: if any argument is NULL,
				 * skip the function call and return NULL.
				 *
				 * We emit a chain of basic blocks that check each argument:
				 *   check_arg0 → check_arg1 → ... → call_func
				 *        ↓            ↓
				 *      skip_bb (return NULL datum)
				 *
				 * Both call and skip paths merge via PHI at merge_bb.
				 */
				if (finfo.fn_strict && nargs > 0 && isnull_out)
				{
					LLVMBasicBlockRef call_bb, skip_bb, merge_bb;
					LLVMValueRef	 result_phi, isnull_phi;
					LLVMValueRef	 incoming_vals[2];
					LLVMBasicBlockRef incoming_bbs[2];

					call_bb = expr_append_block(ctx, "fmgr.strict.call");
					skip_bb = expr_append_block(ctx, "fmgr.strict.skip");
					merge_bb = expr_append_block(ctx, "fmgr.strict.merge");

					/*
					 * Check each argument for NULL.  We OR all the isnull
					 * flags together and branch once.
					 */
					{
						LLVMValueRef any_null = LLVMConstInt(i1, 0, false);
						int			 ai;

						/*
						 * Reuse the isnull each argument already computed.
						 * Re-deriving them with fmgr_load_arg_isnull() would
						 * both recompile every argument and reintroduce the
						 * constant "not null" answer for nested calls, which
						 * is what let a strict function run on a NULL.
						 */
						for (ai = 0; ai < nargs; ai++)
							any_null = LLVMBuildOr(builder, any_null,
												   arg_isnulls[ai],
												   "fmgr.strict.ornull");

						LLVMBuildCondBr(builder, any_null, skip_bb, call_bb);
					}

					/* Skip path: return NULL */
					LLVMPositionBuilderAtEnd(builder, skip_bb);
					LLVMBuildBr(builder, merge_bb);

					/* Call path */
					LLVMPositionBuilderAtEnd(builder, call_bb);

					/* Reset fcinfo->isnull = false before call */
					off = LLVMConstInt(i64, OFF_FCI_ISNULL, false);
					gep = LLVMBuildGEP2(builder, i8, fci_alloca, &off, 1,
										"fci.isnull.ptr");
					LLVMBuildStore(builder, LLVMConstInt(i8, 0, false), gep);

					fn_type = LLVMFunctionType(i64, &ptr, 1, false);
					fn_ptr_val = LLVMConstIntToPtr(
						LLVMConstInt(i64, (uintptr_t) fn_addr, false),
						LLVMPointerType(fn_type, 0));

					call_result = LLVMBuildCall2(builder, fn_type, fn_ptr_val,
						&fci_alloca, 1, "fmgr.result");

					/* Read back fcinfo->isnull */
					off = LLVMConstInt(i64, OFF_FCI_ISNULL, false);
					gep = LLVMBuildGEP2(builder, i8, fci_alloca, &off, 1,
										"fci.resisnull.ptr");
					{
						LLVMValueRef res_isnull_raw = LLVMBuildLoad2(builder, i8, gep,
																	 "fci.resisnull.raw");
						LLVMValueRef res_isnull = LLVMBuildTrunc(builder, res_isnull_raw,
																 i1, "fci.resisnull");

						LLVMBuildBr(builder, merge_bb);

						/* Merge with PHI */
						LLVMPositionBuilderAtEnd(builder, merge_bb);

						/* Datum PHI */
						incoming_vals[0] = LLVMConstInt(i64, 0, false);	/* skip: 0 */
						incoming_vals[1] = call_result;					/* call: result */
						incoming_bbs[0] = skip_bb;
						incoming_bbs[1] = call_bb;
						result_phi = LLVMBuildPhi(builder, i64, "fmgr.datum.phi");
						LLVMAddIncoming(result_phi, incoming_vals, incoming_bbs, 2);

						/* isnull PHI */
						incoming_vals[0] = LLVMConstInt(i1, 1, false);	/* skip: true */
						incoming_vals[1] = res_isnull;					/* call: from fcinfo */
						isnull_phi = LLVMBuildPhi(builder, i1, "fmgr.isnull.phi");
						LLVMAddIncoming(isnull_phi, incoming_vals, incoming_bbs, 2);

						*isnull_out = isnull_phi;
						return result_phi;
					}
				}

				/*
				 * Non-strict or no isnull tracking: just call directly.
				 *
				 * Embed the resolved C function pointer as an LLVM constant
				 * and call it with the fcinfo pointer.
				 *
				 * PGFunction signature: Datum (*)(FunctionCallInfo)
				 * which is: i64 (*)(ptr)
				 */

				/* Reset fcinfo->isnull = false before call */
				off = LLVMConstInt(i64, OFF_FCI_ISNULL, false);
				gep = LLVMBuildGEP2(builder, i8, fci_alloca, &off, 1,
									"fci.isnull.ptr");
				LLVMBuildStore(builder, LLVMConstInt(i8, 0, false), gep);

				fn_type = LLVMFunctionType(i64, &ptr, 1, false);
				fn_ptr_val = LLVMConstIntToPtr(
					LLVMConstInt(i64, (uintptr_t) fn_addr, false),
					LLVMPointerType(fn_type, 0));

				call_result = LLVMBuildCall2(builder, fn_type, fn_ptr_val,
					&fci_alloca, 1, "fmgr.result");

				/* If caller wants isnull, read it back from fcinfo */
				if (isnull_out)
				{
					LLVMValueRef res_isnull_raw, res_isnull;

					off = LLVMConstInt(i64, OFF_FCI_ISNULL, false);
					gep = LLVMBuildGEP2(builder, i8, fci_alloca, &off, 1,
										"fci.resisnull.ptr2");
					res_isnull_raw = LLVMBuildLoad2(builder, i8, gep,
													"fci.resisnull.raw2");
					res_isnull = LLVMBuildTrunc(builder, res_isnull_raw, i1,
												"fci.resisnull2");
					*isnull_out = res_isnull;
				}

				return call_result;
			}

		default:
			elog(ERROR, "uplpgsql: unhandled node type %d in fmgr compilation",
				 (int) nodeTag(expr));
			return NULL;
	}
}


/* ================================================================
 * Public API — called from uplpgsql_compile.c
 * ================================================================
 */

/*
 * Datum conversion: native value → i64 Datum for storage.
 */
static LLVMValueRef
native_to_datum(UPLpgSQL_compile_ctx *ctx, LLVMValueRef val,
				ExprTypeClass type_class)
{
	LLVMBuilderRef builder = ctx->builder;
	LLVMTypeRef i64 = ctx->types[UPLPGSQL_INT64];

	switch (type_class)
	{
		case EXPR_TYPE_INT4:
			return LLVMBuildSExt(builder, val, i64, "int4.datum");
		case EXPR_TYPE_INT8:
			/* Already i64 */
			return val;
		case EXPR_TYPE_FLOAT8:
			/* Bitcast double → i64 for Datum storage */
			return LLVMBuildBitCast(builder, val, i64, "f8.datum");
		default:
			elog(ERROR, "uplpgsql: cannot convert type class %d to Datum",
				 (int) type_class);
			return NULL;
	}
}

/*
 * Datum conversion: i64 Datum → native value.  The inverse of
 * native_to_datum(), for taking the result of a Tier 2 fmgr call back into
 * native form.  Only the pass-by-value classes a native array can hold.
 */
static LLVMValueRef
datum_to_native(UPLpgSQL_compile_ctx *ctx, LLVMValueRef datum,
				ExprTypeClass type_class)
{
	LLVMBuilderRef builder = ctx->builder;

	switch (type_class)
	{
		case EXPR_TYPE_INT4:
			return LLVMBuildTrunc(builder, datum,
								  ctx->types[UPLPGSQL_INT32], "datum.int4");
		case EXPR_TYPE_INT8:
			/* Already i64 */
			return datum;
		case EXPR_TYPE_FLOAT8:
			return LLVMBuildBitCast(builder, datum,
									ctx->types[UPLPGSQL_DOUBLE], "datum.f8");
		default:
			elog(ERROR, "uplpgsql: cannot convert Datum to type class %d",
				 (int) type_class);
			return NULL;
	}
}

/*
 * Map a native array's element type OID to its ExprTypeClass.
 */
static ExprTypeClass
elemtype_to_class(Oid elemtype)
{
	switch (elemtype)
	{
		case INT4OID:
			return EXPR_TYPE_INT4;
		case INT8OID:
			return EXPR_TYPE_INT8;
		case FLOAT8OID:
			return EXPR_TYPE_FLOAT8;
		default:
			return EXPR_TYPE_UNKNOWN;
	}
}

/*
 * Map target type OID to ExprTypeClass.
 */
static ExprTypeClass
oid_to_type_class(Oid typoid)
{
	if (typoid == INT4OID)
		return EXPR_TYPE_INT4;
	if (typoid == INT8OID)
		return EXPR_TYPE_INT8;
	if (typoid == FLOAT8OID)
		return EXPR_TYPE_FLOAT8;
	return EXPR_TYPE_UNKNOWN;
}

/*
 * Try to compile an assignment as native arithmetic or fmgr bypass.
 *
 * Tier 1: native LLVM instructions for int/float ops
 * Tier 2: direct PG function call for other scalar expressions
 *
 * Returns true if inlined, false → caller uses runtime helper.
 */
bool
uplpgsql_try_compile_assign(UPLpgSQL_compile_ctx *ctx,
							UPLpgSQL_stmt_assign *stmt)
{
	Expr		   *expr;
	ExprTypeClass	target_class, expr_class;
	LLVMValueRef	estate_ref, result, datum_val;
	UPLpgSQL_datum *target_datum;
	UPLpgSQL_var   *target_var;

	/* Only inline assignments to scalar variables */
	target_datum = ctx_func(ctx)->datums[stmt->varno];
	if (target_datum->dtype != UPLPGSQL_DTYPE_VAR)
		return false;

	target_var = (UPLpgSQL_var *) target_datum;
	if (target_var->datatype == NULL)
		return false;

	/* Get the parsed expression tree */
	expr = uplpgsql_prepare_and_get_expr(ctx, stmt->expr);
	if (expr == NULL)
		return false;

	target_class = oid_to_type_class(target_var->datatype->typoid);

	/* Tier 1: try native LLVM instructions */
	expr_class = uplpgsql_classify_expr(expr);
	if (expr_class != EXPR_TYPE_UNKNOWN && expr_class == target_class)
	{
		LLVMValueRef	any_null;

		elog(DEBUG1, "uplpgsql: inlining %s assignment to dno %d: %s",
			 (target_class == EXPR_TYPE_INT4 ? "int4" :
			  target_class == EXPR_TYPE_INT8 ? "int8" : "float8"),
			 stmt->varno, stmt->expr->query);

		estate_ref = LLVMGetParam(ctx->function, 0);
		any_null = tier1_expr_any_null(ctx, expr, estate_ref);

		if (any_null == NULL)
		{
			/* Nothing nullable in it — compute unconditionally. */
			result = uplpgsql_compile_expr_datum(ctx, expr, estate_ref,
												 &expr_class);
			datum_val = native_to_datum(ctx, result, expr_class);
			uplpgsql_emit_store_var_datum(ctx, estate_ref, stmt->varno,
										  datum_val);
			return true;
		}

		/*
		 * Some leaf may be NULL, and every Tier 1 operator is strict, so the
		 * result is then NULL.  Branch rather than compute-and-discard: the
		 * arithmetic can raise (division by zero, overflow) on whatever
		 * garbage a NULL variable's datum happens to hold, and PostgreSQL
		 * would have returned NULL without ever invoking the operator.
		 */
		{
			LLVMBasicBlockRef	compute_bb, null_bb, merge_bb;

			compute_bb = LLVMAppendBasicBlockInContext(ctx->context,
													   ctx->function,
													   "t1.notnull");
			null_bb = LLVMAppendBasicBlockInContext(ctx->context,
													ctx->function, "t1.null");
			merge_bb = LLVMAppendBasicBlockInContext(ctx->context,
													 ctx->function,
													 "t1.done");

			LLVMBuildCondBr(ctx->builder, any_null, null_bb, compute_bb);

			LLVMPositionBuilderAtEnd(ctx->builder, compute_bb);
			result = uplpgsql_compile_expr_datum(ctx, expr, estate_ref,
												 &expr_class);
			datum_val = native_to_datum(ctx, result, expr_class);
			uplpgsql_emit_store_var_datum(ctx, estate_ref, stmt->varno,
										  datum_val);
			LLVMBuildBr(ctx->builder, merge_bb);

			LLVMPositionBuilderAtEnd(ctx->builder, null_bb);
			uplpgsql_emit_store_var_null(ctx, estate_ref, stmt->varno);
			LLVMBuildBr(ctx->builder, merge_bb);

			LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);
		}
		return true;
	}

	/*
	 * Tier 2: fmgr bypass — direct PG function call.
	 *
	 * For pass-by-value types, store directly via GEP (no cleanup needed).
	 * For pass-by-reference types (text, numeric, etc.), use the
	 * RT_ASSIGN_VAR_DATUM runtime helper which calls assign_simple_var()
	 * to handle freeval cleanup of old values.
	 */
	if (uplpgsql_can_fmgr_compile(expr))
	{
		estate_ref = LLVMGetParam(ctx->function, 0);

		if (target_var->datatype->typbyval)
		{
			LLVMValueRef	isnull_val;
			bool			scoped = fmgr_expr_allocates(expr);
			LLVMValueRef	old = NULL;

			/*
			 * A by-value result can still be reached through a by-reference
			 * intermediate -- length(upper(t)) returns int4 but allocated a
			 * text along the way.  Bracket the evaluation so that text is
			 * freed; a tree of only by-value operators needs no scope.
			 */
			if (scoped)
			{
				LLVMValueRef a[] = { estate_ref };

				old = LLVMBuildCall2(ctx->builder,
					ctx->rt_fntypes[RT_ALLOC_SCOPE_ENTER],
					ctx->rt_funcs[RT_ALLOC_SCOPE_ENTER], a, 1, "scope.old");
			}

			/*
			 * Take the isnull the expression computed, rather than assuming
			 * not-null: a strict function with a NULL argument is skipped and
			 * yields NULL, and 3VL boolean operators produce NULL of their
			 * own accord.  Storing with isnull hardwired to false turned
			 * those into 0/false.
			 */
			datum_val = uplpgsql_compile_expr_fmgr_full(ctx, expr, estate_ref,
														&isnull_val);
			if (datum_val != NULL)
			{
				elog(DEBUG1, "uplpgsql: fmgr bypass assignment to dno %d: %s",
					 stmt->varno, stmt->expr->query);
				uplpgsql_emit_store_var_datum_isnull(ctx, estate_ref,
													 stmt->varno, datum_val,
													 isnull_val);
				if (scoped)
				{
					LLVMValueRef a[] = { estate_ref, old };

					LLVMBuildCall2(ctx->builder,
						ctx->rt_fntypes[RT_ALLOC_SCOPE_EXIT],
						ctx->rt_funcs[RT_ALLOC_SCOPE_EXIT], a, 2, "");
				}
				return true;
			}
			/* fmgr_full bailed: restore the context before Tier 3 */
			if (scoped)
			{
				LLVMValueRef a[] = { estate_ref, old };

				LLVMBuildCall2(ctx->builder,
					ctx->rt_fntypes[RT_ALLOC_SCOPE_EXIT],
					ctx->rt_funcs[RT_ALLOC_SCOPE_EXIT], a, 2, "");
			}
			/* Fall through to Tier 3 */
		}
		else
		{
			/*
			 * Pass-by-reference Tier 2.
			 *
			 * datumCopy prevents use-after-free either way: the palloc'd
			 * result may reuse the block that held the old variable value.
			 *
			 * When the expression contains an allocating call (upper(t),
			 * a || b, ...) bracket it in an allocation scope so the result and
			 * any intermediates are freed rather than leaked;
			 * RT_COPY_ASSIGN_VAR_DATUM_SCOPED copies the result into the
			 * durable context before resetting the scope.  A bare variable or
			 * constant read (y := x) allocates nothing and must NOT be scoped:
			 * reading a native array whole-datum syncs it into the variable's
			 * own slot, and resetting the scope would free that live value.
			 */
			LLVMValueRef	isnull_val;
			bool			scoped = fmgr_expr_allocates(expr);
			LLVMValueRef	old = NULL;

			if (scoped)
			{
				LLVMValueRef a[] = { estate_ref };

				old = LLVMBuildCall2(ctx->builder,
					ctx->rt_fntypes[RT_ALLOC_SCOPE_ENTER],
					ctx->rt_funcs[RT_ALLOC_SCOPE_ENTER], a, 1, "scope.old");
			}

			datum_val = uplpgsql_compile_expr_fmgr_full(ctx, expr, estate_ref,
														&isnull_val);
			if (datum_val != NULL)
			{

				elog(DEBUG1, "uplpgsql: fmgr bypass (pass-by-ref, copy) assignment to dno %d: %s",
					 stmt->varno, stmt->expr->query);

				if (scoped)
				{
					LLVMValueRef args[] = {
						estate_ref,
						LLVMConstInt(ctx->types[UPLPGSQL_INT32], stmt->varno, false),
						datum_val,
						LLVMBuildZExt(ctx->builder, isnull_val,
									  ctx->types[UPLPGSQL_INT8], "isnull.i8"),
						old
					};
					LLVMBuildCall2(ctx->builder,
						ctx->rt_fntypes[RT_COPY_ASSIGN_VAR_DATUM_SCOPED],
						ctx->rt_funcs[RT_COPY_ASSIGN_VAR_DATUM_SCOPED],
						args, 5, "");
				}
				else
				{
					LLVMValueRef args[] = {
						estate_ref,
						LLVMConstInt(ctx->types[UPLPGSQL_INT32], stmt->varno, false),
						datum_val,
						LLVMBuildZExt(ctx->builder, isnull_val,
									  ctx->types[UPLPGSQL_INT8], "isnull.i8")
					};
					LLVMBuildCall2(ctx->builder,
								   ctx->rt_fntypes[RT_COPY_ASSIGN_VAR_DATUM],
								   ctx->rt_funcs[RT_COPY_ASSIGN_VAR_DATUM],
								   args, 4, "");
				}

				/*
				 * We just wrote a whole PG Datum into the target.  If the
				 * target is a native array its flat memory is now stale,
				 * so reload it from the Datum we stored.
				 */
				{
					UPLpgSQL_native_array *target_na;

					target_na = find_native_array(ctx, stmt->varno);
					if (target_na != NULL)
					{
						elog(DEBUG1, "uplpgsql: native array from_datum dno %d "
							 "(fmgr bypass assign)", target_na->dno);
						uplpgsql_emit_refresh_native_array(ctx, target_na);
					}
				}
				return true;
			}
			/* fmgr_full bailed: restore the context before Tier 3 */
			if (scoped)
			{
				LLVMValueRef a[] = { estate_ref, old };

				LLVMBuildCall2(ctx->builder,
					ctx->rt_fntypes[RT_ALLOC_SCOPE_EXIT],
					ctx->rt_funcs[RT_ALLOC_SCOPE_EXIT], a, 2, "");
			}
			/* Fall through to Tier 3 */
		}
	}

	/*
	 * Native array initialization: intercept array_fill() for native arrays.
	 *
	 * When a native local array (identified by escape analysis) is assigned
	 * a non-subscript expression, we expect it to be the initialization via
	 * array_fill(val, ARRAY[n]).  We intercept this pattern and emit flat
	 * memory allocation instead of building a real PG array Datum.
	 *
	 * The generated IR:
	 *   1. Compile the size expression (n) to an i32
	 *   2. Compute byte_size = n * elem_size
	 *   3. Branch: if byte_size <= 4096 → alloca + memset (stack)
	 *              else → RT_NATIVE_ARRAY_ALLOC (heap palloc0)
	 *   4. PHI merge → store data pointer and length
	 *
	 * If the expression doesn't match array_fill, fall through to the
	 * standard array path (Tier 3 or array subscript helpers).
	 */
	{
		UPLpgSQL_native_array *na = find_native_array(ctx, stmt->varno);

		if (na != NULL && !IsA(expr, SubscriptingRef))
		{
			/*
			 * Look for the pattern: array_fill(<val>, ARRAY[<n>]).
			 * The expression has been SPI_prepare'd, so we can examine
			 * the plan's query_list to find the FuncExpr.
			 *
			 * IMPORTANT: use plansource->query_list (contains Query nodes),
			 * NOT cplan->stmt_list (contains PlannedStmt nodes).
			 */
			CachedPlanSource *plansource;
			Query		   *query;
			TargetEntry	   *tle;
			FuncExpr	   *fexpr;
			Expr		   *size_arg;
			Expr		   *fill_arg;
			ExprTypeClass	fill_class;
			LLVMValueRef	fill_val;
			ExprTypeClass	size_class;
			LLVMValueRef	n_val, byte_size, threshold, use_stack;
			LLVMValueRef	stack_ptr, heap_ptr, data_ptr;
			LLVMBasicBlockRef stack_bb, heap_bb, merge_bb;
			LLVMTypeRef		i32_ty = ctx->types[UPLPGSQL_INT32];
			LLVMTypeRef		i64_ty = ctx->types[UPLPGSQL_INT64];

			estate_ref = LLVMGetParam(ctx->function, 0);

			if (stmt->expr->plan == NULL)
				goto not_native_init;

			plansource = (CachedPlanSource *)
				linitial(SPI_plan_get_plan_sources(stmt->expr->plan));

			/*
			 * Use query_list (contains Query nodes), NOT
			 * cplan->stmt_list (contains PlannedStmt nodes).
			 */
			if (list_length(plansource->query_list) != 1)
				goto not_native_init;
			query = linitial_node(Query, plansource->query_list);
			if (list_length(query->targetList) != 1)
				goto not_native_init;

			tle = linitial_node(TargetEntry, query->targetList);

			/* Unwrap any CoerceViaIO or similar */
			if (!IsA(tle->expr, FuncExpr))
				goto not_native_init;

			fexpr = (FuncExpr *) tle->expr;

			/* Verify it's array_fill by checking function name */
			{
				char *funcname = get_func_name(fexpr->funcid);

				if (funcname == NULL || strcmp(funcname, "array_fill") != 0)
				{
					if (funcname)
						pfree(funcname);
	
					goto not_native_init;
				}
				pfree(funcname);
			}

			/*
			 * array_fill(value, ARRAY[n]) — first arg is the fill
			 * value, second arg is an int4[] constructor.
			 */
			if (list_length(fexpr->args) < 2)
			{

				goto not_native_init;
			}

			/* Extract fill value (first argument) */
			fill_arg = (Expr *) linitial(fexpr->args);

			{
				Node *size_node = lsecond(fexpr->args);

				/* The size argument is ARRAY[n] — an ArrayExpr */
				if (IsA(size_node, ArrayExpr))
				{
					ArrayExpr *aexpr = (ArrayExpr *) size_node;

					if (list_length(aexpr->elements) != 1)
					{
		
						goto not_native_init;
					}
					size_arg = (Expr *) linitial(aexpr->elements);
				}
				else
				{
	
					goto not_native_init;
				}
			}

			/* Compile the size expression to i32 */
			size_class = uplpgsql_classify_expr(size_arg);
			if (size_class != EXPR_TYPE_INT4)
			{
				/*
				 * Size might be a Param (function argument).
				 * Try compiling it anyway.
				 */
				if (IsA(size_arg, Param))
				{
					Param *p = (Param *) size_arg;

					if (p->paramkind == PARAM_EXTERN)
					{
						size_class = EXPR_TYPE_INT4;
						n_val = uplpgsql_compile_expr_datum(ctx, size_arg,
														   estate_ref,
														   &size_class);
					}
					else
						goto not_native_init;
				}
				else
					goto not_native_init;
			}
			else
			{
				n_val = uplpgsql_compile_expr_datum(ctx, size_arg,
												   estate_ref, &size_class);
			}

			/* Compile the fill value */
			fill_class = uplpgsql_classify_expr(fill_arg);
			if (fill_class != EXPR_TYPE_INT4 &&
				fill_class != EXPR_TYPE_INT8 &&
				fill_class != EXPR_TYPE_FLOAT8)
			{
				/* Try as Param */
				if (IsA(fill_arg, Param))
				{
					Param *p = (Param *) fill_arg;

					if (p->paramkind == PARAM_EXTERN)
					{
						/* Determine type from native array */
						if (na->elemtype == INT4OID)
							fill_class = EXPR_TYPE_INT4;
						else if (na->elemtype == INT8OID)
							fill_class = EXPR_TYPE_INT8;
						else
							fill_class = EXPR_TYPE_FLOAT8;
						fill_val = uplpgsql_compile_expr_datum(ctx, fill_arg,
															  estate_ref,
															  &fill_class);
					}
					else
						goto not_native_init;
				}
				else
					goto not_native_init;
			}
			else
			{
				fill_val = uplpgsql_compile_expr_datum(ctx, fill_arg,
													  estate_ref,
													  &fill_class);
			}

			elog(DEBUG1, "uplpgsql: native array alloc dno %d (elem_size %d): %s",
				 na->dno, na->elem_size, stmt->expr->query);

			/*
			 * Store the length.  array_fill always produces a 1-based array
			 * with no NULL elements.
			 */
			LLVMBuildStore(ctx->builder, n_val, na->len_ptr);
			LLVMBuildStore(ctx->builder, upl_const_int32(ctx, 1), na->lb_ptr);
			LLVMBuildStore(ctx->builder,
						   LLVMConstNull(ctx->types[UPLPGSQL_PTR]),
						   na->nulls_ptr);

			/* byte_size = (i64)n * elem_size */
			byte_size = LLVMBuildMul(ctx->builder,
				LLVMBuildSExt(ctx->builder, n_val, i64_ty, "n64"),
				LLVMConstInt(i64_ty, na->elem_size, false),
				"byte_size");

			/*
			 * Stack vs heap decision at runtime:
			 *   if byte_size <= NATIVE_ARRAY_STACK_THRESHOLD → alloca
			 *   else → palloc0 via runtime helper
			 */
			threshold = LLVMConstInt(i64_ty, NATIVE_ARRAY_STACK_THRESHOLD, false);
			use_stack = LLVMBuildICmp(ctx->builder, LLVMIntSLE,
									  byte_size, threshold, "use_stack");

			stack_bb = expr_append_block(ctx, "na.stack");
			heap_bb = expr_append_block(ctx, "na.heap");
			merge_bb = expr_append_block(ctx, "na.merge");

			LLVMBuildCondBr(ctx->builder, use_stack, stack_bb, heap_bb);

			/* Stack path: alloca + memset */
			LLVMPositionBuilderAtEnd(ctx->builder, stack_bb);
			{
				LLVMValueRef alloca_size;

				/* Use i8 alloca with byte_size count */
				alloca_size = LLVMBuildTrunc(ctx->builder, byte_size,
											 i32_ty, "stack_bytes");
				stack_ptr = LLVMBuildArrayAlloca(ctx->builder,
					ctx->types[UPLPGSQL_INT8], alloca_size, "na.stack_mem");

				/* memset to zero */
				LLVMBuildMemSet(ctx->builder, stack_ptr,
					LLVMConstInt(ctx->types[UPLPGSQL_INT8], 0, false),
					byte_size, 0);
			}
			LLVMBuildBr(ctx->builder, merge_bb);

			/* Heap path: palloc0 via runtime helper */
			LLVMPositionBuilderAtEnd(ctx->builder, heap_bb);
			{
				LLVMValueRef args[] = { estate_ref, byte_size };

				heap_ptr = LLVMBuildCall2(ctx->builder,
					ctx->rt_fntypes[RT_NATIVE_ARRAY_ALLOC],
					ctx->rt_funcs[RT_NATIVE_ARRAY_ALLOC],
					args, 2, "na.heap_mem");
			}
			LLVMBuildBr(ctx->builder, merge_bb);

			/* Merge: PHI to select stack or heap pointer */
			LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);
			data_ptr = LLVMBuildPhi(ctx->builder, ctx->types[UPLPGSQL_PTR],
									"na.data");
			{
				LLVMValueRef	vals[] = { stack_ptr, heap_ptr };
				LLVMBasicBlockRef blocks[] = { stack_bb, heap_bb };

				LLVMAddIncoming(data_ptr, vals, blocks, 2);
			}

			LLVMBuildStore(ctx->builder, data_ptr, na->data_ptr);

			/*
			 * Fill the array with the fill value.
			 * Emit a simple loop: for (i = 0; i < n; i++) data[i] = val;
			 */
			{
				LLVMBasicBlockRef fill_cond_bb, fill_body_bb, fill_done_bb;
				LLVMValueRef	idx_ptr, idx, cmp, elem_ptr;
				LLVMTypeRef		elem_llvm_type;

				if (na->elemtype == INT4OID)
					elem_llvm_type = ctx->types[UPLPGSQL_INT32];
				else if (na->elemtype == INT8OID)
					elem_llvm_type = ctx->types[UPLPGSQL_INT64];
				else
					elem_llvm_type = ctx->types[UPLPGSQL_DOUBLE];

				/* Cast fill_val to the element type if needed */
				if (fill_class == EXPR_TYPE_INT4 &&
					na->elemtype == INT8OID)
					fill_val = LLVMBuildSExt(ctx->builder, fill_val,
											 elem_llvm_type, "fill64");
				else if (fill_class == EXPR_TYPE_INT8 &&
						 na->elemtype == INT4OID)
					fill_val = LLVMBuildTrunc(ctx->builder, fill_val,
											  elem_llvm_type, "fill32");

				fill_cond_bb = expr_append_block(ctx, "na.fill.cond");
				fill_body_bb = expr_append_block(ctx, "na.fill.body");
				fill_done_bb = expr_append_block(ctx, "na.fill.done");

				idx_ptr = LLVMBuildAlloca(ctx->builder, i32_ty, "fill_idx");
				LLVMBuildStore(ctx->builder,
							   LLVMConstInt(i32_ty, 0, false), idx_ptr);
				LLVMBuildBr(ctx->builder, fill_cond_bb);

				/* Condition: i < n */
				LLVMPositionBuilderAtEnd(ctx->builder, fill_cond_bb);
				idx = LLVMBuildLoad2(ctx->builder, i32_ty, idx_ptr,
									 "fill_i");
				cmp = LLVMBuildICmp(ctx->builder, LLVMIntSLT,
									idx, n_val, "fill_cmp");
				LLVMBuildCondBr(ctx->builder, cmp,
								fill_body_bb, fill_done_bb);

				/* Body: data[i] = fill_val; i++ */
				LLVMPositionBuilderAtEnd(ctx->builder, fill_body_bb);
				{
					LLVMValueRef byte_off, gep_idx;

					byte_off = LLVMBuildMul(ctx->builder, idx,
						LLVMConstInt(i32_ty, na->elem_size, false),
						"fill_off");
					gep_idx = LLVMBuildSExt(ctx->builder, byte_off,
						i64_ty, "fill_off64");
					elem_ptr = LLVMBuildGEP2(ctx->builder,
						ctx->types[UPLPGSQL_INT8],
						data_ptr, &gep_idx, 1, "fill_elem");
					elem_ptr = LLVMBuildBitCast(ctx->builder, elem_ptr,
						LLVMPointerType(elem_llvm_type, 0), "fill_typed");
					LLVMBuildStore(ctx->builder, fill_val, elem_ptr);
				}
				/* i++ */
				LLVMBuildStore(ctx->builder,
					LLVMBuildAdd(ctx->builder, idx,
								 LLVMConstInt(i32_ty, 1, false), "fill_inc"),
					idx_ptr);
				LLVMBuildBr(ctx->builder, fill_cond_bb);

				LLVMPositionBuilderAtEnd(ctx->builder, fill_done_bb);
			}

			return true;
		}
	}
not_native_init:

	/*
	 * Array subscript operations via dedicated runtime helpers.
	 *
	 * These bypass SPI expression evaluation entirely: the subscript index
	 * is compiled to native Tier 1 IR, and the runtime helper calls
	 * array_get_element/array_set_element directly.
	 */
	if (IsA(expr, SubscriptingRef))
	{
		SubscriptingRef *sbsref = (SubscriptingRef *) expr;

		/* Only handle single-dimension, non-slice subscripts */
		if (list_length(sbsref->refupperindexpr) == 1 &&
			sbsref->reflowerindexpr == NIL &&
			IsA(sbsref->refexpr, Param))
		{
			Param	   *array_param = (Param *) sbsref->refexpr;
			Expr	   *idx_expr = (Expr *) linitial(sbsref->refupperindexpr);
			ExprTypeClass idx_class;

			idx_class = uplpgsql_classify_expr(idx_expr);
			if (idx_class == EXPR_TYPE_INT4 &&
				array_param->paramkind == PARAM_EXTERN)
			{
				int array_dno = array_param->paramid - 1;
				UPLpgSQL_native_array *na = find_native_array(ctx, array_dno);

				estate_ref = LLVMGetParam(ctx->function, 0);

				if (na != NULL && sbsref->refassgnexpr != NULL)
				{
					/*
					 * Native array element write: arr[i] := val
					 *
					 * Compile both the subscript index and value expression
					 * to native types, then:
					 *   1. Inline bounds check (same pattern as read path)
					 *   2. GEP to element pointer (1-based → 0-based)
					 *   3. Store value directly
					 *
					 * This replaces array_set_element() which would
					 * detoast, copy, modify, and re-store the entire array.
					 */
					Expr		   *val_expr = sbsref->refassgnexpr;
					ExprTypeClass	val_class;
					LLVMValueRef	idx_val, val_result;
					LLVMValueRef	val_isnull = NULL;
					LLVMValueRef	data, len, idx0, gep;
					LLVMValueRef	lb, in_range;
					LLVMBasicBlockRef fast_bb, slow_bb, done_bb;
					LLVMTypeRef		i32_ty = ctx->types[UPLPGSQL_INT32];

					val_class = uplpgsql_classify_expr(val_expr);

					if (val_class != EXPR_TYPE_UNKNOWN)
					{
						elog(DEBUG1, "uplpgsql: native array set dno %d[idx]: %s",
							 array_dno, stmt->expr->query);

						emit_set_subscript_null_check(ctx, idx_expr,
													  estate_ref);
						idx_val = uplpgsql_compile_expr_datum(ctx, idx_expr,
															  estate_ref,
															  &idx_class);

						/*
						 * Every Tier 1 operator is strict, so the value is
						 * NULL exactly when one of its leaves is — and it
						 * must then not be computed at all, or a division or
						 * overflow check raises on garbage where PostgreSQL
						 * stores a NULL element.  See
						 * tier1_compile_value_guarded.
						 */
						val_result = tier1_compile_value_guarded(ctx, val_expr,
																 estate_ref,
																 &val_class,
																 &val_isnull);
					}
					else if (uplpgsql_can_fmgr_compile(val_expr))
					{
						/*
						 * Tier 2 value.  Tier 1 covers float8 arithmetic and
						 * the float8 intrinsics, but not a cast down to the
						 * element type — "a[i] := floor(x)::int" on an int[]
						 * lands here.  Without this the whole statement went
						 * to the interpreter, and because the target is a
						 * native array the assign path then reloaded every
						 * element from the Datum afterwards: O(n) per element
						 * write, so filling an array was quadratic.
						 *
						 * The value's type is the array's element type — the
						 * parser has already coerced refassgnexpr to it — so
						 * take the class from there rather than from
						 * classify_expr, which does not model the cast.
						 */
						val_class = elemtype_to_class(na->elemtype);
						if (val_class == EXPR_TYPE_UNKNOWN)
							goto standard_array_path;

						elog(DEBUG1, "uplpgsql: native array set dno %d[idx] "
							 "(fmgr bypass value): %s",
							 array_dno, stmt->expr->query);

						emit_set_subscript_null_check(ctx, idx_expr,
													  estate_ref);
						idx_val = uplpgsql_compile_expr_datum(ctx, idx_expr,
															  estate_ref,
															  &idx_class);
						val_result = uplpgsql_compile_expr_fmgr_full(ctx,
																	 val_expr,
																	 estate_ref,
																	 &val_isnull);
						if (val_result == NULL)
							goto standard_array_path;

						val_result = datum_to_native(ctx, val_result,
													 val_class);
					}
					else
						goto standard_array_path;

					/*
					 * A store into flat memory can only serve a subscript
					 * that is already in range.  PostgreSQL instead *extends*
					 * the array on an out-of-range assignment — a[6] := 9 on
					 * a 3-element array yields [1:6], and a[3] := 7 on a NULL
					 * array yields [3:3] — and raises "wrong number of array
					 * subscripts" for a single-subscript write to a 2-D value.
					 * None of that can be done in place.
					 *
					 * So: take the fast store when the subscript is in range
					 * and the value really is a 1-D array, and otherwise hand
					 * the write to array_set_element via the runtime helper,
					 * which implements all of the above.  The slow path syncs
					 * flat memory out to the Datum first (it is the live copy)
					 * and re-reads the result back in afterwards, so the array
					 * stays native — an extend just makes it bigger.
					 *
					 * The check mirrors the read path in tier1_expr_any_null:
					 * len < 0 means "not a 1-D array".
					 */
					len = LLVMBuildLoad2(ctx->builder, i32_ty,
										 na->len_ptr, "na.len");
					lb = LLVMBuildLoad2(ctx->builder, i32_ty,
										na->lb_ptr, "na.lb");

					in_range = LLVMBuildICmp(ctx->builder, LLVMIntSGE, len,
											 LLVMConstInt(i32_ty, 0, true),
											 "na.flat");
					in_range = LLVMBuildAnd(ctx->builder, in_range,
						LLVMBuildICmp(ctx->builder, LLVMIntSGE, idx_val, lb,
									  "na.ge.lb"),
						"na.set.lo");
					in_range = LLVMBuildAnd(ctx->builder, in_range,
						LLVMBuildICmp(ctx->builder, LLVMIntSLE, idx_val,
							LLVMBuildSub(ctx->builder,
								LLVMBuildAdd(ctx->builder, lb, len, "na.end"),
								LLVMConstInt(i32_ty, 1, false), "na.last"),
							"na.le.hi"),
						"na.set.ok");

					/*
					 * A NULL value cannot go through the flat store: there
					 * may be no null flags allocated to mark it in, and
					 * storing the Datum would silently write a value where
					 * PostgreSQL stores a NULL.  Send it to the slow path,
					 * which hands the write to array_set_element with
					 * valisnull set and re-reads the result.  Rare enough
					 * that its cost does not matter.
					 */
					if (val_isnull != NULL)
						in_range = LLVMBuildAnd(ctx->builder, in_range,
							LLVMBuildNot(ctx->builder, val_isnull,
										 "na.set.notnull"),
							"na.set.ok.nn");

					fast_bb = expr_append_block(ctx, "na.set.fast");
					slow_bb = expr_append_block(ctx, "na.set.slow");
					done_bb = expr_append_block(ctx, "na.set.done");

					LLVMBuildCondBr(ctx->builder, in_range, fast_bb, slow_bb);

					/* In range: store straight into flat memory. */
					LLVMPositionBuilderAtEnd(ctx->builder, fast_bb);
					data = LLVMBuildLoad2(ctx->builder, ctx->types[UPLPGSQL_PTR],
										  na->data_ptr, "na.data");
					idx0 = LLVMBuildSub(ctx->builder, idx_val, lb, "idx0");
					gep = LLVMBuildGEP2(ctx->builder, na->llvm_elemtype,
										data, &idx0, 1, "na.elem_ptr");
					LLVMBuildStore(ctx->builder, val_result, gep);

					/*
					 * The element now has a value, so clear its NULL flag —
					 * it may have been one of the gap elements a previous
					 * extend left behind.  Only if flags exist at all; a NULL
					 * pointer means nothing in the array is NULL.
					 */
					{
						LLVMValueRef		nulls, has;
						LLVMBasicBlockRef	clr_bb, after_bb;

						nulls = LLVMBuildLoad2(ctx->builder,
											   ctx->types[UPLPGSQL_PTR],
											   na->nulls_ptr, "na.nulls");
						has = LLVMBuildICmp(ctx->builder, LLVMIntNE, nulls,
											LLVMConstNull(ctx->types[UPLPGSQL_PTR]),
											"na.has.nulls");

						clr_bb = expr_append_block(ctx, "na.set.clrnull");
						after_bb = expr_append_block(ctx, "na.set.stored");
						LLVMBuildCondBr(ctx->builder, has, clr_bb, after_bb);

						LLVMPositionBuilderAtEnd(ctx->builder, clr_bb);
						{
							LLVMValueRef ngep;

							ngep = LLVMBuildGEP2(ctx->builder,
								ctx->types[UPLPGSQL_INT8], nulls, &idx0, 1,
								"na.null.ptr");
							LLVMBuildStore(ctx->builder,
								LLVMConstInt(ctx->types[UPLPGSQL_INT8], 0,
											 false),
								ngep);
						}
						LLVMBuildBr(ctx->builder, after_bb);

						LLVMPositionBuilderAtEnd(ctx->builder, after_bb);
					}
					LLVMBuildBr(ctx->builder, done_bb);

					/* Out of range, or not 1-D: let PostgreSQL do it. */
					LLVMPositionBuilderAtEnd(ctx->builder, slow_bb);
					uplpgsql_emit_sync_native_array(ctx, na);
					{
						ArrayTypeInfo	ati;
						LLVMValueRef	datum_val;

						datum_val = native_to_datum(ctx, val_result, val_class);
						resolve_array_type_info(ctx, array_dno, &ati);

						{
							LLVMValueRef args[] = {
								estate_ref,
								LLVMConstInt(i32_ty, array_dno, false),
								idx_val,
								datum_val,
								val_isnull != NULL ? val_isnull
									: LLVMConstInt(ctx->types[UPLPGSQL_INT1],
												   0, false),	/* valisnull */
								ati.typlen_val,
								ati.elemtype_val,
								ati.elmlen_val,
								ati.elmbyval_val,
								ati.elmalign_val
							};

							LLVMBuildCall2(ctx->builder,
								ctx->rt_fntypes[RT_ARRAY_SET_ELEMENT],
								ctx->rt_funcs[RT_ARRAY_SET_ELEMENT],
								args, 10, "");
						}
					}
					uplpgsql_emit_refresh_native_array(ctx, na);
					LLVMBuildBr(ctx->builder, done_bb);

					LLVMPositionBuilderAtEnd(ctx->builder, done_bb);
					return true;
				}

				if (na != NULL && sbsref->refassgnexpr == NULL)
				{
					/*
					 * Native array element read in assignment context:
					 *   x := arr[i]
					 *
					 * Similar to the expression-context read, but we also
					 * need to convert the native type (i32/i64/double) to
					 * a Datum and store it into the target variable via
					 * GEP into datums[varno]->value/isnull.
					 *
					 * Conversion: int4 → sext to i64 (Datum is 64-bit)
					 *             int8 → already i64
					 *             float8 → bitcast to i64
					 */
					LLVMValueRef idx_val, data, len, idx0, gep, elem;
					LLVMValueRef na_lb;
					LLVMBasicBlockRef null_bb, val_bb, get_done_bb;
					LLVMTypeRef  i32_ty = ctx->types[UPLPGSQL_INT32];

					elog(DEBUG1, "uplpgsql: native array get dno %d[idx] -> dno %d: %s",
						 array_dno, stmt->varno, stmt->expr->query);

					null_bb = expr_append_block(ctx, "na.get.null");
					get_done_bb = expr_append_block(ctx, "na.get.done");

					/*
					 * A NULL subscript reads as SQL NULL — and, as in the
					 * write path, the test has to run before the subscript's
					 * value is computed: a native array read used as the
					 * subscript is a raw flat load that is only safe once
					 * tier1_expr_any_null() has answered false for it (see
					 * emit_set_subscript_null_check).
					 */
					{
						LLVMValueRef idx_isnull;

						idx_isnull = tier1_expr_any_null(ctx, idx_expr,
														 estate_ref);
						if (idx_isnull != NULL)
						{
							LLVMBasicBlockRef idxok_bb;

							idxok_bb = expr_append_block(ctx, "na.get.idxok");
							LLVMBuildCondBr(ctx->builder, idx_isnull,
											null_bb, idxok_bb);
							LLVMPositionBuilderAtEnd(ctx->builder, idxok_bb);
						}
					}

					idx_val = uplpgsql_compile_expr_datum(ctx, idx_expr,
														  estate_ref,
														  &idx_class);

					len = LLVMBuildLoad2(ctx->builder, i32_ty,
										 na->len_ptr, "na.len");
					na_lb = LLVMBuildLoad2(ctx->builder, i32_ty,
										   na->lb_ptr, "na.lb");

					/*
					 * Out of range reads as SQL NULL, as it does in
					 * PostgreSQL — a[10] on a 3-element array, a[0], a NULL
					 * or empty array, or a single subscript on a
					 * multi-dimensional value (len < 0).  This used to raise.
					 */
					{
						LLVMValueRef	oob;

						oob = LLVMBuildICmp(ctx->builder, LLVMIntSLT, len,
											LLVMConstInt(i32_ty, 0, true),
											"na.notflat");
						oob = LLVMBuildOr(ctx->builder, oob,
							LLVMBuildICmp(ctx->builder, LLVMIntSLT, idx_val,
										  na_lb, "na.below"),
							"na.oob");
						oob = LLVMBuildOr(ctx->builder, oob,
							LLVMBuildICmp(ctx->builder, LLVMIntSGT, idx_val,
								LLVMBuildSub(ctx->builder,
									LLVMBuildAdd(ctx->builder, na_lb, len,
												 "na.end"),
									LLVMConstInt(i32_ty, 1, false), "na.last"),
								"na.above"),
							"na.oob2");

						val_bb = expr_append_block(ctx, "na.get.val");

						LLVMBuildCondBr(ctx->builder, oob, null_bb, val_bb);

						LLVMPositionBuilderAtEnd(ctx->builder, null_bb);
						uplpgsql_emit_store_var_null(ctx, estate_ref,
													 stmt->varno);
						LLVMBuildBr(ctx->builder, get_done_bb);

						LLVMPositionBuilderAtEnd(ctx->builder, val_bb);
					}

					data = LLVMBuildLoad2(ctx->builder, ctx->types[UPLPGSQL_PTR],
										  na->data_ptr, "na.data");
					idx0 = LLVMBuildSub(ctx->builder, idx_val, na_lb, "idx0");
					gep = LLVMBuildGEP2(ctx->builder, na->llvm_elemtype,
										data, &idx0, 1, "na.elem_ptr");
					elem = LLVMBuildLoad2(ctx->builder, na->llvm_elemtype,
										  gep, "na.elem");

					/* Convert native type to Datum and store */
					{
						LLVMValueRef datum_val;

						if (na->elemtype == INT4OID)
							datum_val = LLVMBuildSExt(ctx->builder, elem,
								ctx->types[UPLPGSQL_INT64], "na.datum");
						else if (na->elemtype == FLOAT8OID)
							datum_val = LLVMBuildBitCast(ctx->builder, elem,
								ctx->types[UPLPGSQL_INT64], "na.datum");
						else /* INT8OID */
							datum_val = elem;

						uplpgsql_emit_store_var_datum(ctx, estate_ref,
													  stmt->varno, datum_val);
					}
					LLVMBuildBr(ctx->builder, get_done_bb);

					LLVMPositionBuilderAtEnd(ctx->builder, get_done_bb);
					return true;
				}

standard_array_path:
				if (sbsref->refassgnexpr == NULL)
				{
					/*
					 * Array element read: x := arr[i]
					 */
					LLVMValueRef idx_val, isnull_ptr, elem_datum, elem_isnull;
					LLVMBasicBlockRef entry_bb;
					LLVMBasicBlockRef null_bb = NULL;
					LLVMBasicBlockRef done_bb = NULL;

					elog(DEBUG1, "uplpgsql: array get dno %d[idx] -> dno %d: %s",
						 array_dno, stmt->varno, stmt->expr->query);

					/*
					 * A NULL subscript reads as SQL NULL; consumed as a
					 * garbage index it could just as well land in range and
					 * fetch an arbitrary element.  The test runs before the
					 * subscript's value for the same reason as in the writes
					 * (see emit_set_subscript_null_check).
					 */
					{
						LLVMValueRef idx_isnull;

						idx_isnull = tier1_expr_any_null(ctx, idx_expr,
														 estate_ref);
						if (idx_isnull != NULL)
						{
							LLVMBasicBlockRef idxok_bb;

							null_bb = expr_append_block(ctx, "arr.get.null");
							done_bb = expr_append_block(ctx, "arr.get.done");
							idxok_bb = expr_append_block(ctx, "arr.get.idxok");

							LLVMBuildCondBr(ctx->builder, idx_isnull,
											null_bb, idxok_bb);

							LLVMPositionBuilderAtEnd(ctx->builder, null_bb);
							uplpgsql_emit_store_var_null(ctx, estate_ref,
														 stmt->varno);
							LLVMBuildBr(ctx->builder, done_bb);

							LLVMPositionBuilderAtEnd(ctx->builder, idxok_bb);
						}
					}

					idx_val = uplpgsql_compile_expr_datum(ctx, idx_expr,
														  estate_ref,
														  &idx_class);

					/* Alloca for isNull output (must be in entry block) */
					entry_bb = LLVMGetEntryBasicBlock(ctx->function);
					{
						LLVMBuilderRef tmp = LLVMCreateBuilderInContext(ctx->context);

						LLVMPositionBuilderBefore(tmp,
							LLVMGetFirstInstruction(entry_bb));
						isnull_ptr = LLVMBuildAlloca(tmp,
							ctx->types[UPLPGSQL_INT1], "arr_elem_isnull");
						LLVMDisposeBuilder(tmp);
					}

					{
						ArrayTypeInfo ati;

						resolve_array_type_info(ctx, array_dno, &ati);
						{
							LLVMValueRef args[] = {
								estate_ref,
								LLVMConstInt(ctx->types[UPLPGSQL_INT32],
											 array_dno, false),
								idx_val,
								ati.typlen_val,
								ati.elmlen_val,
								ati.elmbyval_val,
								ati.elmalign_val,
								isnull_ptr
							};
							elem_datum = LLVMBuildCall2(ctx->builder,
								ctx->rt_fntypes[RT_ARRAY_GET_ELEMENT],
								ctx->rt_funcs[RT_ARRAY_GET_ELEMENT],
								args, 8, "arr_elem");
						}
					}

					/*
					 * The element itself may be NULL — an out-of-range read,
					 * or a NULL element — and array_get_element reports that
					 * through isNull_out.  Storing the datum with isnull
					 * hardwired false turned those into 0.
					 */
					elem_isnull = LLVMBuildLoad2(ctx->builder,
												 ctx->types[UPLPGSQL_INT1],
												 isnull_ptr, "arr_elem_isnull");
					uplpgsql_emit_store_var_datum_isnull(ctx, estate_ref,
														 stmt->varno,
														 elem_datum,
														 elem_isnull);

					if (done_bb != NULL)
					{
						LLVMBuildBr(ctx->builder, done_bb);
						LLVMPositionBuilderAtEnd(ctx->builder, done_bb);
					}
					return true;
				}
				else
				{
					/*
					 * Array element write: arr[i] := val
					 *
					 * The value expression must also be compilable.
					 * Try Tier 1 first, then fall through to Tier 3.
					 */
					Expr	   *val_expr = sbsref->refassgnexpr;
					ExprTypeClass val_class;

					val_class = uplpgsql_classify_expr(val_expr);
					if (val_class != EXPR_TYPE_UNKNOWN)
					{
						LLVMValueRef idx_val, val_result, val_datum;
						LLVMValueRef val_isnull;

						elog(DEBUG1, "uplpgsql: array set dno %d[idx] := val: %s",
							 array_dno, stmt->expr->query);

						emit_set_subscript_null_check(ctx, idx_expr,
													  estate_ref);
						idx_val = uplpgsql_compile_expr_datum(ctx, idx_expr,
															  estate_ref,
															  &idx_class);

						/*
						 * Same rule as the native write above: a NULL value
						 * stores a NULL element and is never computed.
						 * Hardwiring isnull to false here stored 0 where
						 * PostgreSQL stores a NULL.
						 */
						val_result = tier1_compile_value_guarded(ctx, val_expr,
																 estate_ref,
																 &val_class,
																 &val_isnull);
						val_datum = native_to_datum(ctx, val_result, val_class);
						if (val_isnull == NULL)
							val_isnull = LLVMConstInt(ctx->types[UPLPGSQL_INT1],
													  0, false);

						{
							ArrayTypeInfo ati;

							resolve_array_type_info(ctx, array_dno, &ati);
							{
								LLVMValueRef args[] = {
									estate_ref,
									LLVMConstInt(ctx->types[UPLPGSQL_INT32],
												 array_dno, false),
									idx_val,
									val_datum,
									val_isnull,
									ati.typlen_val,
									ati.elemtype_val,
									ati.elmlen_val,
									ati.elmbyval_val,
									ati.elmalign_val
								};
								LLVMBuildCall2(ctx->builder,
									ctx->rt_fntypes[RT_ARRAY_SET_ELEMENT],
									ctx->rt_funcs[RT_ARRAY_SET_ELEMENT],
									args, 10, "");
							}
						}
						return true;
					}
				}
			}
		}
	}

	/*
	 * Could not inline this expression.  The caller
	 * (uplpgsql_compile_assign) clears the plan we created at compile time
	 * so that the runtime path (exec_assign_expr) can re-prepare it with
	 * exec_simple_check_plan, enabling the fast "simple expression"
	 * evaluation path.  It cannot be cleared here: the caller first decides
	 * which native arrays the fallback must marshal, and a surviving plan is
	 * its evidence that paramnos reflects a completed parse analysis.
	 */
	return false;
}

/*
 * Try to compile a boolean expression as native comparisons or fmgr bypass.
 *
 * Tier 1: native LLVM icmp/fcmp for known int/float comparisons
 * Tier 2: direct PG function call, result truncated to i1
 *
 * Returns true if inlined (result in *result_out), false → use runtime helper.
 */
bool
uplpgsql_try_compile_bool(UPLpgSQL_compile_ctx *ctx,
						  UPLpgSQL_expr *expr_node,
						  LLVMValueRef *result_out)
{
	Expr		   *expr;
	ExprTypeClass	tc;
	LLVMValueRef	estate_ref;

	/*
	 * Some conditions must not be planned at compile time — see
	 * ctx->defer_cond_plan.  Decline before prepare_and_get_expr() so no plan
	 * is built at all; the caller then emits the runtime evaluator, which
	 * prepares the plan on first execution, by which point the operand types
	 * are settled.
	 */
	if (ctx->defer_cond_plan)
		return false;

	expr = uplpgsql_prepare_and_get_expr(ctx, expr_node);
	if (expr == NULL)
		return false;

	/* Tier 1: native LLVM comparisons */
	tc = uplpgsql_classify_expr(expr);
	if (tc == EXPR_TYPE_BOOL)
	{
		LLVMValueRef	any_null, val;

		elog(DEBUG1, "uplpgsql: inlining bool expression: %s", expr_node->query);

		estate_ref = LLVMGetParam(ctx->function, 0);
		any_null = tier1_expr_any_null(ctx, expr, estate_ref);

		if (any_null == NULL)
		{
			*result_out = uplpgsql_compile_expr_bool(ctx, expr, estate_ref);
			return true;
		}

		/*
		 * A NULL operand makes the condition NULL, and callers (IF, WHILE,
		 * EXIT WHEN) treat NULL as not-true, so the result is false.  As in
		 * the assignment path, branch around the comparison rather than
		 * computing it on a NULL variable's datum.
		 */
		{
			LLVMBasicBlockRef	compute_bb, null_bb, merge_bb, from_bb;
			LLVMValueRef		phi;
			LLVMValueRef		vals[2];
			LLVMBasicBlockRef	blocks[2];

			compute_bb = LLVMAppendBasicBlockInContext(ctx->context,
													   ctx->function,
													   "t1b.notnull");
			null_bb = LLVMAppendBasicBlockInContext(ctx->context,
													ctx->function, "t1b.null");
			merge_bb = LLVMAppendBasicBlockInContext(ctx->context,
													 ctx->function,
													 "t1b.done");

			LLVMBuildCondBr(ctx->builder, any_null, null_bb, compute_bb);

			LLVMPositionBuilderAtEnd(ctx->builder, compute_bb);
			val = uplpgsql_compile_expr_bool(ctx, expr, estate_ref);
			/* the comparison may have added blocks; branch from the current one */
			from_bb = LLVMGetInsertBlock(ctx->builder);
			LLVMBuildBr(ctx->builder, merge_bb);

			LLVMPositionBuilderAtEnd(ctx->builder, null_bb);
			LLVMBuildBr(ctx->builder, merge_bb);

			LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);
			phi = LLVMBuildPhi(ctx->builder, ctx->types[UPLPGSQL_INT1],
							   "t1b.result");
			vals[0] = val;
			blocks[0] = from_bb;
			vals[1] = LLVMConstInt(ctx->types[UPLPGSQL_INT1], 0, false);
			blocks[1] = null_bb;
			LLVMAddIncoming(phi, vals, blocks, 2);

			*result_out = phi;
		}
		return true;
	}

	/* Tier 2: fmgr bypass — result is Datum, truncate to i1 */
	if (uplpgsql_can_fmgr_compile(expr))
	{
		LLVMValueRef	datum_result;
		LLVMValueRef	isnull_val = NULL;
		bool			scoped = fmgr_expr_allocates(expr);
		LLVMValueRef	old = NULL;

		elog(DEBUG1, "uplpgsql: fmgr bypass bool expression: %s",
			 expr_node->query);

		estate_ref = LLVMGetParam(ctx->function, 0);

		/*
		 * A condition can allocate through an intermediate too --
		 * "length(upper(t)) > 3" builds a text every time.  The bool result
		 * is by value, so bracket the evaluation and reset the scope once the
		 * i1 is in hand.  A comparison of plain values allocates nothing and
		 * is not scoped.
		 */
		if (scoped)
		{
			LLVMValueRef a[] = { estate_ref };

			old = LLVMBuildCall2(ctx->builder,
				ctx->rt_fntypes[RT_ALLOC_SCOPE_ENTER],
				ctx->rt_funcs[RT_ALLOC_SCOPE_ENTER], a, 1, "scope.old");
		}

		datum_result = uplpgsql_compile_expr_fmgr_full(ctx, expr, estate_ref,
													   &isnull_val);
		if (datum_result != NULL)
		{
			LLVMValueRef	val;

			val = LLVMBuildTrunc(ctx->builder, datum_result,
								 ctx->types[UPLPGSQL_INT1],
								 "fmgr.bool.result");

			/*
			 * IF/WHILE/EXIT WHEN treat a NULL condition as not-true, so fold
			 * nullness in rather than dropping it: NOT NULL evaluates to a
			 * true datum with isnull set, and without this it would take the
			 * THEN branch.
			 */
			*result_out = LLVMBuildAnd(ctx->builder, val,
									   LLVMBuildNot(ctx->builder, isnull_val,
													"fmgr.bool.notnull"),
									   "fmgr.bool.cond");
			if (scoped)
			{
				LLVMValueRef a[] = { estate_ref, old };

				LLVMBuildCall2(ctx->builder,
					ctx->rt_fntypes[RT_ALLOC_SCOPE_EXIT],
					ctx->rt_funcs[RT_ALLOC_SCOPE_EXIT], a, 2, "");
			}
			return true;
		}
		/* fmgr_full bailed: restore the context before Tier 3 */
		if (scoped)
		{
			LLVMValueRef a[] = { estate_ref, old };

			LLVMBuildCall2(ctx->builder,
				ctx->rt_fntypes[RT_ALLOC_SCOPE_EXIT],
				ctx->rt_funcs[RT_ALLOC_SCOPE_EXIT], a, 2, "");
		}
		/* Fall through to Tier 3 */
	}

	/*
	 * Could not inline — clear the compile-time plan so the runtime path
	 * can re-prepare with exec_simple_check_plan (see comment in
	 * uplpgsql_try_compile_assign for details).
	 */
	if (expr_node->plan != NULL)
	{
		SPI_freeplan(expr_node->plan);
		expr_node->plan = NULL;
	}

	/*
	 * Returning false makes the caller emit RT_EVAL_BOOL, which evaluates
	 * this condition in the interpreter and reads variables as PG Datums.
	 * Sync native arrays first, or a condition over one (IF array_length(x,1)
	 * = 3) sees the stale Datum instead of the live flat memory.
	 */
	{
		int		na_i;

		for (na_i = 0; na_i < ctx_num_native_arrays(ctx); na_i++)
			uplpgsql_emit_sync_native_array(ctx,
											&ctx_native_arrays(ctx)[na_i]);
	}

	return false;
}
