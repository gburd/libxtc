/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * bench/bench_saga.c
 *	Measure the overhead of xtc_saga's bookkeeping: running a saga of
 *	K steps versus calling the SAME K step functions directly, with
 *	no saga machinery at all (no step array, no completed-count
 *	tracking, no compensation walk).  Both paths run the identical
 *	step-action body (a tiny counter bump) so the delta is purely the
 *	saga overhead: xtc_saga_create/step/run/destroy's allocation and
 *	bookkeeping cost, per saga execution, amortized over K.
 *
 *	Every saga run in this benchmark succeeds (no compensation walk):
 *	that walk's cost is a separate, already-covered path (the DST test
 *	and unit test exercise it functionally; its cost is one extra
 *	function call per completed step, symmetric with the forward
 *	pass, so it does not need its own number here).
 *
 *	Usage: bench_saga [iterations]   (default 200000)
 *	Sweeps K = 1, 4, 16, 64 steps per saga.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "xtc.h"
#include "xtc_saga.h"

static long g_counter;   /* volatile-ish sink: prevents dead-code elision */

static int
step_direct(void *arg)
{
	(void)arg;
	g_counter++;
	return XTC_OK;
}

static int
step_compensate(void *arg)
{
	(void)arg;
	return XTC_OK;
}

/* Direct path: call the K step functions with no saga bookkeeping at
 * all -- a plain loop of function calls. */
static void
run_direct(int k)
{
	int i;
	for (i = 0; i < k; i++)
		(void)step_direct(NULL);
}

/* Saga path: build a fresh K-step saga and run it, every iteration
 * (a saga runs at most once, so this is the realistic per-use cost --
 * amortizing saga_create/destroy across many runs is not how the API
 * is meant to be used). */
static int
run_saga(int k)
{
	xtc_saga_t *s = NULL;
	int i, rc;

	if (xtc_saga_create(&s) != XTC_OK) return XTC_E_NOMEM;
	for (i = 0; i < k; i++) {
		if (xtc_saga_step(s, step_direct, step_compensate, NULL)
		    != XTC_OK) { xtc_saga_destroy(s); return XTC_E_INTERNAL; }
	}
	rc = xtc_saga_run(s);
	xtc_saga_destroy(s);
	return rc;
}

static void
bench_k(int k, long iters)
{
	int64_t t0, t1;
	long i;
	double direct_ns, saga_ns;

	t0 = xtc_clock_mono();
	for (i = 0; i < iters; i++)
		run_direct(k);
	t1 = xtc_clock_mono();
	direct_ns = (double)(t1 - t0) / (double)iters;

	t0 = xtc_clock_mono();
	for (i = 0; i < iters; i++) {
		if (run_saga(k) != XTC_OK) {
			fprintf(stderr, "bench_saga: unexpected saga failure "
			    "at k=%d\n", k);
			exit(1);
		}
	}
	t1 = xtc_clock_mono();
	saga_ns = (double)(t1 - t0) / (double)iters;

	printf("K=%3d  direct=%9.1f ns/run  saga=%9.1f ns/run  "
	    "overhead=%8.1f ns/run (%.1f ns/step)  %.2fx\n",
	    k, direct_ns, saga_ns, saga_ns - direct_ns,
	    (saga_ns - direct_ns) / (double)k,
	    direct_ns > 0 ? saga_ns / direct_ns : 0.0);
}

int
main(int argc, char **argv)
{
	long iters = argc > 1 ? atol(argv[1]) : 200000;
	static const int ks[] = { 1, 4, 16, 64 };
	size_t i;

	printf("bench_saga: %ld iterations per K\n", iters);
	for (i = 0; i < sizeof ks / sizeof ks[0]; i++)
		bench_k(ks[i], iters);

	/* Defeat dead-code elimination of the counter bumps. */
	if (g_counter < 0) return 1;
	return 0;
}
