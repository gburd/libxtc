/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_forged_down.c
 *	Deterministic Simulation Testing of the library's DOWN receivers
 *	against user messages shaped like a DOWN.  A DOWN's byte layout is
 *	public ({'D', ref, pid, reason, exit_kind}, packed), so an ordinary
 *	xtc_send of those bytes must never be taken for a death: only the
 *	DOWN the library sends counts.
 *
 *	Per seed, on three loops with seeded cross-loop latency: a
 *	supervisor (one_for_one) over N_KIDS permanent children, a registry
 *	reaper, and N_NAMED procs that name themselves with XTC_REG_MONITOR.
 *	Rogue procs on every loop send seeded bursts of forged DOWNs, naming
 *	live children (to the supervisor) and live named procs (to the
 *	reaper), at seeded times, mixed with ordinary 'D'-led junk.  Midway
 *	one child crashes for real at a seeded time, so a real DOWN is in
 *	flight among the forged ones.
 *
 *	Invariants (per seed):
 *	  (a) the supervisor restarted exactly the one child that crashed,
 *	      once: no forged DOWN restarted a live child (planted as
 *	      SUP_FORGED_DOWN: "a forged DOWN restarted a live child");
 *	  (b) every named proc's name is still registered to it at the end
 *	      (planted as REG_FORGED_DOWN: "a forged DOWN dropped a live
 *	      name");
 *	  (c) the run quiesces;
 *	  (d) REPLAY: the same seed gives the same counts and state hash.
 *
 *	argv: [base [count]] -- seeds base+1 .. base+count (default 0 16).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_inspect.h"
#include "xtc_orc.h"
#include "xtc_proc.h"
#include "xtc_reg.h"
#include "xtc_int.h" /* XTC_PACK_PUSH: the packed DOWN shape */
#include "xtc_sim.h"

#define N_LOOPS 3
#define N_KIDS 4
#define N_NAMED 3
#define N_ROGUES 3
#define MS (1000LL * 1000)

XTC_PACK_PUSH
struct fake_down {
	uint8_t kind;
	uint64_t ref;
	xtc_pid_t pid;
	int reason;
	uint8_t exit_kind;
} XTC_PACKED;
XTC_PACK_POP

static xtc_exec_t *g_e;
static xtc_supervisor_t *g_sup;
static xtc_reg_t *g_reg;
static xtc_atomic_pid_t g_kid[N_KIDS];
static xtc_atomic_pid_t g_named[N_NAMED];
static xtc_pid_t g_reaper;
static int g_starts[N_KIDS];
static int g_crash_kid; /* the child that crashes for real */
static int64_t g_crash_at;
static int g_stop, g_rogues_done, g_forged, g_lost_names, g_done;

static void
kid(void *arg)
{
	int i = (int)(intptr_t)arg;
	int first;

	first = g_starts[i]++ == 0;
	xtc_atomic_pid_store(&g_kid[i], xtc_self());
	if (first && i == g_crash_kid) {
		(void)xtc_proc_sleep(g_crash_at);
		(void)xtc_exit_self(7);
	}
	while (!g_stop)
		(void)xtc_proc_sleep(2 * MS);
}

static void
named(void *arg)
{
	int i = (int)(intptr_t)arg;
	char nm[16];

	(void)snprintf(nm, sizeof nm, "n.%d", i);
	xtc_atomic_pid_store(&g_named[i], xtc_self());
	(void)xtc_reg_register_mon(g_reg, nm, xtc_self());
	while (!g_stop)
		(void)xtc_proc_sleep(2 * MS);
}

/* The supervisor's pid: the proc that is none of ours. */
struct find {
	xtc_pid_t sup;
};

static xtc_atomic_pid_t g_others[N_ROGUES + 1]; /* rogues + coordinator */

static int
is_ours(xtc_pid_t p)
{
	int i;
	for (i = 0; i < N_KIDS; i++)
		if (xtc_pid_eq(p, xtc_atomic_pid_load(&g_kid[i])))
			return 1;
	for (i = 0; i < N_NAMED; i++)
		if (xtc_pid_eq(p, xtc_atomic_pid_load(&g_named[i])))
			return 1;
	for (i = 0; i <= N_ROGUES; i++)
		if (xtc_pid_eq(p, xtc_atomic_pid_load(&g_others[i])))
			return 1;
	return xtc_pid_eq(p, g_reaper);
}

static int
find_sup(const xtc_proc_info_t *i, void *u)
{
	struct find *f = u;
	if (i->alive && !is_ours(i->pid))
		f->sup = i->pid;
	return 0;
}

static void
forge(xtc_pid_t to, xtc_pid_t subject, int junk)
{
	struct fake_down f;

	memset(&f, 0, sizeof f);
	f.kind = 'D';
	f.ref = junk ? __xtc_sim_rng_range(XTC_SIM_RNG_APP, 1u << 20) : 0;
	f.pid = subject;
	f.reason = junk ? 0 : 1;
	f.exit_kind = (uint8_t)(junk ? 0 : 1);
	if (xtc_send(to, &f, sizeof f) == XTC_OK)
		g_forged++;
}

static void
rogue(void *arg)
{
	struct find f;
	int r, k, n;

	xtc_atomic_pid_store(&g_others[(int)(intptr_t)arg], xtc_self());

	for (r = 0; r < 6; r++) {
		(void)xtc_proc_sleep(
		    (int64_t)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 6) * MS);
		f.sup = XTC_PID_NONE;
		(void)xtc_inspect_procs(find_sup, &f);
		n = 1 + (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 3);
		for (k = 0; k < n; k++) {
			int ki =
			    (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, N_KIDS);
			int ni =
			    (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, N_NAMED);
			int junk = __xtc_sim_rng_range(XTC_SIM_RNG_APP, 4) == 0;
			if (!xtc_pid_is_none(f.sup))
				forge(f.sup, xtc_atomic_pid_load(&g_kid[ki]),
				    junk);
			forge(g_reaper, xtc_atomic_pid_load(&g_named[ni]),
			    junk);
		}
	}
	g_rogues_done++;
}

