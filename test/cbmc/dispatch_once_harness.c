/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/cbmc/dispatch_once_harness.c
 *	CBMC bounded model check of the dispatcher's "exactly once"
 *	future-resolution invariant (roadmap B1; src/ptc/dispatch.c:
 *	__dispatch_entry's result-set and __dispatch_finalize's drop,
 *	both gated on the _Atomic `resolved` exchange).
 *
 *	INVARIANT PROVED: the future a dispatched effect hands back is
 *	resolved EXACTLY ONCE -- never lost, never doubled -- across the
 *	race between the fiber completing fn (which sets the promise) and
 *	a concurrent cancel/unwind (which runs the finalizer, dropping the
 *	promise if fn never set it).  xtc_promise_set / xtc_promise_drop
 *	each consume the one-shot promise; if BOTH ran, the second would
 *	touch a consumed promise (a double-resolve / use-after-consume).
 *	The single _Atomic exchange on `resolved` is what makes SURE only
 *	one of {set, drop} ever runs.
 *
 *	WHAT IS MODELLED (transcribed from dispatch.c):
 *	  A foreign thread may fire a cancel (set a request) at ANY point;
 *	  CBMC picks when.  The fiber's own two resolver points then run
 *	  in program order on the fiber's thread -- the effect-return set,
 *	  then the at-exit finalizer drop -- each gated on the SAME
 *	  _Atomic `resolved` exchange:
 *	    - effect return: exchange(resolved,1); if was 0, promise_set.
 *	    - finalizer:     exchange(resolved,1); if was 0, promise_drop.
 *	  These are two points in ONE fiber's lifetime (they never run
 *	  concurrently WITH EACH OTHER), but the cancel request that
 *	  drives whether the return even happens arrives concurrently from
 *	  a foreign thread.  "Resolving" is a monotone counter; the
 *	  invariant is that it ends at EXACTLY 1 on every schedule.
 *
 *	BOUND: one foreign cancel racing the fiber's resolve/finalize
 *	sequence.  --unwind 3 covers it.
 *
 *	If dispatch.c's resolve/finalize gating drifts, update this in
 *	lockstep.
 *
 *	Run: cbmc dispatch_once_harness.c --unwind 3
 */

#include <stdatomic.h>

/* Shared: a foreign cancel request, set atomically from another thread
 * (xtc_dispatch_cancel -> xtc_exit_pid's release-CAS into kill_pending).
 * `resolved` is the one-shot gate both resolver points exchange on. */
 static _Atomic int cancel_req;
 static _Atomic int resolved;

/* "Resolve" the promise: models xtc_promise_set OR xtc_promise_drop,
 * each of which consumes the one-shot promise.  A monotone counter --
 * the invariant is that it ends at exactly 1. */
static int resolves;
static void
resolve_once(void)
{
	resolves++;
}

/* The foreign canceller: xtc_dispatch_cancel from another OS thread
 * sets the request (release-CAS in the real code).  It does NOT itself
 * resolve the promise -- resolution always happens on the fiber. */
static void
foreign_cancel(void)
{
	int expected = 0;
	(void)atomic_compare_exchange_strong_explicit(&cancel_req,
	    &expected, 1, memory_order_release, memory_order_relaxed);
}

/* The effect-return resolver (__dispatch_entry tail).  In the real code
 * the fiber runs fn to completion ONLY if a cancel did not unwind it
 * first; model that by skipping the set when a cancel was observed at
 * this point (the fiber unwinds straight to the finalizer instead). */
static void
effect_return(void)
{
	int was;
	if (atomic_load_explicit(&cancel_req, memory_order_acquire))
		return;   /* cancelled: unwind to the finalizer, no set here */
	was = atomic_exchange_explicit(&resolved, 1, memory_order_acq_rel);
	if (!was)
		resolve_once();   /* xtc_promise_set(value) */
}

/* The finalizer resolver (__dispatch_finalize): drop iff we won the
 * exchange (the effect never resolved).  ALWAYS runs on the fiber's
 * exit path -- normal return, cancel unwind, or crash. */
static void
finalize(void)
{
	int was = atomic_exchange_explicit(&resolved, 1,
	    memory_order_acq_rel);
	if (!was)
		resolve_once();   /* xtc_promise_drop() -> ABORTED */
}

int
main(void)
{
	atomic_store_explicit(&cancel_req, 0, memory_order_relaxed);
	atomic_store_explicit(&resolved, 0, memory_order_relaxed);
	resolves = 0;

	/* A foreign cancel races the fiber.  CBMC picks whether/when it
	 * fires relative to the fiber's resolve/finalize sequence.  The
	 * fiber's two resolver points run in PROGRAM ORDER on the fiber's
	 * own thread (they are never concurrent with each other), so the
	 * faithful model runs them sequentially after the async cancel. */
	__CPROVER_ASYNC_1: foreign_cancel();
	effect_return();   /* fiber completes fn (unless cancelled) */
	finalize();        /* at-exit finalizer, on every exit path */

	/* The promise is resolved EXACTLY once on every schedule: the
	 * effect-return set OR the finalizer drop wins the `resolved`
	 * exchange, never both (no double-resolve of the one-shot promise)
	 * and never neither (the finalizer ALWAYS runs, so a cancelled or
	 * crashed fiber still resolves the future ABORTED -- never lost). */
	__CPROVER_assert(resolves == 1,
	    "a dispatched future is resolved exactly once -- the set/drop "
	    "exchange never loses and never doubles the resolution");
	return 0;
}
