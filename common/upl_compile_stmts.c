/*-------------------------------------------------------------------------
 *
 * uplpgsql_compile.c
 *		AST-to-LLVM-IR compilation pipeline.
 *
 *		This file is the core of the JIT compiler.  It contains:
 *
 *		1. JIT Heuristic (uplpgsql_should_jit / uplpgsql_jit_score_*)
 *		   Walks the AST and scores each statement.  Loops score positive
 *		   (amplified by depth), SPI-dominated statements score negative.
 *		   Returns true if score > 0.
 *
 *		2. Main Compilation Entry (uplpgsql_compile_function)
 *		   Creates LLVM context/module/builder, registers types and runtime
 *		   functions, compiles the function body, verifies, optimizes (O3),
 *		   and hands the module to OrcJIT via bitcode serialization.
 *
 *		3. Statement Compilation (uplpgsql_compile_*)
 *		   One function per PL/pgSQL statement type.  Control flow (IF, WHILE,
 *		   LOOP, FOR, CASE, EXIT/CONTINUE) is compiled to native LLVM basic
 *		   blocks and branches.  SPI-dependent operations delegate to runtime
 *		   helpers or directly to forked executor functions via embedded
 *		   function pointers.
 *
 *		4. Runtime Function Registration (uplpgsql_register_runtime_funcs)
 *		   Declares all uplpgsql_rt_* functions in the LLVM module so they
 *		   can be called from generated IR.  OrcJIT resolves them at link
 *		   time via the process symbol search generator.
 *
 *		5. Loop Stack Management (push/pop/find)
 *		   Tracks active loops for EXIT/CONTINUE target resolution.
 *
 *		Helper utilities:
 *		  - llvm_const_int32/llvm_const_ptr: create LLVM constant values
 *		  - uplpgsql_append_block: append a basic block to the current function
 *		  - uplpgsql_call_fn: call a registered runtime function
 *		  - uplpgsql_call_exec: call an exec_* function directly via embedded
 *		    pointer (bypasses the uplpgsql_rt_* wrapper layer)
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

#include "catalog/pg_type_d.h"
#include "executor/spi_priv.h"
#include "utils/expandedrecord.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/plancache.h"

/*
 * Convenience macros for accessing PL/pgSQL-specific lang_data fields
 * from the UPL_compile_ctx (which is typedef'd as UPLpgSQL_compile_ctx).
 */
#define ctx_lang(ctx)			UPLPGSQL_LANG_DATA(ctx)
#define ctx_plstate(ctx)		(ctx_lang(ctx)->plstate_ref)
#define ctx_func(ctx)			(ctx_lang(ctx)->uplpgsql_func)
#define ctx_native_arrays(ctx)	(ctx_lang(ctx)->native_arrays)
#define ctx_num_native_arrays(ctx) (ctx_lang(ctx)->num_native_arrays)

/* Forward declarations */
static void uplpgsql_register_runtime_funcs(UPLpgSQL_compile_ctx *ctx);
static void uplpgsql_compile_stmts(UPLpgSQL_compile_ctx *ctx,
								   List *stmts);
static void uplpgsql_compile_stmt(UPLpgSQL_compile_ctx *ctx,
								  UPLpgSQL_stmt *stmt);
static void uplpgsql_compile_block(UPLpgSQL_compile_ctx *ctx,
								   UPLpgSQL_stmt_block *stmt);
static void uplpgsql_compile_return(UPLpgSQL_compile_ctx *ctx,
									UPLpgSQL_stmt_return *stmt);
static void uplpgsql_compile_assign(UPLpgSQL_compile_ctx *ctx,
									UPLpgSQL_stmt_assign *stmt);
static void uplpgsql_compile_if(UPLpgSQL_compile_ctx *ctx,
								UPLpgSQL_stmt_if *stmt);
static void uplpgsql_compile_while(UPLpgSQL_compile_ctx *ctx,
								   UPLpgSQL_stmt_while *stmt);
static void uplpgsql_compile_loop(UPLpgSQL_compile_ctx *ctx,
								  UPLpgSQL_stmt_loop *stmt);
static void uplpgsql_compile_fori(UPLpgSQL_compile_ctx *ctx,
								  UPLpgSQL_stmt_fori *stmt);
static void uplpgsql_compile_exit(UPLpgSQL_compile_ctx *ctx,
								  UPLpgSQL_stmt_exit *stmt);
static void uplpgsql_compile_perform(UPLpgSQL_compile_ctx *ctx,
									  UPLpgSQL_stmt_perform *stmt);
static void uplpgsql_compile_execsql(UPLpgSQL_compile_ctx *ctx,
									 UPLpgSQL_stmt_execsql *stmt);
static void uplpgsql_compile_raise(UPLpgSQL_compile_ctx *ctx,
								   UPLpgSQL_stmt_raise *stmt);
static void uplpgsql_compile_case(UPLpgSQL_compile_ctx *ctx,
								  UPLpgSQL_stmt_case *stmt);
static void uplpgsql_compile_assert(UPLpgSQL_compile_ctx *ctx,
									UPLpgSQL_stmt_assert *stmt);
static void uplpgsql_compile_open(UPLpgSQL_compile_ctx *ctx,
								  UPLpgSQL_stmt_open *stmt);
static void uplpgsql_compile_fetch(UPLpgSQL_compile_ctx *ctx,
								   UPLpgSQL_stmt_fetch *stmt);
static void uplpgsql_compile_close(UPLpgSQL_compile_ctx *ctx,
								   UPLpgSQL_stmt_close *stmt);
static void uplpgsql_compile_fors(UPLpgSQL_compile_ctx *ctx,
								  UPLpgSQL_stmt_fors *stmt);
static void uplpgsql_compile_forc(UPLpgSQL_compile_ctx *ctx,
								  UPLpgSQL_stmt_forc *stmt);
static void uplpgsql_compile_dynexecute(UPLpgSQL_compile_ctx *ctx,
										UPLpgSQL_stmt_dynexecute *stmt);
static void uplpgsql_compile_dynfors(UPLpgSQL_compile_ctx *ctx,
									 UPLpgSQL_stmt_dynfors *stmt);
static void uplpgsql_compile_foreach_a(UPLpgSQL_compile_ctx *ctx,
									   UPLpgSQL_stmt_foreach_a *stmt);
static void uplpgsql_compile_return_next(UPLpgSQL_compile_ctx *ctx,
										 UPLpgSQL_stmt_return_next *stmt);
static void uplpgsql_compile_return_query(UPLpgSQL_compile_ctx *ctx,
										  UPLpgSQL_stmt_return_query *stmt);
static void uplpgsql_compile_call(UPLpgSQL_compile_ctx *ctx,
								  UPLpgSQL_stmt_call *stmt);
static void uplpgsql_compile_getdiag(UPLpgSQL_compile_ctx *ctx,
									 UPLpgSQL_stmt_getdiag *stmt);
static void uplpgsql_compile_commit(UPLpgSQL_compile_ctx *ctx,
									UPLpgSQL_stmt_commit *stmt);
static void uplpgsql_compile_rollback(UPLpgSQL_compile_ctx *ctx,
									  UPLpgSQL_stmt_rollback *stmt);

/* Native array analysis (defined later, needed by setup_entry hook) */
static void uplpgsql_analyze_native_arrays(UPLpgSQL_compile_ctx *ctx,
										   UPLpgSQL_function *func);

/* Native array sync (defined later, needed by uplpgsql_call_exec) */
static void uplpgsql_sync_native_arrays(UPLpgSQL_compile_ctx *ctx);
static void uplpgsql_sync_native_arrays_for_expr(UPLpgSQL_compile_ctx *ctx,
												 UPLpgSQL_expr *expr);

/* Native array lookup (defined later, needed by uplpgsql_emit_init_vars) */
static UPLpgSQL_native_array *uplpgsql_find_native_array(UPLpgSQL_compile_ctx *ctx,
														 int dno);

/* Core compilation callbacks */
static void uplpgsql_cb_compile_stmts(UPL_compile_ctx *ctx, void *stmts);
static bool uplpgsql_cb_try_compile_bool(UPL_compile_ctx *ctx, void *expr,
										 LLVMValueRef *result_out);
static void uplpgsql_cb_assign_expr(UPL_compile_ctx *ctx, int varno,
									void *expr);

/* Compilation pipeline hooks */
static void uplpgsql_setup_entry(UPL_compile_ctx *ctx);
static void uplpgsql_compile_body(UPL_compile_ctx *ctx);
static void uplpgsql_compile_block_exceptions(UPL_compile_ctx *ctx,
											  void *exception_data);

/* Helper to create LLVM constant values */
static inline LLVMValueRef
llvm_const_int32(UPLpgSQL_compile_ctx *ctx, int32 val)
{
	return LLVMConstInt(ctx->types[UPLPGSQL_INT32], val, false);
}

static inline LLVMValueRef
llvm_const_ptr(UPLpgSQL_compile_ctx *ctx, void *ptr)
{
	return LLVMConstIntToPtr(
		LLVMConstInt(ctx->types[UPLPGSQL_INT64], (uintptr_t) ptr, false),
		ctx->types[UPLPGSQL_PTR]);
}

static inline LLVMBasicBlockRef
uplpgsql_append_block(UPLpgSQL_compile_ctx *ctx, const char *name)
{
	return LLVMAppendBasicBlockInContext(ctx->context, ctx->function, name);
}

/* Call a registered runtime function */
static inline LLVMValueRef
uplpgsql_call_fn(UPLpgSQL_compile_ctx *ctx, UPLpgSQL_rt_func which,
				 LLVMValueRef *args, unsigned count)
{
	return LLVMBuildCall2(ctx->builder,
						  ctx->rt_fntypes[which],
						  ctx->rt_funcs[which],
						  args, count, "");
}

/*
 * Call an exec_* function directly via embedded function pointer.
 *
 * This is a key optimization for "thin" runtime wrappers.  Many
 * uplpgsql_rt_* functions do nothing but extract plstate from estate
 * and forward to exec_stmt_*.  Instead of calling through the wrapper,
 * we embed the exec_stmt_* function's C address as an LLVM constant
 * and call it directly with (plstate, stmt) arguments.
 *
 * This saves one function call overhead per statement execution and
 * allows LLVM's optimizer to reason about the call more effectively.
 *
 * The function pointer is resolved at JIT compile time via C's normal
 * symbol resolution (the exec_* functions are linked into our .so).
 * The resulting native code contains a direct call instruction to the
 * known address.
 *
 * For i32-returning functions (exec_stmt_*): returns i32.
 * For void-returning functions: returns NULL LLVMValueRef.
 *
 * Native arrays are synced to their PG Datums first.  Every exec_* callee
 * reads variables as Datums, and a native array's Datum is stale between
 * escapes, so anything reaching the interpreter must see marshalled data —
 * RAISE, FOREACH, EXECUTE, PERFORM, cursors, and the rest.  Syncing at this
 * one point covers them all; the alternative is remembering to do it in each
 * of the two dozen callers, which is how RAISE/FOREACH/EXECUTE were missed.
 * So this stays the default: correct for any callee, including ones whose
 * variable reads are not visible here.
 *
 * The wasted marshal it costs a callee that reads no arrays is not always
 * acceptable, though.  It is proportional to the size of every native array
 * in the function, and it is paid every time the statement runs, so a single
 * uncompilable expression in a hot loop -- "p := greatest(a,b,c)", which is
 * a MinMaxExpr and reaches neither tier -- marshals megabytes per iteration
 * for arrays it never touches.  Where the statement's expression is known,
 * uplpgsql_call_exec_nosync() plus uplpgsql_sync_native_arrays_for_expr()
 * syncs only what that expression actually reads.
 */
static inline LLVMValueRef
uplpgsql_call_exec_nosync(UPLpgSQL_compile_ctx *ctx, void *fn_addr,
						  LLVMTypeRef ret_type,
						  LLVMValueRef *args, unsigned count)
{
	LLVMTypeRef		param_types[4];
	LLVMTypeRef		fn_type;
	LLVMValueRef	fn_ptr;
	unsigned		i;
	const char	   *call_name;

	for (i = 0; i < count && i < 4; i++)
		param_types[i] = LLVMTypeOf(args[i]);

	fn_type = LLVMFunctionType(ret_type, param_types, count, false);
	fn_ptr = LLVMConstIntToPtr(
		LLVMConstInt(ctx->types[UPLPGSQL_INT64], (uintptr_t) fn_addr, false),
		LLVMPointerType(fn_type, 0));

	call_name = (ret_type == ctx->types[UPLPGSQL_VOID]) ? "" : "exec.rc";

	return LLVMBuildCall2(ctx->builder, fn_type, fn_ptr,
						  args, count, call_name);
}

/*
 * Call an exec_* function, marshalling every native array first.
 *
 * The safe default, and what every caller without a known expression uses.
 */
static inline LLVMValueRef
uplpgsql_call_exec(UPLpgSQL_compile_ctx *ctx, void *fn_addr,
				   LLVMTypeRef ret_type,
				   LLVMValueRef *args, unsigned count)
{
	uplpgsql_sync_native_arrays(ctx);

	return uplpgsql_call_exec_nosync(ctx, fn_addr, ret_type, args, count);
}

/* ----------------------------------------------------------------
 * Core compilation callbacks
 *
 * These are the UPL_callbacks implementations that the core engine
 * calls when compiling control flow (IF/WHILE/LOOP/FOR/CASE/BLOCK).
 * They bridge from the core's opaque void* parameters to the
 * PL/pgSQL-specific types.
 * ----------------------------------------------------------------
 */

/* Callback: compile a statement list */
static void
uplpgsql_cb_compile_stmts(UPL_compile_ctx *ctx, void *stmts)
{
	uplpgsql_compile_stmts(ctx, (List *) stmts);
}

/* Callback: try to compile a boolean expression natively */
static bool
uplpgsql_cb_try_compile_bool(UPL_compile_ctx *ctx, void *expr,
							 LLVMValueRef *result_out)
{
	return uplpgsql_try_compile_bool(ctx, (UPLpgSQL_expr *) expr, result_out);
}

/* Callback: assign expression to variable (for CASE test expr) */
static void
uplpgsql_cb_assign_expr(UPL_compile_ctx *ctx, int varno, void *expr)
{
	/*
	 * Go through uplpgsql_rt_case_assign_test rather than calling
	 * exec_assign_expr directly: a simple CASE's temporary is an INT4
	 * placeholder until the runtime retypes it to whatever the test
	 * expression actually is, exactly as exec_stmt_case does.  Without that,
	 * CASE over anything but an integer fails to coerce.
	 */
	LLVMValueRef args[3];

	args[0] = LLVMGetParam(ctx->function, 0);
	args[1] = upl_const_int32(ctx, varno);
	args[2] = upl_const_ptr(ctx, expr);

	uplpgsql_call_exec(ctx, (void *) uplpgsql_rt_case_assign_test,
					   ctx->types[UPLPGSQL_VOID], args, 3);
}

/* ----------------------------------------------------------------
 * Compilation pipeline hooks
 *
 * These implement UPL_compile_hooks for the PL/pgSQL driver.
 * Called by upl_compile_function() during the compilation pipeline.
 * ----------------------------------------------------------------
 */