/* Wait for the rogues, then check the names and stop everything. */
static void
coordinator(void *arg)
{
	xtc_pid_t p;
	char nm[16];
	int i, tries;
	(void)arg;

	xtc_atomic_pid_store(&g_others[N_ROGUES], xtc_self());
	for (tries = 0; tries < 400; tries++) {
		if (g_rogues_done ==
		    N_ROGUES)
			break;
		(void)xtc_proc_sleep(MS);
	}
	(void)xtc_proc_sleep(20 * MS); /* in-flight forgeries land */
	for (i = 0; i < N_NAMED; i++) {
		(void)snprintf(nm, sizeof nm, "n.%d", i);
		if (xtc_reg_whereis(g_reg, nm, &p) != XTC_OK ||
		    !xtc_pid_eq(p, xtc_atomic_pid_load(&g_named[i])))
			g_lost_names++;
	}
	g_stop = 1;
	(void)xtc_sup_stop(g_sup);
	(void)xtc_exit_pid(g_reaper, 1);
	g_done = 1;
}

static int
run_one(uint64_t seed, int *out_restarts, uint64_t *out_state)
{
	xtc_sup_opts_t so = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t kids[N_KIDS];
	int i, rc;

	memset(g_starts, 0, sizeof g_starts);
	memset(kids, 0, sizeof kids);
	g_stop = g_rogues_done = g_forged = g_lost_names = g_done = 0;
	for (i = 0; i < N_KIDS; i++)
		xtc_atomic_pid_store(&g_kid[i], XTC_PID_NONE);
	for (i = 0; i < N_NAMED; i++)
		xtc_atomic_pid_store(&g_named[i], XTC_PID_NONE);
	for (i = 0; i <= N_ROGUES; i++)
		xtc_atomic_pid_store(&g_others[i], XTC_PID_NONE);
	if (xtc_exec_init(&g_e, N_LOOPS) != XTC_OK)
		return -1;
	if (xtc_reg_create(&g_reg) != XTC_OK)
		return -1;
	xtc_sim_net_latency(10 * 1000LL, 600 * 1000LL);
	/* The crash plan is a function of the seed, picked before the run. */
	g_crash_kid = (int)((seed >> 7) % N_KIDS);
	g_crash_at = (int64_t)(3 + (seed >> 11) % 7) * MS;
	so.strategy = XTC_SUP_ONE_FOR_ONE;
	so.max_restarts = 10;
	for (i = 0; i < N_KIDS; i++) {
		kids[i].size = sizeof kids[i];
		kids[i].name = "kid";
		kids[i].fn = kid;
		kids[i].arg = (void *)(intptr_t)i;
		kids[i].policy = XTC_RESTART_PERMANENT;
	}
	(void)xtc_proc_spawn(xtc_exec_loop(g_e, 1), xtc_reg_reaper, g_reg, NULL,
	    &g_reaper);
	for (i = 0; i < N_NAMED; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(g_e,
		                         (unsigned)(i % N_LOOPS)),
		    named, (void *)(intptr_t)i, NULL, NULL);
	if (xtc_sup_start(xtc_exec_loop(g_e, 0), &so, kids, N_KIDS, &g_sup) !=
	    XTC_OK)
		return -1;
	for (i = 0; i < N_ROGUES; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(g_e,
		                         (unsigned)(i % N_LOOPS)),
		    rogue, (void *)(intptr_t)i, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(g_e, 2), coordinator, NULL, NULL,
	    NULL);
	rc = xtc_sim_exec_run(g_e, seed, 20000000);
	xtc_sim_net_latency(0, 0);
	*out_restarts = xtc_sup_n_restarts(g_sup);
	*out_state = xtc_sim_state_hash(g_e);
	(void)xtc_sup_join(g_sup, 0);
	(void)xtc_exec_fini(g_e);
	xtc_reg_destroy(g_reg);
	return rc;
}

