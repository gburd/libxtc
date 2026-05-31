/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_mem.c
 *	The xtc-allocator SQLite-memory seam (mem.c).  Proves that with
 *	sx_config_mem(mem_methods()) installed, the SQL engine's
 *	allocations flow through xtc's allocator -- not bare malloc(3).
 *
 *	The proof: install an __os_alloc hook that counts every call the
 *	xtc allocator makes (and delegates to libc), then drive a real
 *	SQL workload (DDL + 200 inserts + an aggregate read) through the
 *	sx_ engine.  Because mem.c routes sqlite3_malloc/realloc/free to
 *	__os_malloc/realloc/free, the engine's allocations land on the
 *	hook: the post-workload delta must be large, and every byte SQLite
 *	asked for must round-trip (the workload result must be correct).
 *	No daemon; standalone, plain asserts.
 */

#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "mem.h"
#include "mutex.h"

#include "os_alloc.h"

/* Counting allocator hook: delegates to libc, tallies each call. */
static _Atomic long h_malloc, h_calloc, h_realloc, h_free;

static void *h_m(size_t n)              { atomic_fetch_add(&h_malloc, 1);  return malloc(n); }
static void *h_c(size_t n, size_t s)    { atomic_fetch_add(&h_calloc, 1);  return calloc(n, s); }
static void *h_r(void *p, size_t n)     { atomic_fetch_add(&h_realloc, 1); return realloc(p, n); }
static void  h_f(void *p)               { if (p) atomic_fetch_add(&h_free, 1); free(p); }
static void *h_a(size_t a, size_t n)
{
	void *p = NULL;
	if (posix_memalign(&p, a, n) != 0)
		return NULL;
	atomic_fetch_add(&h_malloc, 1);
	return p;
}
static void  h_af(void *p)              { if (p) atomic_fetch_add(&h_free, 1); free(p); }

static const struct __os_alloc_hook counting_hook = {
	h_m, h_c, h_r, h_f, h_a, h_af,
};

int
main(void)
{
	sx_db *db = NULL;
	sx_stmt *st = NULL;
	char sql[128];
	long before, delta;
	int i, rc;
	int64_t n_rows = 0;

	/* Route the xtc allocator through the counting hook FIRST, so even
	 * the engine's own xtc allocations (mutex, etc.) are observable;
	 * we measure SQLite's share as the delta across the workload. */
	assert(__os_alloc_set_hook(&counting_hook) == 0);

	/* Install the xtc-backed memory + mutex seams, then init. */
	assert(sx_config_mem(mem_methods()) == SX_OK);
	(void)sx_config_mutex(mutex_methods());
	(void)sx_config_serialized();
	assert(sx_init() == SX_OK);

	assert(sx_open(":memory:", &db) == SX_OK);
	assert(sx_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);",
	    NULL) == SX_OK);

	before = atomic_load(&h_malloc) + atomic_load(&h_realloc);

	for (i = 0; i < 200; i++) {
		snprintf(sql, sizeof sql,
		    "INSERT INTO t(id, v) VALUES(%d, 'row-%06d-payload');", i, i);
		rc = sx_exec(db, sql, NULL);
		assert(rc == SX_OK);
	}

	/* Aggregate read: confirms the data is real, not just allocated. */
	assert(sx_prepare(db, "SELECT count(*), sum(length(v)) FROM t;", -1,
	    &st, NULL) == SX_OK);
	assert(sx_step(st) == SX_ROW);
	n_rows = sx_column_int64(st, 0);
	{
		int64_t sum_len = sx_column_int64(st, 1);
		assert(n_rows == 200);
		/* each value "row-%06d-payload" == 17 chars */
		assert(sum_len == 200 * (int64_t)strlen("row-000000-payload"));
	}
	sx_finalize(st);
	sx_close(db);

	delta = (atomic_load(&h_malloc) + atomic_load(&h_realloc)) - before;

	(void)sx_shutdown();

	/* The workload must have driven many allocations THROUGH xtc. */
	if (delta < 50) {
		fprintf(stderr,
		    "FAIL: only %ld xtc allocations during the SQL workload "
		    "(expected the engine to allocate through xtc)\n", delta);
		return 1;
	}

	printf("  ok   SQLite memory routed through xtc: %lld rows inserted, "
	    "%ld allocations via __os_alloc during the workload\n",
	    (long long)n_rows, delta);
	printf("  ok   allocator tallies: malloc=%ld calloc=%ld realloc=%ld "
	    "free=%ld (all through the xtc allocator hook)\n",
	    atomic_load(&h_malloc), atomic_load(&h_calloc),
	    atomic_load(&h_realloc), atomic_load(&h_free));
	printf("All sqlxtc memory-seam tests passed.\n");
	return 0;
}