/* Hook: driver-specific entry setup */
static void
uplpgsql_setup_entry(UPL_compile_ctx *ctx)
{
	UPLpgSQL_function *func = ctx_func(ctx);
	LLVMValueRef off, gep;

	/* Load plstate = estate->uplpgsql_estate */
	off = LLVMConstInt(ctx->types[UPLPGSQL_INT64],
					   offsetof(UPLpgSQL_exec_state, uplpgsql_estate), false);
	gep = LLVMBuildGEP2(ctx->builder, ctx->types[UPLPGSQL_INT8],
						ctx->estate_ref, &off, 1, "plstate.ptr");
	ctx_plstate(ctx) = LLVMBuildLoad2(ctx->builder, ctx->types[UPLPGSQL_PTR],
									  gep, "plstate");

	/* Native array analysis + allocas */
	uplpgsql_analyze_native_arrays(ctx, func);
	{
		int na_i;

		for (na_i = 0; na_i < ctx_num_native_arrays(ctx); na_i++)
		{
			UPLpgSQL_native_array *na = &ctx_native_arrays(ctx)[na_i];
			char name[64];

			if (na->elemtype == INT4OID)
				na->llvm_elemtype = ctx->types[UPLPGSQL_INT32];
			else if (na->elemtype == INT8OID)
				na->llvm_elemtype = ctx->types[UPLPGSQL_INT64];
			else
				na->llvm_elemtype = ctx->types[UPLPGSQL_DOUBLE];

			snprintf(name, sizeof(name), "na%d.data", na->dno);
			na->data_ptr = LLVMBuildAlloca(ctx->builder,
										   ctx->types[UPLPGSQL_PTR], name);
			LLVMBuildStore(ctx->builder,
						   LLVMConstNull(ctx->types[UPLPGSQL_PTR]),
						   na->data_ptr);

			snprintf(name, sizeof(name), "na%d.len", na->dno);
			na->len_ptr = LLVMBuildAlloca(ctx->builder,
										  ctx->types[UPLPGSQL_INT32], name);
			LLVMBuildStore(ctx->builder, llvm_const_int32(ctx, 0),
						   na->len_ptr);

			/*
			 * Lower bound.  1 until something says otherwise; from_datum
			 * reports the real one for an array that has a different base.
			 */
			snprintf(name, sizeof(name), "na%d.lb", na->dno);
			na->lb_ptr = LLVMBuildAlloca(ctx->builder,
										 ctx->types[UPLPGSQL_INT32], name);
			LLVMBuildStore(ctx->builder, llvm_const_int32(ctx, 1),
						   na->lb_ptr);

			/* Per-element NULL flags; NULL pointer means "no element is" */
			snprintf(name, sizeof(name), "na%d.nulls", na->dno);
			na->nulls_ptr = LLVMBuildAlloca(ctx->builder,
											ctx->types[UPLPGSQL_PTR], name);
			LLVMBuildStore(ctx->builder,
						   LLVMConstNull(ctx->types[UPLPGSQL_PTR]),
						   na->nulls_ptr);

			/*
			 * Allocated capacity of data, in elements.  An append bumps len
			 * up to this; past it the buffers grow through
			 * uplpgsql_rt_native_array_reserve.
			 */
			snprintf(name, sizeof(name), "na%d.cap", na->dno);
			na->cap_ptr = LLVMBuildAlloca(ctx->builder,
										  ctx->types[UPLPGSQL_INT32], name);
			LLVMBuildStore(ctx->builder, llvm_const_int32(ctx, 0),
						   na->cap_ptr);

			/* 1 when data was palloc'd; 0 for the array_fill stack buffer */
			snprintf(name, sizeof(name), "na%d.onheap", na->dno);
			na->is_heap_ptr = LLVMBuildAlloca(ctx->builder,
											  ctx->types[UPLPGSQL_INT8], name);
			LLVMBuildStore(ctx->builder,
						   LLVMConstInt(ctx->types[UPLPGSQL_INT8], 0, false),
						   na->is_heap_ptr);
		}
	}
}

/* Hook: compile the function body */
static void
uplpgsql_compile_body(UPL_compile_ctx *ctx)
{
	UPLpgSQL_function *func = ctx_func(ctx);

	uplpgsql_compile_block(ctx, func->action);
}

/*
 * Emit RT_INIT_VAR for each of a block's declared variables.
 *
 * uplpgsql_rt_init_var() evaluates the DECLARE default with
 * exec_assign_expr(), writing the variable's PG Datum.  For a native array
 * that leaves flat memory untouched — data_ptr NULL, len_ptr 0 — so a later
 * native subscript would fail its bounds check against a length of zero
 * ("array subscript 1 out of range [1..0]") even though the variable holds a
 * perfectly good array.  Reload flat memory from the Datum init_var just
 * wrote.  Arrays with no default refresh from a NULL Datum, which yields the
 * same empty state the entry block already established.
 *
 * The driver emits this rather than the core (which would otherwise do it in
 * upl_emit_block) because refreshing native arrays is language-specific.
 */
static void
uplpgsql_emit_init_vars(UPLpgSQL_compile_ctx *ctx, int n_initvars,
						int *initvarnos)
{
	LLVMValueRef estate_ref = LLVMGetParam(ctx->function, 0);
	int		i;

	for (i = 0; i < n_initvars; i++)
	{
		int			dno = initvarnos[i];
		UPLpgSQL_native_array *na;
		LLVMValueRef args[] = {
			estate_ref,
			upl_const_int32(ctx, dno)
		};

		uplpgsql_call_fn(ctx, RT_INIT_VAR, args, 2);

		na = uplpgsql_find_native_array(ctx, dno);
		if (na != NULL)
		{
			elog(DEBUG1, "uplpgsql: native array from_datum dno %d (init var)",
				 dno);
			uplpgsql_emit_refresh_native_array(ctx, na);
		}
	}
}

/* ----------------------------------------------------------------
 * Exception handling callback
 *
 * This is called from upl_emit_block() when a block has exception
 * handlers.  It contains the full sigsetjmp pattern, handler dispatch,
 * and rethrow logic — all PL/pgSQL-specific.
 * ----------------------------------------------------------------
 */
static void
uplpgsql_compile_block_exceptions(UPL_compile_ctx *ctx, void *exception_data)
{
	UPLpgSQL_stmt_block *stmt = (UPLpgSQL_stmt_block *) exception_data;
	LLVMValueRef		estate_ref = ctx->estate_ref;
	LLVMValueRef		frame_ptr;
	LLVMValueRef		sjrc;
	LLVMValueRef		is_try;
	LLVMBasicBlockRef	try_enter_bb;
	LLVMBasicBlockRef	catch_enter_bb;
	LLVMBasicBlockRef	try_exit_bb;
	LLVMBasicBlockRef	exception_return_bb;
	LLVMBasicBlockRef	handler_return_bb;
	LLVMBasicBlockRef	handler_exit_bb;
	LLVMBasicBlockRef	after_block_bb;
	LLVMBasicBlockRef	rethrow_bb;
	LLVMBasicBlockRef	saved_return_bb;
	LLVMValueRef		handler_idx;
	LLVMValueRef		switch_inst;
	List			   *exc_list = stmt->exceptions->exc_list;
	int					num_handlers;
	ListCell		   *lc;
	int					i;

	ctx->has_exceptions = true;
	num_handlers = list_length(exc_list);

	/* Create all basic blocks upfront */
	try_enter_bb = upl_append_block(ctx, "exc.try_enter");
	catch_enter_bb = upl_append_block(ctx, "exc.catch_enter");
	try_exit_bb = upl_append_block(ctx, "exc.try_exit");
	exception_return_bb = upl_append_block(ctx, "exc.return");
	handler_return_bb = upl_append_block(ctx, "exc.handler_return");
	handler_exit_bb = upl_append_block(ctx, "exc.handler_exit");
	after_block_bb = upl_append_block(ctx, "exc.after");
	rethrow_bb = upl_append_block(ctx, "exc.rethrow");

	/* 1. Push exception frame (allocates frame, begins subtxn) */
	{
		LLVMValueRef args[] = {
			estate_ref,
			upl_const_ptr(ctx, stmt)
		};
		frame_ptr = uplpgsql_call_fn(ctx, RT_EXCEPTION_PUSH_FRAME, args, 2);
	}

	/* 2. Call sigsetjmp(frame, 0) — frame IS the jmpbuf (first field) */
	{
		LLVMValueRef args[] = {
			frame_ptr,
			upl_const_int32(ctx, 0)
		};
		sjrc = LLVMBuildCall2(ctx->builder, ctx->sigsetjmp_fntype,
							  ctx->sigsetjmp_fn, args, 2, "sjrc");
	}

	/* 3. Branch: rc == 0 -> try, else -> catch */
	is_try = LLVMBuildICmp(ctx->builder, LLVMIntEQ, sjrc,
						   upl_const_int32(ctx, 0), "is_try");
	LLVMBuildCondBr(ctx->builder, is_try, try_enter_bb, catch_enter_bb);

	/* === TRY ENTER === */
	LLVMPositionBuilderAtEnd(ctx->builder, try_enter_bb);

	/* Arm the exception frame */
	{
		LLVMValueRef args[] = { estate_ref, frame_ptr };
		uplpgsql_call_fn(ctx, RT_EXCEPTION_ARM, args, 2);
	}

	/* Initialize declared variables */
	uplpgsql_emit_init_vars(ctx, stmt->n_initvars, stmt->initvarnos);

	/*
	 * Compile body statements.  Redirect RETURN to our exception_return_bb
	 * so we can commit the subtransaction before actually returning.
	 */
	saved_return_bb = ctx->return_bb;
	ctx->return_bb = exception_return_bb;

	/*
	 * "EXIT <label>" out of the try body must still release the
	 * subtransaction, so it targets try_exit_bb rather than jumping straight
	 * to after_block_bb.
	 */
	if (stmt->label != NULL)
		upl_push_block_label(ctx, stmt->label, try_exit_bb);

	uplpgsql_compile_stmts(ctx, stmt->body);

	if (stmt->label != NULL)
		upl_pop_loop(ctx);

	ctx->return_bb = saved_return_bb;

	/* Fall through to try_exit */
	LLVMBuildBr(ctx->builder, try_exit_bb);

	/* === EXCEPTION RETURN (RETURN inside try body) === */
	LLVMPositionBuilderAtEnd(ctx->builder, exception_return_bb);
	{
		LLVMValueRef args[] = { estate_ref, frame_ptr };
		uplpgsql_call_fn(ctx, RT_EXCEPTION_TRY_EXIT, args, 2);
	}
	LLVMBuildBr(ctx->builder, saved_return_bb);

	/* === TRY EXIT (normal completion) === */
	LLVMPositionBuilderAtEnd(ctx->builder, try_exit_bb);
	{
		LLVMValueRef args[] = { estate_ref, frame_ptr };
		uplpgsql_call_fn(ctx, RT_EXCEPTION_TRY_EXIT, args, 2);
	}
	LLVMBuildBr(ctx->builder, after_block_bb);

	/* === CATCH ENTER === */
	LLVMPositionBuilderAtEnd(ctx->builder, catch_enter_bb);
	{
		LLVMValueRef args[] = {
			estate_ref,
			upl_const_ptr(ctx, stmt),
			frame_ptr
		};
		handler_idx = uplpgsql_call_fn(ctx, RT_EXCEPTION_CATCH, args, 3);
	}

	/* Switch on handler_idx: -1 -> rethrow, 0..N-1 -> handler blocks */
	switch_inst = LLVMBuildSwitch(ctx->builder, handler_idx,
								   rethrow_bb, num_handlers);

	/*
	 * A RETURN inside a handler body must still run HANDLER_DONE before it
	 * leaves the block: that restores cur_error, pops the stmt_mcontext and
	 * frees the exception frame.  ctx->return_bb was restored to the
	 * function's real return block after the try body above, so a RETURN here
	 * would branch straight past the cleanup.  Point it at a landing pad that
	 * runs HANDLER_DONE first.
	 */
	saved_return_bb = ctx->return_bb;
	ctx->return_bb = handler_return_bb;

	/* === HANDLER BLOCKS === */
	i = 0;
	foreach(lc, exc_list)
	{
		UPLpgSQL_exception *exception = (UPLpgSQL_exception *) lfirst(lc);
		LLVMBasicBlockRef	handler_bb;
		char				bbname[32];

		snprintf(bbname, sizeof(bbname), "exc.handler_%d", i);
		handler_bb = upl_append_block(ctx, bbname);

		/* Add case to switch */
		LLVMAddCase(switch_inst, upl_const_int32(ctx, i), handler_bb);

		/* Compile handler body */
		LLVMPositionBuilderAtEnd(ctx->builder, handler_bb);

		/* Set SQLSTATE/SQLERRM variables */
		{
			LLVMValueRef args[] = {
				estate_ref,
				upl_const_ptr(ctx, stmt),
				upl_const_int32(ctx, i)
			};
			uplpgsql_call_fn(ctx, RT_EXCEPTION_SET_HANDLER_VARS, args, 3);
		}

		/*
		 * "EXIT <label>" out of a handler body leaves the block, but must
		 * run HANDLER_DONE on the way out.
		 */
		if (stmt->label != NULL)
			upl_push_block_label(ctx, stmt->label, handler_exit_bb);

		/* Compile the handler's statements */
		uplpgsql_compile_stmts(ctx, exception->action);

		if (stmt->label != NULL)
			upl_pop_loop(ctx);

		/* Clean up after handler (normal, non-RETURN completion) */
		{
			LLVMValueRef args[] = { estate_ref, frame_ptr };
			uplpgsql_call_fn(ctx, RT_EXCEPTION_HANDLER_DONE, args, 2);
		}

		LLVMBuildBr(ctx->builder, after_block_bb);

		i++;
	}

	ctx->return_bb = saved_return_bb;

	/* === HANDLER RETURN (RETURN inside a handler body) === */
	LLVMPositionBuilderAtEnd(ctx->builder, handler_return_bb);
	{
		LLVMValueRef args[] = { estate_ref, frame_ptr };
		uplpgsql_call_fn(ctx, RT_EXCEPTION_HANDLER_DONE, args, 2);
	}
	LLVMBuildBr(ctx->builder, saved_return_bb);

	/* === HANDLER EXIT (EXIT <label> inside a handler body) === */
	LLVMPositionBuilderAtEnd(ctx->builder, handler_exit_bb);
	{
		LLVMValueRef args[] = { estate_ref, frame_ptr };
		uplpgsql_call_fn(ctx, RT_EXCEPTION_HANDLER_DONE, args, 2);
	}
	LLVMBuildBr(ctx->builder, after_block_bb);

	/* === RETHROW === */
	LLVMPositionBuilderAtEnd(ctx->builder, rethrow_bb);
	{
		LLVMValueRef args[] = { estate_ref, frame_ptr };
		uplpgsql_call_fn(ctx, RT_EXCEPTION_RETHROW, args, 2);
	}
	LLVMBuildUnreachable(ctx->builder);

	/* Continue after the block */
	LLVMPositionBuilderAtEnd(ctx->builder, after_block_bb);
}

/* ----------------------------------------------------------------
 * Native Local Array Escape Analysis (Phase 7)
 *
 * Identifies local array variables that can be lowered to flat native
 * memory (stack alloca or heap palloc0) instead of going through
 * PostgreSQL's array_get_element / array_set_element on every subscript
 * access.  This eliminates ~50-100ns per array access, replacing it with
 * a single GEP+load/store instruction (~1ns).
 *
 * A variable qualifies if ALL of these hold:
 *   1. dtype == UPLPGSQL_DTYPE_VAR, datatype->typisarray == true
 *   2. Element type is int4, int8, or float8 (Tier 1 types)
 *   3. Not a function parameter (parameters may alias caller data)
 *   4. Never passed to RETURN, RAISE, SPI, PERFORM, EXECUTE, CALL,
 *      FOREACH, OPEN, or FETCH (these would need a real PG array Datum)
 *
 * Key design decision: the ASSIGN statement case does NOT disqualify
 * arrays referenced in other assignments' expressions.  This is because
 * PL/pgSQL's paramnos bitmapset (used by expr_references_dno) cannot
 * distinguish a subscript read like x[i] from a whole-datum reference
 * like x.  Subscript reads are safe — they are compiled to native
 * GEP+load by uplpgsql_compile_expr_datum().  True whole-datum escapes
 * (y := x, RETURN x, etc.) are caught by the other statement-type
 * cases in native_array_check_stmt().
 *
 * After analysis, qualifying arrays get:
 *   - data_ptr alloca (ptr, initially NULL) in the LLVM entry block
 *   - len_ptr alloca (i32, initially 0) in the LLVM entry block
 *   - array_fill() interception → stack/heap allocation (in uplpgsql_expr.c)
 *   - arr[i] read/write → inline bounds check + GEP (in uplpgsql_expr.c)
 * ----------------------------------------------------------------
 */

/* Max stack allocation per array (4KB = 512 float8s or 1024 int4s) */
#define NATIVE_ARRAY_STACK_THRESHOLD	4096

/* Max total stack allocation across all native arrays in one function */
#define NATIVE_ARRAY_TOTAL_STACK_MAX	16384

/*
 * Check if an expression contains a Param reference to a given dno
 * anywhere (used for RETURN, RAISE args, etc. where any reference escapes).
 */
static bool
expr_references_dno(UPLpgSQL_expr *expr, int dno)
{
	Bitmapset *paramnos;

	if (expr == NULL)
		return false;

	paramnos = expr->paramnos;
	return bms_is_member(dno, paramnos);
}

/*
 * Walk statement list and disqualify any candidate arrays that escape.
 */
static void
native_array_check_stmts(UPLpgSQL_function *func, List *stmts,
						 bool *candidates, int ndatums);

