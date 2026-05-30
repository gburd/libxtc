/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_sync.h
 *	L3 synchronization primitives.  M9 ships:
 *
 *	  notify       Tokio-style one-shot wake-of-any-waiter
 *	  semaphore    counting; backpressure currency
 *	  abort_source Seastar-style structured cancellation
 *
 *	The full M9 surface (mutex, rwlock, barrier, gate) lands in
 *	M9.5 alongside the lock-manager work in M13.  These three are
 *	the minimum needed for the M10 supervisor.
 */

#ifndef XTC_SYNC_H
#define XTC_SYNC_H

#include <stdint.h>

#include "xtc.h"
#include "xtc_loop.h"

/* ----- notify ----------------------------------------------------- */

typedef struct xtc_notify xtc_notify_t;

/*
 * PUBLIC: int  xtc_notify_create __P((xtc_notify_t **));
 * PUBLIC: void xtc_notify_destroy __P((xtc_notify_t *));
 * PUBLIC: int  xtc_notify_signal __P((xtc_notify_t *));
 * PUBLIC: int  xtc_notify_wait __P((xtc_notify_t *, int64_t));
 */
int  xtc_notify_create(xtc_notify_t **out);
void xtc_notify_destroy(xtc_notify_t *n);

/* Wake one waiter.  If no one is waiting, the signal is "stored"
 * and the next wait returns immediately.  Subsequent signals
 * before a wait collapse into one. */
int  xtc_notify_signal(xtc_notify_t *n);

/* Block (yield) the calling task until a signal arrives.  timeout_ns
 * < 0 = forever; 0 = non-blocking; > 0 = bounded.  Returns XTC_E_AGAIN
 * on timeout, XTC_OK on signal received. */
int  xtc_notify_wait(xtc_notify_t *n, int64_t timeout_ns);

/* ----- semaphore -------------------------------------------------- */

typedef struct xtc_sem xtc_sem_t;

/*
 * PUBLIC: int  xtc_sem_create __P((unsigned, xtc_sem_t **));
 * PUBLIC: void xtc_sem_destroy __P((xtc_sem_t *));
 * PUBLIC: int  xtc_sem_post __P((xtc_sem_t *, unsigned));
 * PUBLIC: int  xtc_sem_acquire __P((xtc_sem_t *, unsigned, int64_t));
 * PUBLIC: int  xtc_sem_try_acquire __P((xtc_sem_t *, unsigned));
 * PUBLIC: int  xtc_sem_count __P((const xtc_sem_t *));
 */
int  xtc_sem_create(unsigned initial, xtc_sem_t **out);
void xtc_sem_destroy(xtc_sem_t *s);

/* Add `n` units. */
int  xtc_sem_post(xtc_sem_t *s, unsigned n);

/* Take `n` units, blocking up to timeout_ns. */
int  xtc_sem_acquire(xtc_sem_t *s, unsigned n, int64_t timeout_ns);

/* Take `n` units, returning XTC_E_AGAIN immediately if not enough. */
int  xtc_sem_try_acquire(xtc_sem_t *s, unsigned n);

int  xtc_sem_count(const xtc_sem_t *s);

/* ----- abort_source ----------------------------------------------- */

typedef struct xtc_abort_source xtc_abort_source_t;
typedef struct xtc_abort_token  xtc_abort_token_t;
/*
 * Structured cancellation.  An abort_source is owned by some parent
 * (e.g. a supervisor) and produces tokens that children check.
 * When the source is fired, every token answers true to is_aborted.
 *
 * PUBLIC: int  xtc_abort_source_create __P((xtc_abort_source_t **));
 * PUBLIC: void xtc_abort_source_destroy __P((xtc_abort_source_t *));
 * PUBLIC: int  xtc_abort_source_fire __P((xtc_abort_source_t *, int));
 * PUBLIC: int  xtc_abort_source_token __P((xtc_abort_source_t *, xtc_abort_token_t *));
 *
 * PUBLIC: int  xtc_abort_token_is_aborted __P((const xtc_abort_token_t *));
 * PUBLIC: int  xtc_abort_token_reason __P((const xtc_abort_token_t *));
 */
int  xtc_abort_source_create(xtc_abort_source_t **out);
void xtc_abort_source_destroy(xtc_abort_source_t *s);

/* Atomically fire the source with a reason code.  All current and
 * future tokens see is_aborted=true. */
int  xtc_abort_source_fire(xtc_abort_source_t *s, int reason);

/* Mint a token bound to the source. */
int  xtc_abort_source_token(xtc_abort_source_t *s, xtc_abort_token_t *out);

/*
 * Public token shape (so callers can keep one on the stack).  The
 * implementation only reads fields documented here.
 */
struct xtc_abort_token {
	xtc_abort_source_t *src;
};

int  xtc_abort_token_is_aborted(const xtc_abort_token_t *t);
int  xtc_abort_token_reason(const xtc_abort_token_t *t);

/* ----- mutex ------------------------------------------------------ */

