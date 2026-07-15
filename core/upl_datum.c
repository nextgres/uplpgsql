/*-------------------------------------------------------------------------
 *
 * upl_datum.c
 *		Parameterized datum load/store operations via GEP.
 *
 *		Emits LLVM IR to access variable values (Datum), isnull flags,
 *		and freeval flags inside language-specific exec state structs.
 *		All struct offsets are parameterized via ctx->datum_offsets, so
 *		this code works identically for PL/pgSQL, SQL/PSM, PL/SQL, and
 *		T-SQL — each driver just fills in its own offsetof() values.
 *
 *		GEP chain (byte-addressed via i8 GEPs):
 *
 *		  estate_ref  →  [+estate_to_lang_state]  →  load ptr (lang_state)
 *		  lang_state  →  [+lang_state_to_datums]  →  load ptr (datums)
 *		  datums      →  [dno * sizeof(ptr)]      →  load ptr (datum)
 *		  datum       →  [+var_to_value]           →  load/store i64
 *		  datum       →  [+var_to_isnull]          →  load/store i8
 *		  datum       →  [+var_to_freeval]         →  store i8
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

#include "upl.h"

/*
 * Navigate the GEP chain from estate_ref down to the individual datum
 * pointer for variable number dno.  This is the common prefix shared by
 * all three public functions.
 *
 * Returns the datum pointer (opaque ptr to the language's variable struct).
 * Also sets *builder_out for convenience (callers always need it).
 */
static LLVMValueRef
emit_datum_ptr(UPL_compile_ctx *ctx, LLVMValueRef estate_ref, int dno,
			   const char *prefix)
{
	LLVMBuilderRef		builder = ctx->builder;
	UPL_datum_offsets  *offsets = &ctx->datum_offsets;
	LLVMTypeRef			i8 = ctx->types[UPL_INT8];
	LLVMTypeRef			i64 = ctx->types[UPL_INT64];
	LLVMTypeRef			ptr = ctx->types[UPL_PTR];
	LLVMValueRef		off, gep, lang_state, datums, datum;
	char				nbuf[64];

	/*
	 * prefix ("ld"/"st"/"in") distinguishes the load, store and init chains in
	 * the emitted IR, which matters once uplpgsql.dump_ir is on — otherwise
	 * every chain produces identically-named values.
	 */
#define UPL_DNAME(suffix) \
	(snprintf(nbuf, sizeof(nbuf), "%s." suffix, prefix), nbuf)

	/* estate → lang_state (byte-offset GEP + pointer load) */
	off = LLVMConstInt(i64, offsets->estate_to_lang_state, false);
	gep = LLVMBuildGEP2(builder, i8, estate_ref, &off, 1,
						 UPL_DNAME("lang_state.ptr"));
	lang_state = LLVMBuildLoad2(builder, ptr, gep, UPL_DNAME("lang_state"));

	/* lang_state → datums array pointer */
	off = LLVMConstInt(i64, offsets->lang_state_to_datums, false);
	gep = LLVMBuildGEP2(builder, i8, lang_state, &off, 1,
						 UPL_DNAME("datums.ptr"));
	datums = LLVMBuildLoad2(builder, ptr, gep, UPL_DNAME("datums"));

	/* datums[dno] → individual datum pointer */
	off = LLVMConstInt(i64, dno, false);
	gep = LLVMBuildGEP2(builder, ptr, datums, &off, 1, UPL_DNAME("datum.slot"));
	datum = LLVMBuildLoad2(builder, ptr, gep, UPL_DNAME("datum"));

#undef UPL_DNAME

	return datum;
}

/*
 * upl_emit_load_var_datum
 *
 * Emit IR to load a plain variable's Datum value (i64) via the
 * parameterized GEP chain.
 *
 * This handles only DTYPE_VAR (plain scalar variables).  Language-specific
 * datum types (RECFIELD, PROMISE, etc.) must be handled by the driver's
 * load_param_datum callback before falling through to this function.
 */
