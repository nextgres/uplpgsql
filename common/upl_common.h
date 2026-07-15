/*-------------------------------------------------------------------------
 *
 * uplpgsql.h
 *		Main header for uplpgsql — LLVM JIT-compiled PL/pgSQL
 *
 *		This file defines all PL/pgSQL-specific types, enums, and function
 *		prototypes for the uplpgsql driver.  Language-agnostic core types
 *		(UPL_compile_ctx, UPL_llvm_type, etc.) are in core/upl.h.
 *
 *		Key types:
 *		  - UPLpgSQL_exec_state: runtime state passed to JIT'd functions
 *		  - UPLpgSQL_func: cached compiled function (wraps UPL_func)
 *		  - UPLpgSQL_exception_frame: heap-allocated frame for sigsetjmp-based
 *		    exception handling in JIT'd code
 *		  - UPLpgSQL_lang_data: PL/pgSQL-specific per-compilation state
 *		    stored in UPL_compile_ctx.lang_data
 *
 *		Key enums:
 *		  - UPLpgSQL_rt_func: indices into the runtime function declaration array
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
#ifndef UPL_COMMON_H
#define UPL_COMMON_H

#include "postgres.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "upl_plpgsql.h"

#include <setjmp.h>

/* Core UPL engine header */
#include "../core/upl.h"

/*
 * Return codes from JIT'd functions.
 * These must match the enum in uplpgsql_plpgsql.h:
 *   UPLPGSQL_RC_OK=0, RC_EXIT=1, RC_RETURN=2, RC_CONTINUE=3
 * We use the enum values directly — no separate defines needed.
 */

/*
 * Backward compatibility aliases for the RT_EXPORT macro.
 */
#define UPLPGSQL_RT_EXPORT UPL_RT_EXPORT

/*
 * Max bytes per native array to allocate on stack via LLVM alloca.
 * Arrays larger than this threshold use palloc0 (heap) via runtime helper.
 * 4096 bytes = 512 float8s or 1024 int4s — fits comfortably in stack frame.
 */
#define NATIVE_ARRAY_STACK_THRESHOLD	4096

/*
 * Backward compatibility aliases for LLVM type indices.
 * Driver code can use either UPL_* or UPLPGSQL_* names.
 */
#define UPLPGSQL_VOID		UPL_VOID
#define UPLPGSQL_INT1		UPL_INT1
#define UPLPGSQL_INT8		UPL_INT8
#define UPLPGSQL_INT16		UPL_INT16
#define UPLPGSQL_INT32		UPL_INT32
#define UPLPGSQL_INT64		UPL_INT64
#define UPLPGSQL_DOUBLE		UPL_DOUBLE
#define UPLPGSQL_PTR		UPL_PTR
#define UPLPGSQL_INTPTR		UPL_INTPTR
#define UPLPGSQL_DATUM		UPL_DATUM
#define UPLPGSQL_FUNC_TYPE	UPL_FUNC_TYPE
#define UPLPGSQL_NUM_TYPES	UPL_NUM_TYPES

/*
 * Backward compatibility aliases for cache constants.
 */
#define UPLPGSQL_CACHE_MISS		UPL_CACHE_MISS
#define UPLPGSQL_CACHE_HIT		UPL_CACHE_HIT
#define UPLPGSQL_CACHE_SKIP		UPL_CACHE_SKIP

/*
 * Runtime function indices — used to index into ctx->rt_funcs[] and
 * ctx->rt_fntypes[] for pre-declared runtime helper references.
 *
 * Each entry corresponds to a C function in uplpgsql_runtime.c that is
 * callable from JIT'd code.  The functions are declared in the LLVM module
 * by uplpgsql_register_runtime_funcs() and resolved at link time by OrcJIT's
 * process symbol search generator.
 *
 * RT_EVAL_*: expression evaluation helpers
 * RT_EXEC_*: statement execution helpers (delegate to exec_stmt_*)
 * RT_ASSIGN_*: variable assignment helpers
 * RT_OPEN/RT_FETCH/RT_CLOSE: cursor management for loop compilation
 * RT_EXCEPTION_*: subtransaction-based exception handling steps
 */
