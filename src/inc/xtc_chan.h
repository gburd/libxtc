/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * src/inc/xtc_chan.h
 *	The L3 channel taxonomy.  M7 ships three:
 *
 *	  oneshot:  one sender, one receiver, exactly-one message.
 *	  mpsc:     bounded multi-producer single-consumer queue.
 *	  watch:    many-to-many "latest value wins" slot (one frame).
 *
 *	mpmc and broadcast arrive in M7.5.  All channels have explicit
 *	bounded capacity; out-of-capacity behaviour is documented per
 *	type and tracked via xtc_res so callers cannot exhaust memory.
 *
 *	Each channel exposes both a blocking variant (caller parks on
 *	a waker if the queue is full/empty) and a non-blocking variant
 *	(returns XTC_E_AGAIN immediately).
 */

#ifndef XTC_CHAN_H
#define XTC_CHAN_H

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_res.h"

/* ----- oneshot ----------------------------------------------------- */

typedef struct xtc_chan_oneshot xtc_chan_oneshot_t;

/*
 * PUBLIC: int  xtc_chan_oneshot_create __P((xtc_res_t *, xtc_chan_oneshot_t **));
 * PUBLIC: void xtc_chan_oneshot_destroy __P((xtc_chan_oneshot_t *));
 * PUBLIC: int  xtc_chan_oneshot_send __P((xtc_chan_oneshot_t *, void *));
 * PUBLIC: int  xtc_chan_oneshot_try_recv __P((xtc_chan_oneshot_t *, void **));
 * PUBLIC: int  xtc_chan_oneshot_set_waker __P((xtc_chan_oneshot_t *, const xtc_waker_t *));
 */
int  xtc_chan_oneshot_create(xtc_res_t *res, xtc_chan_oneshot_t **out);
void xtc_chan_oneshot_destroy(xtc_chan_oneshot_t *c);

/*
 * Send the single message.  Idempotent on a closed channel: a second
 * send returns XTC_E_INVAL.  Always succeeds in the open case (the
 * slot has capacity 1).  If a waker has been registered, fires it.
 */
int  xtc_chan_oneshot_send(xtc_chan_oneshot_t *c, void *msg);

/* Non-blocking receive.  Returns XTC_E_AGAIN if nothing yet. */
int  xtc_chan_oneshot_try_recv(xtc_chan_oneshot_t *c, void **out);

/*
 * Register a waker to fire when send happens.  Replaces any prior
 * waker.  The receiver pattern: register waker, return PENDING from
 * the task; when waker fires, try_recv.
 */
int  xtc_chan_oneshot_set_waker(xtc_chan_oneshot_t *c, const xtc_waker_t *w);

/* ----- mpsc bounded ------------------------------------------------ */

typedef struct xtc_chan_mpsc xtc_chan_mpsc_t;

/*
 * PUBLIC: int  xtc_chan_mpsc_create __P((xtc_res_t *, size_t, xtc_chan_mpsc_t **));
 * PUBLIC: void xtc_chan_mpsc_destroy __P((xtc_chan_mpsc_t *));
 * PUBLIC: int  xtc_chan_mpsc_try_send __P((xtc_chan_mpsc_t *, void *));
 * PUBLIC: int  xtc_chan_mpsc_try_recv __P((xtc_chan_mpsc_t *, void **));
 * PUBLIC: int  xtc_chan_mpsc_set_waker __P((xtc_chan_mpsc_t *, const xtc_waker_t *));
 * PUBLIC: int  xtc_chan_mpsc_close __P((xtc_chan_mpsc_t *));
 * PUBLIC: size_t xtc_chan_mpsc_len __P((const xtc_chan_mpsc_t *));
 */
int    xtc_chan_mpsc_create(xtc_res_t *res, size_t capacity,
                            xtc_chan_mpsc_t **out);
void   xtc_chan_mpsc_destroy(xtc_chan_mpsc_t *c);

/*
 * Try to send.  Returns:
 *   XTC_OK            on success
 *   XTC_E_AGAIN       channel full (caller is responsible for backpressure;
 *                     register a waker via _set_waker if you want to
 *                     wait for capacity, or use the wrapper xtc_chan_send)
 *   XTC_E_INVAL       closed channel
 *   XTC_E_RESOURCE    global slot cap (xtc_res XTC_RES_CHAN_SLOTS) hit
 */
int    xtc_chan_mpsc_try_send(xtc_chan_mpsc_t *c, void *msg);

/* Non-blocking receive.  Returns XTC_E_AGAIN on empty. */
int    xtc_chan_mpsc_try_recv(xtc_chan_mpsc_t *c, void **out);

/* Register the consumer's waker.  Fired on every successful send. */
int    xtc_chan_mpsc_set_waker(xtc_chan_mpsc_t *c, const xtc_waker_t *w);

/*
 * Close the channel.  Subsequent sends fail; pending receives drain
 * the buffer then return XTC_E_INVAL on the next call.
 */
int    xtc_chan_mpsc_close(xtc_chan_mpsc_t *c);

size_t xtc_chan_mpsc_len(const xtc_chan_mpsc_t *c);

/* ----- watch (latest-value-wins) ----------------------------------- */

