/*-------------------------------------------------------------------------
 *
 * upl.h
 *		Master header for UPL Core Engine — language-agnostic LLVM JIT
 *		infrastructure shared by all procedural language drivers.
 *
 *		This file defines the core types, enums, and function prototypes
 *		that every UPL language driver uses.  Language-specific types
 *		(exec state, AST nodes, runtime helpers) remain in the driver.
 *
 *		Key types:
 *		  - UPL_compile_ctx: per-compilation LLVM context and state
 *		  - UPL_func: cached compiled function (function pointer + identity)
 *		  - UPL_loop_info: loop tracking for EXIT/CONTINUE compilation
 *		  - UPL_callbacks: driver callbacks for body/expression compilation
 *		  - UPL_datum_offsets: parameterized struct offsets for GEP
 *		  - UPL_expr_ops: expression wrapper abstraction
 *		  - UPL_compile_hooks: compilation pipeline hooks
 *
 *		Key enums:
 *		  - UPL_llvm_type: indices into ctx->types[] for pre-registered
 *		    LLVM type references
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
#ifndef UPL_H
#define UPL_H

#include "postgres.h"
#include "fmgr.h"
#include "access/transam.h"
#include "storage/itemptr.h"

#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitReader.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Orc.h>
#include <llvm-c/OrcEE.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

/*
 * Macro for functions that must be visible to OrcJIT's process symbol search.
 * We build with -fvisibility=hidden, so runtime helpers called from JIT'd
 * code must be explicitly marked visible.
 */
#define UPL_RT_EXPORT __attribute__((visibility("default")))

/*
 * LLVM type indices — used to index into ctx->types[] for fast access
 * to pre-registered LLVM type references.  Registered once per compilation
 * in upl_register_types().
 */
typedef enum UPL_llvm_type
{
	UPL_VOID,
	UPL_INT1,
	UPL_INT8,
	UPL_INT16,
	UPL_INT32,
	UPL_INT64,
	UPL_DOUBLE,
	UPL_PTR,
	UPL_INTPTR,
	UPL_DATUM,					/* alias for INT64 */
	UPL_FUNC_TYPE,				/* function type: i32(ptr) */
	UPL_NUM_TYPES
} UPL_llvm_type;

/*
 * Loop tracking for EXIT/CONTINUE — singly-linked stack.
 *
 * Each active loop pushes an entry with its label and the LLVM basic blocks
 * for CONTINUE (loop back) and EXIT (break out).  EXIT/CONTINUE statements
 * search the stack by label (NULL = innermost) to find the target blocks.
 *
 * A labeled BEGIN...END block is also an EXIT target, so it pushes an entry
 * with is_loop = false and continue_bb = NULL.  Such entries are matched only
 * by a labeled EXIT: an unlabeled EXIT/CONTINUE targets the innermost real
 * loop and must look straight through them.
 */
typedef struct UPL_loop_info
{
	const char		   *label;
	bool				is_loop;
	LLVMBasicBlockRef	continue_bb;
	LLVMBasicBlockRef	exit_bb;

	/*
	 * ctx->exc_frame_depth at the time this entry was pushed.  An
	 * EXIT/CONTINUE targeting this entry from inside a more deeply nested
	 * exception block must unwind every frame above this depth before it
	 * branches; see upl_emit_loop_exit().
	 */
	int					exc_depth;

	struct UPL_loop_info *next;
} UPL_loop_info;

/*
 * Exception frame tracking for EXIT/CONTINUE — singly-linked stack.
 *
 * A block with exception handlers allocates a runtime frame (subtransaction,
 * saved PG_exception_stack, stmt_mcontext) that its own exit paths release.
 * An EXIT/CONTINUE whose target lies *outside* the block cannot use those
 * paths: it branches straight to the target loop's block, so the compiler
 * must emit the release call for every frame it jumps across — innermost
 * first — or the subtransaction stays open and PG_exception_stack keeps
 * pointing at a dead frame.
 *
 * The driver pushes an entry while compiling a try body (unwind_rt_fn =
 * the try-exit helper, which commits the subtransaction) or a handler body
 * (unwind_rt_fn = the handler-done helper, which pops the stmt_mcontext and
 * restores cur_error), and pops it afterwards.  upl_emit_loop_exit() walks
 * the stack down to the target's recorded depth.
 */
typedef struct UPL_exc_frame
{
	LLVMValueRef		frame_ptr;		/* runtime frame pointer (from push) */
	int					unwind_rt_fn;	/* rt_funcs[] index of release helper */
	struct UPL_exc_frame *next;
} UPL_exc_frame;

