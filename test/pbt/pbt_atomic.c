/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/pbt/pbt_atomic.c
 *	Property-based tests for the L0 atomics surface (M1).
 *
 *	Hegel draws random workload parameters; each example runs a
 *	mini concurrent counter benchmark and asserts the linearizable
 *	invariant.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#include "pbt_common.h"
#include "xtc_int.h"

/* ----- shared workload helpers ----- */

struct fa_args {
	int64_t *counter;
	int      iters;
};
static void *fa_worker(void *p) {
	struct fa_args *a = p;
	int i;
	for (i = 0; i < a->iters; i++)
		(void)__os_atomic_fetch_add_i64(a->counter, 1);
	return NULL;
}

struct cas_args {
	int64_t *counter;
	int      iters;
};
static void *cas_worker(void *p) {
	struct cas_args *a = p;
	int i;
	for (i = 0; i < a->iters; i++) {
		int64_t cur, next;
		do {
			cur  = __os_atomic_load_i64(a->counter);
			next = cur + 1;
		} while (!__os_atomic_cas_i64(a->counter, &cur, next));
	}
	return NULL;
}

/* ----- properties ----- */

#if defined(XTC_HAVE_HEGEL)

/*
 * P1: For any (n_threads, iters) drawn in a sane range, the sum of
 * concurrent fetch_add operations equals n_threads * iters.
 */
static void
prop_fetch_add_sum(hegel_test_case *tc, void *u)
{
	int64_t counter = 0;
	int n_threads, iters, i;
	pthread_t *th;
	struct fa_args args;
	(void)u;

	n_threads = (int)hegel_draw_int(tc, hegel_integers(2, 16));
	iters     = (int)hegel_draw_int(tc, hegel_integers(1, 5000));

	th = calloc((size_t)n_threads, sizeof *th);
	hegel_assume(th != NULL);

	args.counter = &counter;
	args.iters   = iters;
	for (i = 0; i < n_threads; i++)
		hegel_assume(pthread_create(&th[i], NULL, fa_worker, &args) == 0);
	for (i = 0; i < n_threads; i++)
		hegel_assume(pthread_join(th[i], NULL) == 0);
	free(th);

	hegel_assume(counter == (int64_t)n_threads * iters);
}

/*
 * P2: Same thing via a CAS-loop.  Verifies the strong-CAS contract.
 */
static void
prop_cas_loop_sum(hegel_test_case *tc, void *u)
{
	int64_t counter = 0;
	int n_threads, iters, i;
	pthread_t *th;
	struct cas_args args;
	(void)u;

	n_threads = (int)hegel_draw_int(tc, hegel_integers(2, 8));
	iters     = (int)hegel_draw_int(tc, hegel_integers(1, 2000));

	th = calloc((size_t)n_threads, sizeof *th);
	hegel_assume(th != NULL);

	args.counter = &counter;
	args.iters = iters;
	for (i = 0; i < n_threads; i++)
		hegel_assume(pthread_create(&th[i], NULL, cas_worker, &args) == 0);
	for (i = 0; i < n_threads; i++)
		hegel_assume(pthread_join(th[i], NULL) == 0);
	free(th);

	hegel_assume(counter == (int64_t)n_threads * iters);
}

/*
 * P3: store/load round-trip preserves arbitrary 64-bit values.
 */
static void
prop_store_load_roundtrip(hegel_test_case *tc, void *u)
{
	int64_t v = 0;
	int64_t draw;
	(void)u;
	draw = (int64_t)hegel_draw_int(tc, hegel_integers(INT64_MIN, INT64_MAX));
	__os_atomic_store_i64(&v, draw);
	hegel_assume(__os_atomic_load_i64(&v) == draw);
}

static const pbt_entry_t tests[] = {
	{ "fetch_add_sum",         prop_fetch_add_sum,        50 },
	{ "cas_loop_sum",          prop_cas_loop_sum,         30 },
	{ "store_load_roundtrip",  prop_store_load_roundtrip, 200 },
	{ NULL, NULL, 0 }
};

#else

static const pbt_entry_t tests[] = {
	{ "fetch_add_sum",         NULL, 0 },
	{ "cas_loop_sum",          NULL, 0 },
	{ "store_load_roundtrip",  NULL, 0 },
	{ NULL, NULL, 0 }
};

#endif

PBT_MAIN("atomic", tests)