typedef enum UPLpgSQL_rt_func
{
	RT_EVAL_EXPR,
	RT_EVAL_BOOL,
	RT_EVAL_INT,
	RT_INIT_VAR,
	RT_ASSIGN_EXPR,
	RT_SET_FOUND,
	RT_ASSIGN_INT,
	RT_EXEC_RETURN,
	RT_EXEC_PERFORM,
	RT_EXEC_SQL,
	RT_EXEC_DYNEXECUTE,
	RT_EXEC_CALL,
	RT_EXEC_RAISE,
	RT_EXEC_ASSERT,
	RT_EXEC_BLOCK_PROTECTED,
	RT_EXEC_OPEN,
	RT_EXEC_FETCH,
	RT_EXEC_CLOSE,
	RT_EXEC_RETURN_NEXT,
	RT_EXEC_RETURN_QUERY,
	RT_EXEC_FOR_QUERY,
	RT_EXEC_COMMIT,
	RT_EXEC_ROLLBACK,
	RT_EXEC_GETDIAG,
	RT_ASSIGN_NULL,
	RT_CASE_ERROR,
	RT_EXEC_ASSERT_FAIL,
	RT_OPEN_QUERY_CURSOR,
	RT_FETCH_CURSOR_ROW,
	RT_CLOSE_PORTAL,
	RT_OPEN_FORC_CURSOR,
	RT_CLOSE_FORC_CURSOR,
	RT_EXEC_FOREACH_A,
	RT_OPEN_DYNFORS_CURSOR,
	RT_EXCEPTION_PUSH_FRAME,
	RT_EXCEPTION_ARM,
	RT_EXCEPTION_TRY_EXIT,
	RT_EXCEPTION_CATCH,
	RT_EXCEPTION_SET_HANDLER_VARS,
	RT_EXCEPTION_HANDLER_DONE,
	RT_EXCEPTION_RETHROW,
	RT_ASSIGN_VAR_DATUM,
	RT_COPY_ASSIGN_VAR_DATUM,
	RT_GET_RECFIELD,
	RT_GET_RECFIELD_FAST,
	RT_ARRAY_GET_ELEMENT,
	RT_ARRAY_SET_ELEMENT,
	RT_NATIVE_ARRAY_ALLOC,
	RT_NATIVE_ARRAY_BOUNDS_CHECK,
	RT_FREE_VAR_DATUM,
	RT_NATIVE_ARRAY_FROM_DATUM,
	RT_NATIVE_ARRAY_TO_DATUM,
	UPLPGSQL_NUM_RT_FUNCS
} UPLpgSQL_rt_func;

/*
 * Runtime execution state — passed to JIT'd function.
 *
 * This is the single pointer the compiled function receives.
 * It provides access to PL/pgSQL's execution state for runtime helpers,
 * plus our own parallel datum arrays for fast variable access.
 */
typedef struct UPLpgSQL_exec_state
{
	/* Full PL/pgSQL execution state for SPI calls */
	UPLpgSQL_execstate  *uplpgsql_estate;

	/* Return value */
	Datum				retval;
	bool				retisnull;

	/* FOUND flag */
	bool				found;

	/* Current exception (for handlers) */
	ErrorData		   *cur_error;

	/* Statement-lifetime memory context */
	MemoryContext		stmt_mcontext;
} UPLpgSQL_exec_state;

/*
 * Exception frame for native LLVM IR exception handling.
 *
 * Heap-allocated (in parent memory context) so the sigjmp_buf survives
 * across the longjmp.  The JIT'd function calls sigsetjmp on &frame->jmpbuf
 * directly, so the frame must remain valid while the try block executes.
 */
typedef struct UPLpgSQL_exception_frame
{
	sigjmp_buf			jmpbuf;

	/* Saved PG exception stack (restored on catch/exit) */
	sigjmp_buf		   *saved_exception_stack;
	ErrorData		   *saved_cur_error;

	/* Saved error context callback stack (restored on catch/exit) */
	ErrorContextCallback *saved_error_context_stack;

	/* Saved executor state for rollback on catch */
	MemoryContext		oldcontext;
	ResourceOwner		oldowner;
	ExprContext		   *old_eval_econtext;
	MemoryContext		stmt_mcontext;
} UPLpgSQL_exception_frame;

/* JIT'd function pointer type */
typedef int32 (*uplpgsql_jit_func)(UPLpgSQL_exec_state *estate);

/*
 * Cached compiled function — PL/pgSQL-specific wrapper around UPL_func.
 */
typedef struct UPLpgSQL_func
{
	uplpgsql_jit_func	jit_func;

	/* Source function identity for cache invalidation */
	Oid					fn_oid;
	TransactionId		fn_xmin;
	ItemPointerData		fn_tid;
} UPLpgSQL_func;