/* Forward declaration for callbacks that reference the context */
typedef struct UPL_compile_ctx UPL_compile_ctx;

/*
 * Driver callbacks — used by core primitives when they need language-specific
 * behavior.  The driver fills this struct before calling any core primitive.
 *
 * Core calls compile_stmts/try_compile_bool to recurse into the driver's
 * statement and expression compilers.  When try_compile_bool returns false,
 * core falls back to the RT helper at rt_eval_bool.
 *
 * The RT function indices (rt_*) point into ctx->rt_funcs[]/rt_fntypes[].
 * A value of -1 means the operation is not available for this language.
 */
typedef struct UPL_callbacks
{
	/*
	 * Compile a list of body statements.  Core calls this when compiling
	 * the body of IF/WHILE/LOOP/FOR/CASE/BLOCK etc.
	 * The stmts pointer is opaque — only the driver knows its type.
	 */
	void (*compile_stmts)(UPL_compile_ctx *ctx, void *stmts);

	/*
	 * Try to compile a boolean expression natively (Tier 1/2).
	 * Returns true + *result_out if inlined.
	 * Returns false -> core emits RT call via rt_eval_bool index.
	 */
	bool (*try_compile_bool)(UPL_compile_ctx *ctx, void *expr,
							 LLVMValueRef *result_out);

	/*
	 * Assign an expression result to a variable.
	 * Used by CASE for test expression evaluation.
	 */
	void (*assign_expr)(UPL_compile_ctx *ctx, int varno, void *expr);

	/*
	 * Load a parameter's Datum value.  Handles all datum types
	 * (plain var, recfield, promise, etc.) — language specific.
	 */
	LLVMValueRef (*load_param_datum)(UPL_compile_ctx *ctx,
									 LLVMValueRef estate_ref, int dno);

	/*
	 * Load a parameter's isnull flag.  Language specific.
	 */
	LLVMValueRef (*load_param_isnull)(UPL_compile_ctx *ctx,
									  LLVMValueRef estate_ref, int dno);

	/*
	 * Store a Datum into a plain variable.
	 * Sets value, clears isnull and freeval.
	 */
	void (*store_var_datum)(UPL_compile_ctx *ctx,
							LLVMValueRef estate_ref, int dno,
							LLVMValueRef datum_val);

	/*
	 * Parser state management for compile-time SPI_prepare.
	 * setup returns opaque saved state; restore puts it back.
	 */
	void *(*setup_parser_state)(UPL_compile_ctx *ctx, void *expr);
	void (*restore_parser_state)(UPL_compile_ctx *ctx, void *saved);

	/*
	 * Runtime function indices for core to use in fallback paths.
	 * -1 = not available (core skips that optimization).
	 */
	int rt_eval_bool;			/* bool fn(ptr estate, ptr expr) */
	int rt_eval_int;			/* i32 fn(ptr estate, ptr expr) */
	int rt_set_found;			/* void fn(ptr estate, i1 value) */
	int rt_assign_int;			/* void fn(ptr estate, i32 dno, i32 val) */
	int rt_case_error;			/* void fn(ptr estate, i32 lineno) */
	int rt_assign_null;			/* void fn(ptr estate, i32 dno) */
	int rt_init_var;			/* void fn(ptr estate, i32 dno) */
	int rt_assign_expr;			/* void fn(ptr estate, i32 dno, ptr expr) */
	int rt_assign_var_datum;	/* void fn(ptr estate, i32 dno, i64 val, i8 isnull) */
	int rt_copy_assign_var_datum; /* void fn(ptr estate, i32 dno, i64 val, i8 isnull) */
} UPL_callbacks;

/*
 * Struct offsets for parameterized GEP-based variable access.
 *
 * Each language has its own exec_state struct layout.  The driver fills
 * this with offsetof() values so core can navigate the struct hierarchy
 * without knowing the language-specific types.
 *
 * GEP chain: estate_ref → lang_state → datums[dno] → var.{value,isnull,freeval}
 */
typedef struct UPL_datum_offsets
{
	/* estate → language-specific exec state (first field typically) */
	size_t estate_to_lang_state;

	/* language exec state → datums array pointer */
	size_t lang_state_to_datums;

	/* Variable (datum) → value, isnull, freeval fields */
	size_t var_to_value;
	size_t var_to_isnull;
	size_t var_to_freeval;
} UPL_datum_offsets;

