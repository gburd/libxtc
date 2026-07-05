/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_launch.c
 *	Deterministic Simulation Testing of xtc_launch (M_PREEMPTION Phase
 *	3, the bounded-time / cancellable launch) under the seeded
 *	single-thread simulator.
 *
 *	xtc_launch is pure concurrency orchestration -- spawn a child
 *	fiber, monitor it, park on a timed recv for its DOWN, deliver the
 *	result on a clean finish or cancel it on the deadline -- so its
 *	outcome must be a pure function of the seed.  This test runs N
 *	launcher fibers across loops, each launching a fn whose behavior is
 *	seeded (either it finishes quickly and returns a value, or it
 *	sleeps past its deadline and must be cancelled), and checks:
 *
 *	  (a) a fn that finishes within the deadline returns XTC_OK with
 *	      the correct value;
 *	  (b) a fn that would run past the deadline is cancelled and the
 *	      launch returns XTC_E_AGAIN, and the fn's at-exit cleanup ran
 *	      (no resource leak) -- deterministically for the seed;
 *	  (c) clean quiescence;
 *	  (d) REPLAY: the same seed reproduces the exact per-launcher
 *	      outcomes (finish-vs-timeout) and the result fingerprint.
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
#include "xtc_launch.h"
#include "xtc_sim.h"

#define N_LOOPS    3
#define N_LAUNCH   6
#define DEADLINE_NS (20 * 1000 * 1000LL)   /* 20 ms */

static atomic_int g_finished;    /* launches that returned XTC_OK */
static atomic_int g_timed_out;   /* launches that returned XTC_E_AGAIN */
static atomic_int g_cleanups;    /* at-exit cleanups the cancel path ran */
static atomic_int g_bad;         /* an unexpected outcome (BUG) */
static atomic_int g_done;        /* launchers finished */

/* A launched fn.  arg low bit selects the behavior: even -> finish fast
 * and return a value; odd -> sleep well past the deadline (must be
 * cancelled).  The slow path registers an at-exit so the cancel path's
 * cleanup is observable. */
static void
slow_cleanup(void *arg) { (void)arg; atomic_fetch_add(&g_cleanups, 1); }

static intptr_t
launched(void *arg)
{
	int id = (int)(intptr_t)arg;
	if (id & 1) {
		/* Slow: register cleanup, sleep far past the deadline. */
		(void)xtc_proc_at_exit(slow_cleanup, NULL);
		(void)xtc_proc_sleep(500 * 1000 * 1000LL);   /* 500 ms */
		return -999;   /* not reached (cancelled) */
	}
	/* Fast: a couple of yields, then return a deterministic value. */
	xtc_yield();
	xtc_yield();
	return (intptr_t)(id * 10 + 7);
}

struct launcher_arg { int id; };

static void
launcher(void *arg)
{
	struct launcher_arg *la = arg;
	intptr_t v = 0;
	int rc = xtc_launch(NULL, launched,
	    (void *)(intptr_t)la->id, DEADLINE_NS, NULL, &v);

	if (la->id & 1) {
		/* Slow -> must have timed out (cancelled). */
		if (rc == XTC_E_AGAIN)
			atomic_fetch_add(&g_timed_out, 1);
		else
			atomic_store(&g_bad, 1);
	} else {
		/* Fast -> must have finished with the right value. */
		if (rc == XTC_OK && v == (intptr_t)(la->id * 10 + 7))
			atomic_fetch_add(&g_finished, 1);
		else
			atomic_store(&g_bad, 1);
	}
	atomic_fetch_add(&g_done, 1);
}

static void
coordinator(void *arg)
{
	xtc_exec_t *e = arg;
	int tries;
	for (tries = 0; tries < 8000; tries++) {
		if (atomic_load(&g_done) >= N_LAUNCH)
			break;
		(void)xtc_proc_sleep(1000 * 1000LL);
	}
	(void)xtc_proc_sleep(2 * 1000 * 1000LL);
	(void)xtc_exec_stop(e);
}

static int
run_one(uint64_t seed, int *out_fin, int *out_to, int *out_clean,
    int *out_bad, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	struct launcher_arg la[N_LAUNCH];
	int i, rc;

	atomic_store(&g_finished, 0);
	atomic_store(&g_timed_out, 0);
	atomic_store(&g_cleanups, 0);
	atomic_store(&g_bad, 0);
	atomic_store(&g_done, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK)
		return -1;
	xtc_exec_set_service_mode(e, 1);

	for (i = 0; i < N_LAUNCH; i++) {
		la[i].id = i;
		(void)xtc_proc_spawn(xtc_exec_loop(e, i % N_LOOPS), launcher,
		    &la[i], NULL, NULL);
	}
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), coordinator, e, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 20000000);

	if (out_fin)   *out_fin = atomic_load(&g_finished);
	if (out_to)    *out_to = atomic_load(&g_timed_out);
	if (out_clean) *out_clean = atomic_load(&g_cleanups);
	if (out_bad)   *out_bad = atomic_load(&g_bad);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0x6c6e6368; /* "lnch" */
	int n = 16, i, fails = 0;
	int expect_fin = 0, expect_to = 0;

	if (argc > 1) base = strtoull(argv[1], NULL, 0);
	if (argc > 2) n = atoi(argv[2]);

	/* id even -> fast (finish); id odd -> slow (timeout). */
	for (i = 0; i < N_LAUNCH; i++)
		((i & 1) ? &expect_to : &expect_fin)[0]++;

	printf("== xtc_launch DST: %d seeds from base 0x%llx ==\n", n,
	    (unsigned long long)base);

	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		int fin = 0, to = 0, cl = 0, bad = 0, rc, pass = 1;
		int fin2 = 0, to2 = 0, cl2 = 0, bad2 = 0, rc2;
		uint64_t st = 0, st2 = 0;

		rc = run_one(seed, &fin, &to, &cl, &bad, &st);
		if (rc != XTC_OK) pass = 0;
		else if (bad) pass = 0;
		else if (fin != expect_fin) pass = 0;   /* all fast finished */
		else if (to != expect_to) pass = 0;      /* all slow cancelled */
		else if (cl != expect_to) pass = 0;      /* each cancel cleaned up */

		if (pass) {
			rc2 = run_one(seed, &fin2, &to2, &cl2, &bad2, &st2);
			if (rc2 != rc || fin2 != fin || to2 != to || cl2 != cl ||
			    st2 != st)
				pass = 0;
		}

		if (!pass) {
			printf("  seed 0x%016llx: FAIL (fin=%d/%d to=%d/%d "
			    "clean=%d bad=%d rc=%d)\n",
			    (unsigned long long)seed, fin, expect_fin, to,
			    expect_to, cl, bad, rc);
			fails++;
		}
	}

	if (fails == 0) {
		printf("OK: xtc_launch DST -- %d seeds, finish/timeout/cancel "
		    "outcomes + at-exit cleanup deterministic, all replay\n", n);
		return 0;
	}
	printf("FAIL: %d/%d xtc_launch seeds failed\n", fails, n);
	return 1;
}
