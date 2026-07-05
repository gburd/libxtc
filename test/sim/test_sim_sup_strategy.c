/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_sup_strategy.c
 *	Deterministic Simulation Testing of the L4 supervisor's full
 *	RESTART-STRATEGY MATRIX (src/orc/sup.c), driven by the seeded
 *	single-thread simulator.  test_sim_machine_death already covers
 *	one_for_one restart of a single child; this test covers the parts
 *	that were NOT under DST:
 *
 *	  - ONE_FOR_ALL: crashing one child restarts EVERY child (all
 *	    siblings are taken down and respawned together);
 *	  - REST_FOR_ONE: crashing child i restarts i and every child
 *	    started AFTER it, but NOT the ones before;
 *	  - restart POLICIES: PERMANENT restarts on any exit, TRANSIENT
 *	    restarts only on abnormal (reason != 0) exit, TEMPORARY never
 *	    restarts;
 *	  - restart INTENSITY: more than max_restarts crashes within
 *	    period_ns makes the supervisor give up and exit (it does not
 *	    restart forever).
 *
 *	Each scenario is a pure function of the seed (the crash timing is
 *	drawn from the sim RNG), so a repeated seed reproduces the exact
 *	same spawn counts and supervisor-alive outcome -- the FoundationDB
 *	replay property -- verified by a result fingerprint.
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
#include "xtc_orc.h"
#include "xtc_sim.h"

#define N_LOOPS 2
#define N_KIDS  4

/* Per-kid spawn counts (index = child slot).  A child bumps its slot on
 * every (re)spawn, so we can tell which children were restarted. */
static atomic_int g_spawns[N_KIDS];
static xtc_pid_t  g_pids[N_KIDS];
static xtc_supervisor_t *g_sup;

/* A supervised child: record pid, bump my spawn counter, park forever
 * (the supervisor kills me on stop / strategy takedown). */
static void
kid(void *arg)
{
	int idx = (int)(intptr_t)arg;
	g_pids[idx] = xtc_self();
	atomic_fetch_add(&g_spawns[idx], 1);
	for (;;) {
		void *m = NULL;
		size_t sz = 0;
		(void)xtc_recv(&m, &sz, 50 * 1000 * 1000LL);
		if (m != NULL)
			free(m);
	}
}

/* Kill child `victim` after a seeded delay, then settle and stop the
 * supervisor so the run quiesces.  reason != 0 == abnormal. */
struct reaper_arg { int victim; int reason; int64_t settle_ns; };

static void
reaper(void *arg)
{
	struct reaper_arg *ra = arg;
	int64_t delay = (int64_t)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 4) *
	    1000 * 1000LL;                       /* 0..3 ms, seeded */
	xtc_pid_t v;
	int tries;

	(void)xtc_proc_sleep(delay);
	/* Wait until the victim's initial spawn has recorded its pid, so a
	 * zero-delay draw does not race the child's first run.  Bounded, so
	 * a genuinely-never-spawned child still lets the run quiesce. */
	for (tries = 0; tries < 50; tries++) {
		v = g_pids[ra->victim];
		if (!xtc_pid_is_none(v))
			break;
		(void)xtc_proc_sleep(1000 * 1000LL);
	}
	v = g_pids[ra->victim];
	if (!xtc_pid_is_none(v))
		(void)xtc_exit_pid(v, ra->reason);

	(void)xtc_proc_sleep(ra->settle_ns);
	if (g_sup != NULL)
		(void)xtc_sup_stop(g_sup);
}

/* Kill the SAME child repeatedly to blow the restart-intensity budget. */
struct hammer_arg { int victim; int times; };

static void
hammer(void *arg)
{
	struct hammer_arg *ha = arg;
	int i;
	for (i = 0; i < ha->times; i++) {
		xtc_pid_t v;
		int tries;
		(void)xtc_proc_sleep(
		    (int64_t)(1 + __xtc_sim_rng_range(XTC_SIM_RNG_APP, 2)) *
		    1000 * 1000LL);              /* 1..2 ms between kills */
		for (tries = 0; tries < 50; tries++) {
			v = g_pids[ha->victim];
			if (!xtc_pid_is_none(v))
				break;
			(void)xtc_proc_sleep(1000 * 1000LL);
		}
		v = g_pids[ha->victim];
		if (!xtc_pid_is_none(v))
			(void)xtc_exit_pid(v, 99);
	}
	/* Give the supervisor time to exceed intensity and exit on its own;
	 * if it survived (should not for the intensity case) stop it. */
	(void)xtc_proc_sleep(30 * 1000 * 1000LL);
	if (g_sup != NULL && xtc_sup_alive(g_sup))
		(void)xtc_sup_stop(g_sup);
}