/*
 * PL/pgSQL-specific per-compilation data, stored in UPL_compile_ctx.lang_data.
 *
 * This struct holds the PL/pgSQL function being compiled, the cached
 * plstate pointer, and native array optimization data.
 */
typedef struct UPLpgSQL_lang_data
{
	/* The PL/pgSQL function being compiled */
	UPLpgSQL_function   *uplpgsql_func;

	/* Cached plstate pointer (estate->uplpgsql_estate), loaded at entry */
	LLVMValueRef		plstate_ref;

	/* Native local arrays identified by escape analysis (Phase 7) */
	int					num_native_arrays;
	struct UPLpgSQL_native_array *native_arrays;
} UPLpgSQL_lang_data;

/*
 * Metadata for a local array variable lowered to flat native memory
 * by Phase 7 escape analysis.
 *
 * Instead of going through array_get_element/array_set_element per access
 * (~50-100ns each), subscript reads/writes are compiled as direct LLVM
 * GEP+load/store (~1ns).
 *
 * Created by uplpgsql_analyze_native_arrays() in uplpgsql_compile.c.
 * The llvm_elemtype, data_ptr, and len_ptr fields are populated during
 * step 7b of uplpgsql_compile_function() (entry block alloca setup).
 * The actual memory allocation happens when array_fill() is intercepted
 * during expression compilation (uplpgsql_expr.c).
 */
typedef struct UPLpgSQL_native_array
{
	int				dno;			/* datum number of the array variable */
	Oid				elemtype;		/* INT4OID, INT8OID, or FLOAT8OID */
	int				elem_size;		/* sizeof(element): 4 or 8 bytes */
	LLVMTypeRef		llvm_elemtype;	/* i32, i64, or double */
	LLVMValueRef	data_ptr;		/* entry-block alloca holding ptr to flat memory */
	LLVMValueRef	len_ptr;		/* entry-block alloca holding element count (i32) */
} UPLpgSQL_native_array;

/*
 * Backward compatibility: UPLpgSQL_loop_info is now UPL_loop_info in core.
 */
typedef UPL_loop_info UPLpgSQL_loop_info;

/*
 * The compile context is now UPL_compile_ctx from core.
 * Driver code uses it directly with PL/pgSQL-specific data in lang_data.
 *
 * Backward compatibility typedef so existing code compiles unchanged.
 */
typedef UPL_compile_ctx UPLpgSQL_compile_ctx;

/*
 * Helper macros to access PL/pgSQL-specific lang_data from a UPL_compile_ctx.
 */
#define UPLPGSQL_LANG_DATA(ctx) ((UPLpgSQL_lang_data *)(ctx)->lang_data)

/* --- uplpgsql_handler.c GUCs --- */
extern bool uplpgsql_enable_jit_heuristic;

/* --- uplpgsql_compile.c --- */
extern bool uplpgsql_should_jit(UPLpgSQL_function *func);
extern UPLpgSQL_func *uplpgsql_compile_function(UPLpgSQL_function *func);

/* --- uplpgsql_runtime.c --- */
extern int32 uplpgsql_rt_exec_block_protected(UPLpgSQL_exec_state *estate,
											  UPLpgSQL_stmt_block *stmt);
extern Datum uplpgsql_rt_eval_expr(UPLpgSQL_exec_state *estate,
								   UPLpgSQL_expr *expr, bool *isNull);
extern bool uplpgsql_rt_eval_bool(UPLpgSQL_exec_state *estate,
								  UPLpgSQL_expr *expr);
extern int32 uplpgsql_rt_eval_int(UPLpgSQL_exec_state *estate,
								  UPLpgSQL_expr *expr);
extern void uplpgsql_rt_assign_expr(UPLpgSQL_exec_state *estate,
									int target_dno, UPLpgSQL_expr *expr);
extern void uplpgsql_rt_init_var(UPLpgSQL_exec_state *estate, int dno);
extern void uplpgsql_rt_set_found(UPLpgSQL_exec_state *estate, bool value);
extern void uplpgsql_rt_assign_int(UPLpgSQL_exec_state *estate,
								   int dno, int32 value);
extern int32 uplpgsql_rt_exec_return(UPLpgSQL_exec_state *estate,
									 UPLpgSQL_stmt_return *stmt);
extern int32 uplpgsql_rt_exec_perform(UPLpgSQL_exec_state *estate,
									   UPLpgSQL_stmt_perform *stmt);
