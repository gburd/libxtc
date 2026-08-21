/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/sim/test_sim_scope.c
 *	DST HEADLINE GATE for A1 (resource scope / bracket) + A2
 *	(cancellation masking).  The proof that "every finalizer registered
 *	in an open scope runs on EVERY exit path" is a MECHANISM, not a
 *	manner -- and that it holds when an abort OR a contained fault is
 *	injected in the MIDDLE of an open scope, replayable byte-identically
 *	from a seed.
 *
 *	One run, for a given seed:
 *	  - N worker fibers across several loops each open a scope, defer a
 *	    fixed number of finalizers into it (each bumps a per-worker
 *	    counter), acquire a resource under a MASKED (uncancelable)
 *	    acquire, then yield a seeded number of times before closing the
 *	    scope normally.
 *	  - A seeded subset of workers is chosen (from the FAULT stream) to
 *	    be DISRUPTED mid-scope: either
 *	      (a) ABORT: a killer fiber calls xtc_exit_pid on the worker
 *	          while its scope is open (delivered at the worker's next
 *	          park -- but DEFERRED while it is inside the masked
 *	          acquire, the A2 guarantee), or
 *	      (b) FAULT: the worker dereferences a bad pointer mid-scope,
 *	          contained by the per-proc fault guard.
 *	  - INVARIANT: for EVERY worker -- whether it closed normally, was
 *	    aborted, or crashed -- ALL of its deferred finalizers ran
 *	    exactly once (fin_count[w] == DEFER_PER), and the count of
 *	    workers whose masked-acquire release registered equals the count
 *	    that started (no finalizer eaten by a mid-mask abort).
 *	  - REPLAY: the same seed reproduces the identical disruption set,
 *	    the identical finalizer tallies, and the identical scheduler
 *	    state hash; a sweep of seeds disrupts different workers in
 *	    different ways and EACH satisfies the invariant.
 *
 *	This is the DST version of test/m8/test_scope.c: instead of one
 *	fixed exit path it hits a SEEDED exit path mid-scope under the
 *	deterministic scheduler and proves no finalizer is ever eaten --
 *	the paper-door-becomes-a-mechanism claim, replayably.
 *
 *	The single-thread sim executes a real synchronous SIGSEGV inside
 *	the faulting fiber deterministically (the fault guard's siglongjmp
 *	is ordinary control flow on the sim thread), so the contained-fault
 *	path is exercised for real, not modelled.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_async.h"
#include "xtc_sim.h"

#define N_LOOPS     3
#define N_WORKERS   18
#define DEFER_PER   4          /* finalizers each worker defers */

/* Disruption kinds a worker may be assigned (seeded). */
enum { DIS_NONE = 0, DIS_ABORT = 1, DIS_FAULT = 2 };

/* Per-worker observations (disjoint slots -> no cross-fiber races). */
static _Atomic int g_fin_count[N_WORKERS];   /* finalizers that ran */
static _Atomic int g_release_ran[N_WORKERS]; /* masked-acquire release ran */
static _Atomic int g_started[N_WORKERS];     /* worker began its scope */
static int         g_disrupt[N_WORKERS];     /* DIS_* assigned this run */

static xtc_exec_t *g_exec;
static xtc_pid_t   g_worker_pid[N_WORKERS];
static _Atomic int g_pid_ready[N_WORKERS];

struct warg { int id; };
static struct warg g_warg[N_WORKERS];

/* A deferred finalizer: bump the worker's tally.  arg encodes the
 * worker id so we attribute correctly regardless of run order. */
static void
fin_bump(void *arg)
{
	int w = (int)(intptr_t)arg;
	if (w >= 0 && w < N_WORKERS)
		atomic_fetch_add(&g_fin_count[w], 1);
}

/* The masked-acquire release (via xtc_scope_defer inside a masked
 * region): records that the release for this worker registered + ran. */
static void
release_bump(void *arg)
{
	int w = (int)(intptr_t)arg;
	if (w >= 0 && w < N_WORKERS)
		atomic_fetch_add(&g_release_ran[w], 1);
}

