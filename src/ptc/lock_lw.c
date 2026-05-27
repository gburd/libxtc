/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
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
 *	    they re-CAS — equivalent to PG's wake_one-then-retry pattern
 *	    after we've released the lock, just less precise.  Acceptable
 *	    for the xtc workload mix; LWLock is not a heavy-contention
 *	    primitive in xtc (use lockmgr or lrlock for those).
 *	  - LW_FLAG_WAKE_IN_PROGRESS / LW_FLAG_QUEUE_LOCKED are kept for
 *	    state-encoding parity but only one of them is used (queue
 *	    serialisation lives in the pthread_mutex).
 *	  - No global held-LWLocks list (held_lwlocks[]) — xtc has no
 *	    transaction-error backstop equivalent to PG's
 *	    LWLockReleaseAll, and the lockmgr has its own deadlock
 *	    detection.  Held-by-me checks use a TLS slot bitmap.
 *	  - No tranches as a name registry; the `tranche` field is
 *	    user-tag only.
 *
 *	Reference: PG's lwlock.c §"State management".
 */

#include "xtc_int.h"
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

static __thread struct held_entry __held[XTC_LWLOCK_TLS_SLOTS];
static __thread int               __n_held = 0;

static int
__held_push(const xtc_lwlock_t *l, xtc_lwlock_mode_t m)
{
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
				return 0;        /* held shared or exclusive — block */
			desired = old_state + LW_VAL_EXCLUSIVE;
		} else {
			if ((old_state & LW_VAL_EXCLUSIVE) != 0)
				return 0;        /* held exclusive — block */
			desired = old_state + LW_VAL_SHARED;
		}

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
			/* Clear the HAS_WAITERS flag — no one is queued. */
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
