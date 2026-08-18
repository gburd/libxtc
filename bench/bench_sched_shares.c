/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * bench/bench_sched_shares.c
 *	MEASURE-FIRST for the L1 proportional-share (weighted-fair)
 *	scheduler.  Demonstrates the GAP that L1 fills and, once the
 *	scheduler exists, the AFTER number.
 *
 *	INSPIRED BY Glommio (Glauber Costa / ScyllaDB): Glommio's task
 *	queues carry SHARES (1..1000) and a CFS-style vruntime scheduler
 *	gives each queue a weighted CPU fraction.  libxtc had NOTHING
 *	equivalent -- a plain FIFO run queue splits CPU 1:1 among
 *	equally-busy workers no matter what weighting you want.
 *
 *	Setup: ONE loop, two GROUPS of well-behaved worker fibers -- class
 *	A and class B -- each fiber doing equal-size compute chunks with a
 *	cooperative xtc_yield between chunks.  We count how many chunks
 *	each class completes in a fixed wall-clock window; the ratio A:B
 *	is the CPU split.
 *
 *	  BEFORE (plain FIFO, the current run queue): A:B ~= 1:1 no matter
 *	         what -- there is no way to say "A should get 3x B".  THIS
 *	         is the gap.
 *	  AFTER  (two scheduling classes, A weighted 3:1 over B): A:B ~= 3:1.
 *
 *	bench_fairness.c measures a DIFFERENT thing (a single runaway that
 *	never yields, and the cooperative yield-budget remedy); do not
 *	conflate.  This bench is about PROPORTIONAL SHARE among
 *	well-behaved workers.
 *
 *	Usage: bench_sched_shares [window_ms]   (default 300)
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_async.h"
#include "xtc_proc.h"

#define N_PER_CLASS 4            /* fibers in each class */
#define CHUNK_ITERS 2000         /* compute per chunk (equal for A and B) */

static atomic_long g_a_chunks;
static atomic_long g_b_chunks;
static atomic_int  g_stop;
static volatile long g_sink;     /* defeat dead-code elimination */

/* A well-behaved worker: compute one equal-size chunk, then yield,
 * until the window closes.  `which` is 0 for class A, 1 for class B. */
static void
worker(void *arg)
{
	int which = (int)(intptr_t)arg;
	while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
		long acc = 0, i;
		for (i = 0; i < CHUNK_ITERS; i++)
			acc += (i ^ (i << 1)) + (acc >> 3);
		g_sink += acc;
		if (which == 0)
			atomic_fetch_add_explicit(&g_a_chunks, 1,
			    memory_order_relaxed);
		else
			atomic_fetch_add_explicit(&g_b_chunks, 1,
			    memory_order_relaxed);
		xtc_yield();
	}
}

/* Stop the window after `ms` milliseconds by spinning on the wall
 * clock inside a fiber that yields (so it does not itself monopolize). */
static long g_window_ms;
static void
timekeeper(void *arg)
{
	int64_t start, now;
	(void)arg;
	start = xtc_clock_mono();
	for (;;) {
		now = xtc_clock_mono();
		if ((now - start) / (1000 * 1000LL) >= g_window_ms)
			break;
		xtc_yield();
	}
	atomic_store_explicit(&g_stop, 1, memory_order_relaxed);
}

/*
 * Run one window.  `use_shares` selects the scenario:
 *   0 -- plain FIFO (no classes created): the BEFORE case.
 *   1 -- class A weighted `a_shares`, class B weighted `b_shares`.
 * Returns via *out_a / *out_b the chunks completed by each class.
 */
static int
run_window(int use_shares, int a_shares, int b_shares,
    long *out_a, long *out_b)
{
	xtc_loop_t *loop;
	xtc_exec_class_t ca = NULL, cb = NULL;
	xtc_proc_opts_t oa, ob;
	int i;

	atomic_store(&g_a_chunks, 0);
	atomic_store(&g_b_chunks, 0);
	atomic_store(&g_stop, 0);

	if (xtc_loop_init(&loop) != XTC_OK) {
		fprintf(stderr, "loop_init failed\n");
		return -1;
	}

	memset(&oa, 0, sizeof oa);
	memset(&ob, 0, sizeof ob);

	if (use_shares) {
		if (xtc_exec_class_create(loop, a_shares, 0, &ca) != XTC_OK ||
		    xtc_exec_class_create(loop, b_shares, 0, &cb) != XTC_OK) {
			fprintf(stderr, "class_create failed\n");
			(void)xtc_loop_fini(loop);
			return -1;
		}
		oa.sched_class = ca;
		ob.sched_class = cb;
	}

	for (i = 0; i < N_PER_CLASS; i++) {
		(void)xtc_proc_spawn(loop, worker, (void *)(intptr_t)0,
		    use_shares ? &oa : NULL, NULL);
		(void)xtc_proc_spawn(loop, worker, (void *)(intptr_t)1,
		    use_shares ? &ob : NULL, NULL);
	}
	(void)xtc_proc_spawn(loop, timekeeper, NULL, NULL, NULL);

	(void)xtc_loop_run(loop);
	(void)xtc_loop_fini(loop);

	*out_a = atomic_load(&g_a_chunks);
	*out_b = atomic_load(&g_b_chunks);
	return 0;
}

int
main(int argc, char **argv)
{
	long a0 = 0, b0 = 0, a1 = 0, b1 = 0;

	g_window_ms = argc > 1 ? atol(argv[1]) : 300;
	if (g_window_ms < 1)
		g_window_ms = 1;

	printf("# libxtc proportional-share scheduler (L1), inspired by "
	    "Glommio\n");
	printf("# %d workers/class, equal compute chunks, %ld ms window.\n",
	    N_PER_CLASS, g_window_ms);
	printf("# CPU split measured as completed-chunk ratio A:B.\n\n");

	if (run_window(0, 0, 0, &a0, &b0) != 0)
		return 1;
	printf("BEFORE (plain FIFO run queue):   A=%ld B=%ld  ratio=%.2f:1"
	    "   <- ~1:1, no way to weight (the gap)\n",
	    a0, b0, b0 ? (double)a0 / (double)b0 : 0.0);

	if (run_window(1, 3, 1, &a1, &b1) != 0)
		return 1;
	printf("AFTER  (class A shares 3, B shares 1): A=%ld B=%ld  "
	    "ratio=%.2f:1   <- ~3:1, weighted-fair\n",
	    a1, b1, b1 ? (double)a1 / (double)b1 : 0.0);

	printf("\nresult: FIFO gives A:B ~= %.2f:1 regardless of intent; "
	    "3:1 shares give A:B ~= %.2f:1.\n",
	    b0 ? (double)a0 / (double)b0 : 0.0,
	    b1 ? (double)a1 / (double)b1 : 0.0);
	return 0;
}