static void
native_array_check_stmt(UPLpgSQL_function *func, UPLpgSQL_stmt *stmt,
						bool *candidates, int ndatums)
{
	int		dno;

	switch (stmt->cmd_type)
	{
		case UPLPGSQL_STMT_ASSIGN:
			{
				/*
				 * ASSIGN never disqualifies a candidate; escapes are
				 * handled at IR generation instead of here.
				 *
				 * We cannot decide it here: expr_references_dno uses the
				 * paramnos bitmapset, which can't distinguish a subscript
				 * read (x[i], safe and compiled to native GEP+load) from a
				 * whole-datum read (x, an escape).  Disqualifying on
				 * paramnos would drop every array that is ever read by
				 * subscript — that is, all of them.
				 *
				 * So uplpgsql_compile_assign() handles both escape
				 * directions when it falls back to the interpreter: it
				 * syncs flat memory into the PG Datums beforehand (so a
				 * whole-datum read like y := x sees live data), and
				 * reloads flat memory afterwards if the target is itself
				 * a native array (so x := ARRAY[...] is not left stale).
				 */
			}
			break;

		case UPLPGSQL_STMT_RETURN:
		case UPLPGSQL_STMT_RETURN_NEXT:
			{
				UPLpgSQL_stmt_return *r = (UPLpgSQL_stmt_return *) stmt;

				for (dno = 0; dno < ndatums; dno++)
				{
					if (candidates[dno] &&
						expr_references_dno(r->expr, dno))
						candidates[dno] = false;
				}
			}
			break;

		case UPLPGSQL_STMT_RETURN_QUERY:
			{
				UPLpgSQL_stmt_return_query *r =
					(UPLpgSQL_stmt_return_query *) stmt;

				for (dno = 0; dno < ndatums; dno++)
				{
					if (candidates[dno] &&
						(expr_references_dno(r->query, dno) ||
						 expr_references_dno(r->dynquery, dno)))
						candidates[dno] = false;
				}
			}
			break;

		case UPLPGSQL_STMT_RAISE:
			{
				UPLpgSQL_stmt_raise *r = (UPLpgSQL_stmt_raise *) stmt;
				ListCell   *lc;

				foreach(lc, r->params)
				{
					for (dno = 0; dno < ndatums; dno++)
					{
						if (candidates[dno] &&
							expr_references_dno(lfirst(lc), dno))
							candidates[dno] = false;
					}
				}
				foreach(lc, r->options)
				{
					UPLpgSQL_raise_option *opt = lfirst(lc);

					for (dno = 0; dno < ndatums; dno++)
					{
						if (candidates[dno] &&
							expr_references_dno(opt->expr, dno))
							candidates[dno] = false;
					}
				}
			}
			break;

		case UPLPGSQL_STMT_EXECSQL:
			{
				UPLpgSQL_stmt_execsql *e = (UPLpgSQL_stmt_execsql *) stmt;

				for (dno = 0; dno < ndatums; dno++)
				{
					if (candidates[dno] &&
						expr_references_dno(e->sqlstmt, dno))
						candidates[dno] = false;
				}
			}
			break;

		case UPLPGSQL_STMT_DYNEXECUTE:
			{
				UPLpgSQL_stmt_dynexecute *e =
					(UPLpgSQL_stmt_dynexecute *) stmt;
				ListCell *lc;

				for (dno = 0; dno < ndatums; dno++)
				{
					if (candidates[dno] &&
						expr_references_dno(e->query, dno))
						candidates[dno] = false;
				}
				foreach(lc, e->params)
				{
					for (dno = 0; dno < ndatums; dno++)
					{
						if (candidates[dno] &&
							expr_references_dno(lfirst(lc), dno))
							candidates[dno] = false;
					}
				}
			}
			break;

		case UPLPGSQL_STMT_PERFORM:
			{
				UPLpgSQL_stmt_perform *p = (UPLpgSQL_stmt_perform *) stmt;

				for (dno = 0; dno < ndatums; dno++)
				{
					if (candidates[dno] &&
						expr_references_dno(p->expr, dno))
						candidates[dno] = false;
				}
			}
			break;

		case UPLPGSQL_STMT_CALL:
			{
				UPLpgSQL_stmt_call *c = (UPLpgSQL_stmt_call *) stmt;

				for (dno = 0; dno < ndatums; dno++)
				{
					if (candidates[dno] &&
						expr_references_dno(c->expr, dno))
						candidates[dno] = false;
				}
			}
			break;

		case UPLPGSQL_STMT_FOREACH_A:
			{
				UPLpgSQL_stmt_foreach_a *f =
					(UPLpgSQL_stmt_foreach_a *) stmt;

				/* If iterating over a candidate array, disqualify */
				for (dno = 0; dno < ndatums; dno++)
				{
					if (candidates[dno] &&
						expr_references_dno(f->expr, dno))
						candidates[dno] = false;
				}
				native_array_check_stmts(func, f->body, candidates, ndatums);
			}
			break;

		/* Statements with sub-statement lists — recurse */
		case UPLPGSQL_STMT_BLOCK:
			{
				UPLpgSQL_stmt_block *b = (UPLpgSQL_stmt_block *) stmt;
				ListCell *lc;

				native_array_check_stmts(func, b->body, candidates, ndatums);
				if (b->exceptions)
				{
					foreach(lc, b->exceptions->exc_list)
					{
						UPLpgSQL_exception *exc = lfirst(lc);

						native_array_check_stmts(func, exc->action,
												 candidates, ndatums);
					}
				}
			}
			break;

		case UPLPGSQL_STMT_IF:
			{
				UPLpgSQL_stmt_if *i = (UPLpgSQL_stmt_if *) stmt;
				ListCell *lc;

				/* condition can reference arrays */
				for (dno = 0; dno < ndatums; dno++)
				{
					if (candidates[dno] &&
						expr_references_dno(i->cond, dno))
						candidates[dno] = false;
				}
				native_array_check_stmts(func, i->then_body,
										 candidates, ndatums);
				foreach(lc, i->elsif_list)
				{
					UPLpgSQL_if_elsif *elif = lfirst(lc);

					for (dno = 0; dno < ndatums; dno++)
					{
						if (candidates[dno] &&
							expr_references_dno(elif->cond, dno))
							candidates[dno] = false;
					}
					native_array_check_stmts(func, elif->stmts,
											 candidates, ndatums);
				}
				native_array_check_stmts(func, i->else_body,
										 candidates, ndatums);
			}
			break;

		case UPLPGSQL_STMT_CASE:
			{
				UPLpgSQL_stmt_case *c = (UPLpgSQL_stmt_case *) stmt;
				ListCell *lc;

				for (dno = 0; dno < ndatums; dno++)
				{
					if (candidates[dno] &&
						expr_references_dno(c->t_expr, dno))
						candidates[dno] = false;
				}
				foreach(lc, c->case_when_list)
				{
					UPLpgSQL_case_when *w = lfirst(lc);

					for (dno = 0; dno < ndatums; dno++)
					{
						if (candidates[dno] &&
							expr_references_dno(w->expr, dno))
							candidates[dno] = false;
					}
					native_array_check_stmts(func, w->stmts,
											 candidates, ndatums);
				}
				native_array_check_stmts(func, c->else_stmts,
										 candidates, ndatums);
			}
			break;

		case UPLPGSQL_STMT_LOOP:
			{
				UPLpgSQL_stmt_loop *l = (UPLpgSQL_stmt_loop *) stmt;

				native_array_check_stmts(func, l->body,
										 candidates, ndatums);
			}
			break;

		case UPLPGSQL_STMT_WHILE:
			{
				UPLpgSQL_stmt_while *w = (UPLpgSQL_stmt_while *) stmt;

				for (dno = 0; dno < ndatums; dno++)
				{
					if (candidates[dno] &&
						expr_references_dno(w->cond, dno))
						candidates[dno] = false;
				}
				native_array_check_stmts(func, w->body,
										 candidates, ndatums);
			}
			break;

		case UPLPGSQL_STMT_FORI:
			{
				UPLpgSQL_stmt_fori *f = (UPLpgSQL_stmt_fori *) stmt;

				for (dno = 0; dno < ndatums; dno++)
				{
					if (candidates[dno] &&
						(expr_references_dno(f->lower, dno) ||
						 expr_references_dno(f->upper, dno) ||
						 expr_references_dno(f->step, dno)))
						candidates[dno] = false;
				}
				native_array_check_stmts(func, f->body,
										 candidates, ndatums);
			}
			break;

		case UPLPGSQL_STMT_FORS:
		case UPLPGSQL_STMT_DYNFORS:
			{
				/* These have a query and a body */
				UPLpgSQL_stmt_fors *f = (UPLpgSQL_stmt_fors *) stmt;

				for (dno = 0; dno < ndatums; dno++)
				{
					if (candidates[dno] &&
						expr_references_dno(f->query, dno))
						candidates[dno] = false;
				}
				native_array_check_stmts(func, f->body,
										 candidates, ndatums);
			}
			break;

		case UPLPGSQL_STMT_FORC:
			{
				UPLpgSQL_stmt_forc *f = (UPLpgSQL_stmt_forc *) stmt;

				for (dno = 0; dno < ndatums; dno++)
				{
					if (candidates[dno] &&
						expr_references_dno(f->argquery, dno))
						candidates[dno] = false;
				}
				native_array_check_stmts(func, f->body,
										 candidates, ndatums);
			}
			break;

		default:
			/* EXIT, CLOSE, FETCH, GETDIAG, COMMIT, ROLLBACK, ASSERT, OPEN
			 * — no expression that could reference an array */
			break;
	}
}

static void
native_array_check_stmts(UPLpgSQL_function *func, List *stmts,
						 bool *candidates, int ndatums)
{
	ListCell *lc;

	if (stmts == NIL)
		return;

	foreach(lc, stmts)
		native_array_check_stmt(func, lfirst(lc), candidates, ndatums);
}

/*
 * Main escape analysis entry point for native local arrays.
 *
 * Three-step process:
 *   Step 1: Identify candidate arrays (local vars with int4/int8/float8
 *           element types, excluding function parameters).
 *   Step 2: Walk the entire AST via native_array_check_stmts() and
 *           disqualify any candidate that appears in an escaping context
 *           (RETURN, RAISE, SPI, etc.).
 *   Step 3: Build the UPLpgSQL_native_array metadata array from survivors.
 *
 * Populates ctx_native_arrays(ctx) and ctx_num_native_arrays(ctx).
 * Called from uplpgsql_compile_function() step 7b, before IR generation.
 */
static void
uplpgsql_analyze_native_arrays(UPLpgSQL_compile_ctx *ctx,
							   UPLpgSQL_function *func)
{
	bool	   *candidates;
	int			ndatums = func->ndatums;
	int			i, count;

	ctx_num_native_arrays(ctx) = 0;
	ctx_native_arrays(ctx) = NULL;

	if (ndatums == 0)
		return;

	candidates = palloc0(sizeof(bool) * ndatums);

	/* Step 1: Identify candidate array variables */
	for (i = 0; i < ndatums; i++)
	{
		UPLpgSQL_datum *d = func->datums[i];
		UPLpgSQL_var   *var;
		Oid				elemtype;
		bool			is_param = false;
		int				j;

		if (d->dtype != UPLPGSQL_DTYPE_VAR)
			continue;

		var = (UPLpgSQL_var *) d;

		if (!var->datatype->typisarray)
			continue;

		/* Check element type */
		elemtype = get_element_type(var->datatype->typoid);
		if (elemtype != INT4OID && elemtype != INT8OID && elemtype != FLOAT8OID)
			continue;

		/* Exclude function parameters */
		for (j = 0; j < func->fn_nargs; j++)
		{
			if (func->fn_argvarnos[j] == i)
			{
				is_param = true;
				break;
			}
		}
		if (is_param)
			continue;

		candidates[i] = true;
	}

	/* Step 2: Walk AST and disqualify arrays that escape */
	native_array_check_stmts(func, func->action->body, candidates, ndatums);

	/* Step 3: Build native_arrays list from survivors */
	count = 0;
	for (i = 0; i < ndatums; i++)
	{
		if (candidates[i])
			count++;
	}

	if (count > 0)
	{
		int idx = 0;

		ctx_native_arrays(ctx) = palloc(sizeof(UPLpgSQL_native_array) * count);
		ctx_num_native_arrays(ctx) = count;

		for (i = 0; i < ndatums; i++)
		{
			UPLpgSQL_var		   *var;
			Oid						elemtype;
			UPLpgSQL_native_array  *na;

			if (!candidates[i])
				continue;

			var = (UPLpgSQL_var *) func->datums[i];
			elemtype = get_element_type(var->datatype->typoid);
			na = &ctx_native_arrays(ctx)[idx++];

			na->dno = i;
			na->elemtype = elemtype;

			if (elemtype == INT4OID)
				na->elem_size = 4;
			else
				na->elem_size = 8;	/* int8, float8 */

			/* llvm_elemtype and data_ptr/len_ptr set later during IR gen */
			na->llvm_elemtype = NULL;
			na->data_ptr = NULL;
			na->len_ptr = NULL;
			na->lb_ptr = NULL;
			na->nulls_ptr = NULL;
			na->cap_ptr = NULL;
			na->is_heap_ptr = NULL;

			elog(DEBUG1, "uplpgsql: native array candidate dno %d (%s), "
				 "elemtype %u, elem_size %d",
				 i, var->refname, elemtype, na->elem_size);
		}
	}

	pfree(candidates);
}

/*
 * uplpgsql_jit_score_stmts - Recursively score a statement list for JIT
 *                            suitability.
 *
 * Returns a score where positive values favor JIT compilation and negative
 * values favor the interpreter.  The loop_depth multiplier amplifies scores
 * inside loops since those statements execute many times.
 */
static int
uplpgsql_jit_score_stmts(List *stmts, int loop_depth);

static int
uplpgsql_jit_score_stmt(UPLpgSQL_stmt *stmt, int loop_depth)
{
	int		score = 0;

	/*
	 * Only statements inside loops score positively.  Top-level statements
	 * (loop_depth == 0) don't benefit from JIT because the per-call
	 * overhead dominates — the function body executes once per call
	 * regardless of whether it's interpreted or JIT'd.
	 */
	int		mult = (loop_depth > 0) ? 3 * loop_depth : 0;

	switch (stmt->cmd_type)
	{
		case UPLPGSQL_STMT_ASSIGN:
			/* Scalar assignment may be inlined to native IR */
			score = 1 * mult;
			break;

		case UPLPGSQL_STMT_IF:
			{
				UPLpgSQL_stmt_if *ifstmt = (UPLpgSQL_stmt_if *) stmt;
				ListCell   *lc;

				score = 1 * mult;	/* the branch itself */
				score += uplpgsql_jit_score_stmts(ifstmt->then_body,
												  loop_depth);
				foreach(lc, ifstmt->elsif_list)
				{
					UPLpgSQL_if_elsif *ei = lfirst(lc);

					score += 1 * mult;
					score += uplpgsql_jit_score_stmts(ei->stmts, loop_depth);
				}
				if (ifstmt->else_body)
					score += uplpgsql_jit_score_stmts(ifstmt->else_body,
													  loop_depth);
			}
			break;

		case UPLPGSQL_STMT_CASE:
			{
				UPLpgSQL_stmt_case *cs = (UPLpgSQL_stmt_case *) stmt;
				ListCell   *lc;

				score = 1 * mult;
				foreach(lc, cs->case_when_list)
				{
					UPLpgSQL_case_when *cw = lfirst(lc);

					score += 1 * mult;
					score += uplpgsql_jit_score_stmts(cw->stmts, loop_depth);
				}
				if (cs->else_stmts)
					score += uplpgsql_jit_score_stmts(cs->else_stmts,
													  loop_depth);
			}
			break;

		case UPLPGSQL_STMT_WHILE:
			{
				UPLpgSQL_stmt_while *ws = (UPLpgSQL_stmt_while *) stmt;

				score = 3;	/* loops always score positive */
				score += uplpgsql_jit_score_stmts(ws->body, loop_depth + 1);
			}
			break;

		case UPLPGSQL_STMT_LOOP:
			{
				UPLpgSQL_stmt_loop *ls = (UPLpgSQL_stmt_loop *) stmt;

				score = 3;
				score += uplpgsql_jit_score_stmts(ls->body, loop_depth + 1);
			}
			break;

		case UPLPGSQL_STMT_FORI:
			{
				UPLpgSQL_stmt_fori *fi = (UPLpgSQL_stmt_fori *) stmt;

				score = 3;
				score += uplpgsql_jit_score_stmts(fi->body, loop_depth + 1);
			}
			break;

		case UPLPGSQL_STMT_EXIT:
			score = 1 * mult;
			break;

		/* SPI-dominated: cursor-based loops */
		case UPLPGSQL_STMT_FORS:
			{
				UPLpgSQL_stmt_fors *fs = (UPLpgSQL_stmt_fors *) stmt;

				score = -3;	/* SPI cursor always penalizes */
				score += uplpgsql_jit_score_stmts(fs->body, loop_depth + 1);
			}
			break;

		case UPLPGSQL_STMT_FORC:
			{
				UPLpgSQL_stmt_forc *fc = (UPLpgSQL_stmt_forc *) stmt;

				score = -3;
				score += uplpgsql_jit_score_stmts(fc->body, loop_depth + 1);
			}
			break;

		case UPLPGSQL_STMT_DYNFORS:
			{
				UPLpgSQL_stmt_dynfors *df = (UPLpgSQL_stmt_dynfors *) stmt;

				score = -3;
				score += uplpgsql_jit_score_stmts(df->body, loop_depth + 1);
			}
			break;

		/* SPI-dominated: SQL execution — penalize more inside loops */
		case UPLPGSQL_STMT_PERFORM:
		case UPLPGSQL_STMT_EXECSQL:
		case UPLPGSQL_STMT_DYNEXECUTE:
		case UPLPGSQL_STMT_CALL:
			score = (loop_depth > 0) ? -2 * 3 * loop_depth : -2;
			break;

		/* Moderate SPI cost */
		case UPLPGSQL_STMT_FOREACH_A:
		case UPLPGSQL_STMT_RETURN_NEXT:
		case UPLPGSQL_STMT_RETURN_QUERY:
			score = (loop_depth > 0) ? -1 * 3 * loop_depth : -1;
			break;

		/* Neutral */
		case UPLPGSQL_STMT_RETURN:
		case UPLPGSQL_STMT_RAISE:
		case UPLPGSQL_STMT_ASSERT:
		case UPLPGSQL_STMT_GETDIAG:
		case UPLPGSQL_STMT_OPEN:
		case UPLPGSQL_STMT_FETCH:
		case UPLPGSQL_STMT_CLOSE:
		case UPLPGSQL_STMT_COMMIT:
		case UPLPGSQL_STMT_ROLLBACK:
			score = 0;
			break;

		case UPLPGSQL_STMT_BLOCK:
			{
				UPLpgSQL_stmt_block *blk = (UPLpgSQL_stmt_block *) stmt;

				if (blk->exceptions)
					score = -2;

				score += uplpgsql_jit_score_stmts(blk->body, loop_depth);
			}
			break;

		default:
			score = 0;
			break;
	}

	return score;
}