extern int32 uplpgsql_rt_exec_sql(UPLpgSQL_exec_state *estate,
								  UPLpgSQL_stmt_execsql *stmt);
extern void uplpgsql_rt_exec_raise(UPLpgSQL_exec_state *estate,
								   UPLpgSQL_stmt_raise *stmt);
extern void uplpgsql_rt_assign_null(UPLpgSQL_exec_state *estate, int dno);
extern void uplpgsql_rt_case_error(UPLpgSQL_exec_state *estate, int lineno);
extern void uplpgsql_rt_exec_assert(UPLpgSQL_exec_state *estate,
									UPLpgSQL_stmt_assert *stmt);
extern int32 uplpgsql_rt_exec_open(UPLpgSQL_exec_state *estate,
								   UPLpgSQL_stmt_open *stmt);
extern int32 uplpgsql_rt_exec_fetch(UPLpgSQL_exec_state *estate,
									UPLpgSQL_stmt_fetch *stmt);
extern int32 uplpgsql_rt_exec_close(UPLpgSQL_exec_state *estate,
									UPLpgSQL_stmt_close *stmt);
extern void *uplpgsql_rt_open_query_cursor(UPLpgSQL_exec_state *estate,
										   UPLpgSQL_expr *query);
extern bool uplpgsql_rt_fetch_cursor_row(UPLpgSQL_exec_state *estate,
										  void *portal_ptr, int target_dno);
extern void uplpgsql_rt_close_portal(UPLpgSQL_exec_state *estate,
									 void *portal_ptr);
extern void *uplpgsql_rt_open_forc_cursor(UPLpgSQL_exec_state *estate,
										  UPLpgSQL_stmt_forc *stmt);
extern void uplpgsql_rt_close_forc_cursor(UPLpgSQL_exec_state *estate,
										  UPLpgSQL_stmt_forc *stmt,
										  void *portal_ptr);
extern int32 uplpgsql_rt_exec_dynexecute(UPLpgSQL_exec_state *estate,
										 UPLpgSQL_stmt_dynexecute *stmt);
extern int32 uplpgsql_rt_exec_call(UPLpgSQL_exec_state *estate,
								   UPLpgSQL_stmt_call *stmt);
extern void uplpgsql_rt_exec_getdiag(UPLpgSQL_exec_state *estate,
									 UPLpgSQL_stmt_getdiag *stmt);
extern int32 uplpgsql_rt_exec_return_next(UPLpgSQL_exec_state *estate,
										  UPLpgSQL_stmt_return_next *stmt);
extern int32 uplpgsql_rt_exec_return_query(UPLpgSQL_exec_state *estate,
										   UPLpgSQL_stmt_return_query *stmt);
extern void uplpgsql_rt_exec_commit(UPLpgSQL_exec_state *estate,
									UPLpgSQL_stmt_commit *stmt);
extern void uplpgsql_rt_exec_rollback(UPLpgSQL_exec_state *estate,
									  UPLpgSQL_stmt_rollback *stmt);
extern int32 uplpgsql_rt_exec_foreach_a(UPLpgSQL_exec_state *estate,
										UPLpgSQL_stmt_foreach_a *stmt);
extern void *uplpgsql_rt_open_dynfors_cursor(UPLpgSQL_exec_state *estate,
											 UPLpgSQL_stmt_dynfors *stmt);

/* Exception handling runtime helpers */
extern void *uplpgsql_rt_exception_push_frame(UPLpgSQL_exec_state *estate,
											  UPLpgSQL_stmt_block *block);
extern void uplpgsql_rt_exception_arm(UPLpgSQL_exec_state *estate,
									  void *frame_ptr);
extern void uplpgsql_rt_exception_try_exit(UPLpgSQL_exec_state *estate,
										   void *frame_ptr);
extern int32 uplpgsql_rt_exception_catch(UPLpgSQL_exec_state *estate,
										 UPLpgSQL_stmt_block *block,
										 void *frame_ptr);
extern void uplpgsql_rt_exception_set_handler_vars(UPLpgSQL_exec_state *estate,
												   UPLpgSQL_stmt_block *block,
												   int handler_idx);
extern void uplpgsql_rt_exception_handler_done(UPLpgSQL_exec_state *estate,
											   void *frame_ptr);
extern void uplpgsql_rt_exception_rethrow(UPLpgSQL_exec_state *estate,
										  void *frame_ptr);