typedef struct xtc_chan_watch xtc_chan_watch_t;

/*
 * PUBLIC: int  xtc_chan_watch_create __P((xtc_res_t *, xtc_chan_watch_t **));
 * PUBLIC: void xtc_chan_watch_destroy __P((xtc_chan_watch_t *));
 * PUBLIC: int  xtc_chan_watch_send __P((xtc_chan_watch_t *, void *));
 * PUBLIC: int  xtc_chan_watch_recv __P((xtc_chan_watch_t *, void **));
 */
int  xtc_chan_watch_create(xtc_res_t *res, xtc_chan_watch_t **out);
void xtc_chan_watch_destroy(xtc_chan_watch_t *c);
int  xtc_chan_watch_send(xtc_chan_watch_t *c, void *value);
int  xtc_chan_watch_recv(xtc_chan_watch_t *c, void **out);

/* ----- mpmc bounded ----------------------------------------------- */

typedef struct xtc_chan_mpmc xtc_chan_mpmc_t;

/*
 * Bounded multi-producer multi-consumer queue.  M7.5 ships a
 * mutex-protected ring; the lock-free Vyukov variant is a future
 * optimisation.  Out-of-capacity behaviour matches mpsc:
 * try_send returns XTC_E_AGAIN, callers register a waker if they
 * want to wait.
 *
 * PUBLIC: int    xtc_chan_mpmc_create __P((xtc_res_t *, size_t, xtc_chan_mpmc_t **));
 * PUBLIC: void   xtc_chan_mpmc_destroy __P((xtc_chan_mpmc_t *));
 * PUBLIC: int    xtc_chan_mpmc_try_send __P((xtc_chan_mpmc_t *, void *));
 * PUBLIC: int    xtc_chan_mpmc_try_recv __P((xtc_chan_mpmc_t *, void **));
 * PUBLIC: int    xtc_chan_mpmc_close __P((xtc_chan_mpmc_t *));
 * PUBLIC: size_t xtc_chan_mpmc_len __P((const xtc_chan_mpmc_t *));
 */
int    xtc_chan_mpmc_create(xtc_res_t *res, size_t capacity,
                            xtc_chan_mpmc_t **out);
void   xtc_chan_mpmc_destroy(xtc_chan_mpmc_t *c);
int    xtc_chan_mpmc_try_send(xtc_chan_mpmc_t *c, void *msg);
int    xtc_chan_mpmc_try_recv(xtc_chan_mpmc_t *c, void **out);
int    xtc_chan_mpmc_close(xtc_chan_mpmc_t *c);
size_t xtc_chan_mpmc_len(const xtc_chan_mpmc_t *c);

/* ----- broadcast --------------------------------------------------- */

typedef struct xtc_chan_broadcast      xtc_chan_broadcast_t;
typedef struct xtc_chan_broadcast_recv xtc_chan_broadcast_recv_t;

/*
 * Broadcast channel: every subscribed receiver sees every message
 * (best-effort).  Each receiver carries its own cursor; if a
 * receiver lags more than `capacity` messages it observes a
 * "lagged" indicator and skips ahead to the latest available.  This
 * is Tokio's broadcast semantics.
 *
 * Senders never block; the ring is lossy on slow consumers, by
 * design.  For lossless multi-receiver use mpmc + a per-consumer
 * filter, or supervise the slow consumer.
 *
 * PUBLIC: int  xtc_chan_broadcast_create __P((xtc_res_t *, size_t, xtc_chan_broadcast_t **));
 * PUBLIC: void xtc_chan_broadcast_destroy __P((xtc_chan_broadcast_t *));
 * PUBLIC: int  xtc_chan_broadcast_send __P((xtc_chan_broadcast_t *, void *));
 * PUBLIC: int  xtc_chan_broadcast_subscribe __P((xtc_chan_broadcast_t *, xtc_chan_broadcast_recv_t **));
 * PUBLIC: void xtc_chan_broadcast_unsubscribe __P((xtc_chan_broadcast_recv_t *));
 * PUBLIC: int  xtc_chan_broadcast_recv __P((xtc_chan_broadcast_recv_t *, void **, int *));
 */
int  xtc_chan_broadcast_create(xtc_res_t *res, size_t capacity,
                                xtc_chan_broadcast_t **out);
void xtc_chan_broadcast_destroy(xtc_chan_broadcast_t *c);
int  xtc_chan_broadcast_send(xtc_chan_broadcast_t *c, void *msg);
int  xtc_chan_broadcast_subscribe(xtc_chan_broadcast_t *c,
                                   xtc_chan_broadcast_recv_t **out_recv);
void xtc_chan_broadcast_unsubscribe(xtc_chan_broadcast_recv_t *r);

/*
 * Receive the next message visible to this subscriber.  On success
 * returns XTC_OK and writes *out plus *lagged (count of skipped
 * messages because we fell behind).  XTC_E_AGAIN if the cursor is
 * caught up; XTC_E_INVAL if r is NULL.
 */
int  xtc_chan_broadcast_recv(xtc_chan_broadcast_recv_t *r,
                              void **out, int *lagged);

#endif /* XTC_CHAN_H */