static int
uplpgsql_jit_score_stmts(List *stmts, int loop_depth)
{
	ListCell   *lc;
	int			total = 0;

	foreach(lc, stmts)
	{
		total += uplpgsql_jit_score_stmt((UPLpgSQL_stmt *) lfirst(lc),
										 loop_depth);
	}

	return total;
}

/*
 * uplpgsql_should_jit - Decide whether to JIT-compile a function.
 *
 * Walks the AST and scores the function based on statement mix.
 * Returns true if JIT is expected to help.
 */
bool
uplpgsql_should_jit(UPLpgSQL_function *func)
{
	int		score;

	score = uplpgsql_jit_score_stmts(func->action->body, 0);

	elog(DEBUG1, "uplpgsql: JIT score for %s = %d",
		 func->fn_signature, score);

	return (score > 0);
}

/*
 * uplpgsql_compile_function - Main entry point: compile a UPLpgSQL_function
 *                             to native code via LLVM.
 *
 * This is the top-level function that orchestrates the entire compilation
 * pipeline.  The steps are numbered 1-15 in the code below.
 *
 * Input:  UPLpgSQL_function (AST from the forked PL/pgSQL parser)
 * Output: UPLpgSQL_func (contains the native function pointer)
 *
 * The generated LLVM function has signature: i32 func(ptr estate)
 * where estate is a UPLpgSQL_exec_state*.  The return value is one of
 * UPLPGSQL_RC_OK, RC_EXIT, RC_RETURN, RC_CONTINUE.
 *
 * The entire compilation is wrapped in PG_TRY to ensure LLVM resources
 * (context, module, builder) are properly cleaned up on error.
 *
 * Unique symbol names (uplpgsql_fn_<oid>_g<N>) prevent collisions when
 * a function is recompiled after CREATE OR REPLACE — the old symbol
 * remains in OrcJIT but is no longer referenced.
 */
UPLpgSQL_func *
uplpgsql_compile_function(UPLpgSQL_function *func)
{
	UPLpgSQL_compile_ctx	ctx;
	UPLpgSQL_lang_data		lang_data;
	UPL_compile_hooks		hooks;
	UPLpgSQL_func		   *result;
	void				   *fn_ptr;

	memset(&ctx, 0, sizeof(ctx));
	memset(&lang_data, 0, sizeof(lang_data));
	memset(&hooks, 0, sizeof(hooks));

	lang_data.uplpgsql_func = func;
	ctx.lang_data = &lang_data;

	/* Allocate RT function arrays */
	ctx.num_rt_funcs = UPLPGSQL_NUM_RT_FUNCS;
	ctx.rt_funcs = palloc0(sizeof(LLVMValueRef) * UPLPGSQL_NUM_RT_FUNCS);
	ctx.rt_fntypes = palloc0(sizeof(LLVMTypeRef) * UPLPGSQL_NUM_RT_FUNCS);

	/* Setup callbacks */
	ctx.callbacks.compile_stmts = uplpgsql_cb_compile_stmts;
	ctx.callbacks.try_compile_bool = uplpgsql_cb_try_compile_bool;
	ctx.callbacks.assign_expr = uplpgsql_cb_assign_expr;
	ctx.callbacks.rt_eval_bool = RT_EVAL_BOOL;
	ctx.callbacks.rt_eval_int = RT_EVAL_INT;
	ctx.callbacks.rt_set_found = RT_SET_FOUND;
	ctx.callbacks.rt_assign_int = RT_ASSIGN_INT;
	ctx.callbacks.rt_case_error = RT_CASE_ERROR;
	ctx.callbacks.rt_assign_null = RT_ASSIGN_NULL;
	ctx.callbacks.rt_init_var = RT_INIT_VAR;
	ctx.callbacks.rt_assign_expr = RT_ASSIGN_EXPR;

	/* Setup datum offsets */
	ctx.datum_offsets.estate_to_lang_state =
		offsetof(UPLpgSQL_exec_state, uplpgsql_estate);
	ctx.datum_offsets.lang_state_to_datums =
		offsetof(UPLpgSQL_execstate, datums);
	ctx.datum_offsets.var_to_value = offsetof(UPLpgSQL_var, value);
	ctx.datum_offsets.var_to_isnull = offsetof(UPLpgSQL_var, isnull);
	ctx.datum_offsets.var_to_freeval = offsetof(UPLpgSQL_var, freeval);

	/* Setup pipeline hooks */
	hooks.register_rt_funcs = uplpgsql_register_runtime_funcs;
	hooks.setup_entry = uplpgsql_setup_entry;
	hooks.compile_body = uplpgsql_compile_body;
	hooks.func_name_prefix = "uplpgsql_fn";
	hooks.fn_oid = func->fn_oid;
	hooks.fn_xmin = func->cfunc.fn_xmin;
	hooks.fn_tid = func->cfunc.fn_tid;
	hooks.default_rc = UPLPGSQL_RC_OK;
	hooks.dump_ir = uplpgsql_dump_ir;

	/* Run the core compilation pipeline */
	fn_ptr = upl_compile_function(&ctx, &hooks);

	/* Create cached function result */
	result = (UPLpgSQL_func *) MemoryContextAllocZero(TopMemoryContext,
													   sizeof(UPLpgSQL_func));
	result->jit_func = (uplpgsql_jit_func) fn_ptr;
	result->fn_oid = func->fn_oid;
	result->fn_xmin = func->cfunc.fn_xmin;
	result->fn_tid = func->cfunc.fn_tid;

	return result;
}

/*
 * Register all runtime helper function declarations in the LLVM module.
 *
 * This creates LLVM function declarations (not definitions) for every
 * uplpgsql_rt_* runtime helper.  The declarations provide the type
 * information LLVM needs to generate correct call instructions.  The
 * actual function bodies are not in the LLVM module — they are resolved
 * at link time by OrcJIT's process symbol search generator, which finds
 * them in the process's symbol table (they're in our .so with default
 * visibility via UPLPGSQL_RT_EXPORT).
 *
 * Each registration stores both the LLVMValueRef (function declaration)
 * and LLVMTypeRef (function type) in ctx->rt_funcs[] and ctx->rt_fntypes[],
 * indexed by the UPLpgSQL_rt_func enum.  These are used by
 * uplpgsql_call_fn() to emit call instructions.
 */
