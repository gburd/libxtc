/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/ptc/future.c
 *	Futures and promises (PLAN.md 2.4.1).  See src/inc/xtc_future.h
 *	for the model.
 *
 *	Built on the SAME dual-mode park/wake core as xtc_notify
 *	(src/ptc/sync.c): a fiber waiter arms its task waker, enqueues,
 *	and yields with __xtc_proc_ctx_save/restore around the yield so
 *	the current-proc TLS survives a migration; a thread waiter uses a
 *	condvar.  A promise set from ANY thread wakes both kinds.  This
 *	reuse is deliberate -- the prepare/park race, migration, and
 *	cross-thread wake correctness were already gotten right there, and
 *	a future is that same one-shot notification plus a (value, status)
 *	payload and a completion-callback list for the combinators.
 */

#include "xtc_int.h"
#include "preempt_int.h"   /* __xtc_mtx_* */
#include "xtc_future.h"
#include "xtc_loop.h"
#include "xtc_async.h"     /* xtc_yield */
#include "proc_int.h"      /* __xtc_proc_ctx_save/restore */
#include "coro_int.h"      /* __xtc_current_task */
#include "loop_int.h"      /* __xtc_current_loop, __xtc_task_cancel_park_timer */
#include "os_alloc.h"
#include "os_time.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

/* Fiber waiter node (FIFO), identical shape to sync.c's. */
struct fut_waiter {
	xtc_waker_t        waker;
	struct fut_waiter *next;
};

/*
 * A completion callback fired (once, under the cell lock released) when
 * the cell becomes ready.  Combinators register one on their source to
 * resolve the chained cell.  cb receives the completed (value, status)
 * and the callback's user pointer.
 *
 * A registered-but-not-yet-fired callback OWNS ONE REFERENCE on the
 * cell it is registered on (taken by cell_on_complete), so the source
 * cell cannot be freed out from under a pending combinator even after
 * the source future HANDLE is consumed.  That ref is released when the
 * callback fires (in cell_complete / the immediate-fire path).
 */
struct fut_oncomplete {
	void (*cb)(intptr_t value, int status, void *user);
	void *user;
	struct fut_oncomplete *next;
};

/*
 * The shared cell.  promise + future halves and every combinator hold a
 * reference; the cell frees when the last ref drops.
 */
struct future_cell {
	pthread_mutex_t lock;
	pthread_cond_t  cv;              /* thread waiters */
	_Atomic int     refcnt;
	int             ready;           /* 0 pending, 1 completed */
	intptr_t        value;
	int             status;
	struct fut_waiter     *wq_head;  /* fiber waiters, FIFO */
	struct fut_waiter     *wq_tail;
	struct fut_oncomplete *cbs;      /* completion callbacks */
};

struct xtc_future  { struct future_cell *cell; };
struct xtc_promise { struct future_cell *cell; int set_or_dropped; };

/* ---- cell lifecycle ---- */

static int
cell_new(struct future_cell **out)
{
	struct future_cell *c;
	int rc;
	if ((rc = __os_calloc(1, sizeof *c, (void **)&c)) != XTC_OK)
		return rc;
	(void)pthread_mutex_init(&c->lock, NULL);
	(void)pthread_cond_init(&c->cv, NULL);
	atomic_init(&c->refcnt, 0);
	c->ready = 0;
	c->status = XTC_OK;
	*out = c;
	return XTC_OK;
}

static void
cell_ref(struct future_cell *c)
{
	(void)atomic_fetch_add_explicit(&c->refcnt, 1, memory_order_relaxed);
}

static void
cell_unref(struct future_cell *c)
{
	if (c == NULL)
		return;
	if (atomic_fetch_sub_explicit(&c->refcnt, 1, memory_order_acq_rel) == 1) {
		/* Last ref.  No waiters can remain (each waiter holds the
		 * future ref while parked), and callbacks fire at set-time,
		 * so cbs is empty by now unless the cell was dropped never-set
		 * with pending cbs -- free those defensively. */
		struct fut_oncomplete *cb = c->cbs;
		while (cb != NULL) {
			struct fut_oncomplete *n = cb->next;
			__os_free(cb);
			cb = n;
		}
		(void)pthread_mutex_destroy(&c->lock);
		(void)pthread_cond_destroy(&c->cv);
		__os_free(c);
	}
}

