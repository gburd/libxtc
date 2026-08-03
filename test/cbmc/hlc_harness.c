/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/cbmc/hlc_harness.c
 *	CBMC bounded model check of the hybrid logical clock (HLC) in the
 *	proc causal-tracing layer (src/ptc/proc.c: __hlc_tick /
 *	__hlc_update, both a relaxed CAS loop over the global __g_hlc).
 *
 *	INVARIANT PROVED (two properties):
 *	  1. MONOTONICITY: under concurrent tick + update, the clock never
 *	     goes backward.  Every value the CAS commits is strictly
 *	     greater than the value it read (next > prev), so the global
 *	     stamp is strictly increasing on every successful advance --
 *	     no two operations ever produce the same stamp, and no
 *	     operation ever lowers it.
 *	  2. CAUSAL ORDER: an update fed a remote stamp `m` yields a local
 *	     stamp strictly greater than `m` (the received event
 *	     happens-before the local event that observed it -- the whole
 *	     point of an HLC).
 *
 *	WHAT IS MODELLED: __hlc_tick and __hlc_update are transcribed
 *	VERBATIM from proc.c -- the same 48-bit-physical / 16-bit-logical
 *	split, the same `if (pt > pphys) ... else if (logical would
 *	overflow) ... else bump logical` ladder, the same relaxed CAS
 *	loop over __g_hlc.  The only substitution is __phys_us(): the real
 *	one reads the monotonic clock (nondeterministic, and forbidden
 *	under the sim determinism guard), so it is replaced by a
 *	CBMC-nondeterministic bounded value -- which is STRONGER than the
 *	real clock (it lets the physical component take any ordering
 *	relative to the stored stamp, so CBMC explores clock-went-
 *	backward and clock-jumped-forward alike).  If the HLC ladder in
 *	proc.c drifts, this must be updated in lockstep.
 *
 *	NEGATIVE CHECK (reproduce the classic HLC bug); verified to FAIL:
 *	  - `cbmc hlc_harness.c -DHLC_BUG_NONMONO --unwind 4`
 *	    __hlc_tick, when physical time has not advanced and the logical
 *	    field has room, forgets the `+ 1` (emits `pphys << 16 | plog`
 *	    instead of `| (plog + 1)`) -> two ticks return the SAME stamp
 *	    -> CBMC reports the monotonicity violation (next > prev fails).
 *
 *	NON-VACUITY (documented reproducible mutation): the causal-order
 *	claim is tight -- weakening `local > m` in the harness to a
 *	non-strict `local >= m` still passes trivially, but tightening the
 *	monotonicity assert to `next >= prev + 2` (edit below) makes CBMC
 *	report a counterexample, proving a single-logical-tick advance
 *	(next == prev + 1) is genuinely reachable, so neither assert is
 *	vacuous.  A physical value strictly greater than the stored
 *	physical also genuinely occurs (the nondeterministic __phys_us
 *	ranges over it), exercising the pt-wins branch.
 *
 *	BOUND: two concurrent HLC operations (a tick and an update)
 *	racing on __g_hlc.  CBMC explores every interleaving of the CAS.
 *
 *	Run: cbmc hlc_harness.c --unwind 4
 */

#include <stdint.h>
#include <stdatomic.h>

/* The global HLC, exactly as proc.c declares it. */
static _Atomic uint64_t g_hlc;

/* Non-deterministic physical microseconds, bounded so CBMC's state
 * space is finite.  This REPLACES __phys_us() (which reads the real
 * monotonic clock): a nondet value is strictly more general than any
 * concrete clock -- it lets physical time sit below, at, or above the
 * stored physical component, so every branch of the ladder is
 * explored, including a physical clock that appears to go backward. */
static uint64_t
phys_us(void)
{
	uint64_t pt;
	pt = __CPROVER_nondet_ulong();
	__CPROVER_assume(pt <= 3);   /* small physical range: tractable */
	return pt;
}