static void
uplpgsql_register_runtime_funcs(UPLpgSQL_compile_ctx *ctx)
{
	LLVMTypeRef ptr = ctx->types[UPLPGSQL_PTR];
	LLVMTypeRef i1  = ctx->types[UPLPGSQL_INT1];
	LLVMTypeRef i8  = ctx->types[UPLPGSQL_INT8];
	LLVMTypeRef i16 = ctx->types[UPLPGSQL_INT16];
	LLVMTypeRef i32 = ctx->types[UPLPGSQL_INT32];
	LLVMTypeRef i64 = ctx->types[UPLPGSQL_INT64];
	LLVMTypeRef vd  = ctx->types[UPLPGSQL_VOID];

	/* Datum uplpgsql_rt_eval_expr(ptr estate, ptr expr, ptr isNull_out) */
	{
		LLVMTypeRef params[] = { ptr, ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(i64, params, 3, false);

		ctx->rt_fntypes[RT_EVAL_EXPR] = ft;
		ctx->rt_funcs[RT_EVAL_EXPR] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_eval_expr", ft);
	}

	/* bool uplpgsql_rt_eval_bool(ptr estate, ptr expr) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(i1, params, 2, false);

		ctx->rt_fntypes[RT_EVAL_BOOL] = ft;
		ctx->rt_funcs[RT_EVAL_BOOL] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_eval_bool", ft);
	}

	/* int32 uplpgsql_rt_eval_int(ptr estate, ptr expr) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(i32, params, 2, false);

		ctx->rt_fntypes[RT_EVAL_INT] = ft;
		ctx->rt_funcs[RT_EVAL_INT] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_eval_int", ft);
	}

	/* void uplpgsql_rt_init_var(ptr estate, i32 dno) */
	{
		LLVMTypeRef params[] = { ptr, i32 };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 2, false);

		ctx->rt_fntypes[RT_INIT_VAR] = ft;
		ctx->rt_funcs[RT_INIT_VAR] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_init_var", ft);
	}

	/* void uplpgsql_rt_assign_expr(ptr estate, i32 target_dno, ptr expr) */
	{
		LLVMTypeRef params[] = { ptr, i32, ptr };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 3, false);

		ctx->rt_fntypes[RT_ASSIGN_EXPR] = ft;
		ctx->rt_funcs[RT_ASSIGN_EXPR] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_assign_expr", ft);
	}

	/* void uplpgsql_rt_set_found(ptr estate, i1 value) */
	{
		LLVMTypeRef params[] = { ptr, i1 };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 2, false);

		ctx->rt_fntypes[RT_SET_FOUND] = ft;
		ctx->rt_funcs[RT_SET_FOUND] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_set_found", ft);
	}

	/* void uplpgsql_rt_assign_int(ptr estate, i32 dno, i32 value) */
	{
		LLVMTypeRef params[] = { ptr, i32, i32 };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 3, false);

		ctx->rt_fntypes[RT_ASSIGN_INT] = ft;
		ctx->rt_funcs[RT_ASSIGN_INT] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_assign_int", ft);
	}

	/* int32 uplpgsql_rt_exec_return(ptr estate, ptr stmt) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(i32, params, 2, false);

		ctx->rt_fntypes[RT_EXEC_RETURN] = ft;
		ctx->rt_funcs[RT_EXEC_RETURN] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exec_return", ft);
	}

	/* int32 uplpgsql_rt_exec_perform(ptr estate, ptr stmt) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(i32, params, 2, false);

		ctx->rt_fntypes[RT_EXEC_PERFORM] = ft;
		ctx->rt_funcs[RT_EXEC_PERFORM] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exec_perform", ft);
	}

	/* int32 uplpgsql_rt_exec_sql(ptr estate, ptr stmt) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(i32, params, 2, false);

		ctx->rt_fntypes[RT_EXEC_SQL] = ft;
		ctx->rt_funcs[RT_EXEC_SQL] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exec_sql", ft);
	}

	/* void uplpgsql_rt_exec_raise(ptr estate, ptr stmt) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 2, false);

		ctx->rt_fntypes[RT_EXEC_RAISE] = ft;
		ctx->rt_funcs[RT_EXEC_RAISE] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exec_raise", ft);
	}

	/* void uplpgsql_rt_assign_null(ptr estate, i32 dno) */
	{
		LLVMTypeRef params[] = { ptr, i32 };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 2, false);

		ctx->rt_fntypes[RT_ASSIGN_NULL] = ft;
		ctx->rt_funcs[RT_ASSIGN_NULL] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_assign_null", ft);
	}

	/* void uplpgsql_rt_case_error(ptr estate, i32 lineno) */
	{
		LLVMTypeRef params[] = { ptr, i32 };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 2, false);

		ctx->rt_fntypes[RT_CASE_ERROR] = ft;
		ctx->rt_funcs[RT_CASE_ERROR] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_case_error", ft);
	}

	/* void uplpgsql_rt_exec_assert(ptr estate, ptr stmt) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 2, false);

		ctx->rt_fntypes[RT_EXEC_ASSERT_FAIL] = ft;
		ctx->rt_funcs[RT_EXEC_ASSERT_FAIL] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exec_assert", ft);
	}

	/* i32 uplpgsql_rt_exec_open(ptr estate, ptr stmt) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(i32, params, 2, false);

		ctx->rt_fntypes[RT_EXEC_OPEN] = ft;
		ctx->rt_funcs[RT_EXEC_OPEN] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exec_open", ft);
	}

	/* i32 uplpgsql_rt_exec_fetch(ptr estate, ptr stmt) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(i32, params, 2, false);

		ctx->rt_fntypes[RT_EXEC_FETCH] = ft;
		ctx->rt_funcs[RT_EXEC_FETCH] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exec_fetch", ft);
	}

	/* i32 uplpgsql_rt_exec_close(ptr estate, ptr stmt) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(i32, params, 2, false);

		ctx->rt_fntypes[RT_EXEC_CLOSE] = ft;
		ctx->rt_funcs[RT_EXEC_CLOSE] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exec_close", ft);
	}

	/* ptr uplpgsql_rt_open_query_cursor(ptr estate, ptr query) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(ptr, params, 2, false);

		ctx->rt_fntypes[RT_OPEN_QUERY_CURSOR] = ft;
		ctx->rt_funcs[RT_OPEN_QUERY_CURSOR] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_open_query_cursor", ft);
	}

	/* i1 uplpgsql_rt_fetch_cursor_row(ptr estate, ptr portal, i32 target_dno) */
	{
		LLVMTypeRef params[] = { ptr, ptr, i32 };
		LLVMTypeRef ft = LLVMFunctionType(i1, params, 3, false);

		ctx->rt_fntypes[RT_FETCH_CURSOR_ROW] = ft;
		ctx->rt_funcs[RT_FETCH_CURSOR_ROW] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_fetch_cursor_row", ft);
	}

	/* void uplpgsql_rt_close_portal(ptr estate, ptr portal) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 2, false);

		ctx->rt_fntypes[RT_CLOSE_PORTAL] = ft;
		ctx->rt_funcs[RT_CLOSE_PORTAL] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_close_portal", ft);
	}

	/* ptr uplpgsql_rt_open_forc_cursor(ptr estate, ptr stmt) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(ptr, params, 2, false);

		ctx->rt_fntypes[RT_OPEN_FORC_CURSOR] = ft;
		ctx->rt_funcs[RT_OPEN_FORC_CURSOR] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_open_forc_cursor", ft);
	}

	/* void uplpgsql_rt_close_forc_cursor(ptr estate, ptr stmt, ptr portal) */
	{
		LLVMTypeRef params[] = { ptr, ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 3, false);

		ctx->rt_fntypes[RT_CLOSE_FORC_CURSOR] = ft;
		ctx->rt_funcs[RT_CLOSE_FORC_CURSOR] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_close_forc_cursor", ft);
	}

	/* i32 uplpgsql_rt_exec_block_protected(ptr estate, ptr stmt) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(i32, params, 2, false);

		ctx->rt_fntypes[RT_EXEC_BLOCK_PROTECTED] = ft;
		ctx->rt_funcs[RT_EXEC_BLOCK_PROTECTED] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exec_block_protected", ft);
	}

	/* i32 uplpgsql_rt_exec_dynexecute(ptr estate, ptr stmt) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(i32, params, 2, false);

		ctx->rt_fntypes[RT_EXEC_DYNEXECUTE] = ft;
		ctx->rt_funcs[RT_EXEC_DYNEXECUTE] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exec_dynexecute", ft);
	}

	/* i32 uplpgsql_rt_exec_call(ptr estate, ptr stmt) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(i32, params, 2, false);

		ctx->rt_fntypes[RT_EXEC_CALL] = ft;
		ctx->rt_funcs[RT_EXEC_CALL] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exec_call", ft);
	}

	/* void uplpgsql_rt_exec_getdiag(ptr estate, ptr stmt) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 2, false);

		ctx->rt_fntypes[RT_EXEC_GETDIAG] = ft;
		ctx->rt_funcs[RT_EXEC_GETDIAG] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exec_getdiag", ft);
	}

	/* i32 uplpgsql_rt_exec_return_next(ptr estate, ptr stmt) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(i32, params, 2, false);

		ctx->rt_fntypes[RT_EXEC_RETURN_NEXT] = ft;
		ctx->rt_funcs[RT_EXEC_RETURN_NEXT] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exec_return_next", ft);
	}

	/* i32 uplpgsql_rt_exec_return_query(ptr estate, ptr stmt) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(i32, params, 2, false);

		ctx->rt_fntypes[RT_EXEC_RETURN_QUERY] = ft;
		ctx->rt_funcs[RT_EXEC_RETURN_QUERY] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exec_return_query", ft);
	}

	/* void uplpgsql_rt_exec_commit(ptr estate, ptr stmt) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 2, false);

		ctx->rt_fntypes[RT_EXEC_COMMIT] = ft;
		ctx->rt_funcs[RT_EXEC_COMMIT] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exec_commit", ft);
	}

	/* void uplpgsql_rt_exec_rollback(ptr estate, ptr stmt) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 2, false);

		ctx->rt_fntypes[RT_EXEC_ROLLBACK] = ft;
		ctx->rt_funcs[RT_EXEC_ROLLBACK] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exec_rollback", ft);
	}

	/* i32 uplpgsql_rt_exec_foreach_a(ptr estate, ptr stmt) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(i32, params, 2, false);

		ctx->rt_fntypes[RT_EXEC_FOREACH_A] = ft;
		ctx->rt_funcs[RT_EXEC_FOREACH_A] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exec_foreach_a", ft);
	}

	/* ptr uplpgsql_rt_open_dynfors_cursor(ptr estate, ptr stmt) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(ptr, params, 2, false);

		ctx->rt_fntypes[RT_OPEN_DYNFORS_CURSOR] = ft;
		ctx->rt_funcs[RT_OPEN_DYNFORS_CURSOR] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_open_dynfors_cursor", ft);
	}

	/* --- Exception handling runtime functions --- */

	/* ptr uplpgsql_rt_exception_push_frame(ptr estate, ptr block) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(ptr, params, 2, false);

		ctx->rt_fntypes[RT_EXCEPTION_PUSH_FRAME] = ft;
		ctx->rt_funcs[RT_EXCEPTION_PUSH_FRAME] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exception_push_frame", ft);
	}

	/* void uplpgsql_rt_exception_arm(ptr estate, ptr frame) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 2, false);

		ctx->rt_fntypes[RT_EXCEPTION_ARM] = ft;
		ctx->rt_funcs[RT_EXCEPTION_ARM] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exception_arm", ft);
	}

	/* void uplpgsql_rt_exception_try_exit(ptr estate, ptr frame) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 2, false);

		ctx->rt_fntypes[RT_EXCEPTION_TRY_EXIT] = ft;
		ctx->rt_funcs[RT_EXCEPTION_TRY_EXIT] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exception_try_exit", ft);
	}

	/* i32 uplpgsql_rt_exception_catch(ptr estate, ptr block, ptr frame) */
	{
		LLVMTypeRef params[] = { ptr, ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(i32, params, 3, false);

		ctx->rt_fntypes[RT_EXCEPTION_CATCH] = ft;
		ctx->rt_funcs[RT_EXCEPTION_CATCH] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exception_catch", ft);
	}

	/* void uplpgsql_rt_exception_set_handler_vars(ptr estate, ptr block, i32 idx) */
	{
		LLVMTypeRef params[] = { ptr, ptr, i32 };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 3, false);

		ctx->rt_fntypes[RT_EXCEPTION_SET_HANDLER_VARS] = ft;
		ctx->rt_funcs[RT_EXCEPTION_SET_HANDLER_VARS] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exception_set_handler_vars", ft);
	}

	/* void uplpgsql_rt_exception_handler_done(ptr estate, ptr frame) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 2, false);

		ctx->rt_fntypes[RT_EXCEPTION_HANDLER_DONE] = ft;
		ctx->rt_funcs[RT_EXCEPTION_HANDLER_DONE] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exception_handler_done", ft);
	}

	/* void uplpgsql_rt_exception_rethrow(ptr estate, ptr frame) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 2, false);

		ctx->rt_fntypes[RT_EXCEPTION_RETHROW] = ft;
		ctx->rt_funcs[RT_EXCEPTION_RETHROW] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_exception_rethrow", ft);
	}

	/* void uplpgsql_rt_assign_var_datum(ptr estate, i32 dno, i64 value, i8 isnull) */
	{
		LLVMTypeRef i8t = ctx->types[UPLPGSQL_INT8];
		LLVMTypeRef params[] = { ptr, i32, i64, i8t };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 4, false);

		ctx->rt_fntypes[RT_ASSIGN_VAR_DATUM] = ft;
		ctx->rt_funcs[RT_ASSIGN_VAR_DATUM] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_assign_var_datum", ft);
	}

	/* void uplpgsql_rt_copy_assign_var_datum(ptr estate, i32 dno, i64 value, i8 isnull) */
	{
		LLVMTypeRef i8t = ctx->types[UPLPGSQL_INT8];
		LLVMTypeRef params[] = { ptr, i32, i64, i8t };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 4, false);

		ctx->rt_fntypes[RT_COPY_ASSIGN_VAR_DATUM] = ft;
		ctx->rt_funcs[RT_COPY_ASSIGN_VAR_DATUM] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_copy_assign_var_datum", ft);
	}

	/* ptr uplpgsql_rt_alloc_scope_enter(ptr estate) */
	{
		LLVMTypeRef params[] = { ptr };
		LLVMTypeRef ft = LLVMFunctionType(ptr, params, 1, false);

		ctx->rt_fntypes[RT_ALLOC_SCOPE_ENTER] = ft;
		ctx->rt_funcs[RT_ALLOC_SCOPE_ENTER] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_alloc_scope_enter", ft);
	}

	/* void uplpgsql_rt_alloc_scope_exit(ptr estate, ptr old) */
	{
		LLVMTypeRef params[] = { ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 2, false);

		ctx->rt_fntypes[RT_ALLOC_SCOPE_EXIT] = ft;
		ctx->rt_funcs[RT_ALLOC_SCOPE_EXIT] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_alloc_scope_exit", ft);
	}

	/* void uplpgsql_rt_copy_assign_var_datum_scoped(ptr, i32, i64, i8, ptr) */
	{
		LLVMTypeRef i8t = ctx->types[UPLPGSQL_INT8];
		LLVMTypeRef params[] = { ptr, i32, i64, i8t, ptr };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 5, false);

		ctx->rt_fntypes[RT_COPY_ASSIGN_VAR_DATUM_SCOPED] = ft;
		ctx->rt_funcs[RT_COPY_ASSIGN_VAR_DATUM_SCOPED] = LLVMAddFunction(
			ctx->module, "uplpgsql_rt_copy_assign_var_datum_scoped", ft);
	}

	/* i64 uplpgsql_rt_get_recfield(ptr estate, i32 recfield_dno) */
	{
		LLVMTypeRef params[] = { ptr, i32 };
		LLVMTypeRef ft = LLVMFunctionType(i64, params, 2, false);

		ctx->rt_fntypes[RT_GET_RECFIELD] = ft;
		ctx->rt_funcs[RT_GET_RECFIELD] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_get_recfield", ft);
	}

	/*
	 * Datum uplpgsql_rt_get_recfield_fast(ptr estate, i32 rec_dno,
	 *     i32 fnumber, ptr isnull_out)
	 */
	{
		LLVMTypeRef params[] = { ptr, i32, i32, ptr };
		LLVMTypeRef ft = LLVMFunctionType(i64, params, 4, false);

		ctx->rt_fntypes[RT_GET_RECFIELD_FAST] = ft;
		ctx->rt_funcs[RT_GET_RECFIELD_FAST] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_get_recfield_fast", ft);
	}

	/*
	 * Datum uplpgsql_rt_array_get_element(ptr estate, i32 dno, i32 subscript,
	 *     i32 typlen, i16 elmlen, i1 elmbyval, i8 elmalign, ptr isNull)
	 */
	{
		LLVMTypeRef params[] = { ptr, i32, i32, i32, i16, i1, i8, ptr };
		LLVMTypeRef ft = LLVMFunctionType(i64, params, 8, false);

		ctx->rt_fntypes[RT_ARRAY_GET_ELEMENT] = ft;
		ctx->rt_funcs[RT_ARRAY_GET_ELEMENT] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_array_get_element", ft);
	}

	/*
	 * void uplpgsql_rt_array_set_element(ptr estate, i32 dno, i32 subscript,
	 *     i64 value, i1 isnull, i32 typlen, i32 elemtype,
	 *     i16 elmlen, i1 elmbyval, i8 elmalign)
	 */
	{
		LLVMTypeRef params[] = { ptr, i32, i32, i64, i1, i32, i32, i16, i1, i8 };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 10, false);

		ctx->rt_fntypes[RT_ARRAY_SET_ELEMENT] = ft;
		ctx->rt_funcs[RT_ARRAY_SET_ELEMENT] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_array_set_element", ft);
	}

	/*
	 * Phase 7: Native local array heap allocation.
	 * Called when byte_size > NATIVE_ARRAY_STACK_THRESHOLD.
	 * void *uplpgsql_rt_native_array_alloc(ptr estate, i64 byte_size)
	 */
	{
		LLVMTypeRef params[] = { ptr, i64 };
		LLVMTypeRef ft = LLVMFunctionType(ptr, params, 2, false);

		ctx->rt_fntypes[RT_NATIVE_ARRAY_ALLOC] = ft;
		ctx->rt_funcs[RT_NATIVE_ARRAY_ALLOC] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_native_array_alloc", ft);
	}

	/*
	 * Phase 7: Native local array bounds check error path.
	 * Called from inline bounds checks when subscript is out of [1, length].
	 * void uplpgsql_rt_native_array_bounds_check(i32 subscript, i32 length)
	 */
	{
		LLVMTypeRef params[] = { i32, i32 };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 2, false);

		ctx->rt_fntypes[RT_NATIVE_ARRAY_BOUNDS_CHECK] = ft;
		ctx->rt_funcs[RT_NATIVE_ARRAY_BOUNDS_CHECK] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_native_array_bounds_check", ft);
	}

	/* ptr uplpgsql_rt_native_array_from_datum(ptr estate, i64 datum, i32 isnull, i32 elem_size, ptr out_nelems, ptr out_lb) */
	{
		LLVMTypeRef params[] = { ptr, i64, i32, i32, ptr, ptr, ptr };
		LLVMTypeRef ft = LLVMFunctionType(ptr, params, 7, false);

		ctx->rt_fntypes[RT_NATIVE_ARRAY_FROM_DATUM] = ft;
		ctx->rt_funcs[RT_NATIVE_ARRAY_FROM_DATUM] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_native_array_from_datum", ft);
	}

	/* void uplpgsql_rt_free_var_datum(ptr estate, i32 dno) */
	{
		LLVMTypeRef params[] = { ptr, i32 };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 2, false);

		ctx->rt_fntypes[RT_FREE_VAR_DATUM] = ft;
		ctx->rt_funcs[RT_FREE_VAR_DATUM] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_free_var_datum", ft);
	}

	/* void uplpgsql_rt_native_array_to_datum(ptr estate, i32 varno, ptr data, i32 nelems, i32 lb, i32 elemtype, i32 elem_size) */
	{
		LLVMTypeRef params[] = { ptr, i32, ptr, i32, i32, ptr, i32, i32 };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 8, false);

		ctx->rt_fntypes[RT_NATIVE_ARRAY_TO_DATUM] = ft;
		ctx->rt_funcs[RT_NATIVE_ARRAY_TO_DATUM] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_native_array_to_datum", ft);
	}

	/*
	 * void uplpgsql_rt_native_array_reserve(ptr estate, ptr data_io,
	 *     ptr nulls_io, ptr is_heap_io, ptr cap_io, i32 len, i32 elem_size)
	 *
	 * The pointer arguments are the JIT'd code's own alloca slots: the
	 * helper grows the buffers and writes the new pointers/capacity back
	 * through them.
	 */
	{
		LLVMTypeRef params[] = { ptr, ptr, ptr, ptr, ptr, i32, i32 };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 7, false);

		ctx->rt_fntypes[RT_NATIVE_ARRAY_RESERVE] = ft;
		ctx->rt_funcs[RT_NATIVE_ARRAY_RESERVE] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_native_array_reserve", ft);
	}

	/* void uplpgsql_rt_native_array_release(ptr data, ptr nulls, i8 is_heap) */
	{
		LLVMTypeRef i8t = ctx->types[UPLPGSQL_INT8];
		LLVMTypeRef params[] = { ptr, ptr, i8t };
		LLVMTypeRef ft = LLVMFunctionType(vd, params, 3, false);

		ctx->rt_fntypes[RT_NATIVE_ARRAY_RELEASE] = ft;
		ctx->rt_funcs[RT_NATIVE_ARRAY_RELEASE] = LLVMAddFunction(ctx->module,
			"uplpgsql_rt_native_array_release", ft);
	}
}

/*
 * Compile a list of statements
 */
static void
uplpgsql_compile_stmts(UPLpgSQL_compile_ctx *ctx, List *stmts)
{
	ListCell *lc;

	foreach(lc, stmts)
	{
		UPLpgSQL_stmt *stmt = (UPLpgSQL_stmt *) lfirst(lc);

		uplpgsql_compile_stmt(ctx, stmt);
	}
}

/*
 * Compile a single statement (dispatch)
 */
static void
uplpgsql_compile_stmt(UPLpgSQL_compile_ctx *ctx, UPLpgSQL_stmt *stmt)
{
	/*
	 * Store the current statement pointer into plstate->err_stmt so that
	 * the error context callback can report the line number if a runtime
	 * helper raises an error.  This mirrors exec_stmts() in the interpreter.
	 */
	{
		LLVMValueRef off, gep, stmt_ptr;

		off = LLVMConstInt(ctx->types[UPLPGSQL_INT64],
						   offsetof(UPLpgSQL_execstate, err_stmt),
						   false);
		gep = LLVMBuildGEP2(ctx->builder, ctx->types[UPLPGSQL_INT8],
							ctx_plstate(ctx), &off, 1, "err_stmt.ptr");
		stmt_ptr = llvm_const_ptr(ctx, (void *) stmt);
		LLVMBuildStore(ctx->builder, stmt_ptr, gep);
	}

	switch (stmt->cmd_type)
	{
		case UPLPGSQL_STMT_BLOCK:
			uplpgsql_compile_block(ctx, (UPLpgSQL_stmt_block *) stmt);
			break;
		case UPLPGSQL_STMT_RETURN:
			uplpgsql_compile_return(ctx, (UPLpgSQL_stmt_return *) stmt);
			break;
		case UPLPGSQL_STMT_ASSIGN:
			uplpgsql_compile_assign(ctx, (UPLpgSQL_stmt_assign *) stmt);
			break;
		case UPLPGSQL_STMT_IF:
			uplpgsql_compile_if(ctx, (UPLpgSQL_stmt_if *) stmt);
			break;
		case UPLPGSQL_STMT_WHILE:
			uplpgsql_compile_while(ctx, (UPLpgSQL_stmt_while *) stmt);
			break;
		case UPLPGSQL_STMT_LOOP:
			uplpgsql_compile_loop(ctx, (UPLpgSQL_stmt_loop *) stmt);
			break;
		case UPLPGSQL_STMT_FORI:
			uplpgsql_compile_fori(ctx, (UPLpgSQL_stmt_fori *) stmt);
			break;
		case UPLPGSQL_STMT_EXIT:
			uplpgsql_compile_exit(ctx, (UPLpgSQL_stmt_exit *) stmt);
			break;
		case UPLPGSQL_STMT_PERFORM:
			uplpgsql_compile_perform(ctx, (UPLpgSQL_stmt_perform *) stmt);
			break;
		case UPLPGSQL_STMT_EXECSQL:
			uplpgsql_compile_execsql(ctx, (UPLpgSQL_stmt_execsql *) stmt);
			break;
		case UPLPGSQL_STMT_RAISE:
			uplpgsql_compile_raise(ctx, (UPLpgSQL_stmt_raise *) stmt);
			break;
		case UPLPGSQL_STMT_CASE:
			uplpgsql_compile_case(ctx, (UPLpgSQL_stmt_case *) stmt);
			break;
		case UPLPGSQL_STMT_ASSERT:
			uplpgsql_compile_assert(ctx, (UPLpgSQL_stmt_assert *) stmt);
			break;
		case UPLPGSQL_STMT_OPEN:
			uplpgsql_compile_open(ctx, (UPLpgSQL_stmt_open *) stmt);
			break;
		case UPLPGSQL_STMT_FETCH:
			uplpgsql_compile_fetch(ctx, (UPLpgSQL_stmt_fetch *) stmt);
			break;
		case UPLPGSQL_STMT_CLOSE:
			uplpgsql_compile_close(ctx, (UPLpgSQL_stmt_close *) stmt);
			break;
		case UPLPGSQL_STMT_FORS:
			uplpgsql_compile_fors(ctx, (UPLpgSQL_stmt_fors *) stmt);
			break;
		case UPLPGSQL_STMT_FORC:
			uplpgsql_compile_forc(ctx, (UPLpgSQL_stmt_forc *) stmt);
			break;
		case UPLPGSQL_STMT_DYNEXECUTE:
			uplpgsql_compile_dynexecute(ctx, (UPLpgSQL_stmt_dynexecute *) stmt);
			break;
		case UPLPGSQL_STMT_DYNFORS:
			uplpgsql_compile_dynfors(ctx, (UPLpgSQL_stmt_dynfors *) stmt);
			break;
		case UPLPGSQL_STMT_FOREACH_A:
			uplpgsql_compile_foreach_a(ctx, (UPLpgSQL_stmt_foreach_a *) stmt);
			break;
		case UPLPGSQL_STMT_RETURN_NEXT:
			uplpgsql_compile_return_next(ctx, (UPLpgSQL_stmt_return_next *) stmt);
			break;
		case UPLPGSQL_STMT_RETURN_QUERY:
			uplpgsql_compile_return_query(ctx, (UPLpgSQL_stmt_return_query *) stmt);
			break;
		case UPLPGSQL_STMT_CALL:
			uplpgsql_compile_call(ctx, (UPLpgSQL_stmt_call *) stmt);
			break;
		case UPLPGSQL_STMT_GETDIAG:
			uplpgsql_compile_getdiag(ctx, (UPLpgSQL_stmt_getdiag *) stmt);
			break;
		case UPLPGSQL_STMT_COMMIT:
			uplpgsql_compile_commit(ctx, (UPLpgSQL_stmt_commit *) stmt);
			break;
		case UPLPGSQL_STMT_ROLLBACK:
			uplpgsql_compile_rollback(ctx, (UPLpgSQL_stmt_rollback *) stmt);
			break;
		default:
			elog(ERROR, "uplpgsql: unsupported statement type %d",
				 stmt->cmd_type);
			break;
	}
}

