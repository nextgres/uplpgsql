/*-------------------------------------------------------------------------
 *
 * upl_compile.c
 *		Language-agnostic control flow compilation primitives and the
 *		compilation pipeline for the UPL core engine.
 *
 *		All expressions and statement bodies are opaque void* pointers.
 *		Core uses ctx->callbacks to recurse into the driver for expression
 *		evaluation and body compilation.
 *
 *		Functions:
 *		  - Loop stack: push, pop, find
 *		  - Utility calls: upl_emit_rt_call, upl_emit_direct_call
 *		  - Control flow: if, cond_loop, loop, fori, loop_exit, case,
 *		    return, block (with optional exception handling)
 *		  - Pipeline: upl_compile_function
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

#include "miscadmin.h"

/*
 * Symbol name the JIT emits for sigsetjmp (called directly from the compiled
 * function's frame, so it cannot be wrapped).  glibc exposes sigsetjmp only as
 * a macro over __sigsetjmp, so there is no "sigsetjmp" symbol for OrcJIT's
 * process-symbol search (dlsym) to resolve — a compiled function containing an
 * EXCEPTION handler would fail to materialize.  Emit __sigsetjmp there.  On
 * macOS/BSD and musl, sigsetjmp is a real symbol.
 */
#ifdef __GLIBC__
#define UPL_SIGSETJMP_SYM	"__sigsetjmp"
#else
#define UPL_SIGSETJMP_SYM	"sigsetjmp"
#endif

/* Generation counter for unique LLVM symbol names across recompilations */
static uint64 compile_gen = 0;

/* Internal helper: evaluate a boolean expression */
static LLVMValueRef upl_eval_bool(UPL_compile_ctx *ctx, void *cond_expr);


/* ----------------------------------------------------------------
 *		Loop stack management
 * ----------------------------------------------------------------
 */

/*
 * Push a loop onto the tracking stack.
 *
 * continue_bb is the target for CONTINUE/ITERATE statements.
 * exit_bb is the target for EXIT/LEAVE statements.
 */
void
upl_push_loop(UPL_compile_ctx *ctx, const char *label,
			  LLVMBasicBlockRef continue_bb,
			  LLVMBasicBlockRef exit_bb)
{
	UPL_loop_info *info = palloc(sizeof(UPL_loop_info));

	info->label = label;
	info->continue_bb = continue_bb;
	info->exit_bb = exit_bb;
	info->next = ctx->loop_stack;
	ctx->loop_stack = info;
}

/*
 * Pop the top loop from the tracking stack.
 */
void
upl_pop_loop(UPL_compile_ctx *ctx)
{
	UPL_loop_info *top = ctx->loop_stack;

	Assert(top != NULL);
	ctx->loop_stack = top->next;
	pfree(top);
}

/*
 * Find a loop on the stack by label.
 * NULL label means the innermost loop.
 */
UPL_loop_info *
upl_find_loop(UPL_compile_ctx *ctx, const char *label)
{
	UPL_loop_info *info;

	for (info = ctx->loop_stack; info != NULL; info = info->next)
	{
		if (label == NULL)
			return info;			/* innermost loop */
		if (info->label != NULL && strcmp(info->label, label) == 0)
			return info;
	}

	return NULL;
}


/* ----------------------------------------------------------------
 *		Utility call helpers
 * ----------------------------------------------------------------
 */

/*
 * Emit a call to a registered runtime function.
 *
 * rt_func_idx indexes into ctx->rt_funcs[] and ctx->rt_fntypes[].
 */
LLVMValueRef
upl_emit_rt_call(UPL_compile_ctx *ctx, int rt_func_idx,
				 LLVMValueRef *args, unsigned count)
{
	Assert(rt_func_idx >= 0 && rt_func_idx < ctx->num_rt_funcs);

	return LLVMBuildCall2(ctx->builder,
						  ctx->rt_fntypes[rt_func_idx],
						  ctx->rt_funcs[rt_func_idx],
						  args, count, "");
}

/*
 * Emit a direct call to a C function via embedded pointer address.
 *
 * Embeds fn_addr as an LLVM integer constant, casts to a function pointer,
 * and calls it.  This bypasses the runtime wrapper layer for thin wrappers
 * and allows LLVM's optimizer to reason about the call more effectively.
 *
 * The function type is constructed from the argument types and ret_type.
 */