/*
 * Complete the cell exactly once.  Returns 1 if this call did the
 * completion, 0 if it was already ready (idempotent losers).  Wakes all
 * waiters and fires all completion callbacks OUTSIDE the lock.
 */
static int
cell_complete(struct future_cell *c, intptr_t value, int status)
{
	struct fut_waiter *woke;
	struct fut_oncomplete *cbs;

	(void)__xtc_mtx_lock(&c->lock);
	if (c->ready) {
		(void)__xtc_mtx_unlock(&c->lock);
		return 0;
	}
	c->value = value;
	c->status = status;
	c->ready = 1;
	woke = c->wq_head;
	c->wq_head = c->wq_tail = NULL;
	cbs = c->cbs;
	c->cbs = NULL;
	(void)pthread_cond_broadcast(&c->cv);
	(void)__xtc_mtx_unlock(&c->lock);

	/* Wake fiber waiters. */
	while (woke != NULL) {
		struct fut_waiter *n = woke->next;
		(void)xtc_waker_wake(&woke->waker);
		woke = n;
	}
	/* Fire completion callbacks (combinator resolution).  Each pending
	 * callback owned one ref on this cell (taken in cell_on_complete);
	 * release it after firing. */
	while (cbs != NULL) {
		struct fut_oncomplete *n = cbs->next;
		cbs->cb(value, status, cbs->user);
		__os_free(cbs);
		cell_unref(c);
		cbs = n;
	}
	return 1;
}

/* Register a completion callback; if already ready, fire it immediately
 * (outside the lock).  Takes ONE REFERENCE on c that the callback owns
 * until it fires (so the cell outlives a pending combinator even after
 * the source handle is consumed).  Returns XTC_OK or XTC_E_NOMEM. */
static int
cell_on_complete(struct future_cell *c,
    void (*cb)(intptr_t, int, void *), void *user)
{
	struct fut_oncomplete *node;
	int fire_now = 0;
	intptr_t v = 0;
	int st = XTC_OK, rc;

	if ((rc = __os_malloc(sizeof *node, (void **)&node)) != XTC_OK)
		return rc;
	node->cb = cb;
	node->user = user;
	node->next = NULL;
	cell_ref(c);   /* the pending callback's ref */

	(void)__xtc_mtx_lock(&c->lock);
	if (c->ready) {
		fire_now = 1;
		v = c->value;
		st = c->status;
	} else {
		node->next = c->cbs;
		c->cbs = node;
	}
	(void)__xtc_mtx_unlock(&c->lock);

	if (fire_now) {
		cb(v, st, user);
		__os_free(node);
		cell_unref(c);   /* fired immediately: release its ref */
	}
	return XTC_OK;
}

/* ---- FIFO waiter helpers (same as sync.c) ---- */

static void
fut_wq_enqueue(struct future_cell *c, struct fut_waiter *w)
{
	w->next = NULL;
	if (c->wq_tail != NULL)
		c->wq_tail->next = w;
	else
		c->wq_head = w;
	c->wq_tail = w;
}

static void
fut_wq_remove(struct future_cell *c, struct fut_waiter *w)
{
	struct fut_waiter *p = c->wq_head, *prev = NULL;
	while (p != NULL) {
		if (p == w) {
			if (prev != NULL)
				prev->next = p->next;
			else
				c->wq_head = p->next;
			if (c->wq_tail == p)
				c->wq_tail = prev;
			return;
		}
		prev = p;
		p = p->next;
	}
}

/* ---- public: pair, set, drop ---- */

int
xtc_future_new_pair(xtc_promise_t **out_prom, xtc_future_t **out_fut)
{
	struct future_cell *c = NULL;
	xtc_promise_t *p = NULL;
	xtc_future_t *f = NULL;
	int rc;

	if (out_prom == NULL || out_fut == NULL)
		return XTC_E_INVAL;
	*out_prom = NULL;
	*out_fut = NULL;

	if ((rc = cell_new(&c)) != XTC_OK)
		return rc;
	if ((rc = __os_malloc(sizeof *p, (void **)&p)) != XTC_OK)
		goto fail;
	if ((rc = __os_malloc(sizeof *f, (void **)&f)) != XTC_OK)
		goto fail;

	cell_ref(c);   /* promise half */
	cell_ref(c);   /* future half */
	p->cell = c;
	p->set_or_dropped = 0;
	f->cell = c;
	*out_prom = p;
	*out_fut = f;
	return XTC_OK;

fail:
	__os_free(p);
	if (c != NULL) {
		(void)pthread_mutex_destroy(&c->lock);
		(void)pthread_cond_destroy(&c->cv);
		__os_free(c);
	}
	return rc;
}

