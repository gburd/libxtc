/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/inc/xtc_dispatch.h
 *	Dispatcher: the callback -> fiber bridge (roadmap B1, Cats
 *	Effect's Dispatcher).  The blessed one-call front door for
 *	crossing INTO the runtime from an IMPURE callback or a FOREIGN
 *	OS thread -- a C library's completion callback, a signal
 *	handler's follow-up, an embedder's own I/O thread -- with a way
 *	to await the effect's result and to cancel it.
 *
 *	This is the inverse of xtc_blocking (xtc_blocking.h), which runs
 *	blocking work OFF a loop: the dispatcher gets work ON a loop from
 *	the outside.
 *
 *	Model.  xtc_dispatch(loop, fn, arg, &fut, &h) spawns a fiber on
 *	`loop` that runs fn(arg) to completion, then completes a future
 *	with fn's return value (as the intptr_t value, status XTC_OK).
 *	The caller -- whatever thread it is on -- awaits that future
 *	(xtc_future_await from a fiber, xtc_future_wait from a plain OS
 *	thread) for the result, and may xtc_dispatch_cancel(h) to abort a
 *	dispatch that has not completed.  The future ALWAYS resolves
 *	exactly once: with fn's value on completion, or with
 *	XTC_E_ABORTED if the dispatched fiber is cancelled or crashes
 *	before it returns -- never lost, never doubled, never a hang.
 *
 *	Thread-safety.  xtc_dispatch and xtc_dispatch_cancel are safe to
 *	call from ANY OS thread, including one libxtc does not manage.
 *	This is not new machinery: it inherits the thread-safety of
 *	xtc_proc_spawn (a cross-thread spawn posts to the target loop's
 *	MPSC inbox and pings its I/O backend; see src/evt/task.c
 *	__xtc_task_spawn_ex) and of xtc_promise_set (documented safe from
 *	any thread; see xtc_future.h).  The dispatcher is the ergonomic
 *	composition of those two proven primitives plus the A1/A2
 *	scope/cancellation core, packaged as the front door consumers
 *	kept reinventing.
 */

#ifndef XTC_DISPATCH_H
#define XTC_DISPATCH_H

#include "xtc_export.h"

#include <stdint.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_future.h"

typedef struct xtc_dispatch_handle xtc_dispatch_handle_t;

/*
 * PUBLIC: int  xtc_dispatch __P((xtc_loop_t *, int (*)(void *), void *, xtc_future_t **, xtc_dispatch_handle_t **));
 *
 * Submit fn(arg) to run as a fiber on `loop` and hand back a way to
 * await its result and to cancel it.  Callable from ANY OS thread.
 *
 *   out_future -- on XTC_OK, receives a future that resolves with fn's
 *                 return value (value = (intptr_t)fn(arg), status
 *                 XTC_OK) when fn returns, or (value 0, status
 *                 XTC_E_ABORTED) if the dispatch is cancelled or the
 *                 fiber crashes first.  The caller owns and consumes it
 *                 with xtc_future_await / xtc_future_wait, exactly like
 *                 any other future.  Required (non-NULL).
 *   out_handle -- on XTC_OK, receives an opaque cancel handle.  May be
 *                 NULL if the caller never needs to cancel (fire and
 *                 await).  If non-NULL, the caller MUST eventually pass
 *                 it to xtc_dispatch_cancel OR xtc_dispatch_handle_free
 *                 to release it (cancel frees it too).
 *
 * Returns XTC_OK, XTC_E_INVAL (NULL loop / fn / out_future),
 * XTC_E_NOMEM, or a spawn error (e.g. XTC_E_RESOURCE if the loop's
 * proc/task caps are hit).  On any error nothing is spawned and neither
 * out is written to a live object.
 */
XTC_API int  xtc_dispatch(xtc_loop_t *loop, int (*fn)(void *), void *arg,
             xtc_future_t **out_future, xtc_dispatch_handle_t **out_handle);

/*
 * PUBLIC: int  xtc_dispatch_cancel __P((xtc_dispatch_handle_t *));
 *
 * Request cancellation of the dispatched fiber and release the handle.
 * Callable from ANY OS thread.  Cancellation is COOPERATIVE and
 * composes with A1/A2: the dispatched fiber observes it at its next
 * cancellation point (a park, xtc_cancel_requested, or the end of an
 * xtc_uncancelable region), runs any xtc_scope / at-exit finalizers,
 * and unwinds -- so a resource acquired under a scope is still
 * released.  If the fiber has already completed, this is a harmless
 * no-op on the result (the future keeps fn's value).  Either way the
 * future is guaranteed already-resolved-or-will-resolve exactly once.
 * The handle is consumed; do not use it again.  Returns XTC_OK, or
 * XTC_E_INVAL on a NULL handle.
 */
XTC_API int  xtc_dispatch_cancel(xtc_dispatch_handle_t *h);

/*
 * PUBLIC: void xtc_dispatch_handle_free __P((xtc_dispatch_handle_t *));
 *
 * Release a cancel handle WITHOUT cancelling (the caller decided it
 * will never cancel this dispatch).  A no-op on NULL.  Do not use the
 * handle after this.  Not needed if out_handle was passed NULL to
 * xtc_dispatch, or if xtc_dispatch_cancel was already called.
 */
XTC_API void xtc_dispatch_handle_free(xtc_dispatch_handle_t *h);

#endif /* XTC_DISPATCH_H */
