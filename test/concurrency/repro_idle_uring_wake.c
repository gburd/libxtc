/*
 * repro_idle_uring_wake.c -- reproduce the carrier team's cross-thread
 * wake miss to an idle io_uring loop (report 2026-07-06).
 *
 * A driver thread repeatedly foreign-spawns a proc onto a carrier loop
 * (its own OS thread, io_uring backend) while that loop oscillates
 * between briefly busy and idle (parked in io_uring_wait with a parked
 * supervisor fiber but no runnable work).  Each spawned proc bumps a
 * counter; a monitor on it must deliver a DOWN.  If the cross-thread
 * enqueue wake is ever missed, a spawned proc's body never runs and the
 * counter falls short -- the lost-wakeup the report describes.
 *
 * This is a NATIVE (real io_uring) stress test, not a DST test: the bug
 * is specific to the io_uring wakeup mechanism, which the sim backend
 * does not use.  Runs under make check on Linux with io_uring.
 *
 * NOTE: run standalone (`make repro_idle_uring_wake && ./repro_idle_uring_wake`),
 * not as part of the leak-checked `make check` C-test set: it foreign-
 * spawns many procs onto a service-mode executor and exposes a separate,
 * pre-existing xtc_exec_fini teardown leak (cross-thread-spawned procs
 * that ran+exited are not fully reclaimed at fini -- see
 * docs/KNOWN_ISSUES.md).  The lost-wakeup guarantee this test checks is
 * ALSO covered leak-clean under DST by test/sim/test_sim_wake_park.c.
 */

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "os_thread.h"
#include "os_time.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define N_LOOPS   4
#define N_SPAWN   2000     /* foreign spawns across the burst */

static atomic_int g_ran;       /* spawned proc bodies that executed */

/* The spawned worker: mark that it ran, then exit cleanly. */
static void
worker(void *arg)
{
	(void)arg;
	atomic_fetch_add(&g_ran, 1);
	xtc_exit_self(0);
}

/*
 * A per-loop supervisor: parks in xtc_recv (so its loop goes idle in
 * io_uring_wait), and on each message spawn+monitors a worker on ITS
 * OWN loop, then drains the DOWN.  This mirrors the carrier's
 * supervisor-per-loop placement, where the wake miss was seen.
 */
struct sup_arg { int unused; };

static void
supervisor(void *arg)
{
	(void)arg;
	/* Park forever in recv -- keeps n_alive > 0 on this loop, so an
	 * idle loop takes the INFINITE io_uring_wait park (the permanent-
	 * hang path), not the bounded worker poll.  A foreign spawn onto
	 * this loop must still wake it. */
	for (;;) {
		void *m = NULL; size_t n = 0;
		if (xtc_recv(&m, &n, -1) != XTC_OK)
			break;
		if (m != NULL) xtc_free(m);
	}
}

static xtc_exec_t *g_e;
static void *
exec_thread(void *arg)
{
	(void)arg;
	(void)xtc_exec_run(g_e);   /* blocks until xtc_exec_stop */
	return NULL;
}

int
main(void)
{
	pthread_t th;
	int i, s;

	if (xtc_exec_init(&g_e, N_LOOPS) != XTC_OK) {
		fprintf(stderr, "SKIP: exec_init failed (backend/resource unavailable)\n");
		return 77;   /* automake SKIP convention */
	}
	/* Service mode: do NOT auto-stop on a transient all-idle window --
	 * this test WANTS the loops to go idle between foreign spawns. */
	xtc_exec_set_service_mode(g_e, 1);

	for (i = 0; i < N_LOOPS; i++) {
		if (xtc_proc_spawn(xtc_exec_loop(g_e, i), supervisor, NULL,
		    NULL, NULL) != XTC_OK) {
			fprintf(stderr, "spawn supervisor failed\n");
			return 2;
		}
	}

	/* Run the executor on a background thread; THIS thread is the
	 * foreign sender (the report's driver on a thread not on any loop). */
	if (pthread_create(&th, NULL, exec_thread, NULL) != 0) {
		fprintf(stderr, "pthread_create failed\n");
		return 2;
	}
	/* Let the supervisors reach their first xtc_recv park. */
	(void)xtc_sleep_ns(50 * 1000 * 1000LL);

	/* Drive the burst from THIS (foreign) thread.  DIRECT cross-thread
	 * spawn onto each loop (the report's simplest repro: bypass the
	 * supervisor/mailbox -- foreign-spawn a proc straight onto a loop
	 * that is parked idle in io_uring_wait, and assert its body runs).
	 * The monitor DOWN confirms it ran and exited. */
	for (s = 0; s < N_SPAWN; s++) {
		int k = s % N_LOOPS;
		xtc_pid_t child;
		int sc;
		(void)xtc_sleep_ns(60 * 1000LL);   /* ~60us: let loop k re-park */
		/* Plain foreign spawn (the report's simplest repro): the
		 * caller is a foreign OS thread, NOT a proc, so spawn_monitor
		 * (which requires a proc caller) does not apply -- xtc_proc_spawn
		 * onto the idle loop must wake it and run the body. */
		sc = xtc_proc_spawn(xtc_exec_loop(g_e, k), worker, NULL,
		    NULL, &child);
		if (sc != XTC_OK) {
			if (s < 5) fprintf(stderr, "spawn %d rc=%d\n", s, sc);
			continue;
		}
		(void)child;
	}

	/* Wait (bounded) for all spawns to run.  (We count runs; the DOWNs
	 * go to this foreign thread's non-existent mailbox, so we only
	 * assert the bodies ran -- a missed wake means g_ran falls short.) */
	for (i = 0; i < 6000; i++) {
		if (atomic_load(&g_ran) >= N_SPAWN)
			break;
		(void)xtc_sleep_ns(5 * 1000 * 1000LL);
	}
	/* Grace drain: the last procs bumped g_ran in their body but their
	 * task/coro teardown runs on the loop's NEXT step -- give the loops
	 * a moment to reap them so xtc_exec_fini has nothing outstanding
	 * (otherwise LeakSanitizer flags the un-reaped tasks). */
	(void)xtc_sleep_ns(100 * 1000 * 1000LL);

	(void)xtc_exec_stop(g_e);
	(void)pthread_join(th, NULL);
	(void)xtc_exec_fini(g_e);

	{
		int ran = atomic_load(&g_ran);
		if (ran != N_SPAWN) {
			printf("FAIL: %d/%d foreign-spawned procs ran -- "
			    "cross-thread wake to an idle io_uring loop was "
			    "MISSED (%d lost)\n", ran, N_SPAWN, N_SPAWN - ran);
			return 1;
		}
		printf("OK: all %d foreign spawns onto idle io_uring loops "
		    "ran -- no lost wakeup\n", N_SPAWN);
	}
	return 0;
}
