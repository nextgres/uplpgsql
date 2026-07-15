/*-------------------------------------------------------------------------
 *
 * upl_cache.c
 *		Compiled function cache — hash table keyed by function OID.
 *
 *		This cache is language-agnostic.  It stores compiled function
 *		pointers with invalidation based on fn_xmin/fn_tid (pg_proc row
 *		version) and a driver-supplied staleness check callback.
 *
 *		Three cache states per entry:
 *		  - Not present (MISS): function not yet seen → evaluate heuristic,
 *		    then either compile and store, or store skip marker
 *		  - jit_func == skip sentinel (SKIP): heuristic decided JIT
 *		    wouldn't help → use interpreter on all future calls
 *		  - jit_func points to a real UPL_func (HIT): use JIT'd code
 *
 *		Cache invalidation:
 *		  Each entry stores fn_xmin/fn_tid from the pg_proc row at the
 *		  time of compilation, plus an opaque lang_func pointer.  On
 *		  lookup, fn_xmin/fn_tid are compared against the current values.
 *		  The driver-supplied check_fn callback compares lang_func pointers
 *		  to detect when the language compiler recreated its function struct.
 *
 *		Memory management:
 *		  The hash table lives in TopMemoryContext and persists for the
 *		  lifetime of the backend.  UPL_func entries are also in
 *		  TopMemoryContext.  Old JIT'd code is intentionally leaked when
 *		  cache entries are invalidated (LLJIT doesn't support cheap
 *		  per-function removal).
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

#include "utils/hsearch.h"
#include "utils/memutils.h"

/*
 * Sentinel value stored in jit_func to indicate the heuristic decided
 * not to JIT this function.  Must be distinguishable from NULL (not cached)
 * and from any valid pointer.
 */
static UPL_func upl_skip_jit_sentinel;

#define UPL_SKIP_JIT (&upl_skip_jit_sentinel)

typedef struct UPL_cache_entry
{
	Oid				fn_oid;			/* hash key */
	UPL_func	   *jit_func;		/* NULL = not yet decided, SKIP, or real */
	TransactionId	fn_xmin;		/* pg_proc row version for invalidation */
	ItemPointerData	fn_tid;
	void		   *lang_func;		/* opaque language-specific function struct */
} UPL_cache_entry;

static HTAB *upl_cache_htab = NULL;

/*
 * upl_cache_init - Initialize the function cache hash table.
 */
void
upl_cache_init(void)
{
	HASHCTL		ctl;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(UPL_cache_entry);
	ctl.hcxt = TopMemoryContext;

	upl_cache_htab = hash_create("upl function cache",
								 32,
								 &ctl,
								 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/*
 * upl_cache_lookup - Look up a compiled function in the cache.
 *
 * Parameters:
 *   fn_oid, fn_xmin, fn_tid: current function identity from pg_proc
 *   current_lang_func: current language-specific function struct pointer
 *   check_fn: callback to compare cached vs current lang_func (may be NULL
 *             if no lang_func staleness check is needed)
 *   jitfunc_out: set to the cached compiled function on HIT
 *
 * Returns:
 *   UPL_CACHE_MISS   - not in cache, caller should evaluate heuristic
 *   UPL_CACHE_SKIP   - heuristic said skip JIT, use interpreter
 *   UPL_CACHE_HIT    - *jitfunc_out set to compiled function
 */
int
upl_cache_lookup(Oid fn_oid, TransactionId fn_xmin, ItemPointerData fn_tid,
				 void *current_lang_func, upl_cache_check_fn check_fn,
				 UPL_func **jitfunc_out)
{
	UPL_cache_entry	   *entry;
	bool				found;

	*jitfunc_out = NULL;

	if (upl_cache_htab == NULL)
		return UPL_CACHE_MISS;

	entry = (UPL_cache_entry *) hash_search(upl_cache_htab,
											&fn_oid,
											HASH_FIND,
											&found);
	if (!found)
		return UPL_CACHE_MISS;

	/*
	 * Check if the function has been replaced (CREATE OR REPLACE).
	 * The language compiler updates fn_xmin/fn_tid when it recompiles
	 * the AST from a changed pg_proc row.
	 *
	 * Also check via the driver callback if the language compiler returned
	 * a different function struct.  The JIT'd code has AST node pointers
	 * embedded as LLVM constants from the original struct.  If the compiler
	 * recreated the struct, those embedded pointers are dangling.
	 */
	if (entry->fn_xmin != fn_xmin ||
		!ItemPointerEquals(&entry->fn_tid, &fn_tid) ||
		(check_fn && !check_fn(entry->lang_func, current_lang_func)))
	{
		elog(LOG, "upl: invalidating cache for function %u "
			 "(xmin %u -> %u, lang_func %p -> %p)",
			 fn_oid,
			 entry->fn_xmin, fn_xmin,
			 entry->lang_func, current_lang_func);

		/*
		 * Remove the entry so the heuristic is re-evaluated.
		 * Old JIT'd code (if any) is intentionally leaked.
		 */
		hash_search(upl_cache_htab, &fn_oid, HASH_REMOVE, NULL);
		return UPL_CACHE_MISS;
	}

	if (entry->jit_func == UPL_SKIP_JIT)
		return UPL_CACHE_SKIP;

	*jitfunc_out = entry->jit_func;
	return UPL_CACHE_HIT;
}

/*
 * upl_cache_store - Store a compiled function in cache.
 */
void
upl_cache_store(Oid fn_oid, TransactionId fn_xmin, ItemPointerData fn_tid,
				void *lang_func, UPL_func *jitfunc)
{
	UPL_cache_entry	   *entry;
	bool				found;

	if (upl_cache_htab == NULL)
		upl_cache_init();

	entry = (UPL_cache_entry *) hash_search(upl_cache_htab,
											&fn_oid,
											HASH_ENTER,
											&found);
	entry->jit_func = jitfunc;
	entry->fn_xmin = fn_xmin;
	entry->fn_tid = fn_tid;
	entry->lang_func = lang_func;
}

/*
 * upl_cache_store_skip - Record that this function should not be JIT'd.
 */
void
upl_cache_store_skip(Oid fn_oid, TransactionId fn_xmin, ItemPointerData fn_tid,
					 void *lang_func)
{
	upl_cache_store(fn_oid, fn_xmin, fn_tid, lang_func, UPL_SKIP_JIT);
}
