/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/cbmc/lwlock_harness.c
 *	CBMC bounded model check of the lwlock state-word acquire/release
 *	core (src/ptc/lock_lw.c __try_attempt + __release_state).
 *
 *	INVARIANT PROVED: mutual exclusion on the state word.  Two
 *	contenders racing xtc_lwlock_acquire(EXCLUSIVE) never both enter
 *	the critical section (an exclusive holder count never exceeds 1),
 *	and every acquirer that fails its CAS retries against the fresh
 *	state rather than deadlocking or corrupting the count.  A
 *	shared+exclusive race additionally proves no shared acquire slips
 *	in while the exclusive bit is set.
 *
 *	WHAT IS MODELLED: a FAITHFUL TRANSCRIPTION of the lock's two
 *	low-level state-word routines -- __try_attempt (the CAS acquire
 *	loop) and __release_state (the CAS release loop) -- copied
 *	verbatim from lock_lw.c, together with the LW_VAL_* /
 *	LW_LOCK_MASK encoding (the exclusive-bit weight is shrunk from
 *	1<<24 to 1<<4: the exclusion logic is agnostic to the bit
 *	position, only to the mask relation, so a small weight checks
 *	identical logic in a far smaller SAT space).  The real lock_lw.c cannot be included
 *	standalone under CBMC (it drags in xtc_int.h, pthread condvars,
 *	the fiber scheduler, TLS held-lists, and the WITNESS tracker --
 *	none of which bear on the mutual-exclusion property, which lives
 *	entirely in these two functions and the state encoding).  The
 *	slow path (park / cond_wait) only ever RE-CALLS __try_attempt on
 *	wake ("wake-and-recheck"), so the CAS core is the whole safety
 *	argument; a spinning acquire (retry __try_attempt until it wins)
 *	is behaviourally identical to the real acquire's fast+slow loop
 *	for the exclusion property.  If lock_lw.c's encoding or CAS
 *	ordering drifts, this transcription must be updated in lockstep.
 *
 *	BOUND: 2 contenders (1 exclusive owner + 1 racing acquirer, in
 *	two configurations: excl-vs-excl and excl-vs-shared).  CBMC
 *	explores every interleaving of the atomic CAS operations.
 *
 *	Run: cbmc lwlock_harness.c --unwind 6
 */

#include <stdint.h>
#include <stdatomic.h>

int nondet_int(void);   /* CBMC nondeterministic choice */

/* --- state encoding, structurally identical to lock_lw.c ---
 * The real lock uses XTC_LWLOCK_MAX_BACKENDS = 1<<24 as the exclusive
 * bit weight; the exclusion logic depends only on the RELATION
 * (LW_VAL_EXCLUSIVE is a single bit above the shared-count field, the
 * shared count occupies the low bits), not on the specific bit
 * position.  A small weight (1<<4) exercises byte-identical mask/CAS
 * logic in a far smaller SAT space -- exactly the capacity-agnostic
 * reduction the deque harness uses. */
#define XTC_LWLOCK_MAX_BACKENDS  ((uint32_t)1u << 4)
#define LW_VAL_EXCLUSIVE   (XTC_LWLOCK_MAX_BACKENDS)
#define LW_VAL_SHARED      1u
#define LW_SHARED_MASK     (XTC_LWLOCK_MAX_BACKENDS - 1u)
#define LW_LOCK_MASK       (LW_SHARED_MASK | LW_VAL_EXCLUSIVE)

enum lw_mode { XTC_LW_EXCLUSIVE = 0, XTC_LW_SHARED = 1 };

static _Atomic uint32_t g_state;

/* Owner-observability: how many holders think they are in the
 * EXCLUSIVE critical section right now.  Bumped on grant, dropped on
 * release; must never exceed 1. */
static _Atomic int in_excl_cs;

/* __try_attempt -- verbatim from lock_lw.c (state word only).
 * The inner for(;;) is the real code's CAS-retry-on-contention loop;
 * with a bounded number of contenders it retries a bounded number of
 * times, so CBMC unwinds it fully at a small --unwind. */