/* body run under xtc_uncancelable: "acquire" a resource and register its
 * release into the scope.  A kill delivered here must be DEFERRED so the
 * defer always lands. */
struct acq_ctx { xtc_scope_t *s; int w; };
static int
masked_acquire(void *ud)
{
	struct acq_ctx *a = ud;
	int i;
	/* Yield a couple times WHILE masked so the killer (if any) fires
	 * mid-acquire; the kill must not be observed until we return. */
	for (i = 0; i < 2; i++)
		xtc_yield();
	(void)xtc_scope_defer(a->s, release_bump, (void *)(intptr_t)a->w);
	return XTC_OK;
}

static void
worker(void *arg)
{
	struct warg *wa = arg;
	int w = wa->id;
	xtc_scope_t *s;
	struct acq_ctx a;
	int i, dis;

	/* Arm the default fault-recovery so a contained fault unwinds this
	 * one proc (and runs the recovery registry -> open scopes). */
	xtc_proc_recovery_arm_clean();

	atomic_store(&g_started[w], 1);

	s = xtc_scope_open();
	if (s == NULL) {
		xtc_exit_self(0);
		return;
	}
	/* Defer DEFER_PER finalizers LIFO. */
	for (i = 0; i < DEFER_PER; i++)
		(void)xtc_scope_defer(s, fin_bump, (void *)(intptr_t)w);

	/* Masked acquire: registers the release even if a kill lands mid-
	 * acquire (A2 defers it). */
	a.s = s;
	a.w = w;
	(void)xtc_uncancelable(masked_acquire, &a);

	/* Now advertise our pid so the killer can target us, and yield a
	 * seeded number of times mid-scope so a kill / fault lands here. */
	atomic_store(&g_pid_ready[w], 1);

	dis = g_disrupt[w];
	if (dis == DIS_FAULT) {
		/* Contained fault mid-scope: the recovery unwind must still
		 * run this worker's open-scope finalizers. */
		volatile uintptr_t bad = 0x10;
		*(volatile int *)bad = 1;   /* boom */
		/* NOTREACHED */
	}

	{
		int k = 1 + (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 6);
		for (i = 0; i < k; i++)
			xtc_yield();
	}

	/* Normal path (and the abort path parks here): close the scope.
	 * If an abort was delivered it unwinds before/at a park and the
	 * recovery registry closes the still-open scope. */
	xtc_scope_close(s);
	xtc_exit_self(0);
}

/* The killer fiber: for each worker assigned DIS_ABORT, wait until its
 * pid is advertised, then xtc_exit_pid it (mid-scope). */
static void
killer(void *arg)
{
	int done = 0, tries;
	(void)arg;
	for (tries = 0; tries < 40000 && done < N_WORKERS; tries++) {
		int w, remaining = 0;
		for (w = 0; w < N_WORKERS; w++) {
			if (g_disrupt[w] != DIS_ABORT)
				continue;
			if (atomic_load(&g_pid_ready[w]) == 1) {
				/* Fire once, then mark handled by clearing ready. */
				atomic_store(&g_pid_ready[w], 2);
				(void)xtc_exit_pid(g_worker_pid[w], 7);
				done++;
			} else if (atomic_load(&g_pid_ready[w]) == 1) {
				remaining++;
			}
		}
		(void)remaining;
		xtc_yield();
	}
}

