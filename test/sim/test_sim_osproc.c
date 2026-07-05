/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_osproc.c
 *	Deterministic Simulation Testing of the OS-subprocess LIFECYCLE
 *	(src/orc/osproc.c), using the FoundationDB "process = in-process
 *	actor with a simulated lifecycle" pattern.
 *
 *	Production xtc_osproc_spawn fork()s a real child whose scheduling
 *	and clock the single-thread sim cannot control -- the same reason
 *	FoundationDB never fork/exec's under simulation.  FDB instead
 *	models a "process" (ISimulator::ProcessInfo) as an in-process actor
 *	with a simulated lifecycle (spawn / kill / reboot keyed by
 *	KillType), pushing REAL subprocess spawning out to fdbmonitor,
 *	which the simulated code path never crosses.  xtc_osproc does the
 *	same under sim: the fn-callback child runs as an xtc_proc FIBER and
 *	its lifecycle -- running, exit-with-status, signalled-termination,
 *	wait/try_wait/reap -- is modelled on the sim clock.  (The exec
 *	(argv) and live control-SOCKET paths have no in-process equivalent
 *	and decline with XTC_E_NOSYS under sim; a consumer needing them is
 *	on the not-coverable real-kernel tier, exactly as FDB's real exec
 *	lives outside the simulator.)
 *
 *	Scenario: a manager fiber spawns N isolated workers.  Each worker
 *	computes a seeded value and _exits with a low-byte status derived
 *	from it; the manager xtc_osproc_wait()s each and checks the exit
 *	code.  One extra worker is SIGNAL-killed before it can exit and the
 *	manager verifies it reaps as signalled, not normally-exited.
 *
 *	Invariants (per seed): every normally-exiting worker's WEXITSTATUS
 *	matches the seeded value; the killed worker reaps as WIFSIGNALED;
 *	the run quiesces; and a repeated seed reproduces the outcome
 *	(fingerprint) -- the replay property for a simulated process tree.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_osproc.h"
#include "xtc_sim.h"

#define N_LOOPS   2
#define N_WORKERS 5

static atomic_int g_exit_ok;     /* workers whose exit code matched */
static atomic_int g_killed_ok;   /* the killed worker reaped as signalled */
static atomic_int g_done;

/* Worker callback: exit with a status derived from its arg.  Runs as a
 * fiber under sim (ctrl_fd is -1 -- no control socket in this test). */
static int
worker_fn(int ctrl_fd, void *arg)
{
	int code = (int)(intptr_t)arg;
	(void)ctrl_fd;
	/* Yield a few times so the parent and other children interleave
	 * under the seeded scheduler. */
	xtc_yield();
	xtc_yield();
	return code & 0xff;
}

/* A worker that never returns on its own -- parks forever, so the
 * manager must SIGNAL-kill it. */
static int
stuck_fn(int ctrl_fd, void *arg)
{
	(void)ctrl_fd; (void)arg;
	for (;;)
		(void)xtc_proc_sleep(10 * 1000 * 1000LL);
	/* not reached */
}

/* Manager: spawn N workers with seeded exit codes, wait + verify each;
 * then spawn a stuck worker, kill it, and verify signalled reap. */
static void
manager(void *arg)
{
	xtc_exec_t *e = arg;
	int i;

	for (i = 0; i < N_WORKERS; i++) {
		xtc_osproc_t *p = NULL;
		int want = (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 200) + 1;
		int st = 0;
		{
			xtc_osproc_opts_t o;
			memset(&o, 0, sizeof o);
			o.name = "worker";
			o.fn = worker_fn;
			o.arg = (void *)(intptr_t)want;
			o.ctrl_socket = 0;
			if (xtc_osproc_spawn(&o, &p) != XTC_OK || p == NULL)
				continue;
		}
		if (xtc_osproc_wait(p, &st, 1000 * 1000 * 1000LL) == XTC_OK) {
			if (WIFEXITED(st) && WEXITSTATUS(st) == (want & 0xff))
				atomic_fetch_add(&g_exit_ok, 1);
		}
		xtc_osproc_destroy(p);
	}

	/* Signal-kill scenario. */
	{
		xtc_osproc_opts_t o;
		xtc_osproc_t *p = NULL;
		int st = 0;
		memset(&o, 0, sizeof o);
		o.name = "stuck";
		o.fn = stuck_fn;
		o.ctrl_socket = 0;
		if (xtc_osproc_spawn(&o, &p) == XTC_OK && p != NULL) {
			/* Let it start, then SIGTERM it. */
			(void)xtc_proc_sleep(2 * 1000 * 1000LL);
			(void)xtc_osproc_signal(p, 15 /* SIGTERM */);
			if (xtc_osproc_wait(p, &st, 1000 * 1000 * 1000LL) ==
			    XTC_OK) {
				if (WIFSIGNALED(st))
					atomic_store(&g_killed_ok, 1);
			}
			xtc_osproc_destroy(p);
		}
	}

	atomic_fetch_add(&g_done, 1);
	(void)xtc_exec_stop(e);
}

static int
run_one(uint64_t seed, int *out_exit_ok, int *out_killed, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int rc;

	atomic_store(&g_exit_ok, 0);
	atomic_store(&g_killed_ok, 0);
	atomic_store(&g_done, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK)
		return -1;
	xtc_exec_set_service_mode(e, 1);

	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), manager, e, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 20000000);

	if (out_exit_ok != NULL) *out_exit_ok = atomic_load(&g_exit_ok);
	if (out_killed != NULL) *out_killed = atomic_load(&g_killed_ok);
	if (out_state != NULL) *out_state = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0x6f7370; /* "osp" */
	int n = 16, i, fails = 0;

	if (argc > 1) base = strtoull(argv[1], NULL, 0);
	if (argc > 2) n = atoi(argv[2]);

	printf("== osproc-lifecycle DST: %d seeds from base 0x%llx ==\n",
	    n, (unsigned long long)base);

	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		int eok = 0, kok = 0, eok2 = 0, kok2 = 0, rc, rc2, pass = 1;
		uint64_t st = 0, st2 = 0;

		rc = run_one(seed, &eok, &kok, &st);
		if (rc != XTC_OK) pass = 0;
		else if (eok != N_WORKERS) pass = 0;   /* all exit codes matched */
		else if (kok != 1) pass = 0;           /* killed reaped signalled */

		if (pass) {
			rc2 = run_one(seed, &eok2, &kok2, &st2);
			if (rc2 != rc || eok2 != eok || kok2 != kok || st2 != st)
				pass = 0;
		}

		if (!pass) {
			printf("  seed 0x%016llx: FAIL (exit_ok=%d/%d killed=%d "
			    "rc=%d)\n", (unsigned long long)seed, eok, N_WORKERS,
			    kok, rc);
			fails++;
		}
	}

	if (fails == 0) {
		printf("OK: osproc-lifecycle DST -- %d seeds, exit-status + "
		    "signalled-kill + reap modelled deterministically, all "
		    "replay\n", n);
		return 0;
	}
	printf("FAIL: %d/%d osproc-lifecycle seeds failed\n", fails, n);
	return 1;
}