LLVMValueRef
upl_emit_direct_call(UPL_compile_ctx *ctx, void *fn_addr,
					 LLVMTypeRef ret_type,
					 LLVMValueRef *args, unsigned count)
{
	LLVMTypeRef		param_types[8];
	LLVMTypeRef		fn_type;
	LLVMValueRef	fn_ptr;
	unsigned		i;
	const char	   *call_name;

	Assert(count <= 8);

	for (i = 0; i < count; i++)
		param_types[i] = LLVMTypeOf(args[i]);

	fn_type = LLVMFunctionType(ret_type, param_types, count, false);
	fn_ptr = LLVMConstIntToPtr(
		LLVMConstInt(ctx->types[UPL_INT64], (uintptr_t) fn_addr, false),
		LLVMPointerType(fn_type, 0));

	/* Use empty name for void returns, "ret" for value-producing calls */
	call_name = (ret_type == ctx->types[UPL_VOID]) ? "" : "ret";

	return LLVMBuildCall2(ctx->builder, fn_type, fn_ptr,
						  args, count, call_name);
}


/* ----------------------------------------------------------------
 *		Internal: evaluate a boolean expression
 * ----------------------------------------------------------------
 */

/*
 * Try to compile a boolean expression natively via the driver callback.
 * On failure, fall back to the runtime helper at callbacks.rt_eval_bool.
 *
 * Returns an LLVMValueRef of type i1.
 */
static LLVMValueRef
upl_eval_bool(UPL_compile_ctx *ctx, void *cond_expr)
{
	LLVMValueRef result;

	/* Try native (Tier 1/2) compilation first */
	if (ctx->callbacks.try_compile_bool != NULL &&
		ctx->callbacks.try_compile_bool(ctx, cond_expr, &result))
		return result;

	/* Fall back to runtime helper */
	Assert(ctx->callbacks.rt_eval_bool >= 0);
	{
		LLVMValueRef args[] = {
			ctx->estate_ref,
			upl_const_ptr(ctx, cond_expr)
		};

		return upl_emit_rt_call(ctx, ctx->callbacks.rt_eval_bool, args, 2);
	}
}


/* ----------------------------------------------------------------
 *		Control flow primitives
 * ----------------------------------------------------------------
 */

/*
 * Compile an IF / ELSIF / ELSE statement.
 *
 * cond_expr:     the IF condition (opaque)
 * then_stmts:    body for the IF branch
 * num_elsifs:    number of ELSIF clauses
 * elsif_conds:   array of ELSIF condition expressions
 * elsif_bodies:  array of ELSIF statement bodies
 * else_stmts:    ELSE body (NULL if no ELSE)
 */
void
upl_emit_if(UPL_compile_ctx *ctx,
			void *cond_expr,
			void *then_stmts,
			int num_elsifs,
			void **elsif_conds,
			void **elsif_bodies,
			void *else_stmts)
{
	LLVMBasicBlockRef	then_bb;
	LLVMBasicBlockRef	else_bb;
	LLVMBasicBlockRef	merge_bb;
	LLVMValueRef		cond;
	int					i;

	merge_bb = upl_append_block(ctx, "if.merge");

	/* Evaluate the IF condition */
	cond = upl_eval_bool(ctx, cond_expr);

	then_bb = upl_append_block(ctx, "if.then");
	else_bb = upl_append_block(ctx, "if.else");

	LLVMBuildCondBr(ctx->builder, cond, then_bb, else_bb);

	/* Then block */
	LLVMPositionBuilderAtEnd(ctx->builder, then_bb);
	ctx->callbacks.compile_stmts(ctx, then_stmts);
	LLVMBuildBr(ctx->builder, merge_bb);

	/* Else/elsif chain */
	LLVMPositionBuilderAtEnd(ctx->builder, else_bb);

	for (i = 0; i < num_elsifs; i++)
	{
		LLVMBasicBlockRef	elsif_then;
		LLVMBasicBlockRef	elsif_else;
		LLVMValueRef		elsif_cond;

		elsif_cond = upl_eval_bool(ctx, elsif_conds[i]);

		elsif_then = upl_append_block(ctx, "elsif.then");
		elsif_else = upl_append_block(ctx, "elsif.else");

		LLVMBuildCondBr(ctx->builder, elsif_cond, elsif_then, elsif_else);

		LLVMPositionBuilderAtEnd(ctx->builder, elsif_then);
		ctx->callbacks.compile_stmts(ctx, elsif_bodies[i]);
		LLVMBuildBr(ctx->builder, merge_bb);

		LLVMPositionBuilderAtEnd(ctx->builder, elsif_else);
	}

	/* ELSE body or fall through */
	if (else_stmts != NULL)
		ctx->callbacks.compile_stmts(ctx, else_stmts);

	LLVMBuildBr(ctx->builder, merge_bb);

	/* Continue after merge */
	LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);
}

