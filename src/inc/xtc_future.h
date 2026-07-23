/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/inc/xtc_future.h
 *	Futures and promises (PLAN.md 2.4.1) -- a one-shot awaitable
 *	value with a write half (xtc_promise_t) and a read half
 *	(xtc_future_t), plus combinators (then / map / when_all /
 *	when_any / with_timeout).
 *
 *	This is the "task that returns a value" primitive, distinct from
 *	the "task with identity + mailbox" primitive (xtc_proc): code that
 *	just needs one asynchronous result -- a query answer, a lock
 *	grant, a computed value -- uses a future at near-zero overhead
 *	rather than standing up a whole process and mailbox.
 *
 *	Model.  A future carries an intptr_t value and an int status
 *	(the same value/status shape xtc_await uses: the status is an
 *	XTC_* code, XTC_OK on a normal completion).  A promise is set
 *	EXACTLY ONCE; the future is then READY forever and every awaiter
 *	-- present or future -- observes the same (value, status).
 *
 *	Parking.  xtc_future_await parks the calling FIBER cooperatively
 *	(the loop keeps serving everyone else) and resumes it when the
 *	promise is set; xtc_future_wait works from a fiber OR a plain OS
 *	thread (e.g. main) and takes a timeout.  Both are built on the
 *	same dual-mode park/wake core as xtc_notify, so a promise set
 *	from ANY thread correctly wakes an awaiter on its own loop.
 *
 *	Ownership.  The (promise, future) pair share one reference-counted
 *	cell.  Dropping the promise WITHOUT setting it completes the
 *	future with XTC_E_ABORTED (a broken promise is an abort, never a
 *	hang).  Each xtc_future_t is consumed by exactly one terminal
 *	operation -- await, wait, or a combinator that takes ownership --
 *	after which the handle must not be reused; the combinators return
 *	a NEW future for the chained result.
 */

#ifndef XTC_FUTURE_H
#define XTC_FUTURE_H

#include "xtc_export.h"

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"

typedef struct xtc_future  xtc_future_t;
typedef struct xtc_promise xtc_promise_t;

/*
 * PUBLIC: int  xtc_future_new_pair __P((xtc_promise_t **, xtc_future_t **));
 *
 * Create a linked promise/future pair sharing one cell.  On XTC_OK both
 * *out_prom and *out_fut are non-NULL.  Returns XTC_E_INVAL on a NULL
 * out, XTC_E_NOMEM on allocation failure.
 */
XTC_API int  xtc_future_new_pair(xtc_promise_t **out_prom,
             xtc_future_t **out_fut);

/*
 * PUBLIC: int  xtc_promise_set __P((xtc_promise_t *, intptr_t, int));
 *
 * Complete the promise with (value, status) and wake every awaiter.
 * May be called from any thread.  A promise is set exactly once: a
 * second set returns XTC_E_INVAL and changes nothing.  Setting also
 * releases the promise's reference -- after xtc_promise_set the promise
 * handle is consumed and must not be used again (do NOT also call
 * xtc_promise_drop).  Returns XTC_OK, or XTC_E_INVAL (NULL / already
 * set).
 */
XTC_API int  xtc_promise_set(xtc_promise_t *prom, intptr_t value,
             int status);

/*
 * PUBLIC: void xtc_promise_drop __P((xtc_promise_t *));
 *
 * Abandon a promise WITHOUT setting it: the future completes with
 * status XTC_E_ABORTED (value 0), so an awaiter never hangs on a
 * dropped promise.  A no-op on NULL or an already-set promise.  Do not
 * use the handle after this.
 */
XTC_API void xtc_promise_drop(xtc_promise_t *prom);

/*
 * PUBLIC: int  xtc_future_await __P((xtc_future_t *, intptr_t *));
 *
 * Await the result from within a FIBER: parks cooperatively until the
 * promise is set, then resumes and writes the value to *out (may be
 * NULL) and returns the promise's status.  Consumes the future (frees
 * the read half); the handle must not be reused.  Returns the stored
 * status (XTC_OK or an XTC_E_* the producer set, or XTC_E_ABORTED for a
 * dropped promise), or XTC_E_INVAL for a NULL future / call outside a
 * fiber.
 */
XTC_API int  xtc_future_await(xtc_future_t *fut, intptr_t *out);

