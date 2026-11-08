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
#include "xtc_atomic.h"

enum { XTC_TS_SCHEDULED = 0, XTC_TS_RUNNING = 1, XTC_TS_PARKED = 2 };

typedef struct {
	int state;
	int wake_pending;
} xtc_task_t;

static xtc_task_t task;
static xtc_task_t *t = &task;
static int enqueued;
static int wakers_done;
static void *loop;
/* The verdict's enqueue, so the transcribed block stays verbatim. */
#define __xtc_loop_enqueue(l, tk)                                              \
	((void)(l), xtc_atomic_fetch_add_i32(&enqueued, 1, xtc_mo_seq_cst))

/* BEGIN SOURCE src/evt/task.c __task_wake_claim */
static int
__task_wake_claim(xtc_task_t *t)
{
	int expect = XTC_TS_PARKED;
	if (xtc_atomic_cas_i32(&t->state, &expect, XTC_TS_SCHEDULED,
	        xtc_mo_seq_cst, xtc_mo_seq_cst))
		return 1;
#ifdef PLANT_NO_LATCH
	return 0;
#endif
	xtc_atomic_store_i32(&t->wake_pending, 1, xtc_mo_seq_cst);
	if (xtc_atomic_load_i32(&t->state, xtc_mo_seq_cst) != XTC_TS_PARKED ||
	    !xtc_atomic_exchange_i32(&t->wake_pending, 0, xtc_mo_seq_cst))
		return 0;
	expect = XTC_TS_PARKED;
	return xtc_atomic_cas_i32(&t->state, &expect, XTC_TS_SCHEDULED,
	    xtc_mo_seq_cst, xtc_mo_seq_cst);
}
/* END SOURCE */

static void
waker(void)
{
	if (__task_wake_claim(t))
		xtc_atomic_fetch_add_i32(&enqueued, 1, xtc_mo_seq_cst);
	xtc_atomic_fetch_add_i32(&wakers_done, 1, xtc_mo_seq_cst);
}

static void
verdict_pending(void)
{
	/* BEGIN SOURCE src/evt/loop.c XTC_TASK_PENDING */
	if (xtc_atomic_exchange_i32(&t->wake_pending, 0,
	        xtc_mo_acquire)) {
		xtc_atomic_store_i32(&t->state,
		    XTC_TS_SCHEDULED, xtc_mo_release);
		(void)__xtc_loop_enqueue(loop, t);
	} else {
		/* seq_cst store + the seq_cst exchange below pair
		 * with __task_wake_claim's latch-then-load-state
		 * (src/evt/task.c): of this re-check and a waker's
		 * re-check, at least one sees the other's write. */
		xtc_atomic_store_i32(&t->state, XTC_TS_PARKED,
		    xtc_mo_seq_cst);
#ifndef PLANT_NO_RECHECK
		if (xtc_atomic_exchange_i32(&t->wake_pending, 0,
		        xtc_mo_seq_cst)) {
			int exp = XTC_TS_PARKED;
			if (xtc_atomic_cas_i32(&t->state, &exp,
			        XTC_TS_SCHEDULED,
			        xtc_mo_acq_rel, xtc_mo_acquire))
				(void)__xtc_loop_enqueue(loop, t);
		}
#endif
			}
	/* END SOURCE */
}

int
main(void)
{
	xtc_atomic_store_i32(&t->state, XTC_TS_RUNNING, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&t->wake_pending, 0, xtc_mo_seq_cst);

	__CPROVER_ASYNC_1: waker();
	__CPROVER_ASYNC_2: waker();
	verdict_pending();

	__CPROVER_assume(
	    xtc_atomic_load_i32(&wakers_done, xtc_mo_seq_cst) == 2);
	__CPROVER_assert(xtc_atomic_load_i32(&enqueued, xtc_mo_seq_cst) <= 1,
	    "exactly-once: the task is enqueued at most once");
	__CPROVER_assert(xtc_atomic_load_i32(&enqueued, xtc_mo_seq_cst) == 1 ||
	        xtc_atomic_load_i32(&t->state, xtc_mo_seq_cst) ==
	            XTC_TS_SCHEDULED,
	    "no lost wake: a woken task is never left PARKED");
	return 0;
}
