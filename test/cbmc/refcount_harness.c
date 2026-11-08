/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/cbmc/refcount_harness.c
 *	CBMC bounded model check of the proc teardown refcount
 *	(src/ptc/proc.c: _Atomic refs + __proc_ref/__proc_release +
 *	__table_lookup taking a ref under the stripe lock, atomic with
 *	__table_release detaching the slot).
 *
 *	INVARIANT PROVED: a resolver (__table_lookup) that takes a
 *	reference WHILE HOLDING the owning stripe lock either pins a LIVE
 *	object or sees "gone" (NULL) -- never a freed pointer, never a
 *	use-after-free.  The object is freed exactly when its refcount
 *	reaches zero, and the teardown path detaches the slot (under the
 *	SAME stripe lock) BEFORE dropping the owner's reference, so the
 *	resolver's lookup+ref and the teardown's detach+release are
 *	serialised on the stripe lock: the resolver observes either the
 *	still-attached live proc (and pins it) or the already-detached
 *	NULL slot.
 *
 *	WHAT IS MODELLED: the refcount + stripe-locked slot protocol,
 *	transcribed faithfully from proc.c --
 *	  __table_lookup: under the stripe lock, if slot != NULL, take a
 *	                  ref (fetch_add) and return the pinned proc.
 *	  __table_release: under the stripe lock, NULL the slot.
 *	  __proc_release: fetch_sub; free when the count was 1 (last ref).
 *	  teardown: __table_release (detach) THEN __proc_release (drop the
 *	            owner ref) -- the ordering that makes the pin safe.
 *	The real struct xtc_proc is not includable standalone (loop,
 *	waker, mailbox, DST hooks); the refcount safety lives entirely in
 *	the stripe-locked lookup/detach + the fetch_add / fetch_sub-to-
 *	zero free, which is what this models.  "Freed" is a flag the
 *	resolver must never see set on a proc it holds a reference to.  If
 *	proc.c's lookup/release ordering drifts, this must be updated in
 *	lockstep.
 *
 *	BOUND: one resolver racing one teardown on the same slot.  CBMC
 *	explores every interleaving of the stripe-locked sections and the
 *	refcount atomics.
 *
 *	Run: cbmc refcount_harness.c --unwind 6
 */

#include <stdint.h>
#include "xtc_atomic.h"

/* The proc: a refcount + a "freed" flag the resolver must never see
 * set on a proc it holds. */
static int refs;
static int freed;     /* set to 1 the instant the struct is freed */
static int slot_live; /* the table slot: 1 == proc attached, 0 == NULL */

/* The stripe lock (CAS spinlock): the SAME lock guards lookup and
 * detach, which is what serialises them. */
static int stripe;
#define LOCK_SPINS 3
static void
stripe_lock(void)
{
	int expected, i;
	for (i = 0; i < LOCK_SPINS; i++) {
		expected = 0;
		if (xtc_atomic_cas_weak_i32(&stripe, &expected, 1,
		        xtc_mo_acquire, xtc_mo_relaxed))
			return;
	}
	__CPROVER_assume(0);           /* deeper contention than modelled */
}
static void
stripe_unlock(void)
{
	xtc_atomic_store_i32(&stripe, 0, xtc_mo_release);
}

/* __proc_free: mark freed (models the actual free). */
static void
proc_free(void)
{
	xtc_atomic_store_i32(&freed, 1, xtc_mo_release);
}

/* __proc_release: drop a ref; free when the last one goes. */
static void
proc_release(void)
{
	if (xtc_atomic_fetch_sub_i32(&refs, 1, xtc_mo_acq_rel) == 1)
		proc_free();
}

/* __table_lookup: under the stripe lock, pin the proc if the slot is
 * still live; returns 1 if pinned (caller holds a ref), 0 if gone. */
static int
table_lookup(void)
{
	int pinned = 0;
	stripe_lock();
	if (xtc_atomic_load_i32(&slot_live, xtc_mo_relaxed)) {
		/* Take a ref WHILE the stripe is held -- atomic with the
		 * detach below. */
		(void)xtc_atomic_fetch_add_i32(&refs, 1, xtc_mo_relaxed);
		pinned = 1;
	}
	stripe_unlock();
	return pinned;
}

/* __table_release: under the stripe lock, detach the slot (proc = NULL). */
static void
table_release(void)
{
	stripe_lock();
	xtc_atomic_store_i32(&slot_live, 0, xtc_mo_relaxed);
	stripe_unlock();
}

/* Resolver: look up + pin; if pinned, the proc it holds MUST NOT be
 * freed while the reference is held; then drop the ref. */
static void
resolver(void)
{
	if (table_lookup()) {
		__CPROVER_assert(xtc_atomic_load_i32(&freed, xtc_mo_acquire) ==
		        0,
		    "resolver pins a LIVE proc, never a freed one (no UAF)");
		proc_release();
	}
}

/* Teardown: detach the slot (under the stripe lock), THEN drop the
 * owner's reference -- the ordering that makes the resolver's pin safe. */
static void
teardown(void)
{
	table_release();
	proc_release();                /* drop the owner ref */
}

int
main(void)
{
	xtc_atomic_store_i32(&refs, 1, xtc_mo_relaxed); /* owner ref */
	xtc_atomic_store_i32(&freed, 0, xtc_mo_relaxed);
	xtc_atomic_store_i32(&slot_live, 1, xtc_mo_relaxed); /* attached */
	xtc_atomic_store_i32(&stripe, 0, xtc_mo_relaxed);

	__CPROVER_ASYNC_1: resolver();
	teardown();
	return 0;
}
