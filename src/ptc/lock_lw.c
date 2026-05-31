/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/ptc/lock_lw.c
 *	Lightweight lock implementation, ported from
 *	postgres/lrlck/src/backend/storage/lmgr/lwlock.c.
 *
 *	Differences from PG:
 *	  - The wait queue is a pthread_cond rather than PG's proclist of
 *	    PGPROC latches.  pthread_cond_broadcast wakes all waiters and
 *	    they re-CAS -- equivalent to PG's wake_one-then-retry pattern
 *	    after we've released the lock, just less precise.  Acceptable
 *	    for the xtc workload mix; LWLock is not a heavy-contention
 *	    primitive in xtc (use lockmgr or lrlock for those).
 *	  - LW_FLAG_WAKE_IN_PROGRESS / LW_FLAG_QUEUE_LOCKED are kept for
 *	    state-encoding parity but only one of them is used (queue
 *	    serialisation lives in the pthread_mutex).
 *	  - No global held-LWLocks list (held_lwlocks[]) -- xtc has no
 *	    transaction-error backstop equivalent to PG's
 *	    LWLockReleaseAll, and the lockmgr has its own deadlock
 *	    detection.  Held-by-me checks use a TLS slot bitmap.
 *	  - No tranches as a name registry; the `tranche` field is
 *	    user-tag only.
 *
 *	Reference: PG's lwlock.c (S)"State management".
 */

#include "xtc_int.h"
#include "xtc_inject.h"
#include "xtc_lwlock.h"

#include <stdatomic.h>
#include <string.h>

/* ----- state encoding ----- */
#define LW_FLAG_HAS_WAITERS         ((uint32_t)1u << 31)
#define LW_FLAG_WAKE_IN_PROGRESS    ((uint32_t)1u << 30)
#define LW_FLAG_QUEUE_LOCKED        ((uint32_t)1u << 29)
#define LW_FLAG_BITS                3u
#define LW_FLAG_MASK                (((1u << LW_FLAG_BITS) - 1u) << (32 - LW_FLAG_BITS))

#define LW_VAL_EXCLUSIVE   (XTC_LWLOCK_MAX_BACKENDS)
#define LW_VAL_SHARED      1u
#define LW_SHARED_MASK     (XTC_LWLOCK_MAX_BACKENDS - 1u)
#define LW_LOCK_MASK       (LW_SHARED_MASK | LW_VAL_EXCLUSIVE)

/* ----- per-thread held-list -----
 * Each thread tracks a small array of (lock, mode) pairs so
 * xtc_lwlock_release() knows what mode the caller acquired in, and so
 * held_by_me / held_by_me_in_mode work without a per-thread search of
 * the entire universe of locks. */
#define XTC_LWLOCK_TLS_SLOTS 32

struct held_entry {
	const xtc_lwlock_t *lock;
	xtc_lwlock_mode_t   mode;
};

static XTC_THREAD_LOCAL struct held_entry __held[XTC_LWLOCK_TLS_SLOTS];
static XTC_THREAD_LOCAL int               __n_held = 0;

/* ----- optional lock-order (WITNESS) tracker -------------------------
 *
 * Off by default.  When enabled, every acquire records the order
 * "tranche A was held when tranche B was acquired" in a global edge
 * set; if the reverse order (B-before-A) was ever seen, that is a
 * lock-order inversion -- the precursor of a two-lock deadlock the
 * lwlock primitive itself cannot detect (unlike xtc_lockmgr).  Keyed
 * by tranche (the lock class), since the frozen lwlock ABI has no
 * room for per-lock bookkeeping.  Enable in test/staging; it has
 * overhead and is not meant for production hot paths. */
#define LW_TRACK_EDGES 8192u            /* power of two */
static _Atomic int        __lw_track_on = 0;
static _Atomic long       __lw_track_viol = 0;
static pthread_mutex_t    __lw_track_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t           __lw_track_edge[LW_TRACK_EDGES];  /* 0 == empty */
static xtc_lwlock_track_fn __lw_track_fn = NULL;
static void              *__lw_track_user = NULL;

static int
__lw_edge_contains(uint32_t key)
{
	uint32_t i = (key * 2654435761u) & (LW_TRACK_EDGES - 1);
	uint32_t probes = 0;
	while (__lw_track_edge[i] != 0) {
		if (__lw_track_edge[i] == key) return 1;
		i = (i + 1) & (LW_TRACK_EDGES - 1);
		if (++probes >= LW_TRACK_EDGES) break;
	}
	return 0;
}

static void
__lw_edge_insert(uint32_t key)
{
	uint32_t i = (key * 2654435761u) & (LW_TRACK_EDGES - 1);
	uint32_t probes = 0;
	while (__lw_track_edge[i] != 0) {
		if (__lw_track_edge[i] == key) return;
		i = (i + 1) & (LW_TRACK_EDGES - 1);
		if (++probes >= LW_TRACK_EDGES) return;   /* full: degrade */
	}
	__lw_track_edge[i] = key;
}

/* Record that every currently-held tranche precedes `acquired`; flag
 * any reverse edge already observed as an inversion. */