/* --- upl_exec.c (JIT execution path) --- */
extern Datum uplpgsql_exec_function_jit(UPLpgSQL_function *func,
										FunctionCallInfo fcinfo,
										EState *simple_eval_estate,
										ResourceOwner simple_eval_resowner,
										ResourceOwner procedure_resowner,
										bool atomic,
										uplpgsql_jit_func jit_func);
extern HeapTuple uplpgsql_exec_trigger_jit(UPLpgSQL_function *func,
										   TriggerData *trigdata,
										   uplpgsql_jit_func jit_func);

/*
 * --- uplpgsql_expr.c (expression inlining) ---
 *
 * These are called from uplpgsql_compile.c to attempt expression-level
 * optimization.  They return true if the expression was successfully
 * inlined (Tier 1 or Tier 2), false if the caller should fall back to
 * the runtime helper (Tier 3).
 */
extern bool uplpgsql_try_compile_assign(UPLpgSQL_compile_ctx *ctx,
										UPLpgSQL_stmt_assign *stmt);
extern bool uplpgsql_try_compile_bool(UPLpgSQL_compile_ctx *ctx,
									  UPLpgSQL_expr *expr,
									  LLVMValueRef *result_out);

/* Runtime helpers for pass-by-reference variable assignment */
extern void uplpgsql_rt_assign_var_datum(UPLpgSQL_exec_state *estate,
										 int dno, Datum value, bool isnull);
extern void uplpgsql_rt_copy_assign_var_datum(UPLpgSQL_exec_state *estate,
											  int dno, Datum value,
											  bool isnull);

/* Runtime helpers for record field access from inlined expressions */
extern Datum uplpgsql_rt_get_recfield(UPLpgSQL_exec_state *estate,
									  int recfield_dno);
extern Datum uplpgsql_rt_get_recfield_fast(UPLpgSQL_exec_state *estate,
										   int rec_dno, int fnumber,
										   bool *isnull_out);

/*
 * Native array <-> PG Datum marshalling, emitted at escape points.
 *
 * A native array's live contents are in flat memory (data_ptr/len_ptr); the
 * variable's PG Datum is stale between escapes.  Emit sync before handing the
 * variable to anything that reads it as a Datum, and refresh after anything
 * that writes it as a Datum.  Defined in upl_compile_stmts.c; also called
 * from upl_compile_expr.c.
 */
extern void uplpgsql_emit_sync_native_array(UPLpgSQL_compile_ctx *ctx,
											struct UPLpgSQL_native_array *na);
extern void uplpgsql_emit_refresh_native_array(UPLpgSQL_compile_ctx *ctx,
											   struct UPLpgSQL_native_array *na);

/* Runtime helpers for array element access from inlined expressions */
extern Datum uplpgsql_rt_array_get_element(UPLpgSQL_exec_state *estate,
										   int array_dno, int subscript,
										   int typlen, int16 elmlen,
										   bool elmbyval, char elmalign,
										   bool *isNull_out);
extern void uplpgsql_rt_array_set_element(UPLpgSQL_exec_state *estate,
										  int array_dno, int subscript,
										  Datum newvalue, bool valisnull,
										  int typlen, Oid elemtype,
										  int16 elmlen, bool elmbyval,
										  char elmalign);

/* Runtime helpers for inlined overflow/division errors */
extern void uplpgsql_rt_int_overflow(void);
extern void uplpgsql_rt_div_zero(void);

/* --- Native local array runtime helpers (Phase 7) --- */
/* Heap alloc for arrays > NATIVE_ARRAY_STACK_THRESHOLD bytes */
extern void *uplpgsql_rt_native_array_alloc(UPLpgSQL_exec_state *estate,
											int64 byte_size);
/* Cold-path bounds check error (called from inline check, never returns) */
extern void uplpgsql_rt_native_array_bounds_check(int subscript, int length);
/* Free a variable's old pass-by-ref value before fmgr bypass call */
extern void uplpgsql_rt_free_var_datum(UPLpgSQL_exec_state *estate, int dno);
/* Decompose PG array Datum into flat native memory */
extern void *uplpgsql_rt_native_array_from_datum(UPLpgSQL_exec_state *estate,
												 Datum array_datum, bool isnull,
												 int elem_size, int *out_nelems);
/* Marshal native flat array to PG array Datum in variable */
extern void uplpgsql_rt_native_array_to_datum(UPLpgSQL_exec_state *estate,
											  int varno, void *data, int nelems,
											  Oid elemtype, int elem_size);

#endif							/* UPL_COMMON_H */