int
xtc_promise_set(xtc_promise_t *prom, intptr_t value, int status)
{
	struct future_cell *c;
	int did;

	if (prom == NULL || prom->set_or_dropped)
		return XTC_E_INVAL;
	c = prom->cell;
	prom->set_or_dropped = 1;
	did = cell_complete(c, value, status);
	cell_unref(c);            /* release the promise's ref */
	__os_free(prom);
	return did ? XTC_OK : XTC_E_INVAL;
}

void
xtc_promise_drop(xtc_promise_t *prom)
{
	struct future_cell *c;
	if (prom == NULL || prom->set_or_dropped)
		return;
	c = prom->cell;
	prom->set_or_dropped = 1;
	/* A broken promise is an abort, never a hang. */
	(void)cell_complete(c, 0, XTC_E_ABORTED);
	cell_unref(c);
	__os_free(prom);
}

/* ---- public: await / wait / ready ---- */

/*
 * Shared body for await (timeout_ns forced negative, fiber-only in
 * spirit but tolerant) and wait (any timeout, fiber or thread).  On
 * completion, writes *out, returns the status, and CONSUMES the future
 * (frees the handle + drops its ref).  On timeout returns XTC_E_AGAIN
 * WITHOUT consuming.
 */
static int
future_get(xtc_future_t *fut, intptr_t *out, int64_t timeout_ns)
{
	struct future_cell *c;
	xtc_task_t *cur;
	int rc = XTC_OK;
	int timed_out = 0;

	if (fut == NULL || fut->cell == NULL)
		return XTC_E_INVAL;
	c = fut->cell;

	(void)__xtc_mtx_lock(&c->lock);
	if (c->ready)
		goto done_locked;
	if (timeout_ns == 0) {
		(void)__xtc_mtx_unlock(&c->lock);
		return XTC_E_AGAIN;
	}

	cur = __xtc_current_task();
	if (cur != NULL) {
		/* Fiber waiter: park and re-check on wake. */
		struct fut_waiter w;
		void *proc_ctx;
		int64_t deadline = -1;
		(void)xtc_task_waker(cur, &w.waker);
		fut_wq_enqueue(c, &w);
		if (timeout_ns > 0) {
			int64_t now;
			(void)__os_clock_mono(&now);
			deadline = now + timeout_ns;
		}
		for (;;) {
			int64_t now;
			if (c->ready) { rc = c->status; break; }
			if (deadline >= 0) {
				(void)__os_clock_mono(&now);
				if (now >= deadline) { timed_out = 1; break; }
				__xtc_task_cancel_park_timer(cur);
				(void)xtc_task_park_on_timer(cur, deadline - now);
			} else {
				cur->park_requested = 1;
			}
			(void)__xtc_mtx_unlock(&c->lock);
			proc_ctx = __xtc_proc_ctx_save();
			xtc_yield();
			__xtc_proc_ctx_restore(proc_ctx);
			(void)__xtc_mtx_lock(&c->lock);
		}
		fut_wq_remove(c, &w);
		__xtc_task_cancel_park_timer(cur);
		if (timed_out) {
			(void)__xtc_mtx_unlock(&c->lock);
			return XTC_E_AGAIN;   /* wait timed out; future NOT consumed */
		}
		if (out != NULL)
			*out = c->value;
		(void)__xtc_mtx_unlock(&c->lock);
		cell_unref(c);
		__os_free(fut);
		return rc;
	}

	/* Plain OS thread waiter: condvar. */
	if (timeout_ns < 0) {
		while (!c->ready)
			(void)pthread_cond_wait(&c->cv, &c->lock);
	} else {
		struct timespec ts;
		int64_t now_ns = 0;
		(void)__os_clock_real(&now_ns);
		now_ns += timeout_ns;
		ts.tv_sec = (time_t)(now_ns / 1000000000LL);
		ts.tv_nsec = (long)(now_ns % 1000000000LL);
		while (!c->ready) {
			if (pthread_cond_timedwait(&c->cv, &c->lock, &ts) != 0) {
				if (!c->ready) {
					(void)__xtc_mtx_unlock(&c->lock);
					return XTC_E_AGAIN;   /* not consumed */
				}
				break;
			}
		}
	}

done_locked:
	rc = c->status;
	if (out != NULL)
		*out = c->value;
	(void)__xtc_mtx_unlock(&c->lock);
	cell_unref(c);
	__os_free(fut);
	return rc;
}

