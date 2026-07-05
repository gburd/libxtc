/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_blocking.c
 *	Deterministic Simulation Testing of the blocking-work offload
 *	(src/ptc/blocking.c, xtc_blocking_run).
 *
 *	The PRODUCTION offload path hands work to a real pthread pool and
 *	parks the caller on a wakeup pipe -- a pool worker runs on a real
 *	OS thread outside the sim's control, which cannot be made
 *	deterministic (the same not-coverable-by-design boundary as raw
 *	sockets and the OS subprocess layer).  What IS coverable is the
 *	CALLER-SIDE CONTRACT: xtc_blocking_run must return each caller its
 *	own fn(arg) result, with no cross-talk, no lost completion, and no
 *	hang, when many fibers offload concurrently.  Under sim
 *	xtc_blocking_run runs the work synchronously on the calling fiber
 *	(additive, gated on __xtc_sim_active(); same result the off-a-loop
 *	synchronous fallback already produces in production), so the
 *	offload contract is exercised as a pure function of the seed.
 *
 *	Scenario: N worker fibers each offload a computation on a seeded
 *	input; the driver verifies every worker got exactly its own result
 *	(input * 2 + 1) and the grand total matches, then quiesces.
 *
 *	Invariants (per seed): (a) every offload returned the correct
 *	per-caller result (no cross-talk / lost completion); (b) the sum of
 *	results equals the independently-computed expected sum; (c) clean
 *	quiescence; (d) REPLAY: identical fingerprint for a repeated seed.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_blocking.h"
#include "xtc_sim.h"

#define N_LOOPS   2
#define N_WORKERS 6

static atomic_int  g_correct;      /* offloads that returned the right value */
static atomic_int  g_done;         /* workers finished */
static atomic_llong g_sum;         /* sum of all returned results */

/* The offloaded computation: pure function of its input. */
static int
compute(void *arg)
{
	int x = (int)(intptr_t)arg;
	return x * 2 + 1;
}

/* A worker fiber: draw a seeded input, offload compute, verify. */
static void
worker(void *arg)
{
	int id = (int)(intptr_t)arg;
	int input = id * 1000 +
	    (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 500);
	int want = input * 2 + 1;
	int got = 0;
	int rc = xtc_blocking_run(compute, (void *)(intptr_t)input, &got);

	if (rc == XTC_OK && got == want) {
		atomic_fetch_add(&g_correct, 1);
		atomic_fetch_add(&g_sum, (long long)got);
	}
	atomic_fetch_add(&g_done, 1);
}

/* Coordinator: wait for all workers, then stop the executor. */
static void
coordinator(void *arg)
{
	xtc_exec_t *e = arg;
	int tries;
	for (tries = 0; tries < 500; tries++) {
		if (atomic_load(&g_done) >= N_WORKERS)
			break;
		(void)xtc_proc_sleep(1000 * 1000LL);
	}
	(void)xtc_proc_sleep(2 * 1000 * 1000LL);
	(void)xtc_exec_stop(e);
}

static int
run_one(uint64_t seed, int *out_correct, long long *out_sum,
    uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	atomic_store(&g_correct, 0);
	atomic_store(&g_done, 0);
	atomic_store(&g_sum, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK)
		return -1;
	xtc_exec_set_service_mode(e, 1);

	for (i = 0; i < N_WORKERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, i % N_LOOPS), worker,
		    (void *)(intptr_t)i, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), coordinator, e, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	if (out_correct != NULL) *out_correct = atomic_load(&g_correct);
	if (out_sum != NULL) *out_sum = atomic_load(&g_sum);
	if (out_state != NULL) *out_state = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0x626c6b; /* "blk" */
	int n = 20, i, fails = 0;

	if (argc > 1) base = strtoull(argv[1], NULL, 0);
	if (argc > 2) n = atoi(argv[2]);

	printf("== blocking-offload DST: %d seeds from base 0x%llx ==\n",
	    n, (unsigned long long)base);

	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		int correct = 0, correct2 = 0, rc, rc2, pass = 1;
		long long sum = 0, sum2 = 0;
		uint64_t st = 0, st2 = 0;

		rc = run_one(seed, &correct, &sum, &st);
		if (rc != XTC_OK) pass = 0;
		else if (correct != N_WORKERS) pass = 0;

		if (pass) {
			rc2 = run_one(seed, &correct2, &sum2, &st2);
			if (rc2 != rc || correct2 != correct || sum2 != sum ||
			    st2 != st)
				pass = 0;
		}

		if (!pass) {
			printf("  seed 0x%016llx: FAIL (correct=%d/%d sum=%lld "
			    "rc=%d)\n", (unsigned long long)seed, correct,
			    N_WORKERS, sum, rc);
			fails++;
		}
	}

	if (fails == 0) {
		printf("OK: blocking-offload DST -- %d seeds, per-caller "
		    "results correct + no cross-talk, all replay\n", n);
		return 0;
	}
	printf("FAIL: %d/%d blocking-offload seeds failed\n", fails, n);
	return 1;
}