/*
 * Compile a conditional loop.
 *
 * test_at_top = true:  WHILE loop (condition checked before each iteration)
 * test_at_top = false: REPEAT UNTIL (condition checked after each iteration)
 */
void
upl_emit_cond_loop(UPL_compile_ctx *ctx, const char *label,
				   void *cond_expr, bool test_at_top,
				   void *body_stmts)
{
	LLVMBasicBlockRef	cond_bb;
	LLVMBasicBlockRef	body_bb;
	LLVMBasicBlockRef	exit_bb;
	LLVMValueRef		cond;

	cond_bb = upl_append_block(ctx, test_at_top ? "while.cond" : "repeat.cond");
	body_bb = upl_append_block(ctx, test_at_top ? "while.body" : "repeat.body");
	exit_bb = upl_append_block(ctx, test_at_top ? "while.exit" : "repeat.exit");

	if (test_at_top)
	{
		/* WHILE: branch to condition first */
		LLVMBuildBr(ctx->builder, cond_bb);

		/* Condition block */
		LLVMPositionBuilderAtEnd(ctx->builder, cond_bb);
		cond = upl_eval_bool(ctx, cond_expr);
		LLVMBuildCondBr(ctx->builder, cond, body_bb, exit_bb);

		/* Body block */
		LLVMPositionBuilderAtEnd(ctx->builder, body_bb);

		upl_push_loop(ctx, label, cond_bb, exit_bb);
		ctx->callbacks.compile_stmts(ctx, body_stmts);
		upl_pop_loop(ctx);

		/* Loop back to condition */
		LLVMBuildBr(ctx->builder, cond_bb);
	}
	else
	{
		/* REPEAT UNTIL: branch to body first */
		LLVMBuildBr(ctx->builder, body_bb);

		/* Body block */
		LLVMPositionBuilderAtEnd(ctx->builder, body_bb);

		upl_push_loop(ctx, label, cond_bb, exit_bb);
		ctx->callbacks.compile_stmts(ctx, body_stmts);
		upl_pop_loop(ctx);

		/* Fall through to condition */
		LLVMBuildBr(ctx->builder, cond_bb);

		/* Condition block: exit if condition is true (UNTIL semantics) */
		LLVMPositionBuilderAtEnd(ctx->builder, cond_bb);
		cond = upl_eval_bool(ctx, cond_expr);
		LLVMBuildCondBr(ctx->builder, cond, exit_bb, body_bb);
	}

	/* Continue after loop */
	LLVMPositionBuilderAtEnd(ctx->builder, exit_bb);
}

/*
 * Compile an unconditional LOOP ... END LOOP.
 *
 * The loop body executes indefinitely until an EXIT statement branches
 * to exit_bb.
 */
void
upl_emit_loop(UPL_compile_ctx *ctx, const char *label,
			  void *body_stmts)
{
	LLVMBasicBlockRef	body_bb;
	LLVMBasicBlockRef	exit_bb;

	body_bb = upl_append_block(ctx, "loop.body");
	exit_bb = upl_append_block(ctx, "loop.exit");

	/* Branch to body */
	LLVMBuildBr(ctx->builder, body_bb);

	/* Body block */
	LLVMPositionBuilderAtEnd(ctx->builder, body_bb);

	upl_push_loop(ctx, label, body_bb, exit_bb);
	ctx->callbacks.compile_stmts(ctx, body_stmts);
	upl_pop_loop(ctx);

	/* Loop back */
	LLVMBuildBr(ctx->builder, body_bb);

	/* Continue after loop */
	LLVMPositionBuilderAtEnd(ctx->builder, exit_bb);
}

