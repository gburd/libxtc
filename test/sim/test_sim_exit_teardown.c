/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_exit_teardown.c
 *	Deterministic Simulation Testing of clean fiber TEARDOWN under a
 *	monitored N-loop pool -- the shape the xtc-carrier / PostgreSQL
 *	team reported (their "Item 2": a monitored backend that exits
 *	cleanly via xtc_exit_self(0) delivers a DOWN reason that looked
 *	like -SIGSEGV).
 *
 *	Root cause this test pinned down: the reported reason was
 *	XTC_E_NOTFOUND (-11), which COLLIDES with -SIGSEGV -- it is the
 *	"monitor registered on an already-dead target" reason, delivered
 *	when the supervisor's xtc_monitor() raced the backend's clean
 *	exit.  libxtc now delivers a DISTINCT XTC_DOWN_NOPROC for that
 *	case so a supervisor can tell "already gone" from a real crash.
 *
 *	This test drives an N-loop pool where a per-loop supervisor
 *	spawns each worker and monitors it ATOMICALLY (spawn immediately
 *	followed by xtc_monitor, no yield between -- exactly the carrier's
 *	pattern), so the monitor is in place before the worker can run.
 *	Each worker yields a seeded number of times (repeated parks) then
 *	xtc_exit_self(0).  Invariant: EVERY monitored worker delivers a
 *	DOWN with reason 0 (clean) -- never a signal-numbered fault reason,
 *	never XTC_DOWN_NOPROC (the atomic spawn+monitor means no race) --
 *	and spawned == down-observed.  Replay-verified across a seed sweep.
 *
 *	The single-thread sim cannot inject a real hardware SIGSEGV, so
 *	this covers the CLEAN-teardown + monitor-classification path.
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
#include "xtc_sim.h"

#define N_LOOPS   4
#define N_PROCS   24          /* many short-lived monitored procs */

static atomic_int g_spawned;
static atomic_int g_down_ok;      /* DOWN reason 0 (clean) */
static atomic_int g_down_noproc;  /* DOWN XTC_DOWN_NOPROC (monitor raced exit) */
static atomic_int g_down_bad;     /* any OTHER reason (a real classification bug) */
static atomic_int g_sups_done;    /* supervisors that finished draining */

/* A worker: yield a seeded number of times, then exit cleanly. */
static void
worker(void *arg)
{
	int k = 1 + (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 6);
	int i;
	void *scratch = malloc(4096);
	atomic_fetch_add(&g_spawned, 1);
	for (i = 0; i < k; i++) {
		if (scratch)
			memset(scratch, i & 0xff, 4096);
		xtc_yield();
	}
	free(scratch);
	xtc_exit_self(0);            /* clean exit -- must deliver DOWN reason 0 */
	/* NOTREACHED */
}

/* Supervisor on each loop: spawn N/L workers on ITS loop, monitoring
 * each ATOMICALLY (spawn then monitor, no yield between), then drain the
 * DOWNs and classify by reason. */
struct sup_arg { xtc_exec_t *e; int loop; int n; };

static void
supervisor(void *arg)
{
	struct sup_arg *sa = arg;
	xtc_loop_t *l = xtc_exec_loop(sa->e, sa->loop);
	int i, seen = 0, tries;

	for (i = 0; i < sa->n; i++) {
		xtc_pid_t pid;
		if (xtc_proc_spawn(l, worker, NULL, NULL, &pid) == XTC_OK)
			(void)xtc_monitor(pid, NULL);   /* no yield between */
	}

	for (tries = 0; tries < 40000 && seen < sa->n; tries++) {
		void *m = NULL;
		size_t n = 0;
		int reason = 0;
		xtc_pid_t dpid;
		if (xtc_recv(&m, &n, 2 * 1000 * 1000LL) == XTC_OK && m != NULL) {
			if (xtc_down_decode(m, n, &dpid, &reason) == XTC_OK) {
				seen++;
				if (reason == 0)
					atomic_fetch_add(&g_down_ok, 1);
				else if (xtc_down_is_noproc(reason))
					atomic_fetch_add(&g_down_noproc, 1);
				else
					atomic_fetch_add(&g_down_bad, 1);
			}
			xtc_free(m);
		}
	}
	/* Stop only when EVERY supervisor has finished draining, so one
	 * supervisor finishing does not cut off another's pending DOWN. */
	if (atomic_fetch_add(&g_sups_done, 1) + 1 >= N_LOOPS)
		(void)xtc_exec_stop(sa->e);
}