/* Run one scenario.  strategy/policy/victim/reason configure the crash;
 * hammer_times>0 selects the intensity scenario.  Returns the sup-alive
 * flag in *out_alive and the total restart count in *out_restarts. */
static int
run_scenario(uint64_t seed, xtc_restart_strategy_t strat,
    xtc_restart_policy_t policy, int victim, int reason,
    int max_restarts, int hammer_times,
    int *out_alive, int *out_restarts, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	xtc_sup_opts_t opts = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t kids[N_KIDS];
	struct reaper_arg ra;
	struct hammer_arg ha;
	int i, rc;

	for (i = 0; i < N_KIDS; i++) {
		atomic_store(&g_spawns[i], 0);
		g_pids[i] = (xtc_pid_t){0};
	}
	g_sup = NULL;

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK)
		return -1;

	for (i = 0; i < N_KIDS; i++) {
		memset(&kids[i], 0, sizeof(kids[i]));
		kids[i].name = "kid";
		kids[i].fn = kid;
		kids[i].arg = (void *)(intptr_t)i;
		kids[i].policy = policy;
		kids[i].loop = i % N_LOOPS;
	}

	opts.strategy = strat;
	opts.max_restarts = max_restarts;
	opts.period_ns = 1000LL * 1000 * 1000;   /* 1 s window */
	opts.exec = e;

	if (xtc_sup_start(xtc_exec_loop(e, 0), &opts, kids, N_KIDS,
	    &g_sup) != XTC_OK) {
		(void)xtc_exec_fini(e);
		return -1;
	}

	if (hammer_times > 0) {
		ha.victim = victim;
		ha.times = hammer_times;
		(void)xtc_proc_spawn(xtc_exec_loop(e, 1), hammer, &ha,
		    NULL, NULL);
	} else {
		ra.victim = victim;
		ra.reason = reason;
		ra.settle_ns = 20 * 1000 * 1000LL;   /* 20 ms settle */
		(void)xtc_proc_spawn(xtc_exec_loop(e, 1), reaper, &ra,
		    NULL, NULL);
	}

	rc = xtc_sim_exec_run(e, seed, 5000000);

	if (out_alive != NULL)
		*out_alive = (g_sup != NULL) ? xtc_sup_alive(g_sup) : 0;
	if (out_restarts != NULL)
		*out_restarts = (g_sup != NULL) ? xtc_sup_n_restarts(g_sup) : 0;
	if (out_state != NULL)
		*out_state = xtc_sim_state_hash(e);

	/* Reclaim the supervisor struct.  The exec has stopped, so the
	 * supervisor fiber is no longer running; a poll-once join (called
	 * from outside the supervisor loop) frees it -- otherwise the
	 * struct + its children/recent_restarts arrays leak (ASan). */
	if (g_sup != NULL) {
		(void)xtc_sup_join(g_sup, 0);
		g_sup = NULL;
	}

	(void)xtc_exec_fini(e);
	return rc;
}