typedef struct xtc_amutex xtc_amutex_t;

/*
 * Async parking mutex.  When free, the lock is a fast uncontended
 * flag.  When contended, a caller running inside a process /
 * coroutine parks the fiber (yields to its loop) rather than blocking
 * the OS thread, so a process can hold the lock across its own park
 * (e.g. a blocking-pool offload) without wedging the loop when
 * another process on that loop contends.  Fiber waiters form a FIFO
 * queue with direct hand-off (fair, no thundering herd); a caller
 * that is not on a loop blocks on a condvar as a fallback.
 *
 * PUBLIC: int  xtc_amutex_create __P((xtc_amutex_t **));
 * PUBLIC: void xtc_amutex_destroy __P((xtc_amutex_t *));
 * PUBLIC: int  xtc_amutex_lock __P((xtc_amutex_t *, int64_t));
 * PUBLIC: int  xtc_amutex_try_lock __P((xtc_amutex_t *));
 * PUBLIC: int  xtc_amutex_unlock __P((xtc_amutex_t *));
 */
int  xtc_amutex_create(xtc_amutex_t **out);
void xtc_amutex_destroy(xtc_amutex_t *m);
int  xtc_amutex_lock(xtc_amutex_t *m, int64_t timeout_ns);
int  xtc_amutex_try_lock(xtc_amutex_t *m);
int  xtc_amutex_unlock(xtc_amutex_t *m);

/* ----- rwlock ----------------------------------------------------- */

typedef struct xtc_rwlock xtc_rwlock_t;

/*
 * Reader/writer lock with writer priority (writers don't starve).
 *
 * PUBLIC: int  xtc_rwlock_create __P((xtc_rwlock_t **));
 * PUBLIC: void xtc_rwlock_destroy __P((xtc_rwlock_t *));
 * PUBLIC: int  xtc_rwlock_rdlock __P((xtc_rwlock_t *, int64_t));
 * PUBLIC: int  xtc_rwlock_wrlock __P((xtc_rwlock_t *, int64_t));
 * PUBLIC: int  xtc_rwlock_unlock __P((xtc_rwlock_t *));
 */
int  xtc_rwlock_create(xtc_rwlock_t **out);
void xtc_rwlock_destroy(xtc_rwlock_t *r);
int  xtc_rwlock_rdlock(xtc_rwlock_t *r, int64_t timeout_ns);
int  xtc_rwlock_wrlock(xtc_rwlock_t *r, int64_t timeout_ns);
int  xtc_rwlock_unlock(xtc_rwlock_t *r);

/* ----- barrier ---------------------------------------------------- */

typedef struct xtc_barrier xtc_barrier_t;

/*
 * N-task rendezvous.  Reusable: after the Nth waiter arrives, all
 * are released and the barrier resets to wait for another N.
 *
 * PUBLIC: int  xtc_barrier_create __P((unsigned, xtc_barrier_t **));
 * PUBLIC: void xtc_barrier_destroy __P((xtc_barrier_t *));
 * PUBLIC: int  xtc_barrier_wait __P((xtc_barrier_t *));
 */
int  xtc_barrier_create(unsigned n, xtc_barrier_t **out);
void xtc_barrier_destroy(xtc_barrier_t *b);
int  xtc_barrier_wait(xtc_barrier_t *b);

/* ----- gate ------------------------------------------------------- */

typedef struct xtc_gate xtc_gate_t;

/*
 * Seastar-style gate: counts outstanding operations so callers can
 * drain.  enter/leave around each protected operation; close stops
 * accepting new entries; drain blocks until count reaches zero.
 *
 * Pattern:
 *    xtc_gate_enter(g);
 *    do_work();
 *    xtc_gate_leave(g);
 *    ...
 *    xtc_gate_close(g);
 *    xtc_gate_drain(g, timeout_ns);    // wait for in-flight ops to finish
 *
 * PUBLIC: int  xtc_gate_create __P((xtc_gate_t **));
 * PUBLIC: void xtc_gate_destroy __P((xtc_gate_t *));
 * PUBLIC: int  xtc_gate_enter __P((xtc_gate_t *));
 * PUBLIC: int  xtc_gate_leave __P((xtc_gate_t *));
 * PUBLIC: int  xtc_gate_close __P((xtc_gate_t *));
 * PUBLIC: int  xtc_gate_drain __P((xtc_gate_t *, int64_t));
 * PUBLIC: int  xtc_gate_count __P((const xtc_gate_t *));
 */
int  xtc_gate_create(xtc_gate_t **out);
void xtc_gate_destroy(xtc_gate_t *g);
int  xtc_gate_enter(xtc_gate_t *g);
int  xtc_gate_leave(xtc_gate_t *g);
int  xtc_gate_close(xtc_gate_t *g);
int  xtc_gate_drain(xtc_gate_t *g, int64_t timeout_ns);
int  xtc_gate_count(const xtc_gate_t *g);

#endif /* XTC_SYNC_H */