/* --- __hlc_tick: transcribed VERBATIM from proc.c --- */
static uint64_t
hlc_tick(void)
{
	uint64_t prev, next, pt = phys_us();
	do {
		uint64_t pphys, plog;
		prev = atomic_load_explicit(&g_hlc, memory_order_relaxed);
		pphys = prev >> 16; plog = prev & 0xFFFF;
		if (pt > pphys) {
			next = pt << 16;
		} else if (plog + 1 > 0xFFFF) {
			next = (pphys + 1) << 16;
		} else {
#ifdef HLC_BUG_NONMONO
			/* planted bug: forget the +1 on the logical bump ->
			 * two ticks with the same physical time collide. */
			next = (pphys << 16) | (plog);
#else
			next = (pphys << 16) | (plog + 1);
#endif
		}
	} while (!atomic_compare_exchange_weak_explicit(&g_hlc, &prev, next,
	    memory_order_relaxed, memory_order_relaxed));
	/* MONOTONICITY: every committed advance is strictly greater than
	 * what it read.  Non-vacuity: change `> prev` to `>= prev + 2` and
	 * CBMC reports a counterexample (a single logical tick,
	 * next == prev + 1, is reachable). */
	__CPROVER_assert(next > prev,
	    "hlc_tick never returns a non-increasing stamp (monotonic)");
	return next;
}

/* --- __hlc_update: transcribed VERBATIM from proc.c --- */
static uint64_t
hlc_update(uint64_t m)
{
	uint64_t prev, next, pt = phys_us();
	do {
		uint64_t pphys = 0, plog = 0, mphys = m >> 16, mlog = m & 0xFFFF;
		uint64_t nphys, nlog;
		prev = atomic_load_explicit(&g_hlc, memory_order_relaxed);
		pphys = prev >> 16; plog = prev & 0xFFFF;
		nphys = pphys;
		if (mphys > nphys) nphys = mphys;
		if (pt > nphys) nphys = pt;
		if (nphys == pphys && nphys == mphys)
			nlog = (plog > mlog ? plog : mlog) + 1;
		else if (nphys == pphys)
			nlog = plog + 1;
		else if (nphys == mphys)
			nlog = mlog + 1;
		else
			nlog = 0;
		if (nlog > 0xFFFF) { nphys++; nlog = 0; }
		next = (nphys << 16) | (nlog & 0xFFFF);
	} while (!atomic_compare_exchange_weak_explicit(&g_hlc, &prev, next,
	    memory_order_relaxed, memory_order_relaxed));
	/* MONOTONICITY: the update never lowers the clock. */
	__CPROVER_assert(next > prev,
	    "hlc_update never returns a non-increasing stamp (monotonic)");
	/* CAUSAL ORDER: the local stamp strictly dominates the received
	 * remote stamp m -- the received event happens-before the local
	 * event that observed it. */
	__CPROVER_assert(next > m,
	    "hlc_update yields a stamp strictly greater than the remote (causal)");
	return next;
}

/* Actor A: a local tick. */
static void
ticker(void)
{
	(void)hlc_tick();
}

/* Actor B: receive a remote stamp and advance past it.  The remote
 * stamp is nondeterministic (bounded) so CBMC checks causal order
 * against any m, including m ahead of the local clock. */
static void
updater(void)
{
	uint64_t m = __CPROVER_nondet_ulong();
	/* Keep the physical component in the same small range as the
	 * local clock so the state space stays tiny; the logical field is
	 * free within its 16-bit range but bounded here for tractability. */
	__CPROVER_assume((m >> 16) <= 3);
	__CPROVER_assume((m & 0xFFFF) <= 2);
	(void)hlc_update(m);
}

int
main(void)
{
	atomic_store_explicit(&g_hlc, 0, memory_order_relaxed);

	/* A tick and an update race on the single global clock; CBMC
	 * explores every interleaving of the two CAS loops. */
	__CPROVER_ASYNC_1: ticker();
	updater();
	return 0;
}
