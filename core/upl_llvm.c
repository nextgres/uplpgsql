/*-------------------------------------------------------------------------
 *
 * upl_llvm.c
 *		Low-level LLVM C API utilities for UPL Core Engine.
 *
 *		This file manages the LLVM infrastructure that underlies the JIT
 *		compiler.  It is completely language-agnostic — it knows nothing
 *		about statements, expressions, or any specific procedural language.
 *
 *		Key components:
 *
 *		upl_llvm_init()       — One-time initialization per backend:
 *		  initializes LLVM native target, creates a target machine for
 *		  the host platform, and creates a single LLJIT instance that is
 *		  shared by all compiled functions in the backend.  Registers a
 *		  process symbol search generator so that runtime helpers (and any
 *		  other process symbols) are automatically resolvable from JIT'd
 *		  code without explicit symbol registration.
 *
 *		upl_register_types()  — Populates ctx->types[] with LLVM type
 *		  references (void, i1, i8, i16, i32, i64, double, ptr, Datum,
 *		  function type).  Called once per compilation.
 *
 *		upl_verify_module()   — Runs LLVM's IR verification pass.
 *		  Catches malformed IR before it reaches the optimizer or code gen.
 *
 *		upl_optimize_module() — Runs the LLVM new pass manager with
 *		  a specified optimization level (O0-O3).  Uses the target machine
 *		  for target-specific optimizations.
 *
 *		upl_jit_compile()     — The final step: serializes the module
 *		  to bitcode, deserializes in a new OrcJIT ThreadSafeContext (because
 *		  the LLVM C API doesn't support moving modules between contexts),
 *		  adds the module to LLJIT, and looks up the function symbol to
 *		  return a native function pointer.
 *
 *		Design notes:
 *		  - One LLJIT per backend (not per function) to amortize setup cost
 *		  - Bitcode round-trip is the only portable way to transfer modules
 *		    to OrcJIT's context via the LLVM C API
 *		  - Old compiled functions are intentionally leaked because LLJIT
 *		    doesn't support cheap per-function removal
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
 * Per-backend LLVM state.
 *
 * upl_jit_instance: the single LLJIT instance shared by all compiled
 *   functions in this backend.  Created once in upl_llvm_init().
 *
 * upl_target_machine: used for optimization passes (the new pass manager
 *   needs a target machine to apply target-specific transformations).
 *
 * upl_llvm_initialized: prevents double initialization.
 */
static LLVMOrcLLJITRef upl_jit_instance = NULL;
static LLVMTargetMachineRef upl_target_machine = NULL;
static bool upl_llvm_initialized = false;

/*
 * upl_llvm_init - Initialize LLVM (once per process)
 */