int
xtc_future_await(xtc_future_t *fut, intptr_t *out)
{
	if (fut != NULL && __xtc_current_task() == NULL)
		return XTC_E_INVAL;   /* await is fiber-only; use _wait off-loop */
	return future_get(fut, out, -1);
}

int
xtc_future_wait(xtc_future_t *fut, intptr_t *out, int64_t timeout_ns)
{
	return future_get(fut, out, timeout_ns);
}

int
xtc_future_ready(xtc_future_t *fut, int *is_ready)
{
	if (fut == NULL || fut->cell == NULL || is_ready == NULL)
		return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&fut->cell->lock);
	*is_ready = fut->cell->ready ? 1 : 0;
	(void)__xtc_mtx_unlock(&fut->cell->lock);
	return XTC_OK;
}

/* ---- combinators ----
 *
 * Each combinator allocates a NEW (promise, future) pair for the
 * result, registers a completion callback on the source cell(s) that
 * resolves the new promise, then consumes the source future handle(s)
 * (dropping their refs -- the callback keeps the source cell alive via
 * its own ref while pending).
 */

struct map_ctx {
	xtc_promise_t     *prom;
	xtc_future_map_fn  fn;
	void              *user;
};

static void
map_on_complete(intptr_t value, int status, void *u)
{
	struct map_ctx *m = u;
	intptr_t mapped = m->fn(value, status, m->user);
	(void)xtc_promise_set(m->prom, mapped, status);
	__os_free(m);
}

int
xtc_future_map(xtc_future_t *src, xtc_future_map_fn fn, void *user,
    xtc_future_t **out_fut)
{
	xtc_promise_t *prom = NULL;
	xtc_future_t *fut = NULL;
	struct map_ctx *m = NULL;
	struct future_cell *sc;
	int rc;

	if (src == NULL || src->cell == NULL || fn == NULL || out_fut == NULL)
		return XTC_E_INVAL;
	if ((rc = __os_malloc(sizeof *m, (void **)&m)) != XTC_OK)
		return rc;
	if ((rc = xtc_future_new_pair(&prom, &fut)) != XTC_OK) {
		__os_free(m);
		return rc;
	}
	m->prom = prom;
	m->fn = fn;
	m->user = user;

	sc = src->cell;
	if ((rc = cell_on_complete(sc, map_on_complete, m)) != XTC_OK) {
		xtc_promise_drop(prom);
		cell_unref(fut->cell);
		__os_free(fut);
		__os_free(m);
		return rc;
	}
	/* cell_on_complete took its own ref on sc; consume the source
	 * HANDLE (drop its ref).  The pending callback keeps sc alive. */
	cell_unref(src->cell);
	__os_free(src);
	*out_fut = fut;
	return XTC_OK;
}

struct then_ctx {
	xtc_promise_t      *prom;
	xtc_future_then_fn  fn;
	void               *user;
};

/* When the inner (next) future completes, resolve the outer promise. */
struct then_inner_ctx {
	xtc_promise_t *prom;
};

static void
then_inner_on_complete(intptr_t value, int status, void *u)
{
	struct then_inner_ctx *ti = u;
	(void)xtc_promise_set(ti->prom, value, status);
	__os_free(ti);
}

static void
then_on_complete(intptr_t value, int status, void *u)
{
	struct then_ctx *t = u;
	xtc_future_t *next = NULL;
	int rc = t->fn(value, status, t->user, &next);
	if (rc != XTC_OK || next == NULL || next->cell == NULL) {
		/* fn declined to chain: propagate the source status. */
		(void)xtc_promise_set(t->prom, value,
		    rc != XTC_OK ? rc : status);
		if (next != NULL) {
			cell_unref(next->cell);
			__os_free(next);
		}
		__os_free(t);
		return;
	}
	{
		struct then_inner_ctx *ti;
		if (__os_malloc(sizeof *ti, (void **)&ti) != XTC_OK) {
			(void)xtc_promise_set(t->prom, 0, XTC_E_NOMEM);
			cell_unref(next->cell);
			__os_free(next);
			__os_free(t);
			return;
		}
		ti->prom = t->prom;
		if (cell_on_complete(next->cell, then_inner_on_complete, ti)
		    != XTC_OK) {
			(void)xtc_promise_set(t->prom, 0, XTC_E_NOMEM);
			__os_free(ti);
		}
		cell_unref(next->cell);   /* handle ref */
		__os_free(next);
	}
	__os_free(t);
}