static void
__lw_track_acquire(const xtc_lwlock_t *acquired)
{
	int i;
	uint16_t b = acquired->tranche;

	if (!atomic_load_explicit(&__lw_track_on, memory_order_relaxed))
		return;
	(void)pthread_mutex_lock(&__lw_track_mu);
	for (i = 0; i < __n_held; i++) {
		uint16_t a = __held[i].lock->tranche;
		uint32_t fwd, rev;
		if (a == b) continue;            /* same class: not tracked */
		fwd = ((uint32_t)a << 16) | b;
		rev = ((uint32_t)b << 16) | a;
		if (__lw_edge_contains(rev)) {
			atomic_fetch_add_explicit(&__lw_track_viol, 1,
			    memory_order_relaxed);
			if (__lw_track_fn != NULL)
				__lw_track_fn(a, b, __lw_track_user);
		}
		__lw_edge_insert(fwd);
	}
	(void)pthread_mutex_unlock(&__lw_track_mu);
}

/* PUBLIC: void xtc_lwlock_track_enable __P((int)); */
void
xtc_lwlock_track_enable(int on)
{
	atomic_store_explicit(&__lw_track_on, on ? 1 : 0,
	    memory_order_relaxed);
}

/* PUBLIC: long xtc_lwlock_track_violations __P((void)); */
long
xtc_lwlock_track_violations(void)
{
	return atomic_load_explicit(&__lw_track_viol, memory_order_relaxed);
}

/* PUBLIC: void xtc_lwlock_track_reset __P((void)); */
void
xtc_lwlock_track_reset(void)
{
	(void)pthread_mutex_lock(&__lw_track_mu);
	memset(__lw_track_edge, 0, sizeof __lw_track_edge);
	atomic_store_explicit(&__lw_track_viol, 0, memory_order_relaxed);
	(void)pthread_mutex_unlock(&__lw_track_mu);
}

/* PUBLIC: void xtc_lwlock_track_set_handler __P((xtc_lwlock_track_fn, void *)); */
void
xtc_lwlock_track_set_handler(xtc_lwlock_track_fn fn, void *user)
{
	__lw_track_fn = fn;
	__lw_track_user = user;
}

static int
__held_push(const xtc_lwlock_t *l, xtc_lwlock_mode_t m)
{
	/* Record lock-order edges from the currently-held set BEFORE
	 * pushing the new lock (the witness tracker; a no-op when off). */
	__lw_track_acquire(l);
	if (__n_held >= XTC_LWLOCK_TLS_SLOTS) return -1;
	__held[__n_held].lock = l;
	__held[__n_held].mode = m;
	__n_held++;
	return 0;
}

static int
__held_pop(const xtc_lwlock_t *l, xtc_lwlock_mode_t *out_mode)
{
	int i;
	for (i = __n_held - 1; i >= 0; i--) {
		if (__held[i].lock == l) {
			if (out_mode != NULL) *out_mode = __held[i].mode;
			/* Compact tail. */
			if (i != __n_held - 1)
				__held[i] = __held[__n_held - 1];
			__n_held--;
			return 0;
		}
	}
	return -1;
}

static int
__held_find(const xtc_lwlock_t *l, xtc_lwlock_mode_t *out_mode)
{
	int i;
	for (i = 0; i < __n_held; i++) {
		if (__held[i].lock == l) {
			if (out_mode != NULL) *out_mode = __held[i].mode;
			return 1;
		}
	}
	return 0;
}

/* ----- low-level acquire/release on the state word ----- */

/* Try a single CAS to acquire `mode`.  Returns 1 on success. */
static int
__try_attempt(xtc_lwlock_t *lock, xtc_lwlock_mode_t mode)
{
	uint32_t old_state = atomic_load_explicit(&lock->state,
	    memory_order_relaxed);

	for (;;) {
		uint32_t expected = old_state;
		uint32_t desired;

		if (mode == XTC_LW_EXCLUSIVE) {
			if ((old_state & LW_LOCK_MASK) != 0)
				return 0;        /* held shared or exclusive -- block */
			desired = old_state + LW_VAL_EXCLUSIVE;
		} else {
			if ((old_state & LW_VAL_EXCLUSIVE) != 0)
				return 0;        /* held exclusive -- block */
			desired = old_state + LW_VAL_SHARED;
		}

		/* Race window: we have computed `desired` from `expected`
		 * but not yet attempted the CAS.  A test pauses one
		 * acquirer here, lets another acquirer mutate state, then
		 * releases: the weak CAS observes the change, fails, and we
		 * retry from the new state -- exercising the contended
		 * retry path deterministically. */
		XTC_INJECTION_POINT("lwlock.acquire.pre_cas");
		if (atomic_compare_exchange_weak_explicit(&lock->state,
		    &expected, desired,
		    memory_order_acquire, memory_order_relaxed)) {
			return 1;
		}
		old_state = expected;
	}
}

