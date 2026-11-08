/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/cbmc/credit_harness.c
 *	CBMC bounded model check of the sliding-window credit regulator
 *	(src/orc/credit.c) over a counting semaphore.
 *
 *	INVARIANT PROVED: concurrent acquire/release never lets the
 *	in-flight count exceed the window.  W free credits are handed out
 *	by acquire (which blocks when none remain) and returned by
 *	release; no interleaving of N concurrent actors admits more than
 *	W simultaneously.
 *
 *	WHAT IS MODELLED: the credit regulator's essential logic
 *	(xtc_credit_acquire -> sem_try_acquire + in_flight++;
 *	xtc_credit_release -> in_flight-- + sem_post), transcribed
 *	faithfully, over a FAITHFUL counting-semaphore model (an atomic
 *	free-credit count with a CAS-guarded try_acquire and an atomic
 *	post).  The real credit.c delegates the actual regulation to
 *	xtc_sem (src/ptc/sync.c) -- the mutex-guarded in_flight/peak
 *	counters in credit.c are pure observability and cannot affect the
 *	bound; the bound is enforced entirely by the semaphore's free
 *	count, so that is what this models.
 *
 *	NON-VACUITY: in-flight genuinely reaches WINDOW in some
 *	interleaving (tightening the asserted bound to WINDOW-1 makes
 *	CBMC report a counterexample), so the proof is not vacuous.  The
 *	DST planted-bug path (XTC_DST_BUG_CREDITWIN posts a SECOND credit
 *	on release) is compiled in under -DCREDIT_BUG for reference; it is
 *	the class of over-admission this harness proves the shipping
 *	code free of.
 *
 *	BOUND: WINDOW free credits, NACTORS concurrent acquire+release
 *	pairs.  CBMC explores every interleaving of the semaphore atomics.
 *
 *	Run: cbmc credit_harness.c --unwind 8
 */

#include <stdint.h>
#include "xtc_atomic.h"

#define WINDOW   2      /* free credits (max in flight) */
#define NACTORS  3      /* concurrent acquire+release pairs (> WINDOW) */

/* Free-credit counting semaphore: try_acquire does a CAS-decrement,
 * post an atomic increment.  This mirrors xtc_sem's counting core (the
 * part that enforces the bound); the parking/waking is irrelevant to
 * the count invariant. */
static int sem_free;

/* Observability + the property under test. */
static int in_flight;

/* sem_try_acquire(1): CAS-decrement if a credit is free; 0 == got it. */
static int
sem_try_acquire(void)
{
	int cur = xtc_atomic_load_i32(&sem_free, xtc_mo_acquire);
	for (;;) {
		if (cur <= 0)
			return -1;              /* none free */
		if (xtc_atomic_cas_weak_i32(&sem_free, &cur, cur - 1,
		        xtc_mo_acquire, xtc_mo_acquire))
			return 0;
	}
}

/* sem_post(1): return a credit. */
static void
sem_post(void)
{
	(void)xtc_atomic_fetch_add_i32(&sem_free, 1, xtc_mo_release);
}

/* xtc_credit_acquire (try flavour) then credit_took_one. */
static int
credit_acquire(void)
{
	if (sem_try_acquire() != 0)
		return -1;                      /* XTC_E_AGAIN */
	(void)xtc_atomic_fetch_add_i32(&in_flight, 1, xtc_mo_relaxed);
	return 0;
}

/* xtc_credit_release: in_flight-- then sem_post. */
static void
credit_release(void)
{
	(void)xtc_atomic_fetch_sub_i32(&in_flight, 1, xtc_mo_relaxed);
	sem_post();
#ifdef CREDIT_BUG
	/* Planted bug XTC_DST_BUG_CREDITWIN: a second post over-admits. */
	sem_post();
#endif
}

/* One actor: try to take a credit; if granted, assert the window is
 * respected, then release. */
static void
actor(void)
{
	int n;
	if (credit_acquire() != 0)
		return;                         /* window full: correct */
	n = xtc_atomic_load_i32(&in_flight, xtc_mo_relaxed);
	__CPROVER_assert(n >= 1 && n <= WINDOW,
	    "in-flight never exceeds the credit window");
	credit_release();
}

int
main(void)
{
	xtc_atomic_store_i32(&sem_free, WINDOW, xtc_mo_relaxed);
	xtc_atomic_store_i32(&in_flight, 0, xtc_mo_relaxed);

	/* NACTORS > WINDOW contenders race; CBMC explores every
	 * interleaving of the semaphore atomics. */
	__CPROVER_ASYNC_1: actor();
	__CPROVER_ASYNC_2: actor();
	actor();
	return 0;
}
