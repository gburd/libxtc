/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/inc/xtc_lwlock.h
 *	Lightweight lock — multi-reader / single-writer lock with an
 *	atomic state word and an explicit wait queue.  Ported from
 *	postgres/lrlck/src/backend/storage/lmgr/lwlock.c, retaining the
 *	state-encoding scheme verbatim:
 *
 *	  bit 31:  LW_FLAG_HAS_WAITERS        — wakeups are pending
 *	  bit 30:  LW_FLAG_WAKE_IN_PROGRESS   — another thread is waking
 *	  bit 29:  LW_FLAG_QUEUE_LOCKED       — wait-queue spinlock
 *	  bits 0..N-1: shared-holder count
 *	  bit N:   LW_VAL_EXCLUSIVE           — exclusive owner present
 *
 *	N is configurable via XTC_LWLOCK_MAX_BACKENDS (default 4096).
 *
 *	Trade-offs vs xtc_rwlock:
 *	  + Single-CAS fast path for both acquire and release.
 *	  + No fairness guarantees beyond FIFO wakeup order.
 *	  + Cache-line-aligned struct (avoid false sharing).
 *	  - Caller must release in reverse order of acquire.
 *	  - No timeout/cancellable acquire (use xtc_amutex/xtc_rwlock for
 *	    those scenarios).
 *
 *	Trade-offs vs xtc_lrlock:
 *	  + Multiple writers (serialized).
 *	  + No second copy of the data.
 *	  + Reads aren't wait-free \u2014 they CAS the state word.
 *	  - Bigger contention surface; pick lrlock for read-mostly.
 */

#ifndef XTC_LWLOCK_H
#define XTC_LWLOCK_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "xtc.h"

#ifndef XTC_LWLOCK_MAX_BACKENDS
#define XTC_LWLOCK_MAX_BACKENDS 4096   /* power of 2; bits 0..11 = shared count */
#endif

typedef enum xtc_lwlock_mode {
	XTC_LW_EXCLUSIVE = 0,
	XTC_LW_SHARED    = 1
} xtc_lwlock_mode_t;

/* The lock itself.  Caller embeds in any structure or stack-allocates. */
typedef struct xtc_lwlock {
	_Atomic uint32_t  state;
	pthread_mutex_t   wait_mu;
	pthread_cond_t    wait_cv;
	int               n_waiters;     /* protected by wait_mu */
	int               wakers_pending;/* protected by wait_mu */
	uint16_t          tranche;       /* user-tag; for diagnostics only */
	uint8_t           initialised;
	uint8_t           pad_;
} xtc_lwlock_t;

/*
 * PUBLIC: int  xtc_lwlock_init __P((xtc_lwlock_t *, uint16_t));
 * PUBLIC: void xtc_lwlock_destroy __P((xtc_lwlock_t *));
 *
 * PUBLIC: int  xtc_lwlock_acquire __P((xtc_lwlock_t *, xtc_lwlock_mode_t));
 * PUBLIC: int  xtc_lwlock_acquire_cond __P((xtc_lwlock_t *, xtc_lwlock_mode_t));
 * PUBLIC: void xtc_lwlock_release __P((xtc_lwlock_t *));
 *
 * PUBLIC: int  xtc_lwlock_held_by_me __P((const xtc_lwlock_t *));
 * PUBLIC: int  xtc_lwlock_held_by_me_in_mode __P((const xtc_lwlock_t *, xtc_lwlock_mode_t));
 */

int   xtc_lwlock_init(xtc_lwlock_t *lock, uint16_t tranche);
void  xtc_lwlock_destroy(xtc_lwlock_t *lock);

/* Acquire blocks until the lock is held in `mode`.  Returns XTC_OK. */
int   xtc_lwlock_acquire(xtc_lwlock_t *lock, xtc_lwlock_mode_t mode);

/* Conditional acquire — non-blocking.  Returns XTC_OK on success,
 * XTC_E_AGAIN if the lock would block. */
int   xtc_lwlock_acquire_cond(xtc_lwlock_t *lock, xtc_lwlock_mode_t mode);

/* Release the lock.  Mode is recorded in the per-thread held-list,
 * so the caller doesn't have to pass it back. */
void  xtc_lwlock_release(xtc_lwlock_t *lock);

/* Diagnostics. */
int   xtc_lwlock_held_by_me(const xtc_lwlock_t *lock);
int   xtc_lwlock_held_by_me_in_mode(const xtc_lwlock_t *lock,
                                    xtc_lwlock_mode_t mode);

#endif /* XTC_LWLOCK_H */