/*
 * Compile an integer FOR loop.
 *
 * Evaluates lower/upper/step bounds once via runtime helpers, then the
 * actual loop (compare, branch, increment) is pure LLVM IR.
 *
 * Structure:
 *   fori.setup:  evaluate bounds, initialize loop_val alloca
 *   fori.cond:   compare loop_val vs upper -> branch
 *   fori.body:   set found, assign loop var, compile body
 *   fori.step:   increment/decrement with overflow check -> branch to cond
 *   fori.exit:   set FOUND, continue
 */
void
upl_emit_fori(UPL_compile_ctx *ctx, const char *label,
			  int var_dno, void *lower_expr, void *upper_expr,
			  void *step_expr, bool reverse,
			  void *body_stmts)
{
	LLVMValueRef		lower, upper, step;
	LLVMValueRef		loop_val_ptr, found_ptr;
	LLVMValueRef		cur_val, next_val, done, overflow, found_val;
	LLVMBasicBlockRef	cond_bb, body_bb, step_bb, exit_bb, store_bb;

	/* Evaluate lower bound */
	{
		LLVMValueRef args[] = {
			ctx->estate_ref,
			upl_const_ptr(ctx, lower_expr)
		};
		lower = upl_emit_rt_call(ctx, ctx->callbacks.rt_eval_int, args, 2);
	}

	/* Evaluate upper bound */
	{
		LLVMValueRef args[] = {
			ctx->estate_ref,
			upl_const_ptr(ctx, upper_expr)
		};
		upper = upl_emit_rt_call(ctx, ctx->callbacks.rt_eval_int, args, 2);
	}

	/* Evaluate step (default 1) */
	if (step_expr)
	{
		LLVMValueRef args[] = {
			ctx->estate_ref,
			upl_const_ptr(ctx, step_expr)
		};
		step = upl_emit_rt_call(ctx, ctx->callbacks.rt_eval_int, args, 2);
	}
	else
		step = upl_const_int32(ctx, 1);

	/* Allocate loop value and found flag */
	loop_val_ptr = LLVMBuildAlloca(ctx->builder, ctx->types[UPL_INT32],
								   "loop_val");
	LLVMBuildStore(ctx->builder, lower, loop_val_ptr);

	found_ptr = LLVMBuildAlloca(ctx->builder, ctx->types[UPL_INT1], "found");
	LLVMBuildStore(ctx->builder,
				   LLVMConstInt(ctx->types[UPL_INT1], 0, false), found_ptr);

	cond_bb  = upl_append_block(ctx, "fori.cond");
	body_bb  = upl_append_block(ctx, "fori.body");
	step_bb  = upl_append_block(ctx, "fori.step");
	exit_bb  = upl_append_block(ctx, "fori.exit");

	LLVMBuildBr(ctx->builder, cond_bb);

	/* --- Condition block: check loop_val vs upper --- */
	LLVMPositionBuilderAtEnd(ctx->builder, cond_bb);
	cur_val = LLVMBuildLoad2(ctx->builder, ctx->types[UPL_INT32],
							 loop_val_ptr, "cur");

	if (reverse)
		done = LLVMBuildICmp(ctx->builder, LLVMIntSLT, cur_val, upper,
							 "done");
	else
		done = LLVMBuildICmp(ctx->builder, LLVMIntSGT, cur_val, upper,
							 "done");

	LLVMBuildCondBr(ctx->builder, done, exit_bb, body_bb);

	/* --- Body block --- */
	LLVMPositionBuilderAtEnd(ctx->builder, body_bb);

	/* Set found = true */
	LLVMBuildStore(ctx->builder,
				   LLVMConstInt(ctx->types[UPL_INT1], 1, false), found_ptr);

	/* Assign current value to loop variable via runtime helper */
	{
		LLVMValueRef args[] = {
			ctx->estate_ref,
			upl_const_int32(ctx, var_dno),
			cur_val
		};
		upl_emit_rt_call(ctx, ctx->callbacks.rt_assign_int, args, 3);
	}

	/* Push loop: CONTINUE -> step_bb, EXIT -> exit_bb */
	upl_push_loop(ctx, label, step_bb, exit_bb);

	/* Compile loop body */
	ctx->callbacks.compile_stmts(ctx, body_stmts);

	upl_pop_loop(ctx);

	/* Fall through to step */
	LLVMBuildBr(ctx->builder, step_bb);

	/* --- Step block: increment/decrement with overflow check --- */
	LLVMPositionBuilderAtEnd(ctx->builder, step_bb);
	cur_val = LLVMBuildLoad2(ctx->builder, ctx->types[UPL_INT32],
							 loop_val_ptr, "cur2");

	store_bb = upl_append_block(ctx, "fori.store");

	if (reverse)
	{
		next_val = LLVMBuildSub(ctx->builder, cur_val, step, "next");
		/* Overflow if next > cur (underflow in reverse) */
		overflow = LLVMBuildICmp(ctx->builder, LLVMIntSGT, next_val,
								cur_val, "overflow");
	}
	else
	{
		next_val = LLVMBuildAdd(ctx->builder, cur_val, step, "next");
		/* Overflow if next < cur */
		overflow = LLVMBuildICmp(ctx->builder, LLVMIntSLT, next_val,
								cur_val, "overflow");
	}

	LLVMBuildCondBr(ctx->builder, overflow, exit_bb, store_bb);

	/* Store block: save next value, loop back */
	LLVMPositionBuilderAtEnd(ctx->builder, store_bb);
	LLVMBuildStore(ctx->builder, next_val, loop_val_ptr);
	LLVMBuildBr(ctx->builder, cond_bb);

	/* --- Exit block: set FOUND and continue --- */
	LLVMPositionBuilderAtEnd(ctx->builder, exit_bb);
	found_val = LLVMBuildLoad2(ctx->builder, ctx->types[UPL_INT1],
							   found_ptr, "found_val");
	{
		LLVMValueRef args[] = {
			ctx->estate_ref,
			found_val
		};
		upl_emit_rt_call(ctx, ctx->callbacks.rt_set_found, args, 2);
	}
}