/* Fingerprint the observable outcome for replay checking. */
static uint64_t
fingerprint(int alive, int restarts, uint64_t state, int rc)
{
	uint64_t fp = 1469598103934665603ull;
	int i;
#define MIX(v) do { fp ^= (uint64_t)(v); fp *= 1099511628211ull; } while (0)
	for (i = 0; i < N_KIDS; i++)
		MIX(atomic_load(&g_spawns[i]));
	MIX(alive); MIX(restarts); MIX(state); MIX(rc);
#undef MIX
	return fp;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0x5570; /* "Sup" */
	int n = 16, i, fails = 0;

	if (argc > 1) base = strtoull(argv[1], NULL, 0);
	if (argc > 2) n = atoi(argv[2]);

	printf("== supervisor-strategy DST: %d seeds from base 0x%llx ==\n",
	    n, (unsigned long long)base);

	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		int alive = 0, restarts = 0, pass = 1, rc;
		uint64_t st = 0, fp, fp2;
		int s0, s1, s2, s3;

		/* --- ONE_FOR_ALL: crash child 1 -> ALL restart. --- */
		rc = run_scenario(seed, XTC_SUP_ONE_FOR_ALL,
		    XTC_RESTART_PERMANENT, 1, 99, 10, 0,
		    &alive, &restarts, &st);
		fp = fingerprint(alive, restarts, st, rc);
		s0 = atomic_load(&g_spawns[0]);
		s1 = atomic_load(&g_spawns[1]);
		s2 = atomic_load(&g_spawns[2]);
		s3 = atomic_load(&g_spawns[3]);
		if (rc != XTC_OK) { pass = 0; }
		/* every child spawned at least twice (initial + restart). */
		else if (!(s0 >= 2 && s1 >= 2 && s2 >= 2 && s3 >= 2))
			pass = 0;

		/* --- REST_FOR_ONE: crash child 2 -> 2 and 3 restart, 0/1 do
		 * NOT (they started before the victim). --- */
		rc = run_scenario(seed, XTC_SUP_REST_FOR_ONE,
		    XTC_RESTART_PERMANENT, 2, 99, 10, 0,
		    &alive, &restarts, &st);
		s0 = atomic_load(&g_spawns[0]);
		s1 = atomic_load(&g_spawns[1]);
		s2 = atomic_load(&g_spawns[2]);
		s3 = atomic_load(&g_spawns[3]);
		if (rc != XTC_OK) pass = 0;
		else if (!(s0 == 1 && s1 == 1 && s2 >= 2 && s3 >= 2))
			pass = 0;

		/* --- TEMPORARY policy: crashed child is NEVER restarted. --- */
		rc = run_scenario(seed, XTC_SUP_ONE_FOR_ONE,
		    XTC_RESTART_TEMPORARY, 0, 99, 10, 0,
		    &alive, &restarts, &st);
		s0 = atomic_load(&g_spawns[0]);
		if (rc != XTC_OK) pass = 0;
		else if (s0 != 1) pass = 0;     /* spawned once, never again */

		/* --- TRANSIENT policy + NORMAL exit (reason 0): NO restart. */
		rc = run_scenario(seed, XTC_SUP_ONE_FOR_ONE,
		    XTC_RESTART_TRANSIENT, 0, 0, 10, 0,
		    &alive, &restarts, &st);
		s0 = atomic_load(&g_spawns[0]);
		if (rc != XTC_OK) pass = 0;
		else if (s0 != 1) pass = 0;     /* normal exit -> no restart */

		/* --- TRANSIENT policy + ABNORMAL exit (reason != 0): restart. */
		rc = run_scenario(seed, XTC_SUP_ONE_FOR_ONE,
		    XTC_RESTART_TRANSIENT, 0, 99, 10, 0,
		    &alive, &restarts, &st);
		s0 = atomic_load(&g_spawns[0]);
		if (rc != XTC_OK) pass = 0;
		else if (s0 < 2) pass = 0;      /* abnormal exit -> restart */

		/* --- INTENSITY: hammer child 0 more than max_restarts=2 in
		 * the window -> supervisor gives up and is NOT alive. --- */
		rc = run_scenario(seed, XTC_SUP_ONE_FOR_ONE,
		    XTC_RESTART_PERMANENT, 0, 99, 2, 5,
		    &alive, &restarts, &st);
		if (rc != XTC_OK) pass = 0;
		else if (alive != 0) pass = 0;  /* must have exited */

		/* --- REPLAY: the ONE_FOR_ALL scenario reproduces. --- */
		rc = run_scenario(seed, XTC_SUP_ONE_FOR_ALL,
		    XTC_RESTART_PERMANENT, 1, 99, 10, 0,
		    &alive, &restarts, &st);
		fp2 = fingerprint(alive, restarts, st, rc);
		if (fp2 != fp) pass = 0;

		if (!pass) {
			printf("  seed 0x%016llx: FAIL "
			    "(last spawns %d/%d/%d/%d alive=%d restarts=%d "
			    "rc=%d)\n",
			    (unsigned long long)seed,
			    atomic_load(&g_spawns[0]), atomic_load(&g_spawns[1]),
			    atomic_load(&g_spawns[2]), atomic_load(&g_spawns[3]),
			    alive, restarts, rc);
			fails++;
		}
	}

	if (fails == 0) {
		printf("OK: supervisor-strategy DST -- %d seeds, all strategy/"
		    "policy/intensity invariants hold and replay\n", n);
		return 0;
	}
	printf("FAIL: %d/%d supervisor-strategy seeds failed\n", fails, n);
	return 1;
}