int
xtc_future_then(xtc_future_t *src, xtc_future_then_fn fn, void *user,
    xtc_future_t **out_fut)
{
	xtc_promise_t *prom = NULL;
	xtc_future_t *fut = NULL;
	struct then_ctx *t = NULL;
	int rc;

	if (src == NULL || src->cell == NULL || fn == NULL || out_fut == NULL)
		return XTC_E_INVAL;
	if ((rc = __os_malloc(sizeof *t, (void **)&t)) != XTC_OK)
		return rc;
	if ((rc = xtc_future_new_pair(&prom, &fut)) != XTC_OK) {
		__os_free(t);
		return rc;
	}
	t->prom = prom;
	t->fn = fn;
	t->user = user;

	if ((rc = cell_on_complete(src->cell, then_on_complete, t)) != XTC_OK) {
		xtc_promise_drop(prom);
		cell_unref(fut->cell);
		__os_free(fut);
		__os_free(t);
		return rc;
	}
	cell_unref(src->cell);
	__os_free(src);
	*out_fut = fut;
	return XTC_OK;
}

/* when_all: a shared counter; the last input to complete resolves. */
struct all_ctx {
	xtc_promise_t  *prom;
	_Atomic int     remaining;
	int             total;
	_Atomic int     first_err;   /* first non-OK status, or XTC_OK */
	_Atomic int     err_set;
};

static void
all_on_complete(intptr_t value, int status, void *u)
{
	struct all_ctx *a = u;
	(void)value;
	if (status != XTC_OK) {
		int expected = 0;
		if (atomic_compare_exchange_strong_explicit(&a->err_set,
		    &expected, 1, memory_order_acq_rel, memory_order_relaxed))
			atomic_store_explicit(&a->first_err, status,
			    memory_order_relaxed);
	}
	if (atomic_fetch_sub_explicit(&a->remaining, 1,
	    memory_order_acq_rel) == 1) {
		int st = atomic_load_explicit(&a->err_set, memory_order_relaxed)
		    ? atomic_load_explicit(&a->first_err, memory_order_relaxed)
		    : XTC_OK;
		(void)xtc_promise_set(a->prom, (intptr_t)a->total, st);
		__os_free(a);
	}
}

int
xtc_future_when_all(xtc_future_t **futs, int n, xtc_future_t **out_fut)
{
	xtc_promise_t *prom = NULL;
	xtc_future_t *fut = NULL;
	struct all_ctx *a = NULL;
	int i, rc;

	if (futs == NULL || n <= 0 || out_fut == NULL)
		return XTC_E_INVAL;
	for (i = 0; i < n; i++)
		if (futs[i] == NULL || futs[i]->cell == NULL)
			return XTC_E_INVAL;
	if ((rc = __os_malloc(sizeof *a, (void **)&a)) != XTC_OK)
		return rc;
	if ((rc = xtc_future_new_pair(&prom, &fut)) != XTC_OK) {
		__os_free(a);
		return rc;
	}
	a->prom = prom;
	a->total = n;
	atomic_init(&a->remaining, n);
	atomic_init(&a->first_err, XTC_OK);
	atomic_init(&a->err_set, 0);

	for (i = 0; i < n; i++) {
		(void)cell_on_complete(futs[i]->cell, all_on_complete, a);
		cell_unref(futs[i]->cell);   /* handle ref */
		__os_free(futs[i]);
	}
	*out_fut = fut;
	return XTC_OK;
}

/* when_any: the first input to complete resolves; later ones are no-ops
 * (promise_set is idempotent -- second set returns INVAL, harmless). */
struct any_ctx {
	xtc_promise_t *prom;
	_Atomic int    done;
	_Atomic int    remaining;   /* to free the ctx after all fire */
};