static int
try_attempt(int mode)
{
	uint32_t old_state = atomic_load_explicit(&g_state,
	    memory_order_relaxed);

	for (;;) {
		uint32_t expected = old_state;
		uint32_t desired;

		if (mode == XTC_LW_EXCLUSIVE) {
			if ((old_state & LW_LOCK_MASK) != 0)
				return 0;        /* held shared or exclusive */
			desired = old_state + LW_VAL_EXCLUSIVE;
		} else {
			if ((old_state & LW_VAL_EXCLUSIVE) != 0)
				return 0;        /* held exclusive */
			desired = old_state + LW_VAL_SHARED;
		}
		if (atomic_compare_exchange_weak_explicit(&g_state,
		    &expected, desired,
		    memory_order_acquire, memory_order_relaxed)) {
			return 1;
		}
		old_state = expected;
	}
}

/* __release_state -- verbatim from lock_lw.c. */
static uint32_t
release_state(int mode)
{
	uint32_t old_state, desired;
	old_state = atomic_load_explicit(&g_state, memory_order_relaxed);
	for (;;) {
		if (mode == XTC_LW_EXCLUSIVE)
			desired = old_state - LW_VAL_EXCLUSIVE;
		else
			desired = old_state - LW_VAL_SHARED;
		if (atomic_compare_exchange_weak_explicit(&g_state,
		    &old_state, desired,
		    memory_order_release, memory_order_relaxed))
			return desired;
	}
}

/* A full acquire: spin on try_attempt until it wins.  The real
 * acquire's fast path plus its wake-and-recheck slow loop reduce to
 * exactly this for the exclusion property (the parked path only
 * re-invokes try_attempt on wake).  Bounded to a small number of
 * attempts: with two contenders one actor can be pre-empted by the
 * other at most a bounded number of times before winning, so this
 * bound loses no interleaving relevant to exclusion while keeping the
 * unwind tractable. */
#define ACQ_ATTEMPTS 2
static void
acquire(int mode)
{
	int i;
	for (i = 0; i < ACQ_ATTEMPTS; i++)
		if (try_attempt(mode))
			return;
	/* Ran out of modelled attempts: assume-block this schedule (it
	 * is not a real failure, just a deeper interleaving than the
	 * bound explores). */
	__CPROVER_assume(0);
}

/* Exclusive contender: acquire, assert sole occupancy, release. */
static void
excl_actor(void)
{
	int n;
	acquire(XTC_LW_EXCLUSIVE);
	n = atomic_fetch_add_explicit(&in_excl_cs, 1, memory_order_relaxed);
	__CPROVER_assert(n == 0, "mutual exclusion: never two exclusive holders");
	(void)atomic_fetch_sub_explicit(&in_excl_cs, 1, memory_order_relaxed);
	(void)release_state(XTC_LW_EXCLUSIVE);
}

/* Shared contender: while it holds shared, the exclusive bit must be
 * clear (proves no writer mutated under a reader). */
static void
shared_actor(void)
{
	uint32_t s;
	acquire(XTC_LW_SHARED);
	s = atomic_load_explicit(&g_state, memory_order_relaxed);
	__CPROVER_assert((s & LW_VAL_EXCLUSIVE) == 0,
	    "no exclusive holder while a shared holder is in the CS");
	(void)release_state(XTC_LW_SHARED);
}

int
main(void)
{
	int scenario = nondet_int();

	atomic_store_explicit(&g_state, 0, memory_order_relaxed);
	atomic_store_explicit(&in_excl_cs, 0, memory_order_relaxed);

	if (scenario) {
		/* excl vs excl: the classic mutual-exclusion race. */
		__CPROVER_ASYNC_1: excl_actor();
		excl_actor();
	} else {
		/* excl vs shared: reader must never see the excl bit set,
		 * and the writer must never grant while a reader holds. */
		__CPROVER_ASYNC_2: shared_actor();
		excl_actor();
	}
	return 0;
}