/*
 * Expression wrapper abstraction.
 *
 * Each language wraps PG expressions in its own struct (e.g. UPLpgSQL_expr).
 * Core expression compiler uses these ops to access the wrapper's fields
 * without knowing the language-specific type.
 *
 * Types are void* to avoid pulling language-specific headers into core:
 *   - get_paramnos returns Bitmapset*
 *   - get_plan/set_plan use SPIPlanPtr
 *   - get_parse_mode returns RawParseMode (cast to int)
 *   - parser_setup is ParserSetupHook
 */
typedef struct UPL_expr_ops
{
	const char *(*get_query)(void *expr);
	void *(*get_paramnos)(void *expr);
	void *(*get_plan)(void *expr);
	void (*set_plan)(void *expr, void *plan);
	int (*get_parse_mode)(void *expr);
	void (*parser_setup)(void *pstate, void *arg);
} UPL_expr_ops;

/*
 * Per-compilation state — created on stack in the driver's compile_function(),
 * threaded through all compilation functions.
 *
 * Contains the LLVM context/module/builder, the function being compiled,
 * pre-registered type references, loop tracking stack, driver callbacks,
 * datum offsets, expression ops, and exception handling state.
 *
 * The rt_funcs/rt_fntypes arrays are sized by the driver's max RT func
 * count.  The driver is responsible for allocating these arrays with the
 * right size and filling them in.  The core engine indexes into these
 * via the rt_* indices in UPL_callbacks.
 *
 * lang_data is an opaque pointer for driver-specific per-compilation state.
 *
 * Lifetime: exists only during a single call to driver's compile_function().
 * The LLVM context and module are handed off to OrcJIT at the end.
 */
struct UPL_compile_ctx
{
	/* LLVM objects */
	LLVMContextRef		context;
	LLVMModuleRef		module;
	LLVMBuilderRef		builder;

	/* The LLVM function being compiled */
	LLVMValueRef		function;
	LLVMBasicBlockRef	entry_bb;
	LLVMBasicBlockRef	return_bb;

	/* Estate parameter (first function arg, loaded once) */
	LLVMValueRef		estate_ref;

	/* Return code alloca */
	LLVMValueRef		rc_ptr;

	/* Loop label tracking for EXIT/CONTINUE */
	UPL_loop_info	   *loop_stack;

	/* Enclosing exception frames an EXIT/CONTINUE may have to unwind */
	UPL_exc_frame	   *exc_frame_stack;
	int					exc_frame_depth;

	/* Pre-registered LLVM types */
	LLVMTypeRef			types[UPL_NUM_TYPES];

	/*
	 * Pre-declared runtime function refs and types.
	 *
	 * These are allocated and sized by the driver (driver knows how many
	 * runtime functions it has).  Core indexes into these via the rt_*
	 * indices in the callbacks struct.
	 */
	int					num_rt_funcs;	/* driver sets this */
	LLVMValueRef	   *rt_funcs;		/* driver allocates [num_rt_funcs] */
	LLVMTypeRef		   *rt_fntypes;		/* driver allocates [num_rt_funcs] */

	/* sigsetjmp declaration for exception handling */
	LLVMValueRef		sigsetjmp_fn;
	LLVMTypeRef			sigsetjmp_fntype;

	/* Set to true when the function contains exception blocks */
	bool				has_exceptions;

	/*
	 * Set while emitting a condition whose operand types are not yet settled,
	 * so the driver must not prepare a plan for it at compile time.
	 *
	 * A simple CASE's WHEN conditions read the temporary variable holding the
	 * test expression, and that variable's type is only fixed at run time (the
	 * parser cannot know it, so it builds the variable as a placeholder).  A
	 * plan prepared now would bind the parameter as the placeholder type and
	 * stay wrong for the life of the compiled function.
	 */
	bool				defer_cond_plan;

	/* Driver callbacks for body/expression compilation */
	UPL_callbacks		callbacks;

	/* Struct offsets for parameterized GEP datum access */
	UPL_datum_offsets	datum_offsets;

	/* Expression wrapper ops (set by driver, used by core expr compiler) */
	UPL_expr_ops	   *expr_ops;

	/* Opaque pointer for driver-specific per-compilation data */
	void			   *lang_data;
};

/*
 * Cached compiled function — generic version.
 * Stores the JIT'd function pointer and the source function identity
 * for cache invalidation.
 */
typedef struct UPL_func
{
	void			   *jit_func;		/* native function pointer */

	/* Source function identity for cache invalidation */
	Oid					fn_oid;
	TransactionId		fn_xmin;
	ItemPointerData		fn_tid;
} UPL_func;