void
upl_llvm_init(void)
{
	if (upl_llvm_initialized)
		return;

	LLVMInitializeNativeTarget();
	LLVMInitializeNativeAsmPrinter();
	LLVMInitializeNativeAsmParser();

	/*
	 * Create target machine (used for optimization passes).
	 *
	 * Guarded so that a retry — reached when a step below elog(ERROR)s and
	 * longjmps out before upl_llvm_initialized is set — reuses the machine
	 * built by the failed attempt instead of leaking it and building another.
	 */
	if (upl_target_machine == NULL)
	{
		char		   *triple;
		char		   *error;
		LLVMTargetRef	target;

		triple = LLVMGetDefaultTargetTriple();

		if (LLVMGetTargetFromTriple(triple, &target, &error))
		{
			char *msg = pstrdup(error);

			LLVMDisposeMessage(error);
			LLVMDisposeMessage(triple);
			elog(ERROR, "upl: failed to get LLVM target: %s", msg);
		}

		{
			char *host_cpu = LLVMGetHostCPUName();
			char *host_features = LLVMGetHostCPUFeatures();

			upl_target_machine = LLVMCreateTargetMachine(
				target,
				triple,
				host_cpu,
				host_features,
				LLVMCodeGenLevelAggressive,
				LLVMRelocDefault,
				LLVMCodeModelJITDefault);

			LLVMDisposeMessage(host_cpu);
			LLVMDisposeMessage(host_features);
		}

		/*
		 * LLVMCreateTargetMachine reports failure by returning NULL; it has
		 * no error-message out-param, so name the triple we failed on.
		 */
		if (upl_target_machine == NULL)
		{
			char *msg = pstrdup(triple);

			LLVMDisposeMessage(triple);
			elog(ERROR, "upl: failed to create LLVM target machine for %s",
				 msg);
		}

		LLVMDisposeMessage(triple);
	}

	/*
	 * Create OrcJIT instance
	 */
	{
		LLVMOrcLLJITBuilderRef	builder;
		LLVMErrorRef			err;

		builder = LLVMOrcCreateLLJITBuilder();

		err = LLVMOrcCreateLLJIT(&upl_jit_instance, builder);
		if (err)
		{
			char *msg = LLVMGetErrorMessage(err);

			elog(ERROR, "upl: failed to create OrcJIT: %s", msg);
		}

		/*
		 * Register process symbols so runtime helpers are automatically
		 * resolvable from JIT'd code.
		 */
		{
			LLVMOrcDefinitionGeneratorRef	gen;
			LLVMOrcJITDylibRef				dylib;

			err = LLVMOrcCreateDynamicLibrarySearchGeneratorForProcess(
				&gen,
				LLVMOrcLLJITGetGlobalPrefix(upl_jit_instance),
				NULL, NULL);
			if (err)
			{
				char *msg = LLVMGetErrorMessage(err);

				elog(ERROR, "upl: failed to create symbol generator: %s", msg);
			}

			dylib = LLVMOrcLLJITGetMainJITDylib(upl_jit_instance);
			LLVMOrcJITDylibAddGenerator(dylib, gen);
		}
	}

	/*
	 * Set last, once the target machine and JIT instance both exist.  The
	 * steps above elog(ERROR) on failure, which longjmps out; setting the
	 * flag any earlier would make a retry short-circuit above and leave
	 * upl_jit_instance NULL for the life of the backend.
	 */
	upl_llvm_initialized = true;

	elog(DEBUG1, "upl: LLVM %d.%d initialized, OrcJIT ready",
		 LLVM_VERSION_MAJOR, LLVM_VERSION_MINOR);
}

/*
 * upl_llvm_shutdown - Clean up LLVM resources
 */
void
upl_llvm_shutdown(void)
{
	if (upl_jit_instance)
	{
		LLVMOrcDisposeLLJIT(upl_jit_instance);
		upl_jit_instance = NULL;
	}
	if (upl_target_machine)
	{
		LLVMDisposeTargetMachine(upl_target_machine);
		upl_target_machine = NULL;
	}
}

/*
 * upl_get_jit - Get the per-backend OrcJIT instance
 */
LLVMOrcLLJITRef
upl_get_jit(void)
{
	Assert(upl_jit_instance != NULL);
	return upl_jit_instance;
}

/*
 * upl_register_types - Register LLVM types in a compilation context
 */
void
upl_register_types(UPL_compile_ctx *ctx)
{
	ctx->types[UPL_VOID]   = LLVMVoidTypeInContext(ctx->context);
	ctx->types[UPL_INT1]   = LLVMInt1TypeInContext(ctx->context);
	ctx->types[UPL_INT8]   = LLVMInt8TypeInContext(ctx->context);
	ctx->types[UPL_INT16]  = LLVMInt16TypeInContext(ctx->context);
	ctx->types[UPL_INT32]  = LLVMInt32TypeInContext(ctx->context);
	ctx->types[UPL_INT64]  = LLVMInt64TypeInContext(ctx->context);
	ctx->types[UPL_DOUBLE] = LLVMDoubleTypeInContext(ctx->context);
	ctx->types[UPL_PTR]    = LLVMPointerTypeInContext(ctx->context, 0);
	ctx->types[UPL_INTPTR] = LLVMInt64TypeInContext(ctx->context);	/* 64-bit platforms */
	ctx->types[UPL_DATUM]  = ctx->types[UPL_INT64];

	/* Function type: int32 func(ptr estate) */
	{
		LLVMTypeRef params[] = { ctx->types[UPL_PTR] };

		ctx->types[UPL_FUNC_TYPE] = LLVMFunctionType(
			ctx->types[UPL_INT32], params, 1, false);
	}
}

/*
 * upl_verify_module - Verify LLVM module IR is well-formed
 */
void
upl_verify_module(LLVMModuleRef module)
{
	char *error = NULL;

	if (LLVMVerifyModule(module, LLVMReturnStatusAction, &error))
	{
		char *msg = pstrdup(error);

		LLVMDisposeMessage(error);
		elog(ERROR, "upl: LLVM module verification failed: %s", msg);
	}

	if (error)
		LLVMDisposeMessage(error);
}

