/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/cbmc/inbox_harness.c
 *
 *	(src/evt/loop.c: __inbox_stack_push, __xtc_inbox_push's nudge
 *	elision, __xtc_inbox_sleep_begin / _sleep_end, __xtc_inbox_drain's
 *	detach, and the order __loop_step_once runs them in.)
 *
 *	INVARIANTS, with two producers (each pushes one task onto either
 *	stack) racing the owner's "about to block" decision, over every
 *	interleaving:
 *	  - no lost nudge (the Dekker pair): the owner never blocks with no
 *	    wakeup pending while a task sits in the inbox -- if it decided
 *	    to block, some producer saw `sleeping` and wrote the wakeup, or
 *	    the inbox was empty;
 *	  - no task lost, duplicated or fabricated across the lock-free
 *	    pushes and the drain's exchange (every pushed task is drained
 *	    exactly once by the end);
 *	  - at most one wakeup write per sleep (the exchange elides the
 *	    rest).
 *
 *	WHAT IS MODELLED: the statements between BEGIN/END SOURCE are the
 *	real ones (checked by check_drift.sh); xtc_io_wakeup is a counter,
 *	the blocking poll is "returns iff a wakeup is pending", and tasks
 *	carry only inbox_next.  CBMC explores interleavings under
 *	sequential consistency, so this proves the protocol, not the
 *	memory orders (the seq_cst the Dekker pair needs is what the
 *	source uses; a relaxed variant would NOT be caught here).
 *	Planted bugs, each must FAIL:
 *	  -DPLANT_NO_RECHECK  sleep_begin does not re-read the inbox;
 *	  -DPLANT_NO_SLEEPCHK the producer never nudges (elides always);
 *	  -DPLANT_PLAIN_PUSH  the push is load-then-store, not a CAS (two
 *	                      producers on one stack lose a task).
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

#define XTC_OK 0
_Bool nondet_bool(void);
/*
 * CBMC 5.x refuses atomics on POINTERS between threads ("pointer
 * handling for concurrency is unsound"), so a task is its index (0 =
 * NULL) and inbox_next an index array -- the deque harness's encoding.
 * The stack algorithm is unchanged; the pointer-typed statements are
 * the one place this copy differs from the source, and check_drift.sh
 * compares the rest.
 */
typedef int xtc_task_t;			/* 0 = NULL */
#define NULL_T 0
#define NULL NULL_T			/* the source's NULL tests, verbatim */
static int inbox_next[3];
struct xtc_inbox {
	int pub_head;
	int wake_head;
	int sleeping;
};
enum xtc_inbox_kind { XTC_INB_PUBLISH, XTC_INB_WAKE };
typedef struct { struct xtc_inbox inbox; void *io; } xtc_loop_t;

static xtc_loop_t L;
static int wakeups; /* bytes written to the wakeup fd */
static int pushed;
static int drained_ct[3];

#define XTC_SIM_FAULT_POINT(x)	((void)0)
static int
xtc_io_wakeup(void *io)
{
	(void)io;
	xtc_atomic_fetch_add_i32(&wakeups, 1, xtc_mo_seq_cst);
	return XTC_OK;
}

/* src/evt/loop.c __inbox_stack_push, index-encoded (see above). */
static void
__inbox_stack_push(int *head, xtc_task_t t)
{
	xtc_task_t old = xtc_atomic_load_i32(head, xtc_mo_relaxed);
#ifdef PLANT_PLAIN_PUSH
	inbox_next[t] = old;
	xtc_atomic_store_i32(head, t,
	    xtc_mo_seq_cst); /* load-then-store: not one RMW */
#else
	do {
		inbox_next[t] = old;
	} while (!xtc_atomic_cas_weak_i32(head, &old, t, xtc_mo_seq_cst,
	    xtc_mo_seq_cst));
#endif
}

static int
__xtc_inbox_push(xtc_loop_t *loop, enum xtc_inbox_kind k, xtc_task_t t)
{
	/* BEGIN SOURCE src/evt/loop.c __xtc_inbox_push */
	XTC_SIM_FAULT_POINT("sched.inbox.pre_push");
	__inbox_stack_push(k == XTC_INB_PUBLISH ? &loop->inbox.pub_head :
	    &loop->inbox.wake_head, t);
#ifndef PLANT_NO_SLEEPCHK
	if (xtc_atomic_load_i32(&loop->inbox.sleeping, xtc_mo_seq_cst) != 0 &&
	    xtc_atomic_exchange_i32(&loop->inbox.sleeping, 0, xtc_mo_seq_cst) !=
	        0 &&
	    xtc_io_wakeup(loop->io) != XTC_OK)
		xtc_atomic_store_i32(&loop->inbox.sleeping, 1,
		    xtc_mo_seq_cst); /* next push retries */
	return XTC_OK;
#endif
	/* END SOURCE */
	return XTC_OK;
}

