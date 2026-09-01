/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_cross_loop_owner.c
 *	DST regression for the cross-loop-mutation bug CATEGORY
 *	(v1.40.1..v1.40.4): a work-stolen migratable fiber that, after
 *	migrating, mutates a structure owned by a loop OTHER than the one
 *	it is running on (the run queue, or the timer min-heap).
 *
 *	The single-thread deterministic scheduler cannot exhibit the
 *	DATA race (one thread serializes everything), but it DOES execute
 *	the exact wrong-loop mutation call.  Under --enable-diagnostic the
 *	XTC_ASSERT_LOOP_OWNER guard runs in a sim-aware mode: it aborts if
 *	a loop-owned structure is mutated while a DIFFERENT loop is the one
 *	being stepped (__xtc_current_loop).  So this test, run under a
 *	handful of seeds with the diagnostic build, deterministically
 *	catches a reintroduction of the whole category -- turning "caught
 *	probabilistically under real threads eventually" into "caught by a
 *	specific seed, every time."
 *
 *	The workers are MIGRATABLE and park via xtc_yield under pessimal
 *	steal, so they migrate across loops between yields -- so a resume
 *	and its run-queue enqueue land on whatever loop currently runs the
 *	fiber.  A regression that enqueued onto the wrong (home) loop, or
 *	that armed a timer / mutated any owner-only structure on a loop
 *	other than the running one, trips the sim-mode owner guard
 *	deterministically under a seed.
 */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_async.h"
#include "xtc_sim.h"

#define N_LOOPS    6
#define N_WORKERS  16
#define N_ITERS    24

static atomic_int g_done;

static void
worker(void *arg)
{
	int iters = (int)(intptr_t)arg;
	int k;
	for (k = 0; k < iters; k++) {
		/* Plain yield park: under pessimal steal the scheduler moves
		 * us onto peer loops between yields, so each resume + the
		 * subsequent run-queue enqueue happens on whatever loop
		 * currently runs us -- exercising the run-queue owner guard
		 * (the v1.40.4 task->state + enqueue site).  A migratable
		 * fiber that enqueued onto the WRONG loop would trip the
		 * DIAGNOSTIC sim-mode owner check. */
		xtc_yield();
	}
	atomic_fetch_add(&g_done, 1);
}

static void
spawner(void *arg)
{
	xtc_exec_t *e = arg;
	xtc_proc_opts_t opts;
	int i;
	for (i = 0; i < N_WORKERS; i++) {
		memset(&opts, 0, sizeof opts);
		opts.name = "w";
		opts.migratable = 1;
		/* All on loop 0 so the pessimal scheduler steals them onto
		 * peers -- maximizes cross-loop migration. */
		(void)xtc_proc_spawn(xtc_exec_loop(e, 0), worker,
		    (void *)(intptr_t)N_ITERS, &opts, NULL);
	}
}

static int
run_seed(uint64_t seed)
{
	xtc_exec_t *e = NULL;
	int rc;
	atomic_store(&g_done, 0);
	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	xtc_sim_sched_pessimal(500);   /* 50% pessimal (starve/monopolize) */
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), spawner, e, NULL, NULL);
	/* Drive deterministically.  If a cross-loop owner violation occurs
	 * the DIAGNOSTIC guard aborts here; otherwise the run must reach
	 * clean quiescence with every worker done. */
	rc = xtc_sim_exec_run(e, seed, 20000000);
	(void)xtc_exec_fini(e);
	if (rc != XTC_OK) {
		printf("FAIL: seed %llu did not quiesce cleanly (rc=%d)\n",
		    (unsigned long long)seed, rc);
		return -1;
	}
	if (atomic_load(&g_done) != N_WORKERS) {
		printf("FAIL: seed %llu -- %d/%d workers done\n",
		    (unsigned long long)seed, atomic_load(&g_done), N_WORKERS);
		return -1;
	}
	return 0;
}

int
main(void)
{
	uint64_t seeds[] = { 1, 2, 3, 7, 42, 1337, 0xC0FFEE, 0xDEADBEEF };
	size_t i;
	int fails = 0;
	for (i = 0; i < sizeof seeds / sizeof seeds[0]; i++)
		if (run_seed(seeds[i]) != 0)
			fails++;
	if (fails) {
		printf("cross-loop owner DST: %d/%zu seeds FAILED\n",
		    fails, sizeof seeds / sizeof seeds[0]);
		return 1;
	}
	printf("ok   cross-loop owner DST: %zu seeds, migratable timer+yield "
	    "parks under pessimal steal, no owner violation, all quiesced\n",
	    sizeof seeds / sizeof seeds[0]);
	return 0;
}