static void
any_on_complete(intptr_t value, int status, void *u)
{
	struct any_ctx *a = u;
	int expected = 0;
	if (atomic_compare_exchange_strong_explicit(&a->done, &expected, 1,
	    memory_order_acq_rel, memory_order_relaxed))
		(void)xtc_promise_set(a->prom, value, status);
	if (atomic_fetch_sub_explicit(&a->remaining, 1,
	    memory_order_acq_rel) == 1)
		__os_free(a);
}

int
xtc_future_when_any(xtc_future_t **futs, int n, xtc_future_t **out_fut)
{
	xtc_promise_t *prom = NULL;
	xtc_future_t *fut = NULL;
	struct any_ctx *a = NULL;
	int i, rc;

	if (futs == NULL || n <= 0 || out_fut == NULL)
		return XTC_E_INVAL;
	for (i = 0; i < n; i++)
		if (futs[i] == NULL || futs[i]->cell == NULL)
			return XTC_E_INVAL;
	if ((rc = __os_malloc(sizeof *a, (void **)&a)) != XTC_OK)
		return rc;
	if ((rc = xtc_future_new_pair(&prom, &fut)) != XTC_OK) {
		__os_free(a);
		return rc;
	}
	a->prom = prom;
	atomic_init(&a->done, 0);
	atomic_init(&a->remaining, n);

	for (i = 0; i < n; i++) {
		(void)cell_on_complete(futs[i]->cell, any_on_complete, a);
		cell_unref(futs[i]->cell);
		__os_free(futs[i]);
	}
	*out_fut = fut;
	return XTC_OK;
}

/* with_timeout: race the source against a loop timer. */
struct timeout_ctx {
	xtc_promise_t *prom;
	xtc_timer_t   *timer;
	_Atomic int    done;
	_Atomic int    remaining;   /* source cb + timer cb both must retire it */
};

static void
timeout_retire(struct timeout_ctx *t)
{
	if (atomic_fetch_sub_explicit(&t->remaining, 1,
	    memory_order_acq_rel) == 1)
		__os_free(t);
}

static void
timeout_src_complete(intptr_t value, int status, void *u)
{
	struct timeout_ctx *t = u;
	int expected = 0;
	if (atomic_compare_exchange_strong_explicit(&t->done, &expected, 1,
	    memory_order_acq_rel, memory_order_relaxed)) {
		(void)xtc_promise_set(t->prom, value, status);
		if (t->timer != NULL)
			(void)xtc_timer_cancel(t->timer);
	}
	timeout_retire(t);
}

static void
timeout_fire(void *u)
{
	struct timeout_ctx *t = u;
	int expected = 0;
	if (atomic_compare_exchange_strong_explicit(&t->done, &expected, 1,
	    memory_order_acq_rel, memory_order_relaxed))
		(void)xtc_promise_set(t->prom, 0, XTC_E_AGAIN);
	timeout_retire(t);
}

int
xtc_future_with_timeout(xtc_future_t *src, int64_t timeout_ns,
    xtc_future_t **out_fut)
{
	xtc_promise_t *prom = NULL;
	xtc_future_t *fut = NULL;
	struct timeout_ctx *t = NULL;
	xtc_loop_t *loop;
	int rc;

	if (src == NULL || src->cell == NULL || out_fut == NULL)
		return XTC_E_INVAL;
	loop = __xtc_current_loop;
	if (loop == NULL)
		return XTC_E_INVAL;   /* needs a running loop for the timer */
	if ((rc = __os_malloc(sizeof *t, (void **)&t)) != XTC_OK)
		return rc;
	if ((rc = xtc_future_new_pair(&prom, &fut)) != XTC_OK) {
		__os_free(t);
		return rc;
	}
	t->prom = prom;
	t->timer = NULL;
	atomic_init(&t->done, 0);
	atomic_init(&t->remaining, 2);   /* source cb + timer cb */

	/* Arm the timer first so a source that completes synchronously in
	 * cell_on_complete still finds a timer to cancel. */
	if ((rc = xtc_timer_set(loop, timeout_ns, timeout_fire, t,
	    &t->timer)) != XTC_OK) {
		/* No timer: fall back to a plain forward (no timeout). */
		atomic_store_explicit(&t->remaining, 1, memory_order_relaxed);
	}

	cell_on_complete(src->cell, timeout_src_complete, t);
	cell_unref(src->cell);
	__os_free(src);
	*out_fut = fut;
	return XTC_OK;
}