/*
 * Compilation pipeline hooks — the driver provides these to
 * upl_compile_function() which orchestrates the full pipeline.
 *
 * Pipeline steps:
 *   1. Create LLVM context/module/builder
 *   2. Register types
 *   3. register_rt_funcs() — driver declares RT functions in LLVM module
 *   4. Create LLVM function, sigsetjmp, entry/return blocks, rc alloca
 *   5. setup_entry() — driver loads plstate, native array allocas, etc.
 *   6. compile_body() — driver compiles AST using core primitives
 *   7. Fall through to return block, load rc, ret
 *   8. Add nounwind if no exceptions
 *   9. Verify, optimize (O3), JIT compile via OrcJIT
 */
typedef struct UPL_compile_hooks
{
	/* Called after types registered, before function created */
	void (*register_rt_funcs)(UPL_compile_ctx *ctx);

	/* Called after entry block created.  Driver loads plstate, etc. */
	void (*setup_entry)(UPL_compile_ctx *ctx);

	/* Compile the function body (driver dispatches its own AST) */
	void (*compile_body)(UPL_compile_ctx *ctx);

	/* Function name prefix for LLVM symbol (e.g., "uplpgsql_fn") */
	const char *func_name_prefix;

	/* Function OID and identity for cache/symbol naming */
	Oid fn_oid;
	TransactionId fn_xmin;
	ItemPointerData fn_tid;

	/* Default return code value (typically 0 = RC_OK) */
	int32 default_rc;

	/* When true, dump the generated LLVM IR to the log after verification */
	bool dump_ir;
} UPL_compile_hooks;

/*
 * Cache return codes.
 */
#define UPL_CACHE_MISS		0	/* not in cache */
#define UPL_CACHE_HIT		1	/* JIT'd function found */
#define UPL_CACHE_SKIP		2	/* heuristic said skip JIT */

/* --- core/upl_llvm.c --- */
extern void upl_llvm_init(void);
extern void upl_llvm_shutdown(void);
extern LLVMOrcLLJITRef upl_get_jit(void);
extern void upl_register_types(UPL_compile_ctx *ctx);
extern void upl_verify_module(LLVMModuleRef module);
extern void upl_optimize_module(LLVMModuleRef module, int level);
extern void *upl_jit_compile(LLVMModuleRef module,
							 LLVMContextRef context,
							 const char *func_name);

/* --- core/upl_cache.c --- */

/*
 * Staleness check callback type.
 *
 * The cache stores an opaque lang_func pointer (the language-specific
 * function struct, e.g. UPLpgSQL_function *).  On lookup, the cache passes
 * the stored pointer back to this callback.  The driver returns true if
 * the stored pointer is still valid (same pointer as current), false if
 * the compiler returned a different struct and the JIT'd code's embedded
 * AST pointers are stale.
 */
typedef bool (*upl_cache_check_fn)(void *cached_lang_func, void *current_lang_func);

extern void upl_cache_init(void);
extern int upl_cache_lookup(Oid fn_oid, TransactionId fn_xmin,
							ItemPointerData fn_tid,
							void *current_lang_func,
							upl_cache_check_fn check_fn,
							UPL_func **jitfunc_out);
extern void upl_cache_store(Oid fn_oid, TransactionId fn_xmin,
							ItemPointerData fn_tid,
							void *lang_func,
							UPL_func *jitfunc);
extern void upl_cache_store_skip(Oid fn_oid, TransactionId fn_xmin,
								 ItemPointerData fn_tid,
								 void *lang_func);

/* --- core/upl_compile.c --- */

/* Compilation pipeline — driver calls this */
extern void *upl_compile_function(UPL_compile_ctx *ctx,
								  UPL_compile_hooks *hooks);

/* Loop stack management */
extern void upl_push_loop(UPL_compile_ctx *ctx, const char *label,
						   LLVMBasicBlockRef continue_bb,
						   LLVMBasicBlockRef exit_bb);
extern void upl_push_block_label(UPL_compile_ctx *ctx, const char *label,
								 LLVMBasicBlockRef exit_bb);
extern void upl_pop_loop(UPL_compile_ctx *ctx);
extern UPL_loop_info *upl_find_loop(UPL_compile_ctx *ctx, const char *label);

/* Exception frame stack management (for EXIT/CONTINUE unwinding) */
extern void upl_push_exc_frame(UPL_compile_ctx *ctx, LLVMValueRef frame_ptr,
							   int unwind_rt_fn);