/*
 * Compile an EXIT/CONTINUE (or LEAVE/ITERATE) statement.
 *
 * is_exit = true:  EXIT (branch to exit_bb)
 * is_exit = false: CONTINUE (branch to continue_bb)
 *
 * If cond_expr is non-NULL, the branch is conditional.
 */
void
upl_emit_loop_exit(UPL_compile_ctx *ctx, const char *label,
				   bool is_exit, void *cond_expr)
{
	UPL_loop_info	   *loop;
	LLVMBasicBlockRef	target_bb;

	/* Find the target loop */
	loop = upl_find_loop(ctx, label);
	if (loop == NULL)
		elog(ERROR, "upl: EXIT/CONTINUE outside of a loop");

	target_bb = is_exit ? loop->exit_bb : loop->continue_bb;

	if (cond_expr)
	{
		/* Conditional EXIT/CONTINUE */
		LLVMBasicBlockRef	skip_bb;
		LLVMValueRef		cond;

		skip_bb = upl_append_block(ctx, is_exit ? "exit.skip" : "continue.skip");

		cond = upl_eval_bool(ctx, cond_expr);

		LLVMBuildCondBr(ctx->builder, cond, target_bb, skip_bb);

		/* Continue compilation after the skip */
		LLVMPositionBuilderAtEnd(ctx->builder, skip_bb);
	}
	else
	{
		/* Unconditional EXIT/CONTINUE */
		LLVMBuildBr(ctx->builder, target_bb);

		/* Dead block for any statements after unconditional EXIT/CONTINUE */
		{
			LLVMBasicBlockRef dead_bb = upl_append_block(ctx, "exit.dead");

			LLVMPositionBuilderAtEnd(ctx->builder, dead_bb);
		}
	}
}

/*
 * Compile a CASE statement (searched or simple).
 *
 * has_test_expr:     true for simple CASE (test expression assigned to temp var)
 * test_varno:        datum number of the temporary variable for simple CASE
 * test_assign_expr:  the test expression to assign (opaque)
 * num_whens:         number of WHEN clauses
 * when_conds:        array of WHEN condition expressions
 * when_bodies:       array of WHEN statement bodies
 * has_else:          true if there is an ELSE clause
 * else_body:         ELSE body statements (opaque)
 * lineno:            line number for error reporting (CASE without ELSE + no match)
 */
