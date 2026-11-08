/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/cbmc/parktimer_harness.c
 *
 *	(src/evt/loop.c __xtc_drain_due_timers, the waiter branch;
 *	src/evt/task.c __xtc_task_cancel_park_timer; src/evt/timer.c
 *	__xtc_timer_park_release, __xtc_timer_try_cancel.)
 *
 *	A park timer has two references: the heap's and the task's
 *	park_timer slot.  Loop A pops timer T1 as due and fires it while
 *	the fiber -- woken, migrated to loop B -- cancels T1 and arms T2 on
 *	B's heap, then later cancels T2.  The 2.0 bug (KNOWN_ISSUES "a
 *	fired park timer could release a newer timer of a migrated fiber")
 *	was the drain's unconditional slot clear: it dropped T2's slot
 *	reference.
 *
 *	INVARIANTS, over every interleaving:
 *	  - no over-release: a timer's refs never go below zero (a release
 *	    with no reference left is a double free of the node);
 *	  - no leak: when both sides are done, every timer's refs are 0
 *	    except references still legitimately held (T2's heap
 *	    reference on B, which B's own pop or sweep drops);
 *	  - the slot is NULL at the end (no dangling park_timer);
 *	  - loop A releases only references of the timer it popped (T1),
 *	    never one of B's timer (the 2.0 TSan report: A read T2's refs
 *	    with nothing ordering that read after T2's arm on B);
 *	  - no orphan: T2, if still in B's heap, was cancelled or fired --
 *	    a live timer whose slot reference is gone has no owner left to
 *	    cancel it and wakes the fiber spuriously later.
 *
 *	WHAT IS MODELLED: the statements between BEGIN/END SOURCE are the
 *	real ones (check_drift.sh); a timer is an index into an array
 *	(CBMC 5.x rejects atomics on pointers across threads), so the
 *	pointer-typed lines are index-encoded and outside the markers.
 *	CBMC is sequentially consistent here: the relaxed orders are not
 *	what is checked.  -DPLANT_UNCOND_CLEAR restores the 2.0 bug (an
 *	exchange instead of the CAS) and must FAIL.
 */
#include "xtc_atomic.h"
/* The modelled pointers are int indexes (CBMC 5.x rejects pointers shared
 * between threads), so the source's _ptr operations are the _i32 ones
 * here; the transcribed blocks keep the source's spelling. */
#undef xtc_atomic_load_ptr
#undef xtc_atomic_store_ptr
#undef xtc_atomic_exchange_ptr
#undef xtc_atomic_cas_ptr
#undef xtc_atomic_cas_weak_ptr
#define xtc_atomic_load_ptr xtc_atomic_load_i32
#define xtc_atomic_store_ptr xtc_atomic_store_i32
#define xtc_atomic_exchange_ptr xtc_atomic_exchange_i32
#define xtc_atomic_cas_ptr xtc_atomic_cas_i32
#define xtc_atomic_cas_weak_ptr xtc_atomic_cas_weak_i32

#define NONE	0
#define T1	1
#define T2	2
#define XTC_TIMER_CANCELLED 2

static int park_timer; /* the task's slot: a timer index */
static int refs[3];
static int fired[3];
static int over_release;
static int a_touched_t2; /* loop A released a ref of T2 */
static int done;
static int b_heap_dropped;
_Bool nondet_bool(void);

/* src/evt/timer.c __xtc_timer_park_release, index-encoded: the free is
 * "refs reached 0"; a release finding refs already 0 is the over-
 * release the real code's defensive check would mask. */
static void
park_release(int t)
{
	if (t == NONE)
		return;
	if (xtc_atomic_load_i32(&refs[t], xtc_mo_relaxed) == 0) {
		xtc_atomic_store_i32(&over_release, 1, xtc_mo_seq_cst);
		return;
	}
	(void)xtc_atomic_fetch_sub_i32(&refs[t], 1, xtc_mo_acq_rel);
}

/* src/evt/timer.c __xtc_timer_try_cancel: the fired CAS (the heap
 * removal is owner-only and returns the heap ref only on the owner
 * thread; the fiber here is on B, T1 is in A's heap: not removed). */
static int
try_cancel(int t)
{
	int expect = 0;
	return xtc_atomic_cas_i32(&fired[t], &expect, XTC_TIMER_CANCELLED,
	    xtc_mo_acq_rel, xtc_mo_acquire);
}

/* The drain's names for the drift-checked block (index encoding). */
static struct {
	int park_timer;
} *const waiter = (void *)&park_timer;
#define due				T1
#define NULL				NONE
#define __xtc_timer_park_release	park_release

/* Loop A: T1 was popped as due (A now owns T1's heap reference). */
static void
loop_a_fire_t1(void)
{
	int slot;
	int expect = 0;

	/* due->fired = 1 is a plain store in loop.c, after the !cancelled
	 * test; the cancelled-after-pop path drops the heap ref only. */
	if (xtc_atomic_cas_i32(&fired[T1], &expect, 1, xtc_mo_seq_cst,
	        xtc_mo_seq_cst)) {
		/* (xtc_waker_wake runs here) */
#ifdef PLANT_UNCOND_CLEAR
		slot =
		    xtc_atomic_exchange_i32(&park_timer, NONE, xtc_mo_seq_cst);
		if (slot == T2)
			xtc_atomic_store_i32(&a_touched_t2, 1, xtc_mo_seq_cst);
		park_release(slot);
#else
		/* BEGIN SOURCE src/evt/loop.c __xtc_drain_due_timers slot claim */
				slot = due;
		                if (xtc_atomic_cas_ptr(&waiter->park_timer,
		                        &slot, NULL, xtc_mo_relaxed,
		                        xtc_mo_relaxed))
			                __xtc_timer_park_release(due);
		/* END SOURCE */
#endif
	}
	park_release(T1);	/* the heap reference of the popped timer */
	xtc_atomic_fetch_add_i32(&done, 1, xtc_mo_seq_cst);
}
#undef due
#undef NULL
#undef __xtc_timer_park_release

/* src/evt/task.c __xtc_task_cancel_park_timer, index-encoded. */
static void
cancel_park_timer(void)
{
	int pt = xtc_atomic_exchange_i32(&park_timer, NONE, xtc_mo_relaxed);
	if (pt != NONE) {
		(void)try_cancel(pt);
		park_release(pt);
	}
}

/* The fiber, woken another way, migrated to loop B. */
static void
fiber(void)
{
	cancel_park_timer();			/* drops T1's slot ref */
	/* __xtc_task_park_on_timer_at on B: refs = 2, push, publish. */
	xtc_atomic_store_i32(&refs[T2], 2, xtc_mo_relaxed);
	xtc_atomic_store_i32(&park_timer, T2, xtc_mo_relaxed);
	/* Parked on T2.  Either B fires T2 (its drain, same CAS shape as
	 * A's) or something else wakes the fiber and it cancels T2. */
	if (nondet_bool()) {
		int slot = T2, expect = 0;
		if (xtc_atomic_cas_i32(&fired[T2], &expect, 1, xtc_mo_seq_cst,
		        xtc_mo_seq_cst)) {
			if (xtc_atomic_cas_i32(&park_timer, &slot, NONE,
			        xtc_mo_seq_cst, xtc_mo_seq_cst))
				park_release(T2);
		}
		park_release(T2);	/* B's heap reference */
		b_heap_dropped = 1;
	}
	cancel_park_timer();		/* the fiber's post-park cleanup */
	xtc_atomic_fetch_add_i32(&done, 1, xtc_mo_seq_cst);
}

int
main(void)
{
	xtc_atomic_store_i32(&refs[T1], 2, xtc_mo_seq_cst); /* heap + slot */
	xtc_atomic_store_i32(&park_timer, T1, xtc_mo_seq_cst);

	__CPROVER_ASYNC_1: loop_a_fire_t1();
	fiber();

	__CPROVER_assume(xtc_atomic_load_i32(&done, xtc_mo_seq_cst) == 2);
	__CPROVER_assert(!xtc_atomic_load_i32(&over_release, xtc_mo_seq_cst),
	    "no timer reference is released twice");
	__CPROVER_assert(xtc_atomic_load_i32(&refs[T1], xtc_mo_seq_cst) == 0,
	    "T1 is freed exactly once (no leak)");
	__CPROVER_assert(xtc_atomic_load_i32(&refs[T2], xtc_mo_seq_cst) ==
	        (b_heap_dropped ? 0 : 1),
	    "T2 keeps exactly the references still owed (B's heap ref "
	    "until B pops or sweeps it)");
	__CPROVER_assert(xtc_atomic_load_i32(&park_timer, xtc_mo_seq_cst) ==
	        NONE,
	    "the slot is clear");
	__CPROVER_assert(!xtc_atomic_load_i32(&a_touched_t2, xtc_mo_seq_cst),
	    "loop A never releases a reference of B's timer");
	__CPROVER_assert(b_heap_dropped ||
	        xtc_atomic_load_i32(&fired[T2], xtc_mo_seq_cst) != 0,
	    "no orphan: T2 left in B's heap is cancelled or fired");
	return 0;
}
