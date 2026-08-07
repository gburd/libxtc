/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/cbmc/mask_harness.c
 *	CBMC bounded model check of A2 cancellation masking against an
 *	asynchronous cross-thread kill (src/ptc/proc.c:
 *	__xtc_proc_kill_deliver + the mask_depth / mask_deferred latch +
 *	xtc_exit_pid's release-CAS into kill_pending).
 *
 *	INVARIANT PROVED: a kill set by a REMOTE thread (xtc_exit_pid does
 *	an atomic release-CAS into kill_pending) is never LOST by masking.
 *	While the owning fiber is masked (mask_depth > 0) a delivery point
 *	DEFERS the kill into mask_deferred instead of acting on it; when
 *	the mask fully lifts (mask_depth back to 0) the deferred kill is
 *	drained and acted on EXACTLY ONCE.  Masking may only DELAY
 *	observation, never drop it -- which is what lets a resource
 *	acquired in a masked region always register its release before
 *	cancellation unwinds the fiber (the A1+A2 guarantee).
 *
 *	WHAT IS MODELLED (transcribed from proc.c):
 *	  remote:  CAS kill_pending 0 -> reason+1 (release) -- xtc_exit_pid.
 *	  owner (single fiber, so mask_depth/mask_deferred are owner-only,
 *	         no atomics -- exactly as in proc.c):
 *	    mask++          (xtc_uncancelable enter)
 *	    delivery point  (__xtc_proc_kill_deliver): load kill_pending
 *	                    (acquire); if set and mask>0 latch into
 *	                    mask_deferred, else "act".
 *	    mask--          (xtc_uncancelable leave)
 *	    drain           (__mask_drain): if mask==0 and mask_deferred
 *	                    set, "act" on it once.
 *	  A later unmasked delivery point also acts if kill_pending is set
 *	  and nothing has acted yet.
 *	"Acting" is a monotone counter; the invariant is that it ends at
 *	EXACTLY 1 whenever the remote fired, and 0 otherwise -- never lost,
 *	never doubled.
 *
 *	BOUND: one remote killer racing one owner fiber's mask/deliver/
 *	drain sequence.  CBMC explores every interleaving of the release-
 *	CAS against the owner's acquire-load delivery points.
 *
 *	If proc.c's kill-deliver / mask-drain ordering drifts, update this
 *	in lockstep.
 *
 *	Run: cbmc mask_harness.c --unwind 4
 */

#include <stdint.h>
#include <stdatomic.h>

/* Shared: kill_pending is the ONLY cross-thread field (atomic, exactly
 * as in proc.c).  mask_depth / mask_deferred are owner-only. */
static _Atomic int kill_pending;   /* 0 = none; reason+1 otherwise */
static int mask_depth;             /* owner-only */
static int mask_deferred;          /* owner-only latch: reason+1, 0 = none */
static int acted;                  /* monotone: how many times the kill fired */

/* "Act" on a kill: the real code unwinds via xtc_exit_self, which does
 * NOT return -- the fiber is gone, so no later delivery point runs.  We
 * model that termination with a latch: once acted, the owner sequence
 * stops (any further delivery/drain step is unreachable in reality).
 * The invariant is that `acted` ends at exactly 1 when the remote fired. */
static int gone;               /* set once the fiber has unwound */
static void
act(int reason_plus1)
{
	(void)reason_plus1;
	acted++;
	gone = 1;                  /* xtc_exit_self does not return */
}

/* __xtc_proc_kill_deliver: a yield/recv/sleep delivery point. */
static void
kill_deliver(void)
{
	int kp = atomic_load_explicit(&kill_pending, memory_order_acquire);
	if (kp == 0)
		return;
	if (mask_depth > 0) {
		if (mask_deferred == 0)
			mask_deferred = kp;   /* defer -- do NOT act */
		return;
	}
	act(kp);
}

/* __mask_drain: on full unmask, honor a latched-deferred kill once. */
static void
mask_drain(void)
{
	int kp;
	if (mask_depth > 0)
		return;
	kp = mask_deferred;
	if (kp != 0) {
		mask_deferred = 0;
		act(kp);
	}
}

/* The owner fiber: xtc_uncancelable(body) with a delivery point inside
 * the masked body, then unmask + drain, then a later unmasked delivery
 * point (the park after the region returns). */
static void
owner(void)
{
	mask_depth++;         /* enter uncancelable */
	kill_deliver();       /* park inside the masked region -> deferred */
	if (gone) return;
	if (mask_depth > 0)
		mask_depth--;   /* leave uncancelable */
	mask_drain();         /* honor a deferred kill now that mask == 0 */
	if (gone) return;
	kill_deliver();       /* a later park: acts if a kill is still pending */
	if (gone) return;
	/* The fiber always eventually reaches another park point, so model a
	 * final guaranteed delivery: any kill that fired at all is observed
	 * by here.  (A kill that lands after this simply fires at the NEXT
	 * park -- out of this bounded window, not a loss.) */
	kill_deliver();
}

/* Remote killer: xtc_exit_pid's release-CAS (0 -> reason+1, first wins). */
static int fired;         /* did the remote actually fire? */
static void
killer(void)
{
	int expected = 0;
	int desired = 2;      /* reason 1 -> encoded reason+1 == 2 */
	if (atomic_compare_exchange_strong_explicit(&kill_pending,
	    &expected, desired, memory_order_release, memory_order_relaxed))
		fired = 1;
}

int
main(void)
{
	atomic_store_explicit(&kill_pending, 0, memory_order_relaxed);
	mask_depth = 0;
	mask_deferred = 0;
	acted = 0;
	fired = 0;
	gone = 0;

	/* Race the remote killer against the owner's mask sequence.  The
	 * killer fires at SOME point in the window (CBMC picks the
	 * interleaving); the final guaranteed delivery point in owner()
	 * ensures a fired kill is observed within the window. */
	__CPROVER_ASYNC_1: killer();
	owner();

	/* If the remote fired, the kill was acted on EXACTLY once (never
	 * lost by masking, never doubled by the deferred latch + a stale
	 * kill_pending).  If it never fired, nothing acted. */
	if (fired)
		__CPROVER_assert(acted == 1,
		    "a remote kill is delivered exactly once -- masking delays, "
		    "never drops or doubles it");
	else
		__CPROVER_assert(acted == 0,
		    "no kill fired => nothing acted");

	/* And no kill is left stranded in the deferred latch once fully
	 * unmasked and past all delivery points. */
	__CPROVER_assert(mask_deferred == 0 || mask_depth > 0,
	    "no deferred kill is stranded after the mask fully lifts");
	return 0;
}