void
upl_emit_case(UPL_compile_ctx *ctx,
			  bool has_test_expr, int test_varno,
			  void *test_assign_expr,
			  int num_whens,
			  void **when_conds,
			  void **when_bodies,
			  bool has_else, void *else_body,
			  int lineno)
{
	LLVMBasicBlockRef	merge_bb;
	int					i;

	merge_bb = upl_append_block(ctx, "case.merge");

	/*
	 * If simple CASE, evaluate the test expression and assign to the
	 * temporary variable.  The WHEN conditions reference this variable.
	 */
	if (has_test_expr)
		ctx->callbacks.assign_expr(ctx, test_varno, test_assign_expr);

	/* Evaluate each WHEN clause as a conditional branch chain */
	for (i = 0; i < num_whens; i++)
	{
		LLVMBasicBlockRef	when_then_bb;
		LLVMBasicBlockRef	when_else_bb;
		LLVMValueRef		cond;

		cond = upl_eval_bool(ctx, when_conds[i]);

		when_then_bb = upl_append_block(ctx, "case.when.then");
		when_else_bb = upl_append_block(ctx, "case.when.else");

		LLVMBuildCondBr(ctx->builder, cond, when_then_bb, when_else_bb);

		/* WHEN body */
		LLVMPositionBuilderAtEnd(ctx->builder, when_then_bb);

		/* Clear temp variable before executing body */
		if (has_test_expr)
		{
			LLVMValueRef args[] = {
				ctx->estate_ref,
				upl_const_int32(ctx, test_varno)
			};
			upl_emit_rt_call(ctx, ctx->callbacks.rt_assign_null, args, 2);
		}

		ctx->callbacks.compile_stmts(ctx, when_bodies[i]);
		LLVMBuildBr(ctx->builder, merge_bb);

		/* Continue to next WHEN */
		LLVMPositionBuilderAtEnd(ctx->builder, when_else_bb);
	}

	/* Clear temp variable in the fallthrough path too */
	if (has_test_expr)
	{
		LLVMValueRef args[] = {
			ctx->estate_ref,
			upl_const_int32(ctx, test_varno)
		};
		upl_emit_rt_call(ctx, ctx->callbacks.rt_assign_null, args, 2);
	}

	/* ELSE clause or error */
	if (has_else)
	{
		ctx->callbacks.compile_stmts(ctx, else_body);
	}
	else
	{
		/* SQL2003: CASE without ELSE and no match is an error */
		LLVMValueRef args[] = {
			ctx->estate_ref,
			upl_const_int32(ctx, lineno)
		};
		upl_emit_rt_call(ctx, ctx->callbacks.rt_case_error, args, 2);
	}

	LLVMBuildBr(ctx->builder, merge_bb);

	/* Continue after CASE */
	LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);
}

/*
 * Compile a RETURN statement.
 *
 * Calls the runtime function at rt_exec_return with (estate, stmt).
 * Stores the result in rc_ptr, branches to return_bb, and creates a
 * dead block for any subsequent statements.
 */
void
upl_emit_return(UPL_compile_ctx *ctx, int rt_exec_return, void *stmt)
{
	LLVMValueRef rc;

	/* Call rt_exec_return(estate, stmt) */
	{
		LLVMValueRef args[] = {
			ctx->estate_ref,
			upl_const_ptr(ctx, stmt)
		};

		rc = upl_emit_rt_call(ctx, rt_exec_return, args, 2);
	}

	/* Store the return code */
	LLVMBuildStore(ctx->builder, rc, ctx->rc_ptr);

	/* Branch to return block */
	LLVMBuildBr(ctx->builder, ctx->return_bb);

	/*
	 * Create a dead block for any statements after RETURN.
	 * LLVM requires the builder to be positioned at a valid block.
	 */
	{
		LLVMBasicBlockRef dead_bb = upl_append_block(ctx, "dead");

		LLVMPositionBuilderAtEnd(ctx->builder, dead_bb);
	}
}

/*
 * Compile a BLOCK statement with variable initialization and optional
 * exception handling.
 *
 * If has_exceptions is true, emits the full sigsetjmp pattern:
 *   - Push exception frame (allocates frame, begins subtransaction)
 *   - sigsetjmp(frame, 0) — branch try/catch
 *   - Arm exception frame (set PG_exception_stack)
 *   - Init vars, compile body (with return_bb redirected)
 *   - Try exit on normal completion
 *   - Catch: dispatch to handler via compile_exceptions callback
 *   - Rethrow for unmatched exceptions
 *
 * The compile_exceptions callback is driver-specific because exception
 * list structures vary by language.
 *
 * The rt_* indices used here come from ctx->callbacks:
 *   rt_init_var: void fn(ptr estate, i32 dno)
 *
 * Exception-related RT functions are driver-specific and accessed through
 * the compile_exceptions callback.  The core block primitive only handles
 * the sigsetjmp/branch structure and delegates handler dispatch to the
 * driver.
 *
 * If has_exceptions is false, simply initializes variables and compiles
 * the body.
 */
