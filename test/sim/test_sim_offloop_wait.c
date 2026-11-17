/*
 * test_sim_offloop_wait -- a timed wait made on the simulator's own
 * thread outside any fiber expires at once and reads no wall clock.
 *
 * While a run is active the simulator's thread is the only one that can
 * make progress.  A plain-thread wait made there, outside any fiber,
 * blocks that thread: no fiber runs and no virtual time passes, so
 * nothing can end the wait early.  The two shapes 2.1 reaches are a
 * test's own code between xtc_sim_activate and xtc_sim_deactivate, and a
 * plain task (xtc_exec_spawn_on) the run steps.  Before 2.1.3 every
 * timed plain-thread wait took its deadline from the wall clock, so the
 * determinism guard counted a violation (strict mode aborts the run)
 * and the wait slept its whole timeout in real time.
 *
 * Each case below makes a timed wait on every primitive that has one
 * and cannot be satisfied: xtc_notify_wait, xtc_sem_acquire,
 * xtc_amutex_lock and xtc_arwlock_wrlock on a held lock, xtc_rwlock_
 * wrlock on a read-held lock, xtc_gate_drain on an entered gate,
 * xtc_future_wait on an unset future, and xtc_lock_get on a lock
 * another locker holds.  Each must return XTC_E_AGAIN, the guard (in
 * count mode) must count nothing, and the run must replay.  The waits
 * use 50 ms timeouts, so a wait that sleeps shows as real time too.
 *
 * Planted bug (src/inc/xtc_dst_inject.h) this test must catch:
 *   68 SIM_OFFLOOP_WAIT  the waits take their deadline from the wall
 *                        clock again.
 *
 * Usage: test_sim_offloop_wait
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_future.h"
#include "xtc_lockmgr.h"
#include "xtc_sim.h"
#include "xtc_sync.h"

#define MS (1000LL * 1000)
#define N_WAITS 8

static atomic_int g_task_bad = -1;

/* One timed wait on each primitive, none of which can succeed.  Returns
 * the number of waits that did not return XTC_E_AGAIN, or -1 when the
 * objects could not be built. */
static int
timed_waits(void)
{
	xtc_lockmgr_opts_t lo = XTC_LOCKMGR_OPTS_DEFAULT;
	xtc_notify_t *n = NULL;
	xtc_sem_t *s = NULL;
	xtc_amutex_t *am = NULL;
	xtc_arwlock_t *arw = NULL;
	xtc_rwlock_t *rw = NULL;
	xtc_gate_t *g = NULL;
	xtc_promise_t *pr = NULL;
	xtc_future_t *fu = NULL;
	xtc_lockmgr_t *lm = NULL;
	xtc_locker_t l1 = 0, l2 = 0;
	intptr_t v = 0;
	int bad = 0;

	/* No detector thread: it would read the clock on its own. */
	lo.detect_mode = XTC_LOCK_DETECT_NONE;
	if (xtc_notify_create(&n) != XTC_OK ||
	    xtc_sem_create(0, &s) != XTC_OK ||
	    xtc_amutex_create(&am) != XTC_OK ||
	    xtc_arwlock_create(&arw) != XTC_OK ||
	    xtc_rwlock_create(&rw) != XTC_OK || xtc_gate_create(&g) != XTC_OK ||
	    xtc_future_new_pair(&pr, &fu) != XTC_OK ||
	    xtc_lockmgr_create(&lo, &lm) != XTC_OK ||
	    xtc_lockmgr_id(lm, &l1) != XTC_OK ||
	    xtc_lockmgr_id(lm, &l2) != XTC_OK)
		return -1;
	if (xtc_amutex_lock(am, 0) != XTC_OK ||
	    xtc_arwlock_wrlock(arw, 0) != XTC_OK ||
	    xtc_rwlock_rdlock(rw, 0) != XTC_OK || xtc_gate_enter(g) != XTC_OK ||
	    xtc_lock_get(lm, l1, "k", 1, XTC_LOCK_X, 0) != XTC_OK)
		return -1;

	bad += xtc_notify_wait(n, 50 * MS) != XTC_E_AGAIN;
	bad += xtc_sem_acquire(s, 1, 50 * MS) != XTC_E_AGAIN;
	bad += xtc_amutex_lock(am, 50 * MS) != XTC_E_AGAIN;
	bad += xtc_arwlock_wrlock(arw, 50 * MS) != XTC_E_AGAIN;
	bad += xtc_rwlock_wrlock(rw, 50 * MS) != XTC_E_AGAIN;
	bad += xtc_gate_drain(g, 50 * MS) != XTC_E_AGAIN;
	bad += xtc_future_wait(fu, &v, 50 * MS) != XTC_E_AGAIN;
	bad += xtc_lock_get(lm, l2, "k", 1, XTC_LOCK_X, 50 * MS) != XTC_E_AGAIN;

	(void)xtc_lock_release_all(lm, l1);
	(void)xtc_lockmgr_id_free(lm, l1);
	(void)xtc_lockmgr_id_free(lm, l2);
	xtc_lockmgr_destroy(lm);
	(void)xtc_promise_set(pr, 0, XTC_OK);
	(void)xtc_future_wait(fu, &v, -1);
	(void)xtc_gate_leave(g);
	xtc_gate_destroy(g);
	(void)xtc_rwlock_unlock(rw);
	xtc_rwlock_destroy(rw);
	(void)xtc_arwlock_unlock(arw);
	xtc_arwlock_destroy(arw);
	(void)xtc_amutex_unlock(am);
	xtc_amutex_destroy(am);
	xtc_sem_destroy(s);
	xtc_notify_destroy(n);
	return bad;
}

