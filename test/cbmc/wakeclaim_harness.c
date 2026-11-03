/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/cbmc/wakeclaim_harness.c
 *
 *	(src/evt/task.c __task_wake_claim; src/evt/loop.c, the
 *	XTC_TASK_PENDING verdict of __xtc_loop_step -- the 7th and 9th
 *	"surface" fixes.)
 *
 *	INVARIANTS, for a fiber that has decided to park and TWO wakers
 *	racing its verdict (a cross-loop waker and a same-loop or second
 *	cross-loop waker), over every interleaving:
 *	  - no lost wake: if any waker ran, the task ends SCHEDULED or was
 *	    enqueued (never PARKED with every wake consumed);
 *	  - exactly-once enqueue: the task is enqueued at most once (the
 *	    inbox slot and the run queue link are single-owner).
 *
 *	WHAT IS MODELLED: __task_wake_claim and the PENDING verdict,
 *	transcribed from the sources above; enqueue is a counter.  The
 *	statements between the "BEGIN SOURCE" / "END SOURCE" markers are
 *	checked against the real files by test/cbmc/check_drift.sh, so a
 *	change to either function fails that gate until this copy follows.
 *
 *	Note: CBMC explores interleavings under sequential consistency;
 *	the memory orders below are carried for the drift check but are
 *	NOT what is verified.  The property here is the protocol logic.
 *	Planted bugs, each must make CBMC report the lost wake:
 *	  -DPLANT_NO_RECHECK  drop the 9th-surface re-check after PARKED;
 *	  -DPLANT_NO_LATCH    a waker that loses the CAS just gives up (the
 *	                      7th surface, before the wake_pending latch).
 */
#include <stdatomic.h>

enum { XTC_TS_SCHEDULED = 0, XTC_TS_RUNNING = 1, XTC_TS_PARKED = 2 };

typedef struct {
	_Atomic int state;
	_Atomic int wake_pending;
} xtc_task_t;

static xtc_task_t task;
static xtc_task_t *t = &task;
static _Atomic int enqueued;
static _Atomic int wakers_done;
static void *loop;
/* The verdict's enqueue, so the transcribed block stays verbatim. */
#define __xtc_loop_enqueue(l, tk) ((void)(l), atomic_fetch_add(&enqueued, 1))

/* BEGIN SOURCE src/evt/task.c __task_wake_claim */
static int
__task_wake_claim(xtc_task_t *t)
{
	int expect = XTC_TS_PARKED;
	if (atomic_compare_exchange_strong(&t->state, &expect,
	    XTC_TS_SCHEDULED))
		return 1;
#ifdef PLANT_NO_LATCH
	return 0;
#endif
	atomic_store(&t->wake_pending, 1);
	if (atomic_load(&t->state) != XTC_TS_PARKED ||
	    !atomic_exchange(&t->wake_pending, 0))
		return 0;
	expect = XTC_TS_PARKED;
	return atomic_compare_exchange_strong(&t->state, &expect,
	    XTC_TS_SCHEDULED);
}
/* END SOURCE */

static void
waker(void)
{
	if (__task_wake_claim(t))
		atomic_fetch_add(&enqueued, 1);
	atomic_fetch_add(&wakers_done, 1);
}

static void
verdict_pending(void)
{
	/* BEGIN SOURCE src/evt/loop.c XTC_TASK_PENDING */
			if (atomic_exchange_explicit(&t->wake_pending, 0,
			    memory_order_acquire)) {
				atomic_store_explicit(&t->state, XTC_TS_SCHEDULED,
				    memory_order_release);
				(void)__xtc_loop_enqueue(loop, t);
			} else {
				atomic_store(&t->state, XTC_TS_PARKED);
#ifndef PLANT_NO_RECHECK
				if (atomic_exchange(&t->wake_pending, 0)) {
					int exp = XTC_TS_PARKED;
					if (atomic_compare_exchange_strong_explicit(
					    &t->state, &exp, XTC_TS_SCHEDULED,
					    memory_order_acq_rel,
					    memory_order_acquire))
						(void)__xtc_loop_enqueue(loop, t);
				}
#endif
			}
	/* END SOURCE */
}

int
main(void)
{
	atomic_store(&t->state, XTC_TS_RUNNING);
	atomic_store(&t->wake_pending, 0);

	__CPROVER_ASYNC_1: waker();
	__CPROVER_ASYNC_2: waker();
	verdict_pending();

	__CPROVER_assume(atomic_load(&wakers_done) == 2);
	__CPROVER_assert(atomic_load(&enqueued) <= 1,
	    "exactly-once: the task is enqueued at most once");
	__CPROVER_assert(atomic_load(&enqueued) == 1 ||
	    atomic_load(&t->state) == XTC_TS_SCHEDULED,
	    "no lost wake: a woken task is never left PARKED");
	return 0;
}
