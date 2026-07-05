/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_res.c
 *	Deterministic Simulation Testing of resource governance
 *	(src/ptc/res.c, xtc_res_t) -- the bounded-resource accountant that
 *	backs xtc's "a misbehaving client cannot exhaust the host" promise.
 *
 *	Many worker fibers across loops concurrently acquire and release
 *	units of a capped resource under the seeded scheduler.  The cap is
 *	set small so the acquire path genuinely contends and some requests
 *	are rejected (XTC_E_RESOURCE).  The DST invariants:
 *
 *	  (a) SAFETY: `used` NEVER exceeds the cap at any observation --
 *	      the whole point of the accountant (checked by every worker
 *	      right after a successful acquire, and via the high-water mark
 *	      which must be <= cap);
 *	  (b) CONSERVATION: after all workers finish, `used` is exactly 0
 *	      (every acquire had a matching release, none lost/double);
 *	  (c) ACCOUNTING: successes + rejects == total attempts (no attempt
 *	      silently vanished);
 *	  (d) clean quiescence and REPLAY (identical fingerprint per seed).
 *
 *	xtc_res_acquire/release are atomic + lock-free, so this exercises
 *	the CAS accounting under every seeded interleaving the scheduler
 *	can produce -- a lost-update or torn cap-check would show as a
 *	used>cap observation or a nonzero final used.
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
#include "xtc_res.h"
#include "xtc_sim.h"

#define N_LOOPS   3
#define N_WORKERS 6
#define ITERS     20
#define CAP       8            /* small cap so acquires contend + reject */

static xtc_res_t   g_res;
static atomic_int  g_success;   /* successful acquires (== releases) */
static atomic_int  g_reject;    /* XTC_E_RESOURCE returns */
static atomic_int  g_attempts;  /* total acquire attempts */
static atomic_int  g_over_cap;  /* set if used>cap ever observed (BUG) */
static atomic_int  g_done;

/* Worker: repeatedly acquire a seeded 1..3 units, (if granted) hold
 * briefly, verify used<=cap, then release. */
static void
worker(void *arg)
{
	(void)arg;
	int i;
	for (i = 0; i < ITERS; i++) {
		int64_t n = 1 + (int64_t)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 3);
		int rc;

		atomic_fetch_add(&g_attempts, 1);
		rc = xtc_res_acquire(&g_res, XTC_RES_TASKS, n);
		if (rc == XTC_OK) {
			atomic_fetch_add(&g_success, 1);
			/* SAFETY check while holding: used must never exceed
			 * the cap. */
			if (xtc_res_used(&g_res, XTC_RES_TASKS) > CAP)
				atomic_store(&g_over_cap, 1);
			/* Hold across a yield so other fibers interleave. */
			xtc_yield();
			xtc_res_release(&g_res, XTC_RES_TASKS, n);
		} else {
			atomic_fetch_add(&g_reject, 1);
		}
		if ((i & 3) == 0)
			(void)xtc_proc_sleep(500 * 1000LL);
	}
	if (atomic_fetch_add(&g_done, 1) + 1 >= N_WORKERS) {
		/* last worker out: nothing to do -- coordinator stops. */
	}
}

static void
coordinator(void *arg)
{
	xtc_exec_t *e = arg;
	int tries;
	for (tries = 0; tries < 4000; tries++) {
		if (atomic_load(&g_done) >= N_WORKERS)
			break;
		(void)xtc_proc_sleep(1000 * 1000LL);
	}
	(void)xtc_proc_sleep(2 * 1000 * 1000LL);
	(void)xtc_exec_stop(e);
}

static int
run_one(uint64_t seed, int *out_succ, int *out_rej, int *out_over,
    int64_t *out_final_used, int64_t *out_high, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	xtc_res_caps_t caps = XTC_RES_CAPS_DEFAULT;
	int i, rc;

	atomic_store(&g_success, 0);
	atomic_store(&g_reject, 0);
	atomic_store(&g_attempts, 0);
	atomic_store(&g_over_cap, 0);
	atomic_store(&g_done, 0);

	caps.tasks = CAP;    /* the resource we contend on */
	if (xtc_res_init(&g_res, &caps) != XTC_OK)
		return -1;

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK)
		return -1;
	xtc_exec_set_service_mode(e, 1);

	for (i = 0; i < N_WORKERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, i % N_LOOPS), worker,
		    NULL, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), coordinator, e, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 20000000);

	if (out_succ) *out_succ = atomic_load(&g_success);
	if (out_rej) *out_rej = atomic_load(&g_reject);
	if (out_over) *out_over = atomic_load(&g_over_cap);
	if (out_final_used) *out_final_used = xtc_res_used(&g_res, XTC_RES_TASKS);
	if (out_high) *out_high = xtc_res_high(&g_res, XTC_RES_TASKS);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0x726573; /* "res" */
	int n = 20, i, fails = 0;

	if (argc > 1) base = strtoull(argv[1], NULL, 0);
	if (argc > 2) n = atoi(argv[2]);

	printf("== resource-governance DST: %d seeds from base 0x%llx ==\n",
	    n, (unsigned long long)base);

	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		int succ = 0, rej = 0, over = 0, succ2 = 0, rej2 = 0, over2 = 0;
		int64_t used = -1, high = -1, used2 = -1, high2 = -1;
		int rc, rc2, pass = 1, attempts;
		uint64_t st = 0, st2 = 0;

		rc = run_one(seed, &succ, &rej, &over, &used, &high, &st);
		attempts = atomic_load(&g_attempts);
		if (rc != XTC_OK) pass = 0;
		else if (over != 0) pass = 0;              /* used never > cap */
		else if (used != 0) pass = 0;              /* conservation */
		else if (high > CAP) pass = 0;             /* high-water <= cap */
		else if (succ + rej != attempts) pass = 0; /* accounting */
		else if (succ + rej != N_WORKERS * ITERS) pass = 0;

		if (pass) {
			rc2 = run_one(seed, &succ2, &rej2, &over2, &used2,
			    &high2, &st2);
			if (rc2 != rc || succ2 != succ || rej2 != rej ||
			    over2 != over || used2 != used || high2 != high ||
			    st2 != st)
				pass = 0;
		}

		if (!pass) {
			printf("  seed 0x%016llx: FAIL (succ=%d rej=%d over=%d "
			    "final_used=%lld high=%lld rc=%d)\n",
			    (unsigned long long)seed, succ, rej, over,
			    (long long)used, (long long)high, rc);
			fails++;
		}
	}

	if (fails == 0) {
		printf("OK: resource-governance DST -- %d seeds, used never "
		    "exceeds cap + exact conservation + accounting, all "
		    "replay\n", n);
		return 0;
	}
	printf("FAIL: %d/%d resource-governance seeds failed\n", fails, n);
	return 1;
}