/* A plain task the run steps on the simulator's thread, off any fiber. */
static int
waits_task(xtc_task_t *self, void *user)
{
	(void)self;
	(void)user;
	atomic_store(&g_task_bad, timed_waits());
	return XTC_TASK_DONE;
}

/* Case 1: the test's own code between xtc_sim_activate and
 * xtc_sim_deactivate.  Returns 0 when every wait expired at once and
 * nothing was counted. */
static int
between_activate(void)
{
	int64_t t0 = xtc_clock_mono(), ms;
	int bad, nondet;

	/* The virtual clock on, as a run has it: the lock manager reads the
	 * monotonic clock when it makes a locker. */
	xtc_sim_activate(0x0FF100Bu);
	xtc_sim_clock_enable(0);
	bad = timed_waits();
	nondet = xtc_sim_nondeterminism_count();
	xtc_sim_clock_disable();
	xtc_sim_deactivate();
	ms = (xtc_clock_mono() - t0) / MS;
	printf("  between activate: bad=%d nondet=%d real=%lld ms\n", bad,
	    nondet, (long long)ms);
	return bad != 0 || nondet != 0;
}

/* Case 2: a plain task inside xtc_sim_exec_run.  The run must also
 * return XTC_OK, which it refuses after a counted violation. */
static int
in_run(uint64_t seed)
{
	xtc_exec_t *e = NULL;
	int rc, nondet, bad;

	atomic_store(&g_task_bad, -1);
	if (xtc_exec_init(&e, 2) != XTC_OK ||
	    xtc_exec_spawn_on(e, 1, waits_task, NULL, NULL) != XTC_OK)
		return 1;
	rc = xtc_sim_exec_run(e, seed, 100000);
	nondet = xtc_sim_nondeterminism_count();
	(void)xtc_exec_fini(e);
	bad = atomic_load(&g_task_bad);
	printf("  plain task in a run (seed %llu): rc=%d bad=%d nondet=%d\n",
	    (unsigned long long)seed, rc, bad, nondet);
	return rc != XTC_OK || bad != 0 || nondet != 0;
}

int
main(void)
{
	int fails = 0;

	/* Count mode: a wall-clock read is counted, not an abort, so it is
	 * reported here by name. */
	xtc_sim_strict(0);
	fails += between_activate();
	fails += in_run(0x5EED1);
	fails += in_run(0x5EED1);
	fails += in_run(0x5EED2);
	xtc_sim_strict(1);
	if (fails != 0) {
		printf(
		    "FAIL: a timed wait off a fiber did not return at once "
		    "(%d case(s): it read the wall clock or did not expire)\n",
		    fails);
		return 1;
	}
	printf("OK: %d timed waits off a fiber on the simulator's thread "
	       "(between activate and deactivate, and in a plain task of a "
	       "run) expire at once with XTC_E_AGAIN and read no wall clock\n",
	    N_WAITS);
	return 0;
}