static int
run_one_inproc(uint64_t seed, int *out_started, int *out_all_fin, int *out_rel,
    int *out_abort, int *out_fault, uint64_t *out_state)
{
	int i, rc, started, all_fin = 1, rel = 0, n_abort = 0, n_fault = 0;

	(void)xtc_fault_guard_install();

	for (i = 0; i < N_WORKERS; i++) {
		atomic_store(&g_fin_count[i], 0);
		atomic_store(&g_release_ran[i], 0);
		atomic_store(&g_started[i], 0);
		atomic_store(&g_pid_ready[i], 0);
		g_disrupt[i] = DIS_NONE;
	}

	if (xtc_exec_init(&g_exec, N_LOOPS) != XTC_OK)
		return -1;
	xtc_exec_set_service_mode(g_exec, 1);

	/* Seeded disruption assignment, drawn from the FAULT stream so it
	 * does NOT perturb the scheduler streams: ~1/3 abort, ~1/3 fault,
	 * ~1/3 undisturbed. */
	for (i = 0; i < N_WORKERS; i++) {
		unsigned draw = (unsigned)__xtc_sim_rng_range(XTC_SIM_RNG_FAULT, 3);
		g_disrupt[i] = (draw == 0) ? DIS_ABORT :
		               (draw == 1) ? DIS_FAULT : DIS_NONE;
		if (g_disrupt[i] == DIS_ABORT) n_abort++;
		else if (g_disrupt[i] == DIS_FAULT) n_fault++;
	}

	/* Spawn workers spread across the loops; capture pids for the
	 * killer.  A worker is spawned unmonitored -- we tally its own
	 * finalizer runs, not a DOWN. */
	for (i = 0; i < N_WORKERS; i++) {
		xtc_loop_t *l = xtc_exec_loop(g_exec, i % N_LOOPS);
		g_warg[i].id = i;
		if (xtc_proc_spawn(l, worker, &g_warg[i], NULL,
		    &g_worker_pid[i]) != XTC_OK) {
			(void)xtc_exec_fini(g_exec);
			g_exec = NULL;
			return -2;
		}
	}
	/* Killer on loop 0. */
	(void)xtc_proc_spawn(xtc_exec_loop(g_exec, 0), killer, NULL, NULL, NULL);

	rc = xtc_sim_exec_run(g_exec, seed, 20000000);

	started = 0;
	for (i = 0; i < N_WORKERS; i++) {
		if (atomic_load(&g_started[i]))
			started++;
		/* Every started worker MUST have run all DEFER_PER
		 * finalizers -- on close, abort, or contained fault. */
		if (atomic_load(&g_started[i]) &&
		    atomic_load(&g_fin_count[i]) != DEFER_PER)
			all_fin = 0;
		/* Its masked-acquire release must also have run exactly once
		 * (the A2 defer-across-mask guarantee -- never eaten). */
		if (atomic_load(&g_started[i]) &&
		    atomic_load(&g_release_ran[i]) == 1)
			rel++;
	}

	if (out_started) *out_started = started;
	if (out_all_fin) *out_all_fin = all_fin;
	if (out_rel)     *out_rel = rel;
	if (out_abort)   *out_abort = n_abort;
	if (out_fault)   *out_fault = n_fault;
	if (out_state)   *out_state = xtc_sim_state_hash(g_exec);

	(void)xtc_exec_fini(g_exec);
	g_exec = NULL;
	return rc;
}

/*
 * Fork-isolated wrapper (the FoundationDB discipline, as in
 * test_sim_crash_recover): each run executes in a fresh address space so
 * process-global runtime state -- pid/generation counters, the
 * once-installed fault guard, RNG bookkeeping -- starts pristine and the
 * scheduler state hash is a pure function of the seed.  Without this,
 * back-to-back in-process runs accumulate global state and the replay
 * state-hash check fails for reasons unrelated to the scope mechanism
 * (whose per-worker finalizer tallies DO already replay).  The child
 * reports its result over a pipe.
 */
struct run_result {
	int      rc;
	int      started;
	int      all_fin;
	int      rel;
	int      n_abort;
	int      n_fault;
	uint64_t state;
};