/*
 * Compile a BLOCK statement.
 *
 * If the block has exception handlers, we emit native LLVM IR that calls
 * sigsetjmp directly.  The pattern is:
 *
 *   frame = rt_exception_push_frame(estate, block)   // alloc, begin subtxn
 *   rc = sigsetjmp(frame, 0)                         // returns_twice
 *   if (rc == 0) goto try_enter else goto catch_enter
 *
 *   try_enter:
 *     rt_exception_arm(estate, frame)                 // set PG_exception_stack
 *     <init vars, compiled body>
 *     goto try_exit
 *
 *   exception_return_bb:                              // RETURN inside try body
 *     rt_exception_try_exit(estate, frame)
 *     goto real_return_bb
 *
 *   try_exit:
 *     rt_exception_try_exit(estate, frame)
 *     goto after_block
 *
 *   catch_enter:
 *     handler_idx = rt_exception_catch(estate, block, frame)
 *     switch handler_idx [0->h0, 1->h1, ..., default->rethrow]
 *
 *   handler_N:
 *     rt_exception_set_handler_vars(estate, block, N)
 *     <compiled handler body>
 *     rt_exception_handler_done(estate, frame)
 *     goto after_block
 *
 *   rethrow:
 *     rt_exception_rethrow(estate, frame)
 *     unreachable
 *
 * Otherwise, variables are initialized inline and body is compiled normally.
 */
static void
uplpgsql_compile_block(UPLpgSQL_compile_ctx *ctx, UPLpgSQL_stmt_block *stmt)
{
	/*
	 * Initialize the block's variables here instead of letting the core do
	 * it, so native arrays get refreshed from the Datum their DECLARE
	 * default writes; then pass n_initvars = 0 so the core does not repeat
	 * the loop.  Blocks with exception handlers are delegated whole to
	 * uplpgsql_compile_block_exceptions(), which does its own init inside
	 * the TRY — the core ignores n_initvars on that path.
	 */
	if (stmt->exceptions != NULL)
	{
		/*
		 * The exception path pushes its own block label, because there an
		 * EXIT must route through the subtransaction release rather than
		 * branch past it.
		 */
		upl_emit_block(ctx, 0, NULL, stmt->body, true, stmt,
					   uplpgsql_compile_block_exceptions);
		return;
	}

	uplpgsql_emit_init_vars(ctx, stmt->n_initvars, stmt->initvarnos);

	/*
	 * A labeled block is an EXIT target as well as a loop is: "EXIT <label>"
	 * on a named block leaves the block and resumes after it.
	 */
	if (stmt->label != NULL)
	{
		LLVMBasicBlockRef end_bb = upl_append_block(ctx, "block.end");

		upl_push_block_label(ctx, stmt->label, end_bb);

		upl_emit_block(ctx, 0, NULL, stmt->body, false, stmt,
					   uplpgsql_compile_block_exceptions);

		upl_pop_loop(ctx);

		if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx->builder)) == NULL)
			LLVMBuildBr(ctx->builder, end_bb);
		LLVMPositionBuilderAtEnd(ctx->builder, end_bb);
	}
	else
		upl_emit_block(ctx, 0, NULL, stmt->body, false, stmt,
					   uplpgsql_compile_block_exceptions);
}

/*
 * Compile a RETURN statement
 *
 * Delegates to uplpgsql_rt_exec_return which calls exec_stmt_return.
 * This handles all RETURN variants (expression, variable, void, composite).
 * The runtime helper sets retval/retisnull/rettype in the PL/pgSQL estate.
 */

/*
 * Sync all active native arrays to PG Datum variables.
 *
 * Called lazily before runtime helpers that evaluate expressions via
 * the interpreter (RETURN, RAISE, SPI).  Builds a PG array Datum
 * from the flat native memory so the interpreter sees valid data.
 * This avoids the cost of syncing on every subscript write.
 */
void
uplpgsql_emit_sync_native_array(UPLpgSQL_compile_ctx *ctx,
								UPLpgSQL_native_array *na)
{
	LLVMValueRef estate_ref = LLVMGetParam(ctx->function, 0);
	LLVMTypeRef i32_ty = ctx->types[UPLPGSQL_INT32];
	LLVMValueRef data, len, lb, nulls;
	LLVMValueRef args[8];

	data = LLVMBuildLoad2(ctx->builder, ctx->types[UPLPGSQL_PTR],
						  na->data_ptr, "sync.data");
	len = LLVMBuildLoad2(ctx->builder, i32_ty,
						 na->len_ptr, "sync.len");
	lb = LLVMBuildLoad2(ctx->builder, i32_ty, na->lb_ptr, "sync.lb");
	nulls = LLVMBuildLoad2(ctx->builder, ctx->types[UPLPGSQL_PTR],
						   na->nulls_ptr, "sync.nulls");

	args[0] = estate_ref;
	args[1] = LLVMConstInt(i32_ty, na->dno, false);
	args[2] = data;
	args[3] = len;
	args[4] = lb;
	args[5] = nulls;
	args[6] = LLVMConstInt(i32_ty, na->elemtype, false);
	args[7] = LLVMConstInt(i32_ty, na->elem_size, false);

	LLVMBuildCall2(ctx->builder,
		ctx->rt_fntypes[RT_NATIVE_ARRAY_TO_DATUM],
		ctx->rt_funcs[RT_NATIVE_ARRAY_TO_DATUM],
		args, 8, "");
}

static void
uplpgsql_sync_native_arrays(UPLpgSQL_compile_ctx *ctx)
{
	int		na_i;

	for (na_i = 0; na_i < ctx_num_native_arrays(ctx); na_i++)
		uplpgsql_emit_sync_native_array(ctx, &ctx_native_arrays(ctx)[na_i]);
}

/*
 * Marshal only the native arrays that an expression reads.
 *
 * The interpreter reads variables as Datums, so any native array the
 * expression touches must be marshalled out to its Datum first — but only
 * those.  Syncing the rest costs a full copy of each, per execution, for
 * data the callee cannot even see: three "least(greatest(x,0),1)" clamps per
 * pixel dragged a 6912-element image array through construct_md_array every
 * time, and cost more than the path tracer around them.
 *
 * paramnos is exactly the set of variables the expression reads, and it is
 * what escape analysis already trusts for RETURN/RAISE/FOREACH.  A NULL expr
 * means we cannot tell, so fall back to marshalling everything.
 */
static void
uplpgsql_sync_native_arrays_for_expr(UPLpgSQL_compile_ctx *ctx,
									 UPLpgSQL_expr *expr)
{
	int		na_i;

	if (expr == NULL)
	{
		uplpgsql_sync_native_arrays(ctx);
		return;
	}

	for (na_i = 0; na_i < ctx_num_native_arrays(ctx); na_i++)
	{
		UPLpgSQL_native_array *na = &ctx_native_arrays(ctx)[na_i];

		if (expr_references_dno(expr, na->dno))
			uplpgsql_emit_sync_native_array(ctx, na);
	}
}

/*
 * Look up native array metadata by dno, or NULL if dno is not one.
 */
static UPLpgSQL_native_array *
uplpgsql_find_native_array(UPLpgSQL_compile_ctx *ctx, int dno)
{
	int		na_i;

	for (na_i = 0; na_i < ctx_num_native_arrays(ctx); na_i++)
	{
		if (ctx_native_arrays(ctx)[na_i].dno == dno)
			return &ctx_native_arrays(ctx)[na_i];
	}

	return NULL;
}

/*
 * Reload one native array's flat memory from its PG Datum variable.
 *
 * The inverse of uplpgsql_sync_native_arrays().  Called after the
 * interpreter has written a whole array Datum into the variable (SELECT
 * INTO, or an ASSIGN that fell back to exec_assign_expr), which leaves
 * data_ptr/len_ptr pointing at stale memory.  Without this, a later
 * native subscript read would return the pre-assignment contents.
 */
void
uplpgsql_emit_refresh_native_array(UPLpgSQL_compile_ctx *ctx,
								   UPLpgSQL_native_array *na)
{
	LLVMValueRef estate_ref = LLVMGetParam(ctx->function, 0);
	LLVMTypeRef	 i32_ty = ctx->types[UPLPGSQL_INT32];
	LLVMValueRef datum_val, isnull_val, new_data, new_len, new_lb;
	LLVMValueRef nelems_ptr, lb_ptr, nulls_out_ptr, new_nulls;

	datum_val = upl_emit_load_var_datum(ctx, estate_ref, na->dno);
	isnull_val = upl_emit_load_var_isnull(ctx, estate_ref, na->dno);

	nelems_ptr = LLVMBuildAlloca(ctx->builder, i32_ty, "na_nelems");
	lb_ptr = LLVMBuildAlloca(ctx->builder, i32_ty, "na_lb");
	nulls_out_ptr = LLVMBuildAlloca(ctx->builder, ctx->types[UPLPGSQL_PTR],
									"na_nulls_out");
	isnull_val = LLVMBuildZExt(ctx->builder, isnull_val, i32_ty, "isnull_i32");

	/*
	 * The mirror is being replaced wholesale, so release the old buffers
	 * first — they have no other referent.  Without this every refresh
	 * leaked the previous copy into datum_context, and a loop of
	 * out-of-range writes (each one a sync/set/refresh round trip)
	 * accumulated O(n^2) bytes over the call.  The array_fill stack buffer
	 * is skipped via the is_heap flag.
	 */
	{
		LLVMValueRef rel_args[3];

		rel_args[0] = LLVMBuildLoad2(ctx->builder, ctx->types[UPLPGSQL_PTR],
									 na->data_ptr, "na.old_data");
		rel_args[1] = LLVMBuildLoad2(ctx->builder, ctx->types[UPLPGSQL_PTR],
									 na->nulls_ptr, "na.old_nulls");
		rel_args[2] = LLVMBuildLoad2(ctx->builder, ctx->types[UPLPGSQL_INT8],
									 na->is_heap_ptr, "na.old_onheap");

		LLVMBuildCall2(ctx->builder,
			ctx->rt_fntypes[RT_NATIVE_ARRAY_RELEASE],
			ctx->rt_funcs[RT_NATIVE_ARRAY_RELEASE],
			rel_args, 3, "");
	}

	{
		LLVMValueRef call_args[] = {
			estate_ref,
			datum_val,
			isnull_val,
			LLVMConstInt(i32_ty, na->elem_size, false),
			nelems_ptr,
			lb_ptr,
			nulls_out_ptr
		};

		new_data = LLVMBuildCall2(ctx->builder,
			ctx->rt_fntypes[RT_NATIVE_ARRAY_FROM_DATUM],
			ctx->rt_funcs[RT_NATIVE_ARRAY_FROM_DATUM],
			call_args, 7, "na.from_datum");
	}

	LLVMBuildStore(ctx->builder, new_data, na->data_ptr);
	new_len = LLVMBuildLoad2(ctx->builder, i32_ty, nelems_ptr, "na.new_len");
	LLVMBuildStore(ctx->builder, new_len, na->len_ptr);
	new_lb = LLVMBuildLoad2(ctx->builder, i32_ty, lb_ptr, "na.new_lb");
	LLVMBuildStore(ctx->builder, new_lb, na->lb_ptr);
	new_nulls = LLVMBuildLoad2(ctx->builder, ctx->types[UPLPGSQL_PTR],
							   nulls_out_ptr, "na.new_nulls");
	LLVMBuildStore(ctx->builder, new_nulls, na->nulls_ptr);

	/*
	 * from_datum allocates exactly len elements, on the heap.  (len can be 0
	 * or -1 with a NULL data pointer; the capacity is never consulted then,
	 * because the append test requires len >= 0 and a reserve call fixes the
	 * buffers up before anything is stored.)
	 */
	LLVMBuildStore(ctx->builder, new_len, na->cap_ptr);
	LLVMBuildStore(ctx->builder,
				   LLVMConstInt(ctx->types[UPLPGSQL_INT8], 1, false),
				   na->is_heap_ptr);
}

static void
uplpgsql_compile_return(UPLpgSQL_compile_ctx *ctx, UPLpgSQL_stmt_return *stmt)
{
	/* Sync native arrays before interpreter evaluates RETURN expression */
	if (ctx_num_native_arrays(ctx) > 0)
		uplpgsql_sync_native_arrays(ctx);

	upl_emit_return(ctx, RT_EXEC_RETURN, stmt);
}

/*
 * Compile an ASSIGN statement: varno := expr
 */
static void
uplpgsql_compile_assign(UPLpgSQL_compile_ctx *ctx, UPLpgSQL_stmt_assign *stmt)
{
	UPLpgSQL_native_array *target_na;

	/* Try to inline as native int4 arithmetic */
	if (uplpgsql_try_compile_assign(ctx, stmt))
		return;

	/*
	 * Read escape (y := x) is handled by uplpgsql_call_exec below, which
	 * syncs native arrays before entering the interpreter.
	 */

	/* Fall back to direct exec_assign_expr(plstate, datums[dno], expr) */
	{
		LLVMValueRef off, datums_gep, datums_ptr, elem_gep, datum_ptr;
		LLVMValueRef args[3];

		/* Load plstate->datums */
		off = LLVMConstInt(ctx->types[UPLPGSQL_INT64],
						   offsetof(UPLpgSQL_execstate, datums), false);
		datums_gep = LLVMBuildGEP2(ctx->builder, ctx->types[UPLPGSQL_INT8],
								   ctx_plstate(ctx), &off, 1, "datums.ptr");
		datums_ptr = LLVMBuildLoad2(ctx->builder, ctx->types[UPLPGSQL_PTR],
									datums_gep, "datums");

		/* Load datums[dno] */
		off = LLVMConstInt(ctx->types[UPLPGSQL_INT64],
						   stmt->varno * sizeof(void *), false);
		elem_gep = LLVMBuildGEP2(ctx->builder, ctx->types[UPLPGSQL_INT8],
								 datums_ptr, &off, 1, "datum.gep");
		datum_ptr = LLVMBuildLoad2(ctx->builder, ctx->types[UPLPGSQL_PTR],
								  elem_gep, "datum");

		args[0] = ctx_plstate(ctx);
		args[1] = datum_ptr;
		args[2] = llvm_const_ptr(ctx, stmt->expr);

		/*
		 * exec_assign_expr reads exactly what stmt->expr reads, so marshal
		 * only that.  An assignment whose value happens not to compile must
		 * not drag every other array in the function through a round trip.
		 */
		uplpgsql_sync_native_arrays_for_expr(ctx, stmt->expr);
		uplpgsql_call_exec_nosync(ctx, (void *) exec_assign_expr,
								  ctx->types[UPLPGSQL_VOID], args, 3);
	}

	/*
	 * Write escape: if the target is itself a native array, the interpreter
	 * just stored a whole PG array Datum into it (x := ARRAY[...]), leaving
	 * data_ptr/len_ptr stale.  Reload flat memory from the new Datum so
	 * later native subscript reads see it.
	 */
	target_na = uplpgsql_find_native_array(ctx, stmt->varno);
	if (target_na != NULL)
	{
		elog(DEBUG1, "uplpgsql: native array from_datum dno %d (ASSIGN)",
			 target_na->dno);
		uplpgsql_emit_refresh_native_array(ctx, target_na);
	}
}

/*
 * Compile an IF / ELSIF / ELSE statement
 */
static void
uplpgsql_compile_if(UPLpgSQL_compile_ctx *ctx, UPLpgSQL_stmt_if *stmt)
{
	int			num_elsifs = list_length(stmt->elsif_list);
	void	  **elsif_conds = NULL;
	void	  **elsif_bodies = NULL;

	/* Extract elsif list to arrays for core primitive */
	if (num_elsifs > 0)
	{
		ListCell   *lc;
		int			i = 0;

		elsif_conds = palloc(sizeof(void *) * num_elsifs);
		elsif_bodies = palloc(sizeof(void *) * num_elsifs);

		foreach(lc, stmt->elsif_list)
		{
			UPLpgSQL_if_elsif *elsif = (UPLpgSQL_if_elsif *) lfirst(lc);

			elsif_conds[i] = elsif->cond;
			elsif_bodies[i] = elsif->stmts;
			i++;
		}
	}

	upl_emit_if(ctx,
				stmt->cond,
				stmt->then_body,
				num_elsifs,
				elsif_conds,
				elsif_bodies,
				stmt->else_body != NIL ? stmt->else_body : NULL);

	if (elsif_conds)
		pfree(elsif_conds);
	if (elsif_bodies)
		pfree(elsif_bodies);
}