LLVMValueRef
upl_emit_load_var_datum(UPL_compile_ctx *ctx,
						LLVMValueRef estate_ref, int dno)
{
	LLVMBuilderRef		builder = ctx->builder;
	UPL_datum_offsets  *offsets = &ctx->datum_offsets;
	LLVMTypeRef			i8 = ctx->types[UPL_INT8];
	LLVMTypeRef			i64 = ctx->types[UPL_INT64];
	LLVMValueRef		datum, off, gep;

	datum = emit_datum_ptr(ctx, estate_ref, dno, "ld");

	/* datum->value (Datum, i64) */
	off = LLVMConstInt(i64, offsets->var_to_value, false);
	gep = LLVMBuildGEP2(builder, i8, datum, &off, 1, "value.ptr");
	return LLVMBuildLoad2(builder, i64, gep, "var.datum");
}

/*
 * upl_emit_store_var_datum
 *
 * Emit IR to store a Datum (i64) into a plain variable.
 * Also clears the isnull flag (set to 0) and the freeval flag (set to 0).
 *
 * This is the "assign a known non-null value" path.  If the caller needs
 * to set isnull=true, it should use the RT_ASSIGN_NULL runtime helper
 * instead (which also handles freeing the old value if freeval was set).
 */
void
upl_emit_store_var_datum(UPL_compile_ctx *ctx,
						 LLVMValueRef estate_ref, int dno,
						 LLVMValueRef datum_val)
{
	LLVMBuilderRef		builder = ctx->builder;
	UPL_datum_offsets  *offsets = &ctx->datum_offsets;
	LLVMTypeRef			i8 = ctx->types[UPL_INT8];
	LLVMTypeRef			i64 = ctx->types[UPL_INT64];
	LLVMValueRef		datum, off, gep;
	LLVMValueRef		zero_i8;

	datum = emit_datum_ptr(ctx, estate_ref, dno, "st");
	zero_i8 = LLVMConstInt(i8, 0, false);

	/* datum->value = datum_val */
	off = LLVMConstInt(i64, offsets->var_to_value, false);
	gep = LLVMBuildGEP2(builder, i8, datum, &off, 1, "st.value.ptr");
	LLVMBuildStore(builder, datum_val, gep);

	/* datum->isnull = false */
	off = LLVMConstInt(i64, offsets->var_to_isnull, false);
	gep = LLVMBuildGEP2(builder, i8, datum, &off, 1, "st.isnull.ptr");
	LLVMBuildStore(builder, zero_i8, gep);

	/* datum->freeval = false */
	off = LLVMConstInt(i64, offsets->var_to_freeval, false);
	gep = LLVMBuildGEP2(builder, i8, datum, &off, 1, "st.freeval.ptr");
	LLVMBuildStore(builder, zero_i8, gep);
}

/*
 * upl_emit_load_var_isnull
 *
 * Emit IR to load a plain variable's isnull flag as an i1 (bool).
 *
 * The isnull field in the datum struct is stored as an i8 (C bool), so
 * we load it as i8 and truncate to i1 for use in LLVM branch conditions.
 *
 * Like load_var_datum, this handles only plain variables.  RECFIELD and
 * PROMISE types are handled by the driver's load_param_isnull callback.
 */
LLVMValueRef
upl_emit_load_var_isnull(UPL_compile_ctx *ctx,
						 LLVMValueRef estate_ref, int dno)
{
	LLVMBuilderRef		builder = ctx->builder;
	UPL_datum_offsets  *offsets = &ctx->datum_offsets;
	LLVMTypeRef			i1 = ctx->types[UPL_INT1];
	LLVMTypeRef			i8 = ctx->types[UPL_INT8];
	LLVMTypeRef			i64 = ctx->types[UPL_INT64];
	LLVMValueRef		datum, off, gep, isnull_raw;

	datum = emit_datum_ptr(ctx, estate_ref, dno, "in");

	/* datum->isnull (i8, truncated to i1) */
	off = LLVMConstInt(i64, offsets->var_to_isnull, false);
	gep = LLVMBuildGEP2(builder, i8, datum, &off, 1, "isnull.ptr");
	isnull_raw = LLVMBuildLoad2(builder, i8, gep, "isnull.raw");
	return LLVMBuildTrunc(builder, isnull_raw, i1, "isnull");
}