static int
run_one(uint64_t seed, int *out_ok, int *out_noproc, int *out_bad,
    int *out_spawned, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	struct sup_arg sa[N_LOOPS];
	int i, rc, base, rem;

	atomic_store(&g_spawned, 0);
	atomic_store(&g_down_ok, 0);
	atomic_store(&g_down_noproc, 0);
	atomic_store(&g_down_bad, 0);
	atomic_store(&g_sups_done, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK)
		return -1;
	xtc_exec_set_service_mode(e, 1);

	/* One supervisor per loop; split the N workers across them.  Each
	 * supervisor spawns+monitors its share on its own loop. */
	base = N_PROCS / N_LOOPS;
	rem = N_PROCS % N_LOOPS;
	for (i = 0; i < N_LOOPS; i++) {
		sa[i].e = e;
		sa[i].loop = i;
		sa[i].n = base + (i < rem ? 1 : 0);
		(void)xtc_proc_spawn(xtc_exec_loop(e, i), supervisor, &sa[i],
		    NULL, NULL);
	}

	rc = xtc_sim_exec_run(e, seed, 20000000);

	if (out_ok)      *out_ok = atomic_load(&g_down_ok);
	if (out_noproc)  *out_noproc = atomic_load(&g_down_noproc);
	if (out_bad)     *out_bad = atomic_load(&g_down_bad);
	if (out_spawned) *out_spawned = atomic_load(&g_spawned);
	if (out_state)   *out_state = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0x74646e; /* "tdn" */
	int n = 24, i, fails = 0;

	if (argc > 1) base = strtoull(argv[1], NULL, 0);
	if (argc > 2) n = atoi(argv[2]);

	printf("== clean-teardown DST (atomic spawn+monitor, N-loop): %d "
	    "seeds from base 0x%llx ==\n", n, (unsigned long long)base);

	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		int ok = 0, np = 0, bad = 0, sp = 0;
		int ok2 = 0, np2 = 0, bad2 = 0, sp2 = 0;
		int rc, rc2, pass = 1;
		uint64_t st = 0, st2 = 0;

		rc = run_one(seed, &ok, &np, &bad, &sp, &st);
		if (rc != XTC_OK) pass = 0;
		else if (sp != N_PROCS) pass = 0;      /* all workers ran */
		else if (bad != 0) pass = 0;           /* no ambiguous reason */
		/* Atomic spawn+monitor means the monitor is in place before
		 * the worker exits, so every DOWN is a clean reason-0 exit;
		 * NONE should be XTC_DOWN_NOPROC. */
		else if (ok != N_PROCS) pass = 0;
		else if (np != 0) pass = 0;

		if (pass) {
			rc2 = run_one(seed, &ok2, &np2, &bad2, &sp2, &st2);
			if (rc2 != rc || ok2 != ok || np2 != np || bad2 != bad ||
			    st2 != st)
				pass = 0;
		}

		if (!pass) {
			printf("  seed 0x%016llx: FAIL (spawned=%d ok=%d "
			    "noproc=%d bad=%d rc=%d)\n",
			    (unsigned long long)seed, sp, ok, np, bad, rc);
			fails++;
		}
	}

	if (fails == 0) {
		printf("OK: clean-teardown DST -- %d seeds, atomic "
		    "spawn+monitor delivers DOWN reason 0 for every clean "
		    "xtc_exit_self(0), no NOPROC, no ambiguous reason, all "
		    "replay\n", n);
		return 0;
	}
	printf("FAIL: %d/%d clean-teardown seeds failed\n", fails, n);
	return 1;
}