/*
 * Compile a WHILE loop — native LLVM loop.
 *
 * Structure:
 *   while.cond:  eval condition via RT_EVAL_BOOL → branch
 *   while.body:  compiled statements → branch to while.cond
 *   while.exit:  continue after loop
 */
static void
uplpgsql_compile_while(UPLpgSQL_compile_ctx *ctx, UPLpgSQL_stmt_while *stmt)
{
	upl_emit_cond_loop(ctx, stmt->label, stmt->cond, stmt->test_at_top, stmt->body);
}

/*
 * Compile a LOOP (unconditional) — native LLVM loop.
 *
 * Structure:
 *   loop.body:  compiled statements → branch to loop.body
 *   loop.exit:  continue after loop (reached via EXIT statement)
 */
static void
uplpgsql_compile_loop(UPLpgSQL_compile_ctx *ctx, UPLpgSQL_stmt_loop *stmt)
{
	upl_emit_loop(ctx, stmt->label, stmt->body);
}

/*
 * Compile a FOR integer-range loop — native LLVM loop.
 *
 * Evaluates lower/upper/step bounds once via runtime helpers, then the
 * actual loop (compare, branch, increment) is pure LLVM IR.
 *
 * Structure:
 *   fori.setup:  evaluate bounds, initialize loop_val alloca
 *   fori.cond:   compare loop_val vs upper → branch
 *   fori.body:   assign loop var, compile body
 *   fori.step:   increment/decrement with overflow check → branch to cond
 *   fori.exit:   set FOUND, continue
 */
static void
uplpgsql_compile_fori(UPLpgSQL_compile_ctx *ctx, UPLpgSQL_stmt_fori *stmt)
{
	upl_emit_fori(ctx, stmt->label,
				  stmt->var->dno,
				  stmt->lower, stmt->upper, stmt->step,
				  stmt->reverse,
				  stmt->body);
}

/*
 * Compile EXIT / CONTINUE statement.
 *
 * EXIT branches to the loop's exit_bb.
 * CONTINUE branches to the loop's continue_bb.
 * If there's a condition, wrap in a conditional branch.
 * If there's a label, find the matching loop on the stack.
 */
static void
uplpgsql_compile_exit(UPLpgSQL_compile_ctx *ctx, UPLpgSQL_stmt_exit *stmt)
{
	upl_emit_loop_exit(ctx, stmt->label, stmt->is_exit, stmt->cond);
}

/*
 * Compile PERFORM statement — delegates to runtime.
 */
static void
uplpgsql_compile_perform(UPLpgSQL_compile_ctx *ctx, UPLpgSQL_stmt_perform *stmt)
{
	LLVMValueRef args[] = {
		ctx_plstate(ctx),
		llvm_const_ptr(ctx, stmt)
	};

	uplpgsql_call_exec(ctx, (void *) exec_stmt_perform,
					   ctx->types[UPLPGSQL_INT32], args, 2);
}

/*
 * Compile EXECSQL statement
 */
static void
uplpgsql_compile_execsql(UPLpgSQL_compile_ctx *ctx, UPLpgSQL_stmt_execsql *stmt)
{
	LLVMValueRef exec_args[] = {
		ctx_plstate(ctx),
		llvm_const_ptr(ctx, stmt)
	};

	uplpgsql_call_exec(ctx, (void *) exec_stmt_execsql,
					   ctx->types[UPLPGSQL_INT32], exec_args, 2);

	/*
	 * If the SELECT INTO target is a native array, decompose the PG array
	 * Datum that exec_stmt_execsql stored into the variable into flat
	 * native memory.  The interpreter stores a normal PG array Datum, but
	 * native array subscript reads expect data_ptr/len_ptr to point to
	 * flat element memory.
	 */
	if (stmt->into && stmt->target != NULL)
	{

		/*
		 * Check if the INTO target (or any variable it wraps) is a
		 * native array.  The target may be a VAR (direct) or a ROW
		 * wrapping one or more VARs.  For multi-target INTO (ROW with
		 * multiple fields), check each field.
		 */
		int		target_dnos[64];
		int		num_targets = 0;
		int		ti;

		if (stmt->target->dtype == UPLPGSQL_DTYPE_ROW)
		{
			UPLpgSQL_row *row = (UPLpgSQL_row *) stmt->target;
			int		fi;

			for (fi = 0; fi < row->nfields && fi < 64; fi++)
				target_dnos[num_targets++] = row->varnos[fi];
		}
		else
		{
			target_dnos[num_targets++] = stmt->target->dno;
		}

		for (ti = 0; ti < num_targets; ti++)
		{
			UPLpgSQL_native_array *na;

			na = uplpgsql_find_native_array(ctx, target_dnos[ti]);
			if (na != NULL)
			{
				elog(DEBUG1, "uplpgsql: native array from_datum dno %d "
					 "(SELECT INTO)", na->dno);
				uplpgsql_emit_refresh_native_array(ctx, na);
			}
		}
	}
}

/*
 * Compile RAISE statement
 */
static void
uplpgsql_compile_raise(UPLpgSQL_compile_ctx *ctx, UPLpgSQL_stmt_raise *stmt)
{
	LLVMValueRef args[] = {
		ctx_plstate(ctx),
		llvm_const_ptr(ctx, stmt)
	};

	uplpgsql_call_exec(ctx, (void *) exec_stmt_raise,
					   ctx->types[UPLPGSQL_VOID], args, 2);
}

/*
 * Compile a cursor OPEN statement.
 *
 * Delegates directly to exec_stmt_open for portal creation. The three
 * OPEN variants (static query, dynamic query, explicit cursor) are
 * handled by the exec function based on stmt fields set at parse time.
 */
static void
uplpgsql_compile_open(UPLpgSQL_compile_ctx *ctx, UPLpgSQL_stmt_open *stmt)
{
	LLVMValueRef args[] = {
		ctx_plstate(ctx),
		llvm_const_ptr(ctx, stmt)
	};

	uplpgsql_call_exec(ctx, (void *) exec_stmt_open,
					   ctx->types[UPLPGSQL_INT32], args, 2);
}

/*
 * Compile a cursor FETCH/MOVE statement.
 *
 * Delegates directly to exec_stmt_fetch which handles cursor lookup,
 * row fetching, target assignment, and FOUND variable setting.
 */
static void
uplpgsql_compile_fetch(UPLpgSQL_compile_ctx *ctx, UPLpgSQL_stmt_fetch *stmt)
{
	LLVMValueRef args[] = {
		ctx_plstate(ctx),
		llvm_const_ptr(ctx, stmt)
	};

	uplpgsql_call_exec(ctx, (void *) exec_stmt_fetch,
					   ctx->types[UPLPGSQL_INT32], args, 2);
}

/*
 * Compile a cursor CLOSE statement.
 */
static void
uplpgsql_compile_close(UPLpgSQL_compile_ctx *ctx, UPLpgSQL_stmt_close *stmt)
{
	LLVMValueRef args[] = {
		ctx_plstate(ctx),
		llvm_const_ptr(ctx, stmt)
	};

	uplpgsql_call_exec(ctx, (void *) exec_stmt_close,
					   ctx->types[UPLPGSQL_INT32], args, 2);
}

/*
 * Compile an ASSERT statement.
 *
 * Evaluates the condition via RT_EVAL_BOOL, then branches:
 * true → continue, false/null → call RT_EXEC_ASSERT_FAIL which
 * checks the GUC, evaluates the optional message, and raises ERROR.
 *
 * Note: we always evaluate the condition even if asserts are disabled.
 * The runtime helper swallows the failure in that case. This avoids
 * needing to embed a GUC pointer in the IR.
 */
static void
uplpgsql_compile_assert(UPLpgSQL_compile_ctx *ctx, UPLpgSQL_stmt_assert *stmt)
{
	LLVMValueRef		estate_ref = LLVMGetParam(ctx->function, 0);
	LLVMValueRef		cond;
	LLVMBasicBlockRef	fail_bb;
	LLVMBasicBlockRef	cont_bb;

	/* Evaluate condition — try native inlining first */
	if (!uplpgsql_try_compile_bool(ctx, stmt->cond, &cond))
	{
		LLVMValueRef args[] = {
			estate_ref,
			llvm_const_ptr(ctx, stmt->cond)
		};
		cond = uplpgsql_call_fn(ctx, RT_EVAL_BOOL, args, 2);
	}

	fail_bb = uplpgsql_append_block(ctx, "assert.fail");
	cont_bb = uplpgsql_append_block(ctx, "assert.cont");

	LLVMBuildCondBr(ctx->builder, cond, cont_bb, fail_bb);

	/* Failure path: delegate to runtime for message eval + error */
	LLVMPositionBuilderAtEnd(ctx->builder, fail_bb);
	{
		LLVMValueRef args[] = {
			estate_ref,
			llvm_const_ptr(ctx, stmt)
		};
		uplpgsql_call_fn(ctx, RT_EXEC_ASSERT_FAIL, args, 2);
	}
	/* RT_EXEC_ASSERT_FAIL either errors or returns (if asserts disabled) */
	LLVMBuildBr(ctx->builder, cont_bb);

	/* Continue after assert */
	LLVMPositionBuilderAtEnd(ctx->builder, cont_bb);
}

/*
 * Compile a CASE statement — native LLVM IR.
 *
 * Two variants:
 * 1. Simple CASE (t_expr != NULL): evaluate search expression once, store in
 *    temp variable, then each WHEN condition compares against it.
 * 2. Searched CASE (t_expr == NULL): each WHEN is an independent boolean.
 *
 * Both compile to a chain of conditional branches, like IF/ELSIF/ELSE.
 * The PL/pgSQL parser rewrites both forms so that each WHEN expr is a boolean
 * expression — the simple CASE's WHEN exprs contain implicit "t_var = value"
 * comparisons. So we just evaluate each WHEN as a boolean.
 */
static void
uplpgsql_compile_case(UPLpgSQL_compile_ctx *ctx, UPLpgSQL_stmt_case *stmt)
{
	int			num_whens = list_length(stmt->case_when_list);
	void	  **when_conds;
	void	  **when_bodies;
	ListCell   *lc;
	int			i;

	when_conds = palloc(sizeof(void *) * num_whens);
	when_bodies = palloc(sizeof(void *) * num_whens);

	i = 0;
	foreach(lc, stmt->case_when_list)
	{
		UPLpgSQL_case_when *cwt = (UPLpgSQL_case_when *) lfirst(lc);

		when_conds[i] = cwt->expr;
		when_bodies[i] = cwt->stmts;
		i++;
	}

	upl_emit_case(ctx,
				  stmt->t_expr != NULL,
				  stmt->t_varno,
				  stmt->t_expr,
				  num_whens,
				  when_conds,
				  when_bodies,
				  stmt->have_else,
				  stmt->else_stmts,
				  stmt->lineno);

	pfree(when_conds);
	pfree(when_bodies);
}

/*
 * Compile a FOR-query (FORS) loop — native LLVM IR loop.
 *
 * Pattern:
 *   portal = rt_open_query_cursor(estate, query)
 *   found = false
 *   goto cond
 * cond:
 *   has_row = rt_fetch_cursor_row(estate, portal, target_dno)
 *   br has_row → body, exit
 * body:
 *   found = true
 *   <body statements>
 *   goto cond    (CONTINUE also goes here)
 * fors_return:
 *   rt_close_portal(estate, portal)
 *   goto real_return_bb
 * exit:
 *   rt_close_portal(estate, portal)
 *   rt_set_found(estate, found)
 *   <continue after loop>
 *
 * RETURN inside the body is redirected to fors_return_bb which closes
 * the portal before jumping to the function's return block.  EXIT
 * branches to exit_bb which also closes the portal.
 */
static void
uplpgsql_compile_fors(UPLpgSQL_compile_ctx *ctx, UPLpgSQL_stmt_fors *stmt)
{
	LLVMValueRef		estate_ref = LLVMGetParam(ctx->function, 0);
	LLVMValueRef		portal_val, has_row, found_val;
	LLVMValueRef		found_ptr;
	LLVMBasicBlockRef	cond_bb, body_bb, exit_bb, fors_return_bb, cont_bb;
	LLVMBasicBlockRef	saved_return_bb;
	int					target_dno = stmt->var->dno;

	/*
	 * Opening the cursor evaluates the loop query in the interpreter, which
	 * reads variables as PG Datums.  This goes through uplpgsql_call_fn
	 * rather than uplpgsql_call_exec, so it does not get that function's
	 * sync — do it here, or a query over a native array (FOR r IN SELECT
	 * unnest(x)) sees a stale NULL and returns no rows.
	 */
	uplpgsql_sync_native_arrays(ctx);

	/* Open the query cursor */
	{
		LLVMValueRef args[] = {
			estate_ref,
			llvm_const_ptr(ctx, stmt->query)
		};
		portal_val = uplpgsql_call_fn(ctx, RT_OPEN_QUERY_CURSOR, args, 2);
	}

	/* Allocate found flag */
	found_ptr = LLVMBuildAlloca(ctx->builder, ctx->types[UPLPGSQL_INT1], "fors_found");
	LLVMBuildStore(ctx->builder,
				   LLVMConstInt(ctx->types[UPLPGSQL_INT1], 0, false),
				   found_ptr);

	cond_bb = uplpgsql_append_block(ctx, "fors.cond");
	body_bb = uplpgsql_append_block(ctx, "fors.body");
	exit_bb = uplpgsql_append_block(ctx, "fors.exit");
	fors_return_bb = uplpgsql_append_block(ctx, "fors.return");
	cont_bb = uplpgsql_append_block(ctx, "fors.cont");

	LLVMBuildBr(ctx->builder, cond_bb);

	/* --- Condition: fetch next row --- */
	LLVMPositionBuilderAtEnd(ctx->builder, cond_bb);
	{
		LLVMValueRef args[] = {
			estate_ref,
			portal_val,
			llvm_const_int32(ctx, target_dno)
		};
		has_row = uplpgsql_call_fn(ctx, RT_FETCH_CURSOR_ROW, args, 3);
	}
	LLVMBuildCondBr(ctx->builder, has_row, body_bb, exit_bb);

	/* --- Body --- */
	LLVMPositionBuilderAtEnd(ctx->builder, body_bb);

	/* Set found = true */
	LLVMBuildStore(ctx->builder,
				   LLVMConstInt(ctx->types[UPLPGSQL_INT1], 1, false),
				   found_ptr);

	/* Redirect RETURN to fors_return_bb for portal cleanup */
	saved_return_bb = ctx->return_bb;
	ctx->return_bb = fors_return_bb;

	/* Push loop: CONTINUE → cond_bb, EXIT → exit_bb */
	upl_push_loop(ctx, stmt->label, cond_bb, exit_bb);

	/*
	 * Phase 5d: Record field inlining for FOR-query loops.
	 *
	 * If the loop variable is a RECORD-typed rec with no erh, the
	 * expression compiler can't resolve field references like r.a because
	 * the parser callback chain needs a TupleDesc.  We SPI_prepare the
	 * query at compile time, extract resultDesc from the CachedPlanSource,
	 * and create a temporary ExpandedRecordHeader so that body expressions
	 * can resolve record fields through Tiers 1/2.
	 */
	{
		UPLpgSQL_datum	   *target_datum;
		UPLpgSQL_rec	   *rec = NULL;
		ExpandedRecordHeader *saved_erh = NULL;
		bool				installed_erh = false;

		target_datum = ctx_func(ctx)->datums[target_dno];

		if (target_datum->dtype == UPLPGSQL_DTYPE_REC)
		{
			rec = (UPLpgSQL_rec *) target_datum;

			if (rec->rectypeid == RECORDOID && rec->erh == NULL &&
				stmt->query->plan == NULL)
			{
				UPLpgSQL_function  *func = ctx_func(ctx);
				UPLpgSQL_execstate	fake_estate;
				UPLpgSQL_execstate *saved_estate;
				SPIPrepareOptions	options;
				SPIPlanPtr			plan;
				MemoryContext		oldcxt;

				/* Set up fake execstate for parser callbacks */
				memset(&fake_estate, 0, sizeof(fake_estate));
				fake_estate.ndatums = func->ndatums;
				fake_estate.datums = func->datums;

				saved_estate = func->cur_estate;
				func->cur_estate = &fake_estate;

				memset(&options, 0, sizeof(options));
				options.parserSetup = (ParserSetupHook) uplpgsql_parser_setup;
				options.parserSetupArg = stmt->query;
				options.parseMode = stmt->query->parseMode;
				options.cursorOptions = CURSOR_OPT_PARALLEL_OK;

				oldcxt = CurrentMemoryContext;

				PG_TRY();
				{
					plan = SPI_prepare_extended(stmt->query->query, &options);
				}
				PG_CATCH();
				{
					MemoryContextSwitchTo(oldcxt);
					FlushErrorState();
					func->cur_estate = saved_estate;
					plan = NULL;
				}
				PG_END_TRY();

				func->cur_estate = saved_estate;

				if (plan != NULL)
				{
					List		   *plansources;
					CachedPlanSource *plansource;
					TupleDesc		tupdesc;

					SPI_keepplan(plan);
					stmt->query->plan = plan;

					plansources = SPI_plan_get_plan_sources(plan);
					if (list_length(plansources) == 1)
					{
						plansource = (CachedPlanSource *) linitial(plansources);
						tupdesc = plansource->resultDesc;

						if (tupdesc != NULL)
						{
							saved_erh = rec->erh;
							rec->erh = make_expanded_record_from_tupdesc(
								tupdesc, CurrentMemoryContext);
							installed_erh = true;

							elog(DEBUG1,
								 "uplpgsql: installed temporary erh for "
								 "record \"%s\" (dno %d) with %d columns",
								 rec->refname, target_dno,
								 tupdesc->natts);
						}
					}
				}
			}
		}

		/* Compile loop body */
		uplpgsql_compile_stmts(ctx, stmt->body);

		/* Clean up temporary ExpandedRecordHeader */
		if (installed_erh)
		{
			/*
			 * We don't pfree the expanded record — it lives in
			 * CurrentMemoryContext and will be cleaned up with the
			 * compilation memory context.  Just restore the original
			 * (NULL) erh so runtime doesn't see stale compile-time data.
			 */
			rec->erh = saved_erh;
		}
	}

	upl_pop_loop(ctx);

	/* Restore real return block */
	ctx->return_bb = saved_return_bb;

	/* Fall through to next iteration */
	LLVMBuildBr(ctx->builder, cond_bb);

	/* --- fors_return_bb: close portal then jump to real return --- */
	LLVMPositionBuilderAtEnd(ctx->builder, fors_return_bb);
	{
		LLVMValueRef args[] = {
			estate_ref,
			portal_val
		};
		uplpgsql_call_fn(ctx, RT_CLOSE_PORTAL, args, 2);
	}
	LLVMBuildBr(ctx->builder, saved_return_bb);

	/* --- Exit: close portal, set FOUND, continue --- */
	LLVMPositionBuilderAtEnd(ctx->builder, exit_bb);
	{
		LLVMValueRef args[] = {
			estate_ref,
			portal_val
		};
		uplpgsql_call_fn(ctx, RT_CLOSE_PORTAL, args, 2);
	}

	/* Set FOUND */
	found_val = LLVMBuildLoad2(ctx->builder, ctx->types[UPLPGSQL_INT1],
							   found_ptr, "found");
	{
		LLVMValueRef args[] = {
			ctx_plstate(ctx),
			found_val
		};
		uplpgsql_call_exec(ctx, (void *) exec_set_found,
						   ctx->types[UPLPGSQL_VOID], args, 2);
	}

	LLVMBuildBr(ctx->builder, cont_bb);

	/* Continue after loop */
	LLVMPositionBuilderAtEnd(ctx->builder, cont_bb);
}