extern void upl_pop_exc_frame(UPL_compile_ctx *ctx);

/*
 * Core IR primitives — language-agnostic control flow compilation.
 *
 * All take opaque void* for expressions and bodies.  Core uses the
 * callbacks in ctx->callbacks to evaluate expressions and compile bodies.
 */

/* IF/ELSIF/ELSE */
extern void upl_emit_if(UPL_compile_ctx *ctx,
						 void *cond_expr,
						 void *then_stmts,
						 int num_elsifs,
						 void **elsif_conds,
						 void **elsif_bodies,
						 void *else_stmts);

/* Conditional loop (WHILE = test_at_top, REPEAT UNTIL = test_at_bottom) */
extern void upl_emit_cond_loop(UPL_compile_ctx *ctx, const char *label,
								void *cond_expr, bool test_at_top,
								void *body_stmts);

/* Unconditional loop (LOOP ... END LOOP) */
extern void upl_emit_loop(UPL_compile_ctx *ctx, const char *label,
						   void *body_stmts);

/* Integer FOR loop */
extern void upl_emit_fori(UPL_compile_ctx *ctx, const char *label,
						   int var_dno, void *lower_expr, void *upper_expr,
						   void *step_expr, bool reverse,
						   void *body_stmts);

/* EXIT/CONTINUE/LEAVE/ITERATE */
extern void upl_emit_loop_exit(UPL_compile_ctx *ctx, const char *label,
								bool is_exit, void *cond_expr);

/* CASE (searched or simple) */
extern void upl_emit_case(UPL_compile_ctx *ctx,
						   bool has_test_expr, int test_varno,
						   void *test_assign_expr,
						   int num_whens,
						   void **when_conds,
						   void **when_bodies,
						   bool has_else, void *else_body,
						   int lineno);

/* Return — store rc, branch to return block, create dead block */
extern void upl_emit_return(UPL_compile_ctx *ctx, int rt_exec_return,
							 void *stmt);

/* Block with variable init + optional exception handling */
extern void upl_emit_block(UPL_compile_ctx *ctx,
							int n_initvars, int *initvarnos,
							void *body_stmts,
							bool has_exceptions, void *exception_data,
							void (*compile_exceptions)(UPL_compile_ctx *ctx,
													   void *exception_data));

/* Simple runtime call (thin wrapper for common pattern) */
extern LLVMValueRef upl_emit_rt_call(UPL_compile_ctx *ctx, int rt_func_idx,
									   LLVMValueRef *args, unsigned count);

/* Direct function pointer call (bypasses RT wrapper, embeds address) */
extern LLVMValueRef upl_emit_direct_call(UPL_compile_ctx *ctx, void *fn_addr,
										  LLVMTypeRef ret_type,
										  LLVMValueRef *args, unsigned count);

/* --- core/upl_datum.c --- */

/* Parameterized datum access via GEP — uses ctx->datum_offsets */
extern LLVMValueRef upl_emit_load_var_datum(UPL_compile_ctx *ctx,
											 LLVMValueRef estate_ref, int dno);
extern void upl_emit_store_var_datum(UPL_compile_ctx *ctx,
									  LLVMValueRef estate_ref, int dno,
									  LLVMValueRef datum_val);
extern LLVMValueRef upl_emit_load_var_isnull(UPL_compile_ctx *ctx,
											  LLVMValueRef estate_ref, int dno);

/*
 * Inline helpers for common LLVM operations.
 *
 * These are used extensively by driver code and are small enough to inline.
 */
static inline LLVMValueRef
upl_const_int32(UPL_compile_ctx *ctx, int32 val)
{
	return LLVMConstInt(ctx->types[UPL_INT32], val, false);
}

static inline LLVMValueRef
upl_const_int64(UPL_compile_ctx *ctx, int64 val)
{
	return LLVMConstInt(ctx->types[UPL_INT64], val, false);
}

static inline LLVMValueRef
upl_const_ptr(UPL_compile_ctx *ctx, void *ptr)
{
	return LLVMConstIntToPtr(
		LLVMConstInt(ctx->types[UPL_INT64], (uintptr_t) ptr, false),
		ctx->types[UPL_PTR]);
}

static inline LLVMBasicBlockRef
upl_append_block(UPL_compile_ctx *ctx, const char *name)
{
	return LLVMAppendBasicBlockInContext(ctx->context, ctx->function, name);
}

#endif							/* UPL_H */