void
upl_emit_block(UPL_compile_ctx *ctx,
			   int n_initvars, int *initvarnos,
			   void *body_stmts,
			   bool has_exceptions, void *exception_data,
			   void (*compile_exceptions)(UPL_compile_ctx *ctx,
										  void *exception_data))
{
	int i;

	if (has_exceptions)
	{
		ctx->has_exceptions = true;

		/*
		 * Delegate the entire exception block to the driver callback.
		 * The driver handles the complete sigsetjmp pattern, variable
		 * init, body compilation, handler dispatch, and return_bb
		 * redirection.  Exception list structures and subtransaction
		 * management vary by language.
		 */
		compile_exceptions(ctx, exception_data);
		return;
	}

	/* No exceptions: initialize variables and compile body */
	for (i = 0; i < n_initvars; i++)
	{
		LLVMValueRef args[] = {
			ctx->estate_ref,
			upl_const_int32(ctx, initvarnos[i])
		};
		upl_emit_rt_call(ctx, ctx->callbacks.rt_init_var, args, 2);
	}

	/* Compile body statements */
	ctx->callbacks.compile_stmts(ctx, body_stmts);
}


/* ----------------------------------------------------------------
 *		Compilation pipeline
 * ----------------------------------------------------------------
 */

/*
 * upl_compile_function - Orchestrate the full compilation pipeline.
 *
 * This is the main entry point called by language drivers.  It:
 *   1. Creates LLVM context/module/builder
 *   2. Registers types
 *   3. Calls hooks->register_rt_funcs() for runtime function declarations
 *   4. Creates the LLVM function with UPL_FUNC_TYPE signature
 *   5. Registers sigsetjmp (returns_twice attribute)
 *   6. Creates entry/return blocks, rc alloca, estate param
 *   7. Calls hooks->setup_entry() for driver-specific setup
 *   8. Calls hooks->compile_body() for AST compilation
 *   9. Falls through to return block
 *  10. Adds nounwind if no exceptions
 *  11. Verifies, optimizes (O3), JIT compiles via OrcJIT
 *
 * Returns the native function pointer (void*).
 *
 * The entire compilation is wrapped in PG_TRY to ensure LLVM resources
 * are properly cleaned up on error.
 */