/*
 * upl_optimize_module - Run LLVM optimization passes
 */
void
upl_optimize_module(LLVMModuleRef module, int level)
{
	const char *passes[] = {
		"default<O0>",
		"default<O1>",
		"default<O2>",
		"default<O3>"
	};
	LLVMPassBuilderOptionsRef	opts;
	LLVMErrorRef				err;

	if (level < 0 || level > 3)
		level = 2;

	opts = LLVMCreatePassBuilderOptions();

	err = LLVMRunPasses(module, passes[level], upl_target_machine, opts);
	if (err)
	{
		char *msg = LLVMGetErrorMessage(err);
		char *pstr = pstrdup(msg);

		LLVMDisposeErrorMessage(msg);
		LLVMDisposePassBuilderOptions(opts);
		elog(ERROR, "upl: LLVMRunPasses failed: %s", pstr);
	}

	LLVMDisposePassBuilderOptions(opts);
}

/*
 * upl_jit_compile - Add module to OrcJIT and look up a function.
 *
 * Takes ownership of the module. Returns the native function pointer.
 */
void *
upl_jit_compile(LLVMModuleRef module, LLVMContextRef context,
				const char *func_name)
{
	LLVMOrcThreadSafeContextRef		ts_ctx;
	LLVMOrcThreadSafeModuleRef		ts_mod;
	LLVMOrcJITDylibRef				dylib;
	LLVMOrcExecutorAddress			addr;
	LLVMErrorRef					err;

	/*
	 * We need to transfer the module to a fresh context owned by OrcJIT.
	 * Since LLVM C API doesn't support transferring modules between contexts,
	 * we serialize to bitcode and deserialize in a new context.
	 *
	 * As of LLVM 15+, LLVMOrcThreadSafeContextGetContext() was removed and
	 * ownership was inverted: the caller creates the LLVMContext, parses the
	 * module into it, then hands it to a ThreadSafeContext via
	 * LLVMOrcCreateNewThreadSafeContextFromLLVMContext(), which takes
	 * ownership of the context.
	 */
	{
		LLVMMemoryBufferRef		buf;
		LLVMModuleRef			new_module;
		LLVMContextRef			new_ctx;

		/* Serialize to bitcode */
		buf = LLVMWriteBitcodeToMemoryBuffer(module);

		/* Create a fresh context and parse the bitcode into it */
		new_ctx = LLVMContextCreate();

		if (LLVMParseBitcodeInContext2(new_ctx, buf, &new_module))
		{
			LLVMDisposeMemoryBuffer(buf);
			LLVMContextDispose(new_ctx);
			LLVMDisposeModule(module);
			elog(ERROR, "upl: failed to parse bitcode in OrcJIT context");
		}

		/*
		 * Wrap the fresh context in a ThreadSafeContext (takes ownership of
		 * new_ctx), then wrap the module (takes ownership of new_module).
		 */
		ts_ctx = LLVMOrcCreateNewThreadSafeContextFromLLVMContext(new_ctx);
		ts_mod = LLVMOrcCreateNewThreadSafeModule(new_module, ts_ctx);

		/* Dispose original module (we have the copy now) */
		LLVMDisposeModule(module);
	}

	/* Add module to OrcJIT — takes ownership of ts_mod */
	dylib = LLVMOrcLLJITGetMainJITDylib(upl_jit_instance);
	err = LLVMOrcLLJITAddLLVMIRModule(upl_jit_instance, dylib, ts_mod);
	if (err)
	{
		char *msg = LLVMGetErrorMessage(err);
		char *pstr = pstrdup(msg);

		LLVMDisposeErrorMessage(msg);
		LLVMOrcDisposeThreadSafeContext(ts_ctx);
		elog(ERROR, "upl: failed to add module to OrcJIT: %s", pstr);
	}

	/* Look up the compiled function */
	err = LLVMOrcLLJITLookup(upl_jit_instance, &addr, func_name);
	if (err)
	{
		char *msg = LLVMGetErrorMessage(err);
		char *pstr = pstrdup(msg);

		LLVMDisposeErrorMessage(msg);
		elog(ERROR, "upl: symbol lookup failed for %s: %s",
			 func_name, pstr);
	}

	elog(DEBUG1, "upl: JIT compiled %s at %p", func_name, (void *)(uintptr_t)addr);

	return (void *)(uintptr_t)addr;
}