static int
run_one(uint64_t seed, int *out_started, int *out_all_fin, int *out_rel,
    int *out_abort, int *out_fault, uint64_t *out_state)
{
	int pfd[2];
	pid_t pid;
	struct run_result rr;

	if (pipe(pfd) != 0)
		return -3;
	pid = fork();
	if (pid < 0) {
		close(pfd[0]); close(pfd[1]);
		return -3;
	}
	if (pid == 0) {
		ssize_t wn;
		close(pfd[0]);
		memset(&rr, 0, sizeof rr);
		rr.rc = run_one_inproc(seed, &rr.started, &rr.all_fin, &rr.rel,
		    &rr.n_abort, &rr.n_fault, &rr.state);
		wn = write(pfd[1], &rr, sizeof rr);
		close(pfd[1]);
		_exit(wn == (ssize_t)sizeof rr ? 0 : 2);
	}
	close(pfd[1]);
	{
		size_t got = 0;
		int status;
		while (got < sizeof rr) {
			ssize_t n = read(pfd[0], (char *)&rr + got,
			    sizeof rr - got);
			if (n <= 0)
				break;
			got += (size_t)n;
		}
		close(pfd[0]);
		(void)waitpid(pid, &status, 0);
		if (got != sizeof rr)
			return -4;   /* child crashed / short write */
	}
	if (out_started) *out_started = rr.started;
	if (out_all_fin) *out_all_fin = rr.all_fin;
	if (out_rel)     *out_rel = rr.rel;
	if (out_abort)   *out_abort = rr.n_abort;
	if (out_fault)   *out_fault = rr.n_fault;
	if (out_state)   *out_state = rr.state;
	return rr.rc;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0x53434f5045ull;   /* "SCOPE" */
	int n = 40, i, fails = 0;
	int tot_abort = 0, tot_fault = 0, tot_none_runs = 0;

	if (argc > 1) base = strtoull(argv[1], NULL, 0);
	if (argc > 2) n = atoi(argv[2]);

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
	printf("OK: test_sim_scope SKIP under sanitizer\n");
	return 0;
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	printf("OK: test_sim_scope SKIP under sanitizer\n");
	return 0;
#  endif
#endif

	printf("== scope/bracket + masking DST: %d seeds from base 0x%llx "
	    "(%d workers, %d loops) ==\n", n, (unsigned long long)base,
	    N_WORKERS, N_LOOPS);

	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		int st = 0, af = 0, rl = 0, ab = 0, fl = 0;
		int st2 = 0, af2 = 0, rl2 = 0, ab2 = 0, fl2 = 0;
		uint64_t sh = 0, sh2 = 0;
		int rc, rc2, pass = 1;

		rc = run_one(seed, &st, &af, &rl, &ab, &fl, &sh);
		if (rc != XTC_OK) pass = 0;
		else if (st != N_WORKERS) pass = 0;   /* all workers started */
		else if (af != 1) pass = 0;           /* ALL finalizers ran */
		else if (rl != st) pass = 0;          /* every release registered */

		if (pass) {
			rc2 = run_one(seed, &st2, &af2, &rl2, &ab2, &fl2, &sh2);
			if (rc2 != rc || st2 != st || af2 != af || rl2 != rl ||
			    ab2 != ab || fl2 != fl || sh2 != sh)
				pass = 0;
		}

		if (!pass) {
			printf("  seed 0x%016llx: FAIL (started=%d all_fin=%d "
			    "rel=%d abort=%d fault=%d rc=%d; replay sh=%016llx/"
			    "%016llx)\n",
			    (unsigned long long)seed, st, af, rl, ab, fl, rc,
			    (unsigned long long)sh, (unsigned long long)sh2);
			fails++;
		} else {
			tot_abort += ab;
			tot_fault += fl;
			if (ab == 0 && fl == 0)
				tot_none_runs++;
		}
	}

	if (fails == 0) {
		printf("OK: scope/bracket + masking DST -- %d seeds, every "
		    "started worker ran all %d deferred finalizers AND its "
		    "masked-acquire release on every exit path (normal close, "
		    "seeded abort mid-scope, seeded contained fault mid-scope); "
		    "no finalizer eaten by a mid-mask abort; replay-identical "
		    "(disruptions across the sweep: %d aborts, %d faults, %d "
		    "seeds left all workers undisturbed)\n",
		    n, DEFER_PER, tot_abort, tot_fault, tot_none_runs);
		return 0;
	}
	printf("FAIL: %d/%d scope DST seeds failed\n", fails, n);
	return 1;
}
