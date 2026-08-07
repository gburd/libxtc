/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/ptc/dispatch.c
 *	Dispatcher (roadmap B1): the callback -> fiber bridge.  A thin,
 *	blessed ergonomic wrapper over two already-thread-safe
 *	primitives -- xtc_proc_spawn (cross-thread spawn via the target
 *	loop's MPSC inbox + I/O wakeup; src/evt/task.c) and
 *	xtc_promise_set (safe from any thread; src/inc/xtc_future.h) --
 *	plus the A1/A2 scope/cancellation core, so an impure callback or
 *	a foreign OS thread can submit an effect to the runtime and get
 *	back a future for its result and a cancel handle.
 *
 *	The whole point is the "exactly once" resolution: the future
 *	resolves with fn's value on a normal return, or XTC_E_ABORTED on
 *	cancel/crash, never lost and never doubled.  That guarantee is
 *	NOT re-implemented here; it falls out of xtc_promise_set's
 *	set-once contract combined with an at-exit finalizer that drops
 *	the promise (which completes the future ABORTED) only if fn never
 *	set it.  See test/sim/test_sim_dispatch.c and
 *	test/cbmc/dispatch_once.c for the proofs.
 */

#include "xtc_int.h"
#include "xtc_dispatch.h"
#include "xtc_proc.h"
#include "xtc_future.h"

#include <stdatomic.h>

/*
 * Per-dispatch context, owned by the spawned fiber and freed by its
 * at-exit finalizer.  The cancel handle deliberately holds NONE of
 * this by pointer -- only the fiber's pid (a value type) -- so cancel
 * from a foreign thread can never dereference a context the fiber has
 * already freed.
 */
struct dispatch_ctx {
	int          (*fn)(void *arg);
	void          *arg;
	xtc_promise_t *prom;          /* set by fn's result or dropped */
	_Atomic int    resolved;      /* 0 until the promise is set/dropped */
};

struct xtc_dispatch_handle {
	xtc_pid_t pid;                /* the dispatched fiber (value, stable) */
};

/*
 * At-exit finalizer.  Runs on EVERY exit path of the dispatched fiber
 * (normal return, cancel via xtc_exit_pid, or a contained crash),
 * outside signal context, with the proc still current.  If fn already
 * resolved the promise this is a no-op; otherwise the promise is
 * dropped, which completes the future with XTC_E_ABORTED -- so an
 * awaiter never hangs on a cancelled or crashed dispatch.  The
 * set-once / drop-once guard is the _Atomic exchange: xtc_promise_set
 * and xtc_promise_drop both consume the promise exactly once, and this
 * exchange makes SURE only one of {fn's set, this drop} runs.
 */
static void
__dispatch_finalize(void *v)
{
	struct dispatch_ctx *c = v;
	int was;

	was = atomic_exchange_explicit(&c->resolved, 1, memory_order_acq_rel);
	if (!was)
		xtc_promise_drop(c->prom);   /* fiber ended without a result */
	__os_free(c);
}

/*
 * The dispatched fiber body.  Registers the finalizer FIRST (so even an
 * immediate crash inside fn resolves the future), then runs fn and
 * records its result.  Returning falls through to the at-exit chain,
 * which runs the finalizer.
 */
static void
__dispatch_entry(void *v)
{
	struct dispatch_ctx *c = v;
	int r, was;

	/* If the finalizer cannot be registered (resource cap), resolve
	 * the promise here so the future never hangs, then fall through:
	 * fn still runs, but its result is dropped (the future already
	 * carries ABORTED).  Registering first is the norm; this only
	 * guards the pathological cap-exhausted spawn. */
	if (xtc_proc_at_exit(__dispatch_finalize, c) != XTC_OK) {
		was = atomic_exchange_explicit(&c->resolved, 1,
		    memory_order_acq_rel);
		if (!was)
			xtc_promise_drop(c->prom);
		(void)c->fn(c->arg);
		return;
	}

	r = c->fn(c->arg);

	/* Record fn's result exactly once.  If a concurrent cancel already
	 * won the exchange (unwinding this fiber before we got here is not
	 * possible on a single fiber, but a defensive check keeps the
	 * set-once contract explicit), skip the set. */
	was = atomic_exchange_explicit(&c->resolved, 1, memory_order_acq_rel);
	if (!was)
		(void)xtc_promise_set(c->prom, (intptr_t)r, XTC_OK);
	/* Return -> the at-exit finalizer runs; it sees resolved==1 and
	 * just frees c. */
}

/* PUBLIC: int  xtc_dispatch __P((xtc_loop_t *, int (*)(void *), void *, xtc_future_t **, xtc_dispatch_handle_t **)); */
int
xtc_dispatch(xtc_loop_t *loop, int (*fn)(void *), void *arg,
             xtc_future_t **out_future, xtc_dispatch_handle_t **out_handle)
{
	struct dispatch_ctx *c = NULL;
	struct xtc_dispatch_handle *h = NULL;
	xtc_promise_t *prom = NULL;
	xtc_future_t *fut = NULL;
	xtc_pid_t pid;
	int rc;

	if (loop == NULL || fn == NULL || out_future == NULL)
		return XTC_E_INVAL;

	if ((rc = __os_calloc(1, sizeof *c, (void **)&c)) != XTC_OK)
		return rc;
	if (out_handle != NULL) {
		if ((rc = __os_calloc(1, sizeof *h, (void **)&h)) != XTC_OK) {
			__os_free(c);
			return rc;
		}
	}
	if ((rc = xtc_future_new_pair(&prom, &fut)) != XTC_OK) {
		__os_free(h);
		__os_free(c);
		return rc;
	}

	c->fn = fn;
	c->arg = arg;
	c->prom = prom;
	atomic_store_explicit(&c->resolved, 0, memory_order_relaxed);

	/* Spawn the fiber on `loop`.  Safe from any thread: a cross-thread
	 * spawn posts to loop's MPSC inbox and pings its I/O backend (see
	 * __xtc_task_spawn_ex).  After this returns XTC_OK we must NOT
	 * touch c -- the fiber owns it and may already be running/reaped on
	 * loop's thread. */
	rc = xtc_proc_spawn(loop, __dispatch_entry, c, NULL, &pid);
	if (rc != XTC_OK) {
		/* Nothing was spawned; c is still ours.  Drop the promise so a
		 * (never-handed-out) future would resolve, then free. */
		xtc_promise_drop(prom);
		(void)xtc_future_wait(fut, NULL, 0);   /* consume/free the read half */
		__os_free(h);
		__os_free(c);
		return rc;
	}

	if (out_handle != NULL) {
		h->pid = pid;
		*out_handle = h;
	}
	*out_future = fut;
	return XTC_OK;
}

/* PUBLIC: int  xtc_dispatch_cancel __P((xtc_dispatch_handle_t *)); */
int
xtc_dispatch_cancel(xtc_dispatch_handle_t *h)
{
	if (h == NULL)
		return XTC_E_INVAL;
	/* Best-effort cooperative cancel: signal the fiber to exit.  If it
	 * has already completed / been reaped, xtc_exit_pid is a harmless
	 * no-op (the future keeps fn's value).  The fiber observes the kill
	 * at its next cancellation point, runs its scope/at-exit finalizers
	 * (the finalizer resolves the future ABORTED if fn had not), and
	 * unwinds.  XTC_E_ABORTED as the reason marks it a cancellation. */
	(void)xtc_exit_pid(h->pid, XTC_E_ABORTED);
	__os_free(h);
	return XTC_OK;
}

/* PUBLIC: void xtc_dispatch_handle_free __P((xtc_dispatch_handle_t *)); */
void
xtc_dispatch_handle_free(xtc_dispatch_handle_t *h)
{
	__os_free(h);
}