/* 1 iff the seed's run met (a)-(c); prints what failed. */
static int
check_one(uint64_t k, int rc, int restarts)
{
	int i, pass = 1, extra = 0;

	for (i = 0; i < N_KIDS; i++)
		extra += g_starts[i] - (i == g_crash_kid ? 2 : 1);
	if (extra != 0 || restarts != 1) {
		printf("  seed %llu: a forged DOWN restarted a live child "
		       "(starts beyond the one crash %d, restarts %d)\n",
		    (unsigned long long)k, extra, restarts);
		pass = 0;
	}
	if (g_lost_names != 0) {
		printf("  seed %llu: a forged DOWN dropped a live name (%d of "
		       "%d)\n",
		    (unsigned long long)k, g_lost_names, N_NAMED);
		pass = 0;
	}
	if (rc != XTC_OK || !g_done) {
		printf("  seed %llu: rc=%d done=%d\n", (unsigned long long)k,
		    rc, g_done);
		pass = 0;
	}
	return pass;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0, seed, st, st2;
	int n = 16, k, fails = 0, rc, rc2, r, r2, forged = 0;
	int starts[N_KIDS];

	if (argc > 1)
		base = strtoull(argv[1], NULL, 0);
	if (argc > 2)
		n = atoi(argv[2]);
	printf("== forged DOWN DST: %d seeds from base %llu ==\n", n,
	    (unsigned long long)base);
	for (k = 1; k <= n; k++) {
		seed = 0x9E3779B97F4A7C15ull * (base + (uint64_t)k);
		rc = run_one(seed, &r, &st);
		forged += g_forged;
		if (!check_one(base + (uint64_t)k, rc, r)) {
			fails++;
			continue;
		}
		memcpy(starts, g_starts, sizeof starts);
		rc2 = run_one(seed, &r2, &st2);
		if (rc2 != rc || r2 != r || st2 != st ||
		    memcmp(starts, g_starts, sizeof starts) != 0) {
			printf("  seed %llu: replay diverged\n",
			    (unsigned long long)(base + (uint64_t)k));
			fails++;
		}
	}
	if (fails != 0) {
		printf("FAIL: %d/%d forged DOWN seeds failed\n", fails, n);
		return 1;
	}
	if (forged == 0) {
		printf("FAIL: no forged DOWN was delivered\n");
		return 1;
	}
	printf("OK: forged DOWN DST -- %d seeds, %d forged DOWNs to a "
	       "supervisor and a reaper across 3 loops with latency, one real "
	       "crash each: only it restarted, every name kept, all replay\n",
	    n, forged);
	return 0;
}