/*
 * PUBLIC: int  xtc_future_wait __P((xtc_future_t *, intptr_t *, int64_t));
 *
 * Like xtc_future_await but usable from a plain OS thread (e.g. main)
 * as well as a fiber, and bounded by timeout_ns (< 0 waits forever,
 * 0 polls).  On timeout returns XTC_E_AGAIN and does NOT consume the
 * future (it may be awaited again).  On completion writes *out, returns
 * the status, and consumes the future.
 */
XTC_API int  xtc_future_wait(xtc_future_t *fut, intptr_t *out,
             int64_t timeout_ns);

/*
 * PUBLIC: int  xtc_future_ready __P((xtc_future_t *, int *));
 *
 * Non-blocking readiness check.  Sets *is_ready to 1 if the promise is
 * already set (await/wait would return immediately), else 0.  Does NOT
 * consume the future.  Returns XTC_OK or XTC_E_INVAL.
 */
XTC_API int  xtc_future_ready(xtc_future_t *fut, int *is_ready);

/*
 * Combinator callbacks.
 *   xtc_future_map_fn transforms a completed value into a new value
 *   (pure, synchronous; runs when the source completes, on the setter's
 *   thread).  Receives the source value and status; returns the mapped
 *   value.  It may also inspect status to short-circuit.
 */
typedef intptr_t (*xtc_future_map_fn)(intptr_t value, int status,
            void *user);

/*
 * PUBLIC: int  xtc_future_map __P((xtc_future_t *, xtc_future_map_fn, void *, xtc_future_t **));
 *
 * Return a NEW future whose value is fn(src_value, src_status, user)
 * and whose status is the source's status, computed when the source
 * completes.  Consumes the source future.  Returns XTC_OK (with
 * *out_fut set), XTC_E_INVAL, or XTC_E_NOMEM.
 */
XTC_API int  xtc_future_map(xtc_future_t *src, xtc_future_map_fn fn,
             void *user, xtc_future_t **out_fut);

/*
 * then-callback: consumes the source result and returns a NEW future
 * (the flat-map / chaining case -- "when src completes, start the next
 * async step").  Ownership of the returned future transfers to the
 * combinator, which resolves the outer future from it.
 */
typedef int (*xtc_future_then_fn)(intptr_t value, int status, void *user,
            xtc_future_t **out_next);

/*
 * PUBLIC: int  xtc_future_then __P((xtc_future_t *, xtc_future_then_fn, void *, xtc_future_t **));
 *
 * When src completes, call fn to produce the next future; the outer
 * (returned) future completes with THAT future's result.  Consumes
 * src.  Returns XTC_OK / XTC_E_INVAL / XTC_E_NOMEM.
 */
XTC_API int  xtc_future_then(xtc_future_t *src, xtc_future_then_fn fn,
             void *user, xtc_future_t **out_fut);

/*
 * PUBLIC: int  xtc_future_when_all __P((xtc_future_t **, int, xtc_future_t **));
 *
 * Return a future that completes when ALL n input futures have
 * completed.  Its value is n (the count) and its status is XTC_OK if
 * every input succeeded, else the FIRST non-OK status observed.
 * Consumes all n input futures (they must not be awaited separately).
 * Returns XTC_OK / XTC_E_INVAL / XTC_E_NOMEM.
 */
XTC_API int  xtc_future_when_all(xtc_future_t **futs, int n,
             xtc_future_t **out_fut);

/*
 * PUBLIC: int  xtc_future_when_any __P((xtc_future_t **, int, xtc_future_t **));
 *
 * Return a future that completes as soon as ANY one of the n inputs
 * completes, carrying that input's (value, status).  Consumes all n
 * inputs.  Returns XTC_OK / XTC_E_INVAL / XTC_E_NOMEM.
 */
XTC_API int  xtc_future_when_any(xtc_future_t **futs, int n,
             xtc_future_t **out_fut);

/*
 * PUBLIC: int  xtc_future_with_timeout __P((xtc_future_t *, int64_t, xtc_future_t **));
 *
 * Return a future that completes with src's result if it completes
 * within timeout_ns, else completes with XTC_E_AGAIN (value 0).
 * Consumes src.  Requires a running loop (arms a timer).  Returns
 * XTC_OK / XTC_E_INVAL / XTC_E_NOMEM.
 */
XTC_API int  xtc_future_with_timeout(xtc_future_t *src, int64_t timeout_ns,
             xtc_future_t **out_fut);

#endif /* XTC_FUTURE_H */