void *
upl_compile_function(UPL_compile_ctx *ctx, UPL_compile_hooks *hooks)
{
	char			func_name[NAMEDATALEN + 32];
	LLVMValueRef	estate_ref;
	LLVMValueRef	rc_val;
	void		   *fn_ptr = NULL;

	/*
	 * Generate a unique function name for LLVM.  Include a generation
	 * counter so that recompiled versions (after CREATE OR REPLACE) don't
	 * collide with the old symbol still present in OrcJIT.
	 */
	snprintf(func_name, sizeof(func_name), "%s_%u_g" UINT64_FORMAT,
			 hooks->func_name_prefix, hooks->fn_oid, compile_gen++);

	/* 1. Create LLVM context, module, builder */
	ctx->context = LLVMContextCreate();
	ctx->module = LLVMModuleCreateWithNameInContext(func_name, ctx->context);
	ctx->builder = LLVMCreateBuilderInContext(ctx->context);

	/*
	 * Wrap the rest of compilation in PG_TRY so that LLVM resources are
	 * cleaned up if any step raises an error (elog(ERROR)).
	 */
	PG_TRY();
	{
		/* 2. Register types (core engine) */
		upl_register_types(ctx);

		/* 3. Register runtime function declarations (driver) */
		hooks->register_rt_funcs(ctx);

		/* 4. Create the LLVM function: int32 func(ptr estate) */
		ctx->function = LLVMAddFunction(ctx->module, func_name,
										ctx->types[UPL_FUNC_TYPE]);

		/*
		 * 5. Register sigsetjmp as an external function with returns_twice
		 * attribute.  Signature: int sigsetjmp(ptr jmpbuf, int savesigs)
		 */
		{
			LLVMTypeRef sjparams[] = { ctx->types[UPL_PTR],
									   ctx->types[UPL_INT32] };
			LLVMTypeRef sjft = LLVMFunctionType(ctx->types[UPL_INT32],
												sjparams, 2, false);
			LLVMValueRef sjfn;
			unsigned kind;
			LLVMAttributeRef attr;

			ctx->sigsetjmp_fntype = sjft;
			sjfn = LLVMAddFunction(ctx->module, UPL_SIGSETJMP_SYM, sjft);
			ctx->sigsetjmp_fn = sjfn;

			/* Mark as returns_twice -- critical for correct codegen */
			kind = LLVMGetEnumAttributeKindForName("returns_twice", 13);
			attr = LLVMCreateEnumAttribute(ctx->context, kind, 0);
			LLVMAddAttributeAtIndex(sjfn, LLVMAttributeFunctionIndex, attr);
		}

		/* 6. Create entry and return blocks */
		ctx->entry_bb = upl_append_block(ctx, "entry");
		ctx->return_bb = upl_append_block(ctx, "return");

		LLVMPositionBuilderAtEnd(ctx->builder, ctx->entry_bb);

		/* 7. Allocate return code storage, store default */
		ctx->rc_ptr = LLVMBuildAlloca(ctx->builder, ctx->types[UPL_INT32],
									  "rc");
		LLVMBuildStore(ctx->builder,
					   upl_const_int32(ctx, hooks->default_rc), ctx->rc_ptr);

		/* 8. Get estate parameter (first arg) */
		estate_ref = LLVMGetParam(ctx->function, 0);
		LLVMSetValueName(estate_ref, "estate");
		ctx->estate_ref = estate_ref;

		/* 9. Driver-specific entry setup (load plstate, allocas, etc.) */
		hooks->setup_entry(ctx);

		/* 10. Compile the function body (driver dispatches its AST) */
		hooks->compile_body(ctx);

		/* 11. Fall through to return block */
		LLVMBuildBr(ctx->builder, ctx->return_bb);

		/* 12. Return block: load rc and return */
		LLVMPositionBuilderAtEnd(ctx->builder, ctx->return_bb);
		rc_val = LLVMBuildLoad2(ctx->builder, ctx->types[UPL_INT32],
								ctx->rc_ptr, "rc_val");
		LLVMBuildRet(ctx->builder, rc_val);

		/*
		 * 13. Add nounwind only if no exception blocks were compiled.
		 * Functions that call sigsetjmp (returns_twice) must NOT be
		 * nounwind, or LLVM may misoptimize around the setjmp point.
		 */
		if (!ctx->has_exceptions)
		{
			unsigned kind = LLVMGetEnumAttributeKindForName("nounwind", 8);
			LLVMAttributeRef attr = LLVMCreateEnumAttribute(ctx->context,
															 kind, 0);

			LLVMAddAttributeAtIndex(ctx->function,
									LLVMAttributeFunctionIndex, attr);
		}

		/* 14. Verify the module */
		upl_verify_module(ctx->module);

		/* 15. Optimize */
		upl_optimize_module(ctx->module, 3);

		/*
		 * 16. Compile via OrcJIT.
		 *
		 * upl_jit_compile() always disposes the module (via bitcode
		 * serialization), so NULL ctx->module first to prevent the
		 * PG_CATCH block from double-freeing it.
		 */
		{
			LLVMModuleRef mod = ctx->module;

			ctx->module = NULL;		/* prevent double-free on error */
			fn_ptr = upl_jit_compile(mod, ctx->context, func_name);
		}

		/* 17. Clean up builder and context */
		LLVMDisposeBuilder(ctx->builder);
		ctx->builder = NULL;
		LLVMContextDispose(ctx->context);
		ctx->context = NULL;
	}
	PG_CATCH();
	{
		/* Clean up LLVM resources on compilation failure */
		if (ctx->builder)
			LLVMDisposeBuilder(ctx->builder);
		if (ctx->module)
			LLVMDisposeModule(ctx->module);
		if (ctx->context)
			LLVMContextDispose(ctx->context);

		PG_RE_THROW();
	}
	PG_END_TRY();

	return fn_ptr;
}