/* Decrement the holder count; return the new state. */
static uint32_t
__release_state(xtc_lwlock_t *lock, xtc_lwlock_mode_t mode)
{
	uint32_t old_state, desired;
	old_state = atomic_load_explicit(&lock->state, memory_order_relaxed);
	for (;;) {
		if (mode == XTC_LW_EXCLUSIVE)
			desired = old_state - LW_VAL_EXCLUSIVE;
		else
			desired = old_state - LW_VAL_SHARED;
		if (atomic_compare_exchange_weak_explicit(&lock->state,
		    &old_state, desired,
		    memory_order_release, memory_order_relaxed))
			return desired;
	}
}

/* ----- public API ----- */

int
xtc_lwlock_init(xtc_lwlock_t *lock, uint16_t tranche)
{
	if (lock == NULL) return XTC_E_INVAL;
	memset(lock, 0, sizeof *lock);
	atomic_store_explicit(&lock->state, 0, memory_order_relaxed);
	(void)pthread_mutex_init(&lock->wait_mu, NULL);
	(void)pthread_cond_init (&lock->wait_cv, NULL);
	lock->tranche = tranche;
	lock->initialised = 1;
	return XTC_OK;
}

void
xtc_lwlock_destroy(xtc_lwlock_t *lock)
{
	if (lock == NULL || !lock->initialised) return;
	(void)pthread_cond_destroy (&lock->wait_cv);
	(void)pthread_mutex_destroy(&lock->wait_mu);
	lock->initialised = 0;
}

int
xtc_lwlock_acquire(xtc_lwlock_t *lock, xtc_lwlock_mode_t mode)
{
	if (lock == NULL || !lock->initialised) return XTC_E_INVAL;

	for (;;) {
		if (__try_attempt(lock, mode)) {
			(void)__held_push(lock, mode);
			return XTC_OK;
		}

		/* Slow path: enqueue + cond_wait. */
		(void)pthread_mutex_lock(&lock->wait_mu);
		/* Set HAS_WAITERS on the state.  Use compare_exchange so we
		 * don't wipe other flags. */
		{
			uint32_t s = atomic_load_explicit(&lock->state,
			    memory_order_acquire);
			while ((s & LW_FLAG_HAS_WAITERS) == 0) {
				if (atomic_compare_exchange_weak_explicit(&lock->state,
				    &s, s | LW_FLAG_HAS_WAITERS,
				    memory_order_release, memory_order_acquire)) break;
			}
		}
		lock->n_waiters++;
		/* Re-check before sleeping. */
		if (__try_attempt(lock, mode)) {
			lock->n_waiters--;
			(void)pthread_mutex_unlock(&lock->wait_mu);
			(void)__held_push(lock, mode);
			return XTC_OK;
		}
		(void)pthread_cond_wait(&lock->wait_cv, &lock->wait_mu);
		lock->n_waiters--;
		(void)pthread_mutex_unlock(&lock->wait_mu);
		/* Loop: try CAS again.  Spurious wakeups are harmless; a
		 * legitimate wakeup means the lock just became free and we
		 * should beat the next thread to it. */
	}
}

int
xtc_lwlock_acquire_cond(xtc_lwlock_t *lock, xtc_lwlock_mode_t mode)
{
	if (lock == NULL || !lock->initialised) return XTC_E_INVAL;
	if (__try_attempt(lock, mode)) {
		(void)__held_push(lock, mode);
		return XTC_OK;
	}
	return XTC_E_AGAIN;
}

void
xtc_lwlock_release(xtc_lwlock_t *lock)
{
	xtc_lwlock_mode_t mode;
	uint32_t new_state;

	if (lock == NULL || !lock->initialised) return;
	if (__held_pop(lock, &mode) != 0) return;   /* not held by us */

	new_state = __release_state(lock, mode);

	/* Wake waiters if the lock is now free or only-shared and someone
	 * is queued. */
	if ((new_state & LW_FLAG_HAS_WAITERS) != 0 &&
	    (new_state & LW_VAL_EXCLUSIVE) == 0) {
		(void)pthread_mutex_lock(&lock->wait_mu);
		if (lock->n_waiters > 0) {
			(void)pthread_cond_broadcast(&lock->wait_cv);
		} else {
			/* Clear the HAS_WAITERS flag -- no one is queued. */
			uint32_t s = atomic_load_explicit(&lock->state,
			    memory_order_relaxed);
			while ((s & LW_FLAG_HAS_WAITERS) != 0) {
				if (atomic_compare_exchange_weak_explicit(&lock->state,
				    &s, s & ~LW_FLAG_HAS_WAITERS,
				    memory_order_release, memory_order_relaxed)) break;
			}
		}
		(void)pthread_mutex_unlock(&lock->wait_mu);
	}
}

int
xtc_lwlock_held_by_me(const xtc_lwlock_t *lock)
{
	return __held_find(lock, NULL);
}

int
xtc_lwlock_held_by_me_in_mode(const xtc_lwlock_t *lock,
                              xtc_lwlock_mode_t mode)
{
	xtc_lwlock_mode_t m;
	if (!__held_find(lock, &m)) return 0;
	return m == mode;
}
