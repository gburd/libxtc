/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/cbmc/wakepark_harness.c
 *	CBMC bounded model check of the loop prepare/park latch
 *	(src/evt/loop.c, the v1.8.0 fix: the XTC_INB_WAKE inbox-drain
 *	branch + the XTC_TASK_PENDING verdict + the wake_pending atomic).
 *
 *	INVARIANT PROVED: a cross-thread wake that arrives while the
 *	target task is RUNNING -- in the window between the task arming
 *	its waker and the loop performing the RUNNING->PARKED transition
 *	-- is NOT lost.  The drain, finding the task not-yet-PARKED,
 *	latches wake_pending; the RUNNING->PARKED transition atomically
 *	consumes the latch and RE-SCHEDULES instead of parking.  The task
 *	is therefore never left PARKED after a wake it should have seen --
 *	it always ends SCHEDULED (guaranteed to run again).  Without the
 *	latch (the pre-v1.8.0 code) the wake is silently dropped and the
 *	task hangs.
 *
 *	WHAT IS MODELLED: the loop's per-step drain + verdict, transcribed
 *	faithfully from loop.c.  A wake's ONLY cross-thread step is the
 *	inbox enqueue (an atomic flag); the loop thread then, in a single
 *	step, drains the inbox (XTC_INB_WAKE: if the task is PARKED
 *	enqueue it, else latch wake_pending) and -- since the task has
 *	returned XTC_TASK_PENDING and wants to park -- runs the verdict
 *	handler (atomic_exchange the latch: re-schedule if set, else
 *	park).  The drain and verdict both execute ON THE LOOP THREAD in
 *	program order (loop.c drains the inbox, then acts on the task's
 *	verdict), so they are modelled as one loop step (state and
 *	wake_pending are loop-thread-private; only the inbox flag is
 *	shared with the waker); the enqueue races that step.  This is exactly the window the v1.8.0
 *	latch closes.  The real loop.c is not includable standalone (the
 *	whole executor, deque, I/O poller); the lost-wake safety lives
 *	entirely in this drain/latch/verdict choreography.  If loop.c's
 *	drain or verdict handling drifts, this must be updated in
 *	lockstep.
 *
 *	BOUND: one cross-thread wake enqueued at any point relative to one
 *	park step.  CBMC explores every interleaving of the enqueue with
 *	the step.
 *
 *	Run: cbmc wakepark_harness.c --unwind 6
 */

#include "xtc_atomic.h"

enum { TS_RUNNING = 0, TS_PARKED = 1, TS_SCHEDULED = 2 };

static int state;          /* the task's run state */
static int wake_pending;   /* the v1.8.0 latch */
static int inbox_wake;     /* a WAKE message sits in the inbox */
static int wake_delivered; /* test bookkeeping: a wake was enqueued */

/* Cross-thread waker: the ONLY cross-thread step is enqueuing a WAKE
 * message into the loop's inbox (under inbox.lock in loop.c). */
static void
waker(void)
{
	/* Enqueue the WAKE, THEN mark delivered -- with release ordering,
	 * so an observer that sees wake_delivered == 1 is guaranteed the
	 * inbox store already happened (no "delivered but not enqueued"
	 * artifact; the real enqueue is a single locked inbox push). */
	xtc_atomic_store_i32(&inbox_wake, 1, xtc_mo_release);
	xtc_atomic_store_i32(&wake_delivered, 1, xtc_mo_release);
}

/* The loop's park step, on the loop thread: drain the inbox, then act
 * on the task's PENDING verdict, both in program order.  The ONLY
 * shared state the concurrent waker touches is inbox_wake; state and
 * wake_pending are loop-thread-private, so a single atomic load of
 * inbox_wake at the top is the whole interaction with the waker -- no
 * atomic section needed.  The waker's enqueue races this load: it may
 * land before it (drained + latched here) or after the whole step (the
 * wake stays in the inbox for a later step -- not lost). */
static void
park_step(void)
{
	int s, woke;

	/* --- inbox drain (XTC_INB_WAKE) --- */
	woke = xtc_atomic_exchange_i32(&inbox_wake, 0, xtc_mo_acquire);
	if (woke) {
		s = xtc_atomic_load_i32(&state, xtc_mo_relaxed);
		if (s == TS_PARKED)
			xtc_atomic_store_i32(&state, TS_SCHEDULED,
			    xtc_mo_relaxed);
		else
			/* still RUNNING (the prepare/park window): latch it. */
			xtc_atomic_store_i32(&wake_pending, 1, xtc_mo_relaxed);
	}
	/* --- PENDING verdict: the task wants to park; consume the latch --- */
	if (xtc_atomic_exchange_i32(&wake_pending, 0, xtc_mo_relaxed))
		xtc_atomic_store_i32(&state, TS_SCHEDULED, xtc_mo_relaxed);
	else
		xtc_atomic_store_i32(&state, TS_PARKED, xtc_mo_relaxed);
}

int
main(void)
{
	int final_state;

	/* The task is RUNNING (armed its waker, about to return PENDING). */
	xtc_atomic_store_i32(&state, TS_RUNNING, xtc_mo_relaxed);
	xtc_atomic_store_i32(&wake_pending, 0, xtc_mo_relaxed);
	xtc_atomic_store_i32(&inbox_wake, 0, xtc_mo_relaxed);
	xtc_atomic_store_i32(&wake_delivered, 0, xtc_mo_relaxed);

	/* A cross-thread wake races the park step. */
	__CPROVER_ASYNC_1: waker();
	park_step();

	/* Lost-wake safety: if the wake was delivered AND the park step
	 * already drained it (inbox now empty), the task must NOT be left
	 * PARKED -- the latch must have re-scheduled it.  (If the wake is
	 * still in the inbox, a later step delivers it; that is not a lost
	 * wake, so we only assert once the wake has been drained.) */
	final_state = xtc_atomic_load_i32(&state, xtc_mo_acquire);
	if (xtc_atomic_load_i32(&wake_delivered, xtc_mo_acquire) &&
	    xtc_atomic_load_i32(&inbox_wake, xtc_mo_acquire) == 0) {
		__CPROVER_assert(final_state == TS_SCHEDULED,
		    "a drained wake re-schedules the task, never lost "
		    "(the RUNNING->PARKED latch)");
	}
	return 0;
}