/*
 * Compile a FOR-cursor (FORC) loop — native LLVM IR loop.
 *
 * Same loop pattern as FORS, but cursor open/close uses FORC-specific
 * runtime helpers that handle argument evaluation, plan preparation,
 * and cursor variable management.
 */
static void
uplpgsql_compile_forc(UPLpgSQL_compile_ctx *ctx, UPLpgSQL_stmt_forc *stmt)
{
	LLVMValueRef		estate_ref = LLVMGetParam(ctx->function, 0);
	LLVMValueRef		portal_val, has_row, found_val;
	LLVMValueRef		found_ptr;
	LLVMValueRef		stmt_ptr = llvm_const_ptr(ctx, stmt);
	LLVMBasicBlockRef	cond_bb, body_bb, exit_bb, forc_return_bb, cont_bb;
	LLVMBasicBlockRef	saved_return_bb;
	int					target_dno = stmt->var->dno;

	/*
	 * Open the cursor.
	 *
	 * This must go through uplpgsql_rt_open_forc_cursor(), not straight to
	 * exec_open_forc_cursor(): the latter returns a bare Portal, but the
	 * fetch below reads the result as a UPLpgSQL_cursor_ctx (portal plus
	 * prefetch batch state).  The rt wrapper allocates that context and is
	 * what uplpgsql_rt_close_forc_cursor() expects to free.
	 *
	 * Opening evaluates the cursor's query and any arguments in the
	 * interpreter, and uplpgsql_call_fn does not sync native arrays the way
	 * uplpgsql_call_exec does, so sync explicitly first.
	 */
	uplpgsql_sync_native_arrays(ctx);
	{
		LLVMValueRef args[] = {
			LLVMGetParam(ctx->function, 0),
			stmt_ptr
		};
		portal_val = uplpgsql_call_fn(ctx, RT_OPEN_FORC_CURSOR, args, 2);
	}

	/* Allocate found flag */
	found_ptr = LLVMBuildAlloca(ctx->builder, ctx->types[UPLPGSQL_INT1], "forc_found");
	LLVMBuildStore(ctx->builder,
				   LLVMConstInt(ctx->types[UPLPGSQL_INT1], 0, false),
				   found_ptr);

	cond_bb = uplpgsql_append_block(ctx, "forc.cond");
	body_bb = uplpgsql_append_block(ctx, "forc.body");
	exit_bb = uplpgsql_append_block(ctx, "forc.exit");
	forc_return_bb = uplpgsql_append_block(ctx, "forc.return");
	cont_bb = uplpgsql_append_block(ctx, "forc.cont");

	LLVMBuildBr(ctx->builder, cond_bb);

	/* --- Condition: fetch next row --- */
	LLVMPositionBuilderAtEnd(ctx->builder, cond_bb);
	{
		LLVMValueRef args[] = {
			estate_ref,
			portal_val,
			llvm_const_int32(ctx, target_dno)
		};
		has_row = uplpgsql_call_fn(ctx, RT_FETCH_CURSOR_ROW, args, 3);
	}
	LLVMBuildCondBr(ctx->builder, has_row, body_bb, exit_bb);

	/* --- Body --- */
	LLVMPositionBuilderAtEnd(ctx->builder, body_bb);

	/* Set found = true */
	LLVMBuildStore(ctx->builder,
				   LLVMConstInt(ctx->types[UPLPGSQL_INT1], 1, false),
				   found_ptr);

	/* Redirect RETURN to forc_return_bb for portal cleanup */
	saved_return_bb = ctx->return_bb;
	ctx->return_bb = forc_return_bb;

	/* Push loop: CONTINUE → cond_bb, EXIT → exit_bb */
	upl_push_loop(ctx, stmt->label, cond_bb, exit_bb);

	/* Compile loop body */
	uplpgsql_compile_stmts(ctx, stmt->body);

	upl_pop_loop(ctx);

	/* Restore real return block */
	ctx->return_bb = saved_return_bb;

	/* Fall through to next iteration */
	LLVMBuildBr(ctx->builder, cond_bb);

	/* --- forc_return_bb: close cursor then jump to real return --- */
	LLVMPositionBuilderAtEnd(ctx->builder, forc_return_bb);
	{
		LLVMValueRef args[] = {
			LLVMGetParam(ctx->function, 0),
			stmt_ptr,
			portal_val
		};
		uplpgsql_call_fn(ctx, RT_CLOSE_FORC_CURSOR, args, 3);
	}
	LLVMBuildBr(ctx->builder, saved_return_bb);

	/* --- Exit: close cursor, set FOUND, continue --- */
	LLVMPositionBuilderAtEnd(ctx->builder, exit_bb);
	{
		LLVMValueRef args[] = {
			LLVMGetParam(ctx->function, 0),
			stmt_ptr,
			portal_val
		};
		uplpgsql_call_fn(ctx, RT_CLOSE_FORC_CURSOR, args, 3);
	}

	/* Set FOUND */
	found_val = LLVMBuildLoad2(ctx->builder, ctx->types[UPLPGSQL_INT1],
							   found_ptr, "found");
	{
		LLVMValueRef args[] = {
			ctx_plstate(ctx),
			found_val
		};
		uplpgsql_call_exec(ctx, (void *) exec_set_found,
						   ctx->types[UPLPGSQL_VOID], args, 2);
	}

	LLVMBuildBr(ctx->builder, cont_bb);

	/* Continue after loop */
	LLVMPositionBuilderAtEnd(ctx->builder, cont_bb);
}

/*
 * Compile EXECUTE (dynamic SQL) — delegate to runtime.
 */
static void
uplpgsql_compile_dynexecute(UPLpgSQL_compile_ctx *ctx,
							UPLpgSQL_stmt_dynexecute *stmt)
{
	LLVMValueRef args[] = {
		ctx_plstate(ctx),
		llvm_const_ptr(ctx, stmt)
	};

	uplpgsql_call_exec(ctx, (void *) exec_stmt_dynexecute,
					   ctx->types[UPLPGSQL_INT32], args, 2);
}

/*
 * Compile DYNFORS (FOR ... IN EXECUTE) — native LLVM IR loop.
 *
 * Same pattern as FORS, but uses a dynamic query cursor opener.
 */
static void
uplpgsql_compile_dynfors(UPLpgSQL_compile_ctx *ctx,
						 UPLpgSQL_stmt_dynfors *stmt)
{
	LLVMValueRef		estate_ref = LLVMGetParam(ctx->function, 0);
	LLVMValueRef		portal_val, has_row, found_val;
	LLVMValueRef		found_ptr;
	LLVMBasicBlockRef	cond_bb, body_bb, exit_bb, dynfors_return_bb, cont_bb;
	LLVMBasicBlockRef	saved_return_bb;
	int					target_dno = stmt->var->dno;

	/*
	 * As in uplpgsql_compile_fors(): the open evaluates the query text and
	 * its USING arguments in the interpreter, and reaches it via
	 * uplpgsql_call_fn, which does not sync.
	 */
	uplpgsql_sync_native_arrays(ctx);

	/* Open the dynamic cursor */
	{
		LLVMValueRef args[] = {
			estate_ref,
			llvm_const_ptr(ctx, stmt)
		};
		portal_val = uplpgsql_call_fn(ctx, RT_OPEN_DYNFORS_CURSOR, args, 2);
	}

	/* Allocate found flag */
	found_ptr = LLVMBuildAlloca(ctx->builder, ctx->types[UPLPGSQL_INT1], "dynfors_found");
	LLVMBuildStore(ctx->builder,
				   LLVMConstInt(ctx->types[UPLPGSQL_INT1], 0, false),
				   found_ptr);

	cond_bb = uplpgsql_append_block(ctx, "dynfors.cond");
	body_bb = uplpgsql_append_block(ctx, "dynfors.body");
	exit_bb = uplpgsql_append_block(ctx, "dynfors.exit");
	dynfors_return_bb = uplpgsql_append_block(ctx, "dynfors.return");
	cont_bb = uplpgsql_append_block(ctx, "dynfors.cont");

	LLVMBuildBr(ctx->builder, cond_bb);

	/* --- Condition: fetch next row --- */
	LLVMPositionBuilderAtEnd(ctx->builder, cond_bb);
	{
		LLVMValueRef args[] = {
			estate_ref,
			portal_val,
			llvm_const_int32(ctx, target_dno)
		};
		has_row = uplpgsql_call_fn(ctx, RT_FETCH_CURSOR_ROW, args, 3);
	}
	LLVMBuildCondBr(ctx->builder, has_row, body_bb, exit_bb);

	/* --- Body --- */
	LLVMPositionBuilderAtEnd(ctx->builder, body_bb);

	LLVMBuildStore(ctx->builder,
				   LLVMConstInt(ctx->types[UPLPGSQL_INT1], 1, false),
				   found_ptr);

	saved_return_bb = ctx->return_bb;
	ctx->return_bb = dynfors_return_bb;

	upl_push_loop(ctx, stmt->label, cond_bb, exit_bb);
	uplpgsql_compile_stmts(ctx, stmt->body);
	upl_pop_loop(ctx);

	ctx->return_bb = saved_return_bb;

	LLVMBuildBr(ctx->builder, cond_bb);

	/* --- dynfors_return_bb: close portal then jump to real return --- */
	LLVMPositionBuilderAtEnd(ctx->builder, dynfors_return_bb);
	{
		LLVMValueRef args[] = { estate_ref, portal_val };
		uplpgsql_call_fn(ctx, RT_CLOSE_PORTAL, args, 2);
	}
	LLVMBuildBr(ctx->builder, saved_return_bb);

	/* --- Exit: close portal, set FOUND --- */
	LLVMPositionBuilderAtEnd(ctx->builder, exit_bb);
	{
		LLVMValueRef args[] = { estate_ref, portal_val };
		uplpgsql_call_fn(ctx, RT_CLOSE_PORTAL, args, 2);
	}

	found_val = LLVMBuildLoad2(ctx->builder, ctx->types[UPLPGSQL_INT1],
							   found_ptr, "found");
	{
		LLVMValueRef args[] = { ctx_plstate(ctx), found_val };
		uplpgsql_call_exec(ctx, (void *) exec_set_found,
						   ctx->types[UPLPGSQL_VOID], args, 2);
	}

	LLVMBuildBr(ctx->builder, cont_bb);
	LLVMPositionBuilderAtEnd(ctx->builder, cont_bb);
}

/*
 * Compile FOREACH array loop — delegate to runtime.
 *
 * Array element/slice iteration involves deep executor internals
 * (array detoasting, element extraction, type coercion). The entire
 * statement is delegated to the interpreter.
 */
static void
uplpgsql_compile_foreach_a(UPLpgSQL_compile_ctx *ctx,
						   UPLpgSQL_stmt_foreach_a *stmt)
{
	LLVMValueRef args[] = {
		ctx_plstate(ctx),
		llvm_const_ptr(ctx, stmt)
	};

	uplpgsql_call_exec(ctx, (void *) exec_stmt_foreach_a,
					   ctx->types[UPLPGSQL_INT32], args, 2);
}

/*
 * Compile RETURN NEXT — delegate to runtime.
 */
static void
uplpgsql_compile_return_next(UPLpgSQL_compile_ctx *ctx,
							 UPLpgSQL_stmt_return_next *stmt)
{
	LLVMValueRef args[] = {
		ctx_plstate(ctx),
		llvm_const_ptr(ctx, stmt)
	};

	uplpgsql_call_exec(ctx, (void *) exec_stmt_return_next,
					   ctx->types[UPLPGSQL_INT32], args, 2);
}

/*
 * Compile RETURN QUERY — delegate to runtime.
 */
static void
uplpgsql_compile_return_query(UPLpgSQL_compile_ctx *ctx,
							  UPLpgSQL_stmt_return_query *stmt)
{
	LLVMValueRef args[] = {
		ctx_plstate(ctx),
		llvm_const_ptr(ctx, stmt)
	};

	uplpgsql_call_exec(ctx, (void *) exec_stmt_return_query,
					   ctx->types[UPLPGSQL_INT32], args, 2);
}

/*
 * Compile CALL — delegate to runtime.
 */
static void
uplpgsql_compile_call(UPLpgSQL_compile_ctx *ctx, UPLpgSQL_stmt_call *stmt)
{
	LLVMValueRef args[] = {
		ctx_plstate(ctx),
		llvm_const_ptr(ctx, stmt)
	};

	uplpgsql_call_exec(ctx, (void *) exec_stmt_call,
					   ctx->types[UPLPGSQL_INT32], args, 2);
}

/*
 * Compile GET DIAGNOSTICS — delegate to runtime.
 */
static void
uplpgsql_compile_getdiag(UPLpgSQL_compile_ctx *ctx,
						 UPLpgSQL_stmt_getdiag *stmt)
{
	LLVMValueRef args[] = {
		ctx_plstate(ctx),
		llvm_const_ptr(ctx, stmt)
	};

	uplpgsql_call_exec(ctx, (void *) exec_stmt_getdiag,
					   ctx->types[UPLPGSQL_VOID], args, 2);
}

/*
 * Compile COMMIT — delegate to runtime.
 */
static void
uplpgsql_compile_commit(UPLpgSQL_compile_ctx *ctx,
						UPLpgSQL_stmt_commit *stmt)
{
	LLVMValueRef args[] = {
		ctx_plstate(ctx),
		llvm_const_ptr(ctx, stmt)
	};

	uplpgsql_call_exec(ctx, (void *) exec_stmt_commit,
					   ctx->types[UPLPGSQL_VOID], args, 2);
}

/*
 * Compile ROLLBACK — delegate to runtime.
 */
static void
uplpgsql_compile_rollback(UPLpgSQL_compile_ctx *ctx,
						  UPLpgSQL_stmt_rollback *stmt)
{
	LLVMValueRef args[] = {
		ctx_plstate(ctx),
		llvm_const_ptr(ctx, stmt)
	};

	uplpgsql_call_exec(ctx, (void *) exec_stmt_rollback,
					   ctx->types[UPLPGSQL_VOID], args, 2);
}

/* Loop stack management now lives in core/upl_compile.c:
 * upl_push_loop(), upl_pop_loop(), upl_find_loop()
 */
