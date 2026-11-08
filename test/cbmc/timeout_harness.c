/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/cbmc/timeout_harness.c
 *
 *	(src/ptc/future.c xtc_future_with_timeout: timeout_src_complete,
 *	timeout_fire, timeout_retire; src/evt/timer.c
 *	__xtc_timer_heap_pop_due's fire claim and __xtc_timer_try_cancel.)
 *
 *	The context of a with_timeout future has two owners -- the source
 *	future's completion callback and the timer's callback -- counted in
 *	`remaining` (2), and is freed by whichever retires last.  A source
 *	that wins also cancels the timer; if that cancel stops the fire the
 *	timer callback will never run, so the source retires for it too.
 *	1.55.1 fixed the leak when it did not.
 *
 *	INVARIANTS, the source completing on one thread while the loop pops
 *	the timer as due on another, over every interleaving:
 *	  - the promise is set exactly once (first of source / timeout);
 *	  - the context is freed exactly once, and only after neither
 *	    callback will touch it again (no use after free, no leak);
 *	  - each of the two owners retires exactly once (`remaining` ends
 *	    at 0: an extra retire after the free is a use after free);
 *	  - the timer callback runs iff the timer's fire claim won.
 *
 *	WHAT IS MODELLED: the statements between BEGIN/END SOURCE are the
 *	real ones (check_drift.sh); the timer is one `fired` word claimed
 *	by CAS (0 -> FIRED by the loop's pop, 0 -> CANCELLED by a cancel).
 *	CBMC is sequentially consistent here; memory orders are carried for
 *	the drift check, not verified.  Planted bugs, each must FAIL:
 *	  -DPLANT_NO_CANCEL_RETIRE  the pre-1.55.1 source path: cancel the
 *	                            timer but retire nothing for the timer
 *	                            callback that will now never run (leak);
 *	  -DPLANT_RETIRE_ALWAYS     retire for the timer even when the
 *	                            cancel lost to the fire (a double free).
 */
#include "xtc_atomic.h"
#include <stddef.h>
#include <stdint.h>

#define XTC_OK			0
#define XTC_E_AGAIN		(-5)
#define XTC_TIMER_FIRED		1
#define XTC_TIMER_CANCELLED	2

struct timeout_ctx {
	void          *prom;
	void          *timer;
	int done;
	int remaining;
};

static struct timeout_ctx ctx;
static int the_timer;			/* stands for the xtc_timer_t */
static int fired;                       /* the timer's claim word */
static int promise_sets;
static int frees;
static int timer_cb_ran;
static int done_ct;

static void
__os_free(void *p)
{
	(void)p;
	xtc_atomic_fetch_add_i32(&frees, 1, xtc_mo_seq_cst);
}
static int
xtc_timer_release(void *t)
{
	(void)t;
	return XTC_OK;
}
static int
xtc_promise_set(void *p, intptr_t v, int status)
{
	(void)p; (void)v; (void)status;
	/* A use after free of the context would reach here with it freed. */
	__CPROVER_assert(xtc_atomic_load_i32(&frees, xtc_mo_seq_cst) == 0,
	    "the promise is set while the context is live");
	xtc_atomic_fetch_add_i32(&promise_sets, 1, xtc_mo_seq_cst);
	return XTC_OK;
}
/* timer.c __xtc_timer_try_cancel: the fired CAS (the heap removal it
 * may also do does not bear on the context). */
static int
__xtc_timer_try_cancel(void *timer)
{
	int expect = 0;
	(void)timer;
	return xtc_atomic_cas_i32(&fired, &expect, XTC_TIMER_CANCELLED,
	    xtc_mo_acq_rel, xtc_mo_acquire);
}

static void
timeout_retire(struct timeout_ctx *t)
{
	/* BEGIN SOURCE src/ptc/future.c timeout_retire */
	if (xtc_atomic_fetch_sub_i32(&t->remaining, 1, xtc_mo_acq_rel) == 1) {
		if (t->timer != NULL)
			(void)xtc_timer_release(t->timer);
		__os_free(t);
	}
	/* END SOURCE */
}

static void
timeout_src_complete(intptr_t value, int status, void *u)
{
	/* BEGIN SOURCE src/ptc/future.c timeout_src_complete */
	struct timeout_ctx *t = u;
	int expected = 0;
	if (xtc_atomic_cas_i32(&t->done, &expected, 1, xtc_mo_acq_rel,
	        xtc_mo_relaxed)) {
		(void)xtc_promise_set(t->prom, value, status);
#if defined(PLANT_NO_CANCEL_RETIRE)
		/* Before 1.55.1: cancel the timer, retire nothing for it. */
		if (t->timer != NULL)
			(void)__xtc_timer_try_cancel(t->timer);
#elif defined(PLANT_RETIRE_ALWAYS)
		/* Retire for the timer whether or not the cancel stopped it:
		 * if the fire won, its callback retires too. */
		if (t->timer != NULL) {
			(void)__xtc_timer_try_cancel(t->timer);
			timeout_retire(t);
		}
#else
		if (t->timer != NULL && __xtc_timer_try_cancel(t->timer))
			timeout_retire(t);
#endif
	}
	timeout_retire(t);
	/* END SOURCE */
}

static void
timeout_fire(void *u)
{
	/* BEGIN SOURCE src/ptc/future.c timeout_fire */
	struct timeout_ctx *t = u;
	int expected = 0;
	if (xtc_atomic_cas_i32(&t->done, &expected, 1, xtc_mo_acq_rel,
	        xtc_mo_relaxed))
		(void)xtc_promise_set(t->prom, 0, XTC_E_AGAIN);
	timeout_retire(t);
	/* END SOURCE */
}

/* The source future completes (on whatever thread resolves it). */
static void
source(void)
{
	timeout_src_complete(42, XTC_OK, &ctx);
	xtc_atomic_fetch_add_i32(&done_ct, 1, xtc_mo_seq_cst);
}

/* The loop pops the timer as due: claim the fire, run the callback. */
static void
loop(void)
{
	int expect = 0;
	if (xtc_atomic_cas_i32(&fired, &expect, XTC_TIMER_FIRED, xtc_mo_acq_rel,
	        xtc_mo_acquire)) {
		xtc_atomic_store_i32(&timer_cb_ran, 1, xtc_mo_seq_cst);
		timeout_fire(&ctx);
	}
	xtc_atomic_fetch_add_i32(&done_ct, 1, xtc_mo_seq_cst);
}

int
main(void)
{
	ctx.prom = &ctx;
	ctx.timer = &the_timer;
	xtc_atomic_store_i32(&ctx.done, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&ctx.remaining, 2,
	    xtc_mo_seq_cst); /* source cb + timer cb */

	__CPROVER_ASYNC_1: loop();
	source();

	__CPROVER_assume(xtc_atomic_load_i32(&done_ct, xtc_mo_seq_cst) == 2);
	__CPROVER_assert(xtc_atomic_load_i32(&promise_sets, xtc_mo_seq_cst) ==
	        1,
	    "the promise is set exactly once");
	__CPROVER_assert(xtc_atomic_load_i32(&frees, xtc_mo_seq_cst) == 1,
	    "the context is freed exactly once (no leak, no double free)");
	__CPROVER_assert(xtc_atomic_load_i32(&ctx.remaining, xtc_mo_seq_cst) ==
	        0,
	    "every owner retired exactly once (no over-retire)");
	__CPROVER_assert(xtc_atomic_load_i32(&timer_cb_ran, xtc_mo_seq_cst) ==
	        (xtc_atomic_load_i32(&fired, xtc_mo_seq_cst) ==
	            XTC_TIMER_FIRED),
	    "the timer callback runs iff its fire claim won");
	return 0;
}