static int
__xtc_inbox_sleep_begin(xtc_loop_t *loop)
{
	/* BEGIN SOURCE src/evt/loop.c __xtc_inbox_sleep_begin */
	xtc_atomic_store_i32(&loop->inbox.sleeping, 1, xtc_mo_seq_cst);
#ifndef PLANT_NO_RECHECK
	if (xtc_atomic_load_ptr(&loop->inbox.pub_head, xtc_mo_seq_cst) !=
	        NULL ||
	    xtc_atomic_load_ptr(&loop->inbox.wake_head, xtc_mo_seq_cst) !=
	        NULL) {
		xtc_atomic_store_i32(&loop->inbox.sleeping, 0, xtc_mo_relaxed);
		return 1;
	}
#endif
	return 0;
	/* END SOURCE */
}

static void
__xtc_inbox_sleep_end(xtc_loop_t *loop)
{
	xtc_atomic_store_i32(&loop->inbox.sleeping, 0, xtc_mo_relaxed);
}

/* The drain's detach (the reverse/re-push of deferred tasks is
 * single-threaded owner work and not modelled). */
static void
drain(xtc_loop_t *loop)
{
	xtc_task_t pub, wake, t;
	int i;

	pub = xtc_atomic_exchange_i32(&loop->inbox.pub_head, NULL_T,
	    xtc_mo_seq_cst);
	wake = xtc_atomic_exchange_i32(&loop->inbox.wake_head, NULL_T,
	    xtc_mo_seq_cst);
	for (i = 0, t = pub; t != NULL_T && i < 3; t = inbox_next[t], i++)
		drained_ct[t]++;
	for (i = 0, t = wake; t != NULL_T && i < 3; t = inbox_next[t], i++)
		drained_ct[t]++;
}

static void
producer(int id)
{
	enum xtc_inbox_kind k = nondet_bool() ? XTC_INB_PUBLISH :
	    XTC_INB_WAKE;
	(void)__xtc_inbox_push(&L, k, id);
	xtc_atomic_fetch_add_i32(&pushed, 1, xtc_mo_seq_cst);
}
static void p1(void) { producer(1); }
static void p2(void) { producer(2); }

int
main(void)
{
	int blocked = 0, wk;

	xtc_atomic_store_i32(&L.inbox.pub_head, NULL_T, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&L.inbox.wake_head, NULL_T, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&L.inbox.sleeping, 0, xtc_mo_seq_cst);

	__CPROVER_ASYNC_1: p1();
	__CPROVER_ASYNC_2: p2();

	/* The owner: one idle turn (timeout != 0) of __loop_step_once. */
	drain(&L);
	if (!__xtc_inbox_sleep_begin(&L)) {
		/* The poll: it can only return if a wakeup byte is pending.
		 * Wait for the producers to finish, then ask whether the
		 * owner would still be asleep with work queued. */
		__CPROVER_assume(
		    xtc_atomic_load_i32(&pushed, xtc_mo_seq_cst) == 2);
		wk = xtc_atomic_load_i32(&wakeups, xtc_mo_seq_cst);
		if (wk == 0 &&
		    (xtc_atomic_load_i32(&L.inbox.pub_head, xtc_mo_seq_cst) !=
		            NULL_T ||
		        xtc_atomic_load_i32(&L.inbox.wake_head,
		            xtc_mo_seq_cst) != NULL_T))
			blocked = 1;
		__CPROVER_assert(wk <= 1,
		    "at most one wakeup write per sleep");
		__xtc_inbox_sleep_end(&L);
	}
	__CPROVER_assert(!blocked,
	    "no lost nudge: never asleep with no wakeup and work queued");

	__CPROVER_assume(xtc_atomic_load_i32(&pushed, xtc_mo_seq_cst) == 2);
	drain(&L);
	__CPROVER_assert(drained_ct[1] == 1 && drained_ct[2] == 1,
	    "every pushed task drained exactly once");
	__CPROVER_assert(drained_ct[0] == 0, "nothing fabricated");
	return 0;
}
