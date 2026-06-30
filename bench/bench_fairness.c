/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * bench/bench_fairness.c
 *	Measure FAIRNESS UNDER A RUNAWAY TASK -- the axis where libxtc's
 *	cooperative scheduling is known to be weaker than BEAM's
 *	reduction-counting preemption.  libxtc cannot forcibly preempt a
 *	fiber; a tight CPU loop that never yields starves its loop.  This
 *	benchmark quantifies that starvation and shows how the
 *	cooperative yield-budget watchdog (xtc_yield_if_due) mitigates it.
 *
 *	Setup: one loop, K cooperative worker fibers that each do a small
 *	chunk of work then xtc_yield (well-behaved), plus ONE runaway
 *	fiber doing a long CPU burn.  We measure how many cooperative
 *	iterations complete while the runaway runs.
 *
 *	Two scenarios:
 *	  (1) runaway NEVER yields: it monopolizes the loop until done;
 *	      the cooperative workers make ZERO progress meanwhile --
 *	      the worst-case starvation libxtc's cooperative model
 *	      permits (the BEAM gap).
 *	  (2) runaway calls xtc_yield_if_due() against a per-loop budget:
 *	      it yields back periodically, so the cooperative workers
 *	      interleave -- the mitigation, and the recommended discipline
 *	      for any long compute inside a fiber.
 *
 *	The contrast is the headline number: cooperative progress during
 *	a runaway, budget OFF vs budget ON.  It documents the cost of the
 *	cooperative model AND that the library ships a usable remedy.
 *
 *	Usage: bench_fairness [burn_iters]   (default 50000000)
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_async.h"
#include "xtc_proc.h"

#define N_WORKERS 8

static atomic_long g_coop_iters;   /* cooperative work done while runaway runs */
static atomic_int  g_runaway_done;
static atomic_int  g_use_budget;   /* scenario toggle */
static long        g_burn;
static volatile long g_sink;        /* defeat dead-code elimination */

/* A well-behaved cooperative worker: do a tiny chunk, yield, repeat,
 * until the runaway is finished. */
static void
worker(void *arg)
{
	(void)arg;
	while (!atomic_load_explicit(&g_runaway_done, memory_order_relaxed)) {
		long acc = 0, i;
		for (i = 0; i < 1000; i++)
			acc += i;
		g_sink += acc;
		atomic_fetch_add_explicit(&g_coop_iters, 1, memory_order_relaxed);
		xtc_yield();   /* the cooperative contract */
	}
}

/* The runaway: a long CPU burn.  With budget OFF it never yields and
 * monopolizes the loop.  With budget ON it calls xtc_yield_if_due()
 * which yields back when the run quantum exceeds the loop budget. */
static void
runaway(void *arg)
{
	long acc = 0, i;
	int use_budget = atomic_load(&g_use_budget);
	(void)arg;
	for (i = 0; i < g_burn; i++) {
		acc += (i ^ (i << 1)) + (acc >> 3);
		if (use_budget && (i & 0x3ff) == 0)
			(void)xtc_yield_if_due();   /* cooperative preemption */
	}
	g_sink += acc;
	atomic_store_explicit(&g_runaway_done, 1, memory_order_relaxed);
}

static long
run_scenario(int use_budget)
{
	xtc_loop_t *loop;
	int i;

	atomic_store(&g_coop_iters, 0);
	atomic_store(&g_runaway_done, 0);
	atomic_store(&g_use_budget, use_budget);

	if (xtc_loop_init(&loop) != XTC_OK) {
		fprintf(stderr, "loop_init failed\n");
		return -1;
	}
	if (use_budget)
		xtc_yield_set_budget(loop, 1 * 1000 * 1000LL);  /* 1 ms quantum */

	for (i = 0; i < N_WORKERS; i++)
		(void)xtc_proc_spawn(loop, worker, NULL, NULL, NULL);
	(void)xtc_proc_spawn(loop, runaway, NULL, NULL, NULL);

	(void)xtc_loop_run(loop);
	(void)xtc_loop_fini(loop);
	return atomic_load(&g_coop_iters);
}

int
main(int argc, char **argv)
{
	long off, on;

	g_burn = argc > 1 ? atol(argv[1]) : 50L * 1000 * 1000;
	if (g_burn < 1)
		g_burn = 1;

	printf("# libxtc fairness under a runaway task: %d cooperative "
	    "workers + 1 CPU-burn runaway (%ld iters)\n", N_WORKERS, g_burn);
	printf("# Cooperative iterations completed WHILE the runaway runs.\n");
	printf("# libxtc is cooperatively scheduled (no forcible preemption,\n");
	printf("# unlike BEAM); the yield-budget watchdog is the remedy.\n\n");

	off = run_scenario(0);
	printf("budget OFF (runaway never yields):  coop_iters=%ld%s\n",
	    off, off == 0 ? "   <- total starvation (the cooperative gap)" : "");

	on = run_scenario(1);
	printf("budget ON  (xtc_yield_if_due, 1ms): coop_iters=%ld%s\n",
	    on, on > 0 ? "   <- workers interleave (the mitigation)" : "");

	printf("\nresult: the yield-budget watchdog turns %ld cooperative "
	    "iterations into %ld during a runaway burn.\n", off, on);
	if (on <= off) {
		printf("WARN: budget ON did not improve cooperative progress "
		    "(expected it to)\n");
	}
	return 0;
}
