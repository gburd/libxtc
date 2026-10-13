/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 */
#include "xtc_int.h"
#include "xtc_batch.h"

#ifdef XTC_ENABLE_SCHED_POLICY
#include "xtc_dst_inject.h" /* DST bug-injection harness (no-op in prod) */
#include "loop_int.h"
#include "preempt_int.h"
#include "xtc_accel.h"
#include "xtc_async.h"
#include "xtc_blocking.h"
#include "xtc_chan.h"
#include "xtc_future.h"
#include "future_int.h"
#include "proc_int.h"
#include "xtc_proc.h"
#include "xtc_sync.h"
#include "os_time.h"
#include <pthread.h>
#include <stdio.h> /* sysfs numa_node read in the placement helper */
#include <stdlib.h> /* abort */

/* src/ptc/accel.c (internal): fence wait to an absolute deadline with a
 * stop predicate consulted after every wake. */
int __xtc_accel_fence_wait(int, int64_t, int (*)(void *), void *);

enum batch_slot_state {
	B_FREE,
	B_WAITING,
	B_READY,
	B_BLOCKED,
	B_INFLIGHT,
	B_TERMINAL,
	B_DELIVERING
};
/* Slots live on exactly one index-linked list per state class, so every
 * per-boundary operation is O(members touched), not O(capacity):
 *   free     B_FREE
 *   wait     B_WAITING, in arrival order (FCFS admission takes the head)
 *   active   B_READY / B_BLOCKED / B_INFLIGHT, in admission order (the
 *            vector is built by walking it)
 *   term     B_TERMINAL, in the order outcomes were recorded
 * A B_DELIVERING slot is on no list (b->delivering names it). A cancel that
 * lands on a WAITING/READY/BLOCKED slot also links the slot on the pending-
 * cancel list (cnext) consumed at the next boundary. */
#define B_NIL UINT32_MAX
#define B_NSTATES 7
struct batch_slot {
	uint64_t handle, charge;
	void *request;
	enum batch_slot_state state;
	int outcome, cancel, on_clist, hinted;
	uint32_t prev, next, cnext;
};
struct batch_list {
	uint32_t head, tail;
};
/* Open-addressing handle map (linear probing, backward-shift delete, load
 * <= 1/2). val < capacity is a slot index; capacity + k is held record k. */
struct batch_hent {
	uint64_t key; /* 0 = empty (handles are nonzero) */
	uint32_t val;
};
/* At most one offloaded op per group (one per iteration). The frozen vector
 * is b->items/b->results[0..n); `idx` records which slot each vector member
 * belongs to. `valid` is the single token: set when submit succeeds, cleared
 * when batch_collect_inflight consumes it -- so double-terminalize is
 * structurally impossible.
 *
 * FUTURE kind: the device future is chained (xtc_future_map) to a heap
 * token. When the application resolves its promise, from any thread, the
 * token records the status and signals the group notify; the driver parks
 * on that notify, so a device completion, an owner cancel, a cancel-close,
 * a cross-loop cancel and a kill all wake the same single wait. The token
 * is refcounted (the map callback holds one ref, the engine the other) and
 * the engine detaches it under its lock at finalize, so a promise resolved
 * after the group is destroyed touches only the token. `wait` is the
 * mapped future (engine-owned, discarded at finalize); if the map could
 * not be allocated, `wait` is the device future itself and the driver
 * polls it in 1 ms slices instead. */
struct batch_tok {
	pthread_mutex_t lock;
	xtc_batch_t *b; /* NULL once the engine detached */
	int refs, done, status;
};
struct batch_inflight {
	xtc_batch_exec_t exec;
	struct batch_tok *tok;
	xtc_future_t *wait;
	uint32_t *idx;
	size_t n;
	int valid;
};

/* Cross-loop admission (S2). One group-wide bounded MPSC ring carries SUBMIT,
 * CANCEL and RESUME control records in FIFO order to the single driver. The
 * records are drawn from a preallocated pool seeded into a SECOND MPSC
 * "free-pool" channel (claim = try_recv, release = try_send), so submit_mt is
 * allocation-free and the ABA hazard of a bespoke lock-free free-list is
 * avoided by reusing the already-proven channel. See the design's
 * "Decisions locked" record-pool text. */
enum batch_mt_op { MT_SUBMIT, MT_CANCEL, MT_RESUME };
struct batch_mt_rec {
	enum batch_mt_op op;
	uint64_t handle, charge;
	void *request;
	struct batch_mt_rec *next; /* held FIFO link (driver only) */
	int cancel;                /* cancel_mt landed while held */
};
struct batch_mt {
	xtc_chan_mpsc_t *ring;      /* producers -> driver, control records */
	xtc_chan_mpmc_t *pool;      /* free record pointers (claim/release) */
	struct batch_mt_rec *recs;  /* preallocated record storage */
	uint32_t cap;               /* mt_queue_cap (recs length) */
	/* SUBMITs drained while every slot was taken. They were accepted
	 * (the producer holds a handle), so they wait here in arrival order
	 * and are admitted as slots free; their records stay out of the pool,
	 * which is the producer-side backpressure. Driver only. */
	struct batch_mt_rec *held, *held_tail;
	uint32_t n_held;
	/* Producer-side stat counters. submit_mt/cancel_mt/resume_mt run on
	 * ANY thread and bump these concurrently, so they are atomic; the
	 * driver-only astats fields (offloads/offload_*) stay plain. A relaxed
	 * fetch-add is enough: these are monotonic totals, not ordering
	 * barriers, and a wrap at 2^64 on a lifetime counter is a non-issue. */
	_Atomic uint64_t n_queued;
	_Atomic uint64_t n_rejections;
	_Atomic int gate;           /* 0 open, 1 closed, 2 driver done */
	_Atomic uint32_t inflight;  /* producers between gate and push */
};

struct xtc_batch {
	xtc_loop_t *loop;
	xtc_batch_opts_t opts;
	xtc_batch_stats_t stats;
	xtc_notify_t *notify;
	xtc_pid_t driver;
	struct batch_slot *slots;
	xtc_batch_item_t *items;
	xtc_batch_result_t *results;
	uint32_t *indices;
	struct batch_list l_free, l_wait, l_active, l_term;
	uint32_t n_state[B_NSTATES];
	uint32_t clist;      /* pending-cancel list head (slot index) */
	uint32_t delivering; /* the B_DELIVERING slot, or B_NIL */
	struct batch_hent *map;
	size_t map_mask;
	uint32_t live;
	uint64_t last_handle;
	/* Group-global monotonic handle source. Owner-loop submit (under the
	 * bracket) and submit_mt (off-thread) draw from the SAME atomic space
	 * so handles are never reused across the two paths. last_handle above
	 * mirrors the owner-loop view and backs the exhaustion test hook. */
	_Atomic uint64_t handle_ctr;
	int closed, cancelling, callback, class_index;
	/* Async offload (xtc_batch_create_async). async == 0 => classic sync
	 * step; the fields below are unused and the engine takes the sync
	 * path verbatim. */
	int async;
	int64_t exec_timeout_ns;
	int (*submit)(const xtc_batch_item_t *, size_t, void *,
	              xtc_batch_exec_t *);
	int (*collect)(const xtc_batch_item_t *, xtc_batch_result_t *, size_t,
	               int, void *);
	void (*cancel_hint)(uint64_t, void *);
	struct batch_inflight inflight;
	struct batch_mt mt;
	uint32_t mt_cap_req; /* requested mt_queue_cap; 0 => mt disabled */
	xtc_batch_async_stats_t astats;
};

static uint32_t
batch_active(const xtc_batch_t *b)
{
	return b->n_state[B_READY] + b->n_state[B_BLOCKED]
	    + b->n_state[B_INFLIGHT];
}

/* Publish only committed membership. Called under the owner's short bracket. */
static void
batch_hint(xtc_batch_t *b)
{
	if (b->class_index < 0)
		return;
	__xtc_sched_batch_hint(b->loop, b->class_index, b->n_state[B_READY],
	                       batch_active(b));
}

static struct batch_list *
batch_list_of(xtc_batch_t *b, enum batch_slot_state st)
{
	switch (st) {
	case B_FREE:
		return &b->l_free;
	case B_WAITING:
		return &b->l_wait;
	case B_READY:
	case B_BLOCKED:
	case B_INFLIGHT:
		return &b->l_active;
	case B_TERMINAL:
		return &b->l_term;
	default:
		return NULL;
	}
}

static void
batch_list_push(xtc_batch_t *b, struct batch_list *l, uint32_t i)
{
	b->slots[i].next = B_NIL;
	b->slots[i].prev = l->tail;
	if (l->tail != B_NIL)
		b->slots[l->tail].next = i;
	else
		l->head = i;
	l->tail = i;
}

static void
batch_list_unlink(xtc_batch_t *b, struct batch_list *l, uint32_t i)
{
	struct batch_slot *s = &b->slots[i];
	if (s->prev != B_NIL)
		b->slots[s->prev].next = s->next;
	else
		l->head = s->next;
	if (s->next != B_NIL)
		b->slots[s->next].prev = s->prev;
	else
		l->tail = s->prev;
	s->prev = s->next = B_NIL;
}

/* The one state-transition primitive: keeps lists and counters exact. A
 * move within one list class keeps the slot's position (admission order). */
static void
batch_move(xtc_batch_t *b, struct batch_slot *s, enum batch_slot_state to)
{
	uint32_t i = (uint32_t)(s - b->slots);
	struct batch_list *from = batch_list_of(b, s->state);
	struct batch_list *dst = batch_list_of(b, to);
	b->n_state[s->state]--;
	b->n_state[to]++;
	if (from != dst) {
		if (from != NULL)
			batch_list_unlink(b, from, i);
		if (dst != NULL)
			batch_list_push(b, dst, i);
	}
	s->state = to;
}

static size_t
batch_map_home(const xtc_batch_t *b, uint64_t key)
{
	uint64_t h = key * 0x9E3779B97F4A7C15ULL;
	h ^= h >> 29;
	return (size_t)h & b->map_mask;
}

static void
batch_map_put(xtc_batch_t *b, uint64_t key, uint32_t val)
{
	size_t i = batch_map_home(b, key);
	while (b->map[i].key != 0 && b->map[i].key != key)
		i = (i + 1) & b->map_mask;
	b->map[i].key = key;
	b->map[i].val = val;
}

/* Returns 1 and *val, or 0 if absent. */
static int
batch_map_get(const xtc_batch_t *b, uint64_t key, uint32_t *val)
{
	size_t i;
	if (key == 0)
		return 0;
	for (i = batch_map_home(b, key); b->map[i].key != 0;
	     i = (i + 1) & b->map_mask)
		if (b->map[i].key == key) {
			*val = b->map[i].val;
			return 1;
		}
	return 0;
}

static void
batch_map_del(xtc_batch_t *b, uint64_t key)
{
	size_t i, j, k;
	for (i = batch_map_home(b, key); b->map[i].key != key;
	     i = (i + 1) & b->map_mask)
		if (b->map[i].key == 0)
			return;
	/* Backward-shift: pull later probes of the same run into the hole. */
	for (j = i;;) {
		j = (j + 1) & b->map_mask;
		if (b->map[j].key == 0)
			break;
		k = batch_map_home(b, b->map[j].key);
		if (((j - k) & b->map_mask) >= ((j - i) & b->map_mask)) {
			b->map[i] = b->map[j];
			i = j;
		}
	}
	b->map[i].key = 0;
}

static void
batch_inc(uint64_t *n)
{
	if (*n != UINT64_MAX)
		++*n;
}

/* Producer-side counter bump. Relaxed fetch-add from any thread; wrap at
 * 2^64 on a lifetime total is accepted (see struct batch_mt). */
static void
batch_inc_atomic(_Atomic uint64_t *n)
{
	(void)atomic_fetch_add_explicit(n, 1, memory_order_relaxed);
}

static int
batch_error(int rc)
{
	return rc <= XTC_E_INVAL && rc >= XTC_E_IO;
}

/* Stamp the next group-global handle (0 reserved). Returns 0 on exhaustion
 * (the counter already at UINT64_MAX), else a fresh never-reused handle.
 * Atomic so owner-loop submit and off-thread submit_mt share one space. */
static uint64_t
batch_next_handle(xtc_batch_t *b)
{
	/* CAS, not fetch-add: the counter must never pass through a wrapped
	 * value another producer could stamp. */
	uint64_t cur =
	    atomic_load_explicit(&b->handle_ctr, memory_order_relaxed);
	do {
		if (cur == UINT64_MAX)
			return 0;
	} while (!atomic_compare_exchange_weak_explicit(&b->handle_ctr, &cur,
	    cur + 1, memory_order_relaxed, memory_order_relaxed));
	return cur + 1;
}

/* NULL TLS is allowed only for externally serialized setup, or explicitly
 * quiescent stats/destroy. It is NOT evidence that another thread is idle. */
static int
batch_owner(xtc_batch_t *b, int quiescent)
{
	if (b == NULL)
		return XTC_E_INVAL;
	if (__xtc_current_loop == b->loop)
		return XTC_OK;
	if (__xtc_current_loop != NULL)
		return XTC_E_INVAL;
	if (quiescent
	    || atomic_load_explicit(&b->loop->sched.phase, memory_order_acquire)
	           == 0)
		return XTC_OK;
	return XTC_E_INVAL;
}

/* Caller holds the short unsafe bracket, never across user code or a park. */
static int
batch_mutable(xtc_batch_t *b)
{
	int rc = batch_owner(b, 0);
	if (rc == XTC_OK && b->callback)
		rc = XTC_E_DEADLK;
	return rc;
}

/* O(1) handle -> live slot. */
static struct batch_slot *
batch_find(xtc_batch_t *b, uint64_t handle)
{
	uint32_t v, i;
	if (batch_map_get(b, handle, &v) && v < b->opts.capacity
	    && b->slots[v].state != B_FREE)
		return &b->slots[v];
#if XTC_DST_BUG(XTC_DST_BUG_BATCHSTALE)
	/* Planted bug: ignore handle identity, take any live slot. */
	for (i = 0; i < b->opts.capacity; ++i)
		if (b->slots[i].state != B_FREE)
			return &b->slots[i];
#else
	(void)i;
#endif
	return NULL;
}

/* Record a cancel. A not-yet-running member is linked for the next boundary
 * to consume. */
static void
batch_mark_cancel(xtc_batch_t *b, struct batch_slot *s)
{
	s->cancel = 1;
	if (!s->on_clist
	    && (s->state == B_WAITING || s->state == B_READY
	        || s->state == B_BLOCKED)) {
		s->on_clist = 1;
		s->cnext = b->clist;
		b->clist = (uint32_t)(s - b->slots);
	}
}

static void
batch_terminal(xtc_batch_t *b, struct batch_slot *s, int outcome)
{
	if (s->state == B_READY || s->state == B_BLOCKED
	    || s->state == B_INFLIGHT) {
#if !XTC_DST_BUG(XTC_DST_BUG_BATCHRES)
		b->stats.reserved_units -= s->charge;
#endif
	}
	batch_move(b, s, B_TERMINAL);
	s->outcome = outcome;
	batch_hint(b);
}

/* Consume recorded cancels (and a cancel-close) before any member is
 * exposed. O(cancels) normally; O(live) once when cancelling. */
static void
batch_consume_cancels(xtc_batch_t *b)
{
	struct batch_slot *s;
	uint32_t i, nx;
	for (i = b->clist; i != B_NIL; i = nx) {
		s = &b->slots[i];
		nx = s->cnext;
		s->cnext = B_NIL;
		s->on_clist = 0;
		if (s->cancel
		    && (s->state == B_WAITING || s->state == B_READY
		        || s->state == B_BLOCKED))
			batch_terminal(b, s, XTC_E_ABORTED);
	}
	b->clist = B_NIL;
	if (!b->cancelling)
		return;
	for (i = b->l_wait.head; i != B_NIL; i = nx) {
		nx = b->slots[i].next;
		batch_terminal(b, &b->slots[i], XTC_E_ABORTED);
	}
	for (i = b->l_active.head; i != B_NIL; i = nx) {
		nx = b->slots[i].next;
		if (b->slots[i].state != B_INFLIGHT)
			batch_terminal(b, &b->slots[i], XTC_E_ABORTED);
	}
}

static void batch_mt_admit_held(xtc_batch_t *);
static void batch_mt_drain(xtc_batch_t *);
static int batch_mt_quiesced(xtc_batch_t *);

static void
batch_retire(xtc_batch_t *b, struct batch_slot *s)
{
	if (s->outcome == XTC_OK)
		batch_inc(&b->stats.completed);
	else if (s->outcome == XTC_E_ABORTED)
		batch_inc(&b->stats.cancelled);
	else
		batch_inc(&b->stats.failed);
	batch_map_del(b, s->handle);
	if (b->delivering == (uint32_t)(s - b->slots))
		b->delivering = B_NIL;
	s->handle = 0;
	s->charge = 0;
	s->request = NULL;
	s->outcome = 0;
	s->cancel = 0;
	s->hinted = 0;
	batch_move(b, s, B_FREE);
	--b->live;
	batch_mt_admit_held(b);
}

static void
batch_deliver(xtc_batch_t *b)
{
	struct batch_slot *s;
	for (;;) {
		(void)xtc_mask_enter();
		__xtc_unsafe_enter();
		if (b->l_term.head == B_NIL) {
			__xtc_unsafe_leave();
			(void)xtc_mask_leave();
			return;
		}
		s = &b->slots[b->l_term.head];
		/* Persistent delivery-started survives an explicit callback
		 * exit. */
		batch_move(b, s, B_DELIVERING);
		b->delivering = (uint32_t)(s - b->slots);
		b->callback = 1;
		__xtc_unsafe_leave();
		b->opts.complete(s->handle, s->request, s->outcome,
		                 b->opts.user);
		__xtc_unsafe_enter();
		batch_retire(b, s);
		b->callback = 0;
		__xtc_unsafe_leave();
		(void)xtc_mask_leave();
	}
}

static void
batch_tok_unref(struct batch_tok *t)
{
	int left;
	(void)__xtc_mtx_lock(&t->lock);
	left = --t->refs;
	(void)__xtc_mtx_unlock(&t->lock);
	if (left == 0) {
		(void)pthread_mutex_destroy(&t->lock);
		__os_free(t);
	}
}

/* xtc_future_map callback: runs once, on the thread that resolved (or
 * dropped) the application's promise. */
static intptr_t
batch_tok_fire(intptr_t value, int status, void *user)
{
	struct batch_tok *t = user;
	(void)__xtc_mtx_lock(&t->lock);
	t->done = 1;
	t->status = status;
	if (t->b != NULL) {
		(void)xtc_notify_signal(t->b->notify);
		(void)xtc_loop_wake(t->b->loop);
	}
	(void)__xtc_mtx_unlock(&t->lock);
	batch_tok_unref(t);
	return value;
}

/* Fire cancel_hint, once per member, for every in-flight member whose
 * cancel (owner cancel, cancel_mt, or cancel-close) has landed -- or, with
 * `all`, for every in-flight member (forced teardown). Runs on the driver
 * as soon as it observes the cancel, while the op may still be running.
 * Caller must not hold the unsafe bracket (the hint is user code; mutators
 * return XTC_E_DEADLK during it, as during collect). */
static void
batch_fire_hints(xtc_batch_t *b, int all)
{
	struct batch_slot *s;
	size_t j;
	if (b->cancel_hint == NULL || !b->inflight.valid)
		return;
	for (j = 0; j < b->inflight.n; ++j) {
		__xtc_unsafe_enter();
		s = &b->slots[b->inflight.idx[j]];
		if (s->hinted || !(all || s->cancel || b->cancelling)) {
			__xtc_unsafe_leave();
			continue;
		}
		s->hinted = 1;
		b->callback = 1;
		__xtc_unsafe_leave();
		b->cancel_hint(s->handle, b->opts.user);
		__xtc_unsafe_enter();
		b->callback = 0;
		__xtc_unsafe_leave();
	}
}

/* THE SINGLE FINALIZER for an offloaded op. collect() runs exactly once; each
 * in-flight member is finalized by exactly one call to this. Reached from (1)
 * the driver after the token wait returns and (2) batch_cleanup on forced
 * teardown (exec_status XTC_BATCH_EXEC_ABORTED), only when inflight.valid.
 * Caller must NOT hold the unsafe bracket (collect is user code and may yield).
 * The single-token inflight.valid flag, cleared here, makes a second
 * terminalize of the same member structurally impossible.
 *
 * Unless the op is known finished (exec_status OK), no member re-enters a
 * vector: after TIMEOUT a member collect leaves CONTINUE/BLOCKED/UNSET fails
 * with XTC_E_AGAIN, after ABORTED every member is cancelled, and after a
 * negative status (the engine could not await the op) every member fails
 * with that code. The device op may still be running in all three cases,
 * so its buffers are never handed to a new submit. */
static void
batch_collect_inflight(xtc_batch_t *b, int exec_status)
{
	struct batch_slot *s;
	xtc_batch_result_t *r;
	size_t n, j;
	int rc, aborted;
	batch_fire_hints(b, exec_status == XTC_BATCH_EXEC_ABORTED);
	__xtc_unsafe_enter();
#if defined(XTC_DIAGNOSTIC)
	if (!b->inflight.valid)
		abort(); /* finalizer reached with no outstanding op */
#endif
	n = b->inflight.n;
	b->callback = 1;
	__xtc_unsafe_leave();
	rc = b->collect(b->items, b->results, n, exec_status, b->opts.user);
	__xtc_unsafe_enter();
	for (j = 0; j < n; ++j) {
		s = &b->slots[b->inflight.idx[j]];
		r = &b->results[j];
		aborted = s->cancel || exec_status == XTC_BATCH_EXEC_ABORTED;
		if (aborted)
			batch_terminal(b, s, XTC_E_ABORTED);
		else if (exec_status < 0)
			batch_terminal(b, s, batch_error(exec_status) ?
			                         exec_status : XTC_E_INTERNAL);
		else if (rc != XTC_OK)
			batch_terminal(b, s,
			               batch_error(rc) ? rc : XTC_E_INTERNAL);
		else if (r->state == XTC_BATCH_DONE)
			batch_terminal(b, s, XTC_OK);
		else if (r->state == XTC_BATCH_ERROR)
			batch_terminal(b, s,
			               batch_error(r->error) ? r->error :
			                                       XTC_E_INTERNAL);
		else if (exec_status == XTC_BATCH_EXEC_TIMEOUT)
			batch_terminal(b, s, XTC_E_AGAIN); /* op may be live */
		else if (r->state == XTC_BATCH_BLOCKED)
			batch_move(b, s, B_BLOCKED);
		else if (r->state == XTC_BATCH_CONTINUE)
			batch_move(b, s, B_READY); /* re-enters next vector */
		else
			batch_terminal(b, s, XTC_E_INTERNAL);
	}
	if (exec_status == XTC_BATCH_EXEC_TIMEOUT)
		batch_inc(&b->astats.offload_timeouts);
	else if (exec_status < 0)
		batch_inc(&b->astats.offload_errors);
	/* Reclaim the engine-owned FUTURE state exactly once: detach the token
	 * (a later promise resolution then touches only the token) and discard
	 * the engine's future handle. */
	if (b->inflight.tok != NULL) {
		(void)__xtc_mtx_lock(&b->inflight.tok->lock);
		b->inflight.tok->b = NULL;
		(void)__xtc_mtx_unlock(&b->inflight.tok->lock);
		batch_tok_unref(b->inflight.tok);
		b->inflight.tok = NULL;
	}
	if (b->inflight.wait != NULL) {
		__xtc_future_discard(b->inflight.wait);
		b->inflight.wait = NULL;
	}
	b->inflight.exec.future = NULL;
	b->inflight.valid = 0;
	b->inflight.n = 0;
	b->callback = 0;
	batch_hint(b);
	__xtc_unsafe_leave();
}

/* Installed before the first callback/yield. Proc-table removal happens AFTER
 * this hook and any other (possibly parking) hooks, so this is not a join. */
static void
batch_cleanup(void *arg)
{
	xtc_batch_t *b = arg;
	struct batch_slot *s;
	uint32_t i;
	/* An outstanding op is finalized FIRST (collect still runs, every member
	 * ABORTED exactly once), before the slot loop; that loop then never sees
	 * a B_INFLIGHT slot. */
	if (b->inflight.valid)
		batch_collect_inflight(b, XTC_BATCH_EXEC_ABORTED);
	__xtc_unsafe_enter();
#if defined(XTC_DIAGNOSTIC)
	/* After the finalizer, no slot may be B_INFLIGHT and no record remains. */
	if (b->inflight.valid)
		abort();
	for (i = 0; i < b->opts.capacity; ++i)
		if (b->slots[i].state == B_INFLIGHT)
			abort();
#endif
	b->closed = 1;
	b->callback = 0;
	if (b->delivering != B_NIL)
		batch_retire(b, &b->slots[b->delivering]);
	/* Each delivery round frees slots that held hand-ins then take, and a
	 * producer that passed the admission gate may still push: repeat until
	 * no accepted request is left anywhere. */
	for (;;) {
		batch_mt_drain(b);
		if (b->live == 0 && b->mt.n_held == 0) {
			if (batch_mt_quiesced(b))
				break;
			__xtc_unsafe_leave();
			xtc_yield();
			__xtc_unsafe_enter();
			continue;
		}
		while ((i = b->l_wait.head) != B_NIL || (i = b->l_active.head)
		    != B_NIL) {
			s = &b->slots[i];
			batch_terminal(b, s,
			               b->cancelling || s->cancel ?
			                   XTC_E_ABORTED :
			                   XTC_E_INTERNAL);
		}
		__xtc_unsafe_leave();
		batch_deliver(b);
		__xtc_unsafe_enter();
	}
	__xtc_unsafe_leave();
}

static size_t
batch_vector(xtc_batch_t *b)
{
	struct batch_slot *s;
	uint32_t i;
	size_t n = 0;
#if defined(XTC_DIAGNOSTIC)
	/* The frozen-vector invariant: batch_vector is the ONE call site that
	 * builds b->items/b->results/b->indices, and it must NEVER run between
	 * a submit and its collect (admission into a frozen in-flight vector).
	 * An outstanding inflight record here means exactly that corruption. */
	if (b->inflight.valid)
		abort();
#endif
	/* A peer can run between delivery and vector construction. Consume its
	 * cancellation under this same bracket before exposing any member. */
	batch_consume_cancels(b);
	/* FCFS admission: the wait list is in arrival order, so only its head
	 * is ever a candidate. O(admitted). */
#if XTC_DST_BUG(XTC_DST_BUG_BATCHCAP)
	while (batch_active(b) < b->opts.capacity) {
#else
	while (batch_active(b) < b->opts.max_active) {
#endif
		if (b->l_wait.head == B_NIL)
			break;
		s = &b->slots[b->l_wait.head];
		if (s->charge > b->opts.resource_capacity
		                    - b->stats.reserved_units)
			break;
		batch_move(b, s, B_READY);
		b->stats.reserved_units += s->charge;
	}
	/* The vector is the READY members in admission order: one walk of the
	 * active list, O(max_active). */
	for (i = b->l_active.head; i != B_NIL; i = s->next) {
		s = &b->slots[i];
		if (s->state != B_READY)
			continue;
		b->indices[n] = i;
		b->items[n].handle = s->handle;
		b->items[n].request = s->request;
		b->results[n].state = XTC_BATCH_UNSET;
		b->results[n].error = 0;
		n++;
	}
	batch_hint(b);
	return n;
}

/* ----- cross-loop admission (S2) --------------------------------------- *
 *
 * submit_mt/cancel_mt/resume_mt run on ANY loop or plain thread and touch
 * ONLY the lock-free ring + free-pool + atomic handle counter + the atomic
 * admission gate (+ a wake of the owner loop). They NEVER read or write
 * b->slots, the handle map, the held FIFO, or any other driver-owned state.
 * The driver (owner loop) is the only mutator of those, so there is no data
 * race between a producer and the driver.
 *
 * Admission gate (closed groups): gate is 0 while open, 1 once the group is
 * closed (SUBMIT refused), 2 once the driver has finished (every mt call
 * refused). A producer announces itself in `inflight` BEFORE reading the
 * gate and leaves AFTER its push; the driver moves the gate BEFORE reading
 * inflight. With seq_cst on both sides (Dekker), a producer that saw the
 * gate open has either finished its push or is still counted, so the
 * driver's final drain, taken after it sees inflight == 0, finds every
 * record that will ever be pushed. A SUBMIT that passed the gate before
 * close is an accepted request: it completes exactly once (DRAIN finishes
 * it, CANCEL aborts it). */

static int batch_alloc(size_t count, size_t size, void **out);

/* Create the ring, free-pool and record storage, and seed the free-pool
 * with every record pointer. Charged through the group's loop res. mt stays
 * disabled (ring == NULL) when cap == 0. */
static int
batch_mt_init(xtc_batch_t *b, uint32_t cap)
{
	xtc_res_t *res = xtc_loop_res(b->loop);
	uint32_t i;
	int rc;
	if (cap == 0)
		return XTC_OK;
	if ((rc = batch_alloc(cap, sizeof(*b->mt.recs),
	                      (void **)&b->mt.recs))
	        != XTC_OK
	    || (rc = xtc_chan_mpsc_create(res, cap, &b->mt.ring)) != XTC_OK
	    /* The pool only recirculates the preallocated records, so it is
	     * not charged per slot: a release must never fail for want of a
	     * resource unit (see batch_mt_release). */
	    || (rc = xtc_chan_mpmc_create(NULL, cap, &b->mt.pool)) != XTC_OK)
		return rc;
	b->mt.cap = cap;
	atomic_store_explicit(&b->mt.n_queued, 0, memory_order_relaxed);
	atomic_store_explicit(&b->mt.n_rejections, 0, memory_order_relaxed);
	/* Seed the free-pool with each record pointer.  The pool capacity is
	 * cap (its backing ring may round up internally), so all cap records
	 * always fit. */
	for (i = 0; i < cap; ++i)
		if ((rc = xtc_chan_mpmc_try_send(b->mt.pool, &b->mt.recs[i]))
		    != XTC_OK)
			return rc;
	return XTC_OK;
}

static void
batch_mt_fini(xtc_batch_t *b)
{
	if (b->mt.ring != NULL) {
		(void)xtc_chan_mpsc_close(b->mt.ring);
		xtc_chan_mpsc_destroy(b->mt.ring);
		b->mt.ring = NULL;
	}
	if (b->mt.pool != NULL) {
		(void)xtc_chan_mpmc_close(b->mt.pool);
		xtc_chan_mpmc_destroy(b->mt.pool);
		b->mt.pool = NULL;
	}
	__os_free(b->mt.recs);
	b->mt.recs = NULL;
}

/* Producer side: claim a free record (NULL if the pool is empty). The pool is
 * an mpmc channel so concurrent producer claims each pop a distinct record
 * (its try_recv serializes consumers under a lock); an mpsc pool would hand
 * the same record to two racing claimers. */
static struct batch_mt_rec *
batch_mt_claim(xtc_batch_t *b)
{
	void *p = NULL;
	if (xtc_chan_mpmc_try_recv(b->mt.pool, &p) != XTC_OK)
		return NULL;
	return p;
}

/* Return a record to the free-pool.  Conservation invariant: a record is
 * only ever released after it was claimed from this same pool, so
 * claimed + in-pool == cap always and the pool can never be legitimately
 * full here, and the pool is uncharged so no resource limit applies.  A
 * try_send can therefore only fail transiently (a DST buggify spurious-
 * full, a fresh draw per call); retry until it lands so a record is never
 * lost to circulation, which would slowly starve the pool.  2^20
 * consecutive failures cannot happen while the invariant holds, so that
 * is treated as the invariant violation it is: fail-stop, not a silent
 * drop. */
static void
batch_mt_release(xtc_batch_t *b, struct batch_mt_rec *rec)
{
	int tries = 0;
	while (xtc_chan_mpmc_try_send(b->mt.pool, rec) != XTC_OK)
		if (++tries >= 1 << 20)
			abort();
}

/* Producer side: push a filled record onto the ring, wake the driver. On a
 * full ring the record is returned to the pool and XTC_E_AGAIN is reported.
 * Closed groups are refused before this, by the admission gate. Freedom
 * from use-after-free rests on the caller's owner-outlives-group obligation
 * (no producer in flight when destroy frees the ring), documented in
 * xtc_batch.3; the group heap and ring are not freed until the driver proc
 * is gone. */
static int
batch_mt_push(xtc_batch_t *b, struct batch_mt_rec *rec)
{
	int rc = xtc_chan_mpsc_try_send(b->mt.ring, rec);
	if (rc != XTC_OK) {
		batch_mt_release(b, rec);
		return XTC_E_AGAIN;
	}
	/* notify makes the parked driver fiber runnable; loop_wake nudges the
	 * owner loop's poller out of its I/O wait (the two together are the
	 * lost-wake-free cross-thread nudge). Both are safe from any thread. */
	(void)xtc_notify_signal(b->notify);
	(void)xtc_loop_wake(b->loop);
	return XTC_OK;
}

/* Driver side: apply a CANCEL/RESUME to handle H. H is a live slot, a held
 * SUBMIT, or neither -- and "neither" always means stale (H already
 * completed, the client-disconnect race) or never issued, so the control
 * is discarded in O(1); nothing is retained for it.
 *
 * There is no "cancel arrived before its submit" case to remember. The
 * ring (src/ptc/chan.c, xtc_chan_mpsc_try_send) reserves a slot by CAS on
 * head, and the consumer drains strictly in tail order, spinning on a
 * reserved-but-unwritten slot rather than skipping it. submit_mt writes
 * *out only after its push succeeded, so any producer that can name H in
 * cancel_mt/resume_mt reserves its slot after SUBMIT(H)'s: the control is
 * always drained after the submit. (The private __xtc_batch_mt_inject hook
 * can forge the reverse order; such a control is treated as stale.) */
static void
batch_mt_control(xtc_batch_t *b, uint64_t handle, enum batch_mt_op op)
{
	struct batch_slot *s;
	uint32_t v;
	if (!batch_map_get(b, handle, &v))
		return;
	if (v >= b->opts.capacity) {
		/* Held: not yet in a slot. A cancel aborts it before it runs;
		 * a resume of a never-run request is a no-op. */
		if (op == MT_CANCEL)
			b->mt.recs[v - b->opts.capacity].cancel = 1;
		return;
	}
	s = &b->slots[v];
	if (op == MT_CANCEL) {
		if (s->state != B_TERMINAL && s->state != B_DELIVERING)
			batch_mark_cancel(b, s);
	} else if (op == MT_RESUME
	    && (s->state == B_BLOCKED || s->state == B_READY)) {
		batch_move(b, s, B_READY);
		batch_hint(b);
	}
}

/* Driver side: place an accepted SUBMIT into the first free slot (caller
 * has checked live < capacity) and release its record. */
static void
batch_mt_place(xtc_batch_t *b, struct batch_mt_rec *rec)
{
	struct batch_slot *s = &b->slots[b->l_free.head];
	s->handle = rec->handle;
	s->request = rec->request;
	s->charge = rec->charge;
	s->cancel = 0;
	batch_move(b, s, B_WAITING);
	batch_map_put(b, s->handle, (uint32_t)(s - b->slots));
	b->live++;
	if (rec->cancel)
		batch_mark_cancel(b, s);
	rec->next = NULL;
	rec->cancel = 0;
	batch_mt_release(b, rec);
}

/* Driver side: move held SUBMITs into free slots, oldest first. Called
 * after every retire, so held work takes a freed slot before any later
 * submission can (n_held > 0 implies live == capacity). */
static void
batch_mt_admit_held(xtc_batch_t *b)
{
	struct batch_mt_rec *rec;
	while (b->mt.held != NULL && b->live < b->opts.capacity) {
		rec = b->mt.held;
		b->mt.held = rec->next;
		if (b->mt.held == NULL)
			b->mt.held_tail = NULL;
		b->mt.n_held--;
		batch_mt_place(b, rec);
	}
}

/* Driver side: drain the whole admission ring at an iteration boundary, before
 * the next vector is built. A SUBMIT takes a free WAITING slot, or, when the
 * group is full, joins the held FIFO: it was accepted when submit_mt returned
 * its handle, so it is never dropped and never counted as a rejection.
 * CANCEL/RESUME apply to the slot or held record. Caller holds the unsafe
 * bracket. */
static void
batch_mt_drain(xtc_batch_t *b)
{
	struct batch_mt_rec *rec;
	void *p;
	if (b->mt.ring == NULL)
		return;
	while (xtc_chan_mpsc_try_recv(b->mt.ring, &p) == XTC_OK) {
		rec = p;
		rec->next = NULL;
		rec->cancel = 0;
		if (rec->op != MT_SUBMIT) {
			batch_mt_control(b, rec->handle, rec->op);
			batch_mt_release(b, rec);
		} else if (b->mt.held != NULL || b->live == b->opts.capacity) {
			if (b->mt.held_tail != NULL)
				b->mt.held_tail->next = rec;
			else
				b->mt.held = rec;
			b->mt.held_tail = rec;
			b->mt.n_held++;
			batch_map_put(b, rec->handle,
			    b->opts.capacity + (uint32_t)(rec - b->mt.recs));
		} else {
			batch_mt_place(b, rec);
		}
	}
}

static int
batch_mt_idle(xtc_batch_t *b)
{
	return atomic_load_explicit(&b->mt.inflight, memory_order_seq_cst) == 0
	    && xtc_chan_mpsc_len(b->mt.ring) == 0;
}

/* Driver side, closed group (gate >= 1, so no SUBMIT can enter any more):
 * may the driver exit? True once no producer is between the gate and its
 * push and the ring is drained; the gate then moves to 2, refusing every
 * later mt call. The idle check runs before AND after the move: if it held
 * before, every accepted SUBMIT had been drained, so a control refused at
 * gate 2 names a request that has already completed. */
static int
batch_mt_quiesced(xtc_batch_t *b)
{
	if (b->mt.ring == NULL)
		return 1;
	if (atomic_load_explicit(&b->mt.gate, memory_order_relaxed) < 1)
		atomic_store_explicit(&b->mt.gate, 1, memory_order_seq_cst);
	if (!batch_mt_idle(b))
		return 0;
	atomic_store_explicit(&b->mt.gate, 2, memory_order_seq_cst);
	return batch_mt_idle(b);
}

/* Producer side: enter the gate (XTC_OK) or report it shut (XTC_E_AGAIN).
 * `limit` is the first gate value that refuses this call; a refusal counts
 * in mt_rejections when `count` is set. Every producer access to the group
 * happens between enter and leave, so once the driver has seen inflight ==
 * 0 at gate 2 no producer touches the group again. */
static int
batch_mt_enter(xtc_batch_t *b, int limit, int count)
{
	(void)atomic_fetch_add_explicit(&b->mt.inflight, 1,
	    memory_order_seq_cst);
	if (atomic_load_explicit(&b->mt.gate, memory_order_seq_cst) >= limit) {
		if (count)
			batch_inc_atomic(&b->mt.n_rejections);
		(void)atomic_fetch_sub_explicit(&b->mt.inflight, 1,
		    memory_order_release);
		return XTC_E_AGAIN;
	}
	return XTC_OK;
}

/* Leave the gate, counting the call as queued or rejected first. */
static void
batch_mt_leave(xtc_batch_t *b, int queued)
{
	batch_inc_atomic(queued ? &b->mt.n_queued : &b->mt.n_rejections);
	(void)atomic_fetch_sub_explicit(&b->mt.inflight, 1,
	    memory_order_release);
}

/* FUTURE kind: chain the device future to a completion token (see struct
 * batch_inflight). On allocation failure the device future itself is kept
 * and polled. Called on the driver fiber. */
static void
batch_future_arm(xtc_batch_t *b)
{
	struct batch_tok *t;
	xtc_future_t *mapped = NULL;
	b->inflight.wait = b->inflight.exec.future;
	b->inflight.exec.future = NULL;
	if (b->inflight.wait == NULL
	    || __os_calloc(1, sizeof(*t), (void **)&t) != XTC_OK)
		return;
	(void)pthread_mutex_init(&t->lock, NULL);
	t->b = b;
	t->refs = 2; /* the map callback + the engine */
	if (xtc_future_map(b->inflight.wait, batch_tok_fire, t, &mapped)
	    != XTC_OK) {
		(void)pthread_mutex_destroy(&t->lock);
		__os_free(t);
		return; /* not consumed: poll the device future */
	}
	b->inflight.wait = mapped;
	b->inflight.tok = t;
}

/* After every wake of an await: apply cross-loop controls that landed
 * meanwhile (a cancel_mt on an in-flight member must reach cancel_hint
 * now, not at the boundary), fire hints, and report a cancel-close. */
static int
batch_await_stop(void *arg)
{
	xtc_batch_t *b = arg;
	__xtc_unsafe_enter();
	batch_mt_drain(b);
	__xtc_unsafe_leave();
	batch_fire_hints(b, 0);
	return b->cancelling;
}

static int
batch_exec_error(int rc)
{
	return batch_error(rc) ? rc : XTC_E_INTERNAL;
}

static int64_t
batch_deadline(xtc_batch_t *b)
{
	int64_t now = 0;
	if (b->exec_timeout_ns <= 0)
		return -1;
	(void)__os_clock_mono(&now);
	return b->exec_timeout_ns > INT64_MAX - now ? INT64_MAX :
	                                             now + b->exec_timeout_ns;
}

/* FUTURE await: one notify wait against an absolute deadline (the
 * simulator virtualizes __os_clock_mono), re-evaluated after every wake, so
 * stray wakes never shorten it. Builds with the kill-wake proc layer run it
 * unmasked: a kill wakes the notify wait, which unlinks its waiter and
 * delivers, and batch_cleanup finalizes the op. Otherwise it runs masked
 * and a kill seen at a wake ends it as ABORTED (delivered at the leave). */
static int
batch_await_future(xtc_batch_t *b)
{
	int64_t deadline = batch_deadline(b), now = 0, wait_ns;
	intptr_t val;
	int rc, ready, st;
#ifndef XTC_HAVE_PROC_KILL_WAKE
	(void)xtc_mask_enter();
#endif
	for (;;) {
		if (batch_await_stop(b)) {
			rc = XTC_BATCH_EXEC_ABORTED;
			break;
		}
#ifndef XTC_HAVE_PROC_KILL_WAKE
		if (xtc_cancel_requested()) {
			rc = XTC_BATCH_EXEC_ABORTED;
			break;
		}
#endif
		if (b->inflight.tok != NULL) {
			(void)__xtc_mtx_lock(&b->inflight.tok->lock);
			ready = b->inflight.tok->done;
			st = b->inflight.tok->status;
			(void)__xtc_mtx_unlock(&b->inflight.tok->lock);
		} else if (b->inflight.wait == NULL) {
			ready = 1; /* submit handed over no future */
			st = XTC_E_INVAL;
		} else {
			ready = 0;
			st = XTC_OK;
			(void)xtc_future_ready(b->inflight.wait, &ready);
			if (ready) {
				st = xtc_future_wait(b->inflight.wait, &val, 0);
				b->inflight.wait = NULL; /* consumed */
			}
		}
		if (ready) {
			rc = st == XTC_OK ? XTC_BATCH_EXEC_OK :
			                    batch_exec_error(st);
			break;
		}
		wait_ns = -1;
		if (deadline >= 0) {
			(void)__os_clock_mono(&now);
			if (now >= deadline) {
				rc = XTC_BATCH_EXEC_TIMEOUT;
				break;
			}
			wait_ns = deadline - now;
		}
		if (b->inflight.tok == NULL
		    && (wait_ns < 0 || wait_ns > 1000000))
			wait_ns = 1000000; /* polling fallback */
		(void)xtc_notify_wait(b->notify, wait_ns);
	}
#ifndef XTC_HAVE_PROC_KILL_WAKE
	(void)xtc_mask_leave();
#endif
	return rc;
}

/* Await the offload token. Returns the exec_status to hand collect: OK on
 * completion, TIMEOUT if exec_timeout_ns elapsed first, ABORTED if the
 * group was cancel-closed (or the driver killed) while the op was
 * outstanding, or a NEGATIVE XTC_E_* when the engine could not await the
 * op at all (bad or closed fence fd, no accel support, the blocking pool
 * refused the closure, a dropped or failed device promise, an unknown
 * exec kind). Not reached for SYNC (no park). */
static int
batch_await(xtc_batch_t *b)
{
	int rc, result = 0;
	switch (b->inflight.exec.kind) {
	case XTC_BATCH_EXEC_FENCE:
		/* Interruptible fd park; the app owns fence_fd. close(CANCEL)
		 * and cancels wake it (xtc_proc_wake); a kill unwinds out of
		 * it and batch_cleanup finalizes. */
		rc = __xtc_accel_fence_wait(b->inflight.exec.fence_fd,
		    batch_deadline(b), batch_await_stop, b);
		if (rc == XTC_OK)
			return XTC_BATCH_EXEC_OK;
		if (rc == XTC_E_AGAIN)
			return XTC_BATCH_EXEC_TIMEOUT;
		if (rc == XTC_E_ABORTED)
			return XTC_BATCH_EXEC_ABORTED;
		return batch_exec_error(rc);
	case XTC_BATCH_EXEC_FUTURE:
		return batch_await_future(b);
	case XTC_BATCH_EXEC_BLOCKING:
		/* The pool call runs to completion; no engine timeout applies
		 * to a blocking closure (it holds a pool thread for its whole
		 * run). A closure the pool refused never ran. */
		rc = xtc_blocking_run(b->inflight.exec.run,
		                      b->inflight.exec.arg, &result);
		return rc == XTC_OK ? XTC_BATCH_EXEC_OK : batch_exec_error(rc);
	default:
		return XTC_E_INVAL; /* unknown exec kind */
	}
}

static void
batch_driver(void *arg)
{
	xtc_batch_t *b = arg;
	struct batch_slot *s;
	xtc_batch_result_t *r;
	size_t n, j;
	int rc, finished, pending, exec_status;
	xtc_batch_exec_t exec;
	if (xtc_proc_at_exit(batch_cleanup, b) != XTC_OK) {
		batch_cleanup(b);
		return;
	}
	for (;;) {
		__xtc_unsafe_enter();
		/* Drain cross-loop hand-ins FIRST: create WAITING slots for
		 * SUBMITs and apply CANCEL/RESUME control records, so a cancel
		 * is consumed below before batch_vector exposes any member. */
		batch_mt_drain(b);
		batch_consume_cancels(b);
		__xtc_unsafe_leave();
		batch_deliver(b);
		(void)xtc_mask_enter();
		__xtc_unsafe_enter();
		batch_mt_drain(b);
		n = batch_vector(b);
		/* A closed group finishes only once no producer can still hand
		 * in a request it was promised (see the admission gate). */
		finished = b->closed && b->live == 0 && b->mt.n_held == 0
		    && batch_mt_quiesced(b);
		pending = b->l_term.head != B_NIL
		    || (b->mt.ring != NULL
		        && xtc_chan_mpsc_len(b->mt.ring) != 0);
		if (n != 0) {
			b->callback = 1;
			batch_inc(&b->stats.iterations);
			if (n > b->stats.max_vector)
				b->stats.max_vector = n;
			if (UINT64_MAX - b->stats.total_members < n)
				b->stats.total_members = UINT64_MAX;
			else
				b->stats.total_members += n;
		}
		__xtc_unsafe_leave();
		if (n == 0) {
			if (!finished && !pending && b->closed
			    && b->live == 0 && b->mt.n_held == 0)
				xtc_yield(); /* a producer is mid-push: brief */
			else if (!finished && !pending)
				(void)xtc_notify_wait(b->notify, -1);
			/* Wait has unlinked its stack waiter before kill may
			 * unwind. */
			(void)xtc_mask_leave();
			if (finished)
				return;
			continue;
		}
		if (b->async) {
			/* Split step. submit runs here (callback held), then the
			 * driver parks on the token OUTSIDE the mask so the loop
			 * serves others; batch_collect_inflight is the single
			 * finalizer for every member of this frozen vector. */
			exec = (xtc_batch_exec_t){ 0 };
			rc = b->submit(b->items, n, b->opts.user, &exec);
			__xtc_unsafe_enter();
			if (rc != XTC_OK) {
				batch_inc(&b->astats.offload_errors);
				for (j = 0; j < n; ++j)
					batch_terminal(b,
					    &b->slots[b->indices[j]],
					    batch_error(rc) ? rc :
					                      XTC_E_INTERNAL);
				b->callback = 0;
				batch_hint(b);
				__xtc_unsafe_leave();
				(void)xtc_mask_leave();
				batch_deliver(b);
				xtc_yield();
				continue;
			}
			for (j = 0; j < n; ++j)
				batch_move(b, &b->slots[b->indices[j]],
				    B_INFLIGHT);
			b->inflight.exec = exec;
			b->inflight.idx = b->indices; /* frozen vector order */
			b->inflight.n = n;
			b->inflight.valid = 1;
			batch_inc(&b->astats.offloads);
			b->callback = 0;
			__xtc_unsafe_leave();
			(void)xtc_mask_leave();
			b->inflight.tok = NULL;
			b->inflight.wait = NULL;
			if (exec.kind == XTC_BATCH_EXEC_FUTURE)
				batch_future_arm(b);
			exec_status = exec.kind == XTC_BATCH_EXEC_SYNC ?
			                  XTC_BATCH_EXEC_OK :
			                  batch_await(b);
			batch_collect_inflight(b, exec_status);
			batch_deliver(b);
			xtc_yield();
			continue;
		}
		rc = b->opts.step(b->items, b->results, n, b->opts.user);
		__xtc_unsafe_enter();
		for (j = 0; j < n; ++j) {
			s = &b->slots[b->indices[j]];
			r = &b->results[j];
			if (rc != XTC_OK)
				batch_terminal(b, s,
				               batch_error(rc) ?
				                   rc :
				                   XTC_E_INTERNAL);
			else if (r->state == XTC_BATCH_DONE)
#if XTC_DST_BUG(XTC_DST_BUG_BATCHDONE)
				batch_move(b, s, B_READY);
#else
				batch_terminal(b, s, XTC_OK);
#endif
			else if (r->state == XTC_BATCH_BLOCKED)
				batch_move(b, s, B_BLOCKED);
			else if (r->state == XTC_BATCH_ERROR)
				batch_terminal(b, s,
				               batch_error(r->error) ?
				                   r->error :
				                   XTC_E_INTERNAL);
			else if (r->state != XTC_BATCH_CONTINUE)
				batch_terminal(b, s, XTC_E_INTERNAL);
		}
		b->callback = 0;
		batch_hint(b);
		__xtc_unsafe_leave();
		(void)xtc_mask_leave();
		batch_deliver(b);
		xtc_yield();
	}
}

static void
batch_free(xtc_batch_t *b)
{
	batch_mt_fini(b);
	xtc_notify_destroy(b->notify);
	__os_free(b->map);
	__os_free(b->indices);
	__os_free(b->results);
	__os_free(b->items);
	__os_free(b->slots);
	__os_free(b);
}

static int
batch_alloc(size_t count, size_t size, void **out)
{
	if (count > SIZE_MAX / size)
		return XTC_E_RANGE;
	return __os_calloc(count, size, out);
}

/* Handle map sized for every live slot plus every held record at load
 * <= 1/2: the next power of two >= 2 * (capacity + mt_queue_cap). */
static int
batch_map_init(xtc_batch_t *b)
{
	size_t want = 2 * ((size_t)b->opts.capacity + b->mt_cap_req), n = 4;
	while (n < want) {
		if (n > SIZE_MAX / 2)
			return XTC_E_RANGE;
		n <<= 1;
	}
	b->map_mask = n - 1;
	return batch_alloc(n, sizeof(*b->map), (void **)&b->map);
}

/* Shared tail of create / create_async: allocate per-group storage, attach the
 * class, and spawn the pinned driver. b->opts and the async fields must be set
 * by the caller. On any failure b is freed and *out is untouched. */
static int
batch_finish_create(xtc_batch_t *b, xtc_loop_t *loop, int lane,
    xtc_batch_t **out)
{
	xtc_proc_opts_t po = { 0 };
	uint32_t i;
	int rc;
	if ((rc = batch_alloc(b->opts.capacity, sizeof(*b->slots),
	                      (void **)&b->slots))
	        != XTC_OK
	    || (rc = batch_alloc(b->opts.max_active, sizeof(*b->items),
	                         (void **)&b->items))
	           != XTC_OK
	    || (rc = batch_alloc(b->opts.max_active, sizeof(*b->results),
	                         (void **)&b->results))
	           != XTC_OK
	    || (rc = batch_alloc(b->opts.max_active, sizeof(*b->indices),
	                         (void **)&b->indices))
	           != XTC_OK
	    || (rc = batch_map_init(b)) != XTC_OK
	    || (rc = xtc_notify_create(&b->notify)) != XTC_OK
	    || (rc = batch_mt_init(b, b->mt_cap_req)) != XTC_OK) {
		batch_free(b);
		return rc;
	}
	b->l_wait.head = b->l_wait.tail = B_NIL;
	b->l_active.head = b->l_active.tail = B_NIL;
	b->l_term.head = b->l_term.tail = B_NIL;
	b->l_free.head = b->l_free.tail = B_NIL;
	b->clist = B_NIL;
	b->delivering = B_NIL;
	for (i = 0; i < b->opts.capacity; ++i) {
		b->slots[i].cnext = B_NIL;
		batch_list_push(b, &b->l_free, i);
	}
	b->n_state[B_FREE] = b->opts.capacity;
	if (lane >= 0 && (rc = __xtc_sched_batch_attach(loop, lane)) != XTC_OK) {
		batch_free(b);
		return rc;
	}
	po.name = "batch";
	po.sched_class = b->opts.sched_class;
	/* Last fallible operation: only now publish the borrowed group
	 * argument. */
	rc = xtc_proc_spawn(loop, batch_driver, b, &po, &b->driver);
	if (rc != XTC_OK) {
		if (lane >= 0)
			__xtc_sched_batch_detach(loop, lane);
		batch_free(b);
		return rc;
	}
	*out = b;
	return XTC_OK;
}

/* Resolve an optional sched_class to its lane on `loop`. Returns XTC_OK with
 * *lane set (-1 for the default class) or XTC_E_INVAL for a foreign class. */
static int
batch_class_lane(xtc_loop_t *loop, xtc_exec_class_t cls, int *lane)
{
	int i, found = cls == NULL;
	*lane = -1;
	for (i = 0; i < loop->n_classes; ++i)
		if (loop->classes[i].in_use && cls == &loop->classes[i]) {
			found = 1;
			*lane = i;
		}
	return found ? XTC_OK : XTC_E_INVAL;
}

/* Test-only (not PUBLIC): position the handle counter so a test reaches
 * UINT64_MAX exhaustion without 2^64 submissions. Pre-run setup only. */
void
__xtc_batch_set_last_handle(xtc_batch_t *b, uint64_t last)
{
	b->last_handle = last;
	atomic_store_explicit(&b->handle_ctr, last, memory_order_relaxed);
}

/* Test-only (not PUBLIC): push a raw control/submit record onto the mt ring
 * with a CALLER-CHOSEN handle (bypassing the atomic stamp and the admission
 * gate), so a test can place exact records in the ring -- e.g. SUBMIT(H)
 * then CANCEL(H) in one boundary. A forged CANCEL ahead of its SUBMIT is
 * treated as stale (see batch_mt_control). Returns XTC_OK, XTC_E_AGAIN (ring/pool full), or XTC_E_NOSYS (mt disabled).
 * Setup-time only; the record is drained by the driver at the next boundary. */
int
__xtc_batch_mt_inject(xtc_batch_t *b, int op_cancel_resume_submit,
    uint64_t handle, void *request, uint64_t charge)
{
	struct batch_mt_rec *rec;
	if (b == NULL || b->mt.ring == NULL)
		return XTC_E_NOSYS;
	rec = batch_mt_claim(b);
	if (rec == NULL)
		return XTC_E_AGAIN;
	rec->op = (enum batch_mt_op)op_cancel_resume_submit;
	rec->handle = handle;
	rec->request = request;
	rec->charge = charge;
	return batch_mt_push(b, rec);
}

/* ----- placement helpers (S3) ------------------------------------------ *
 *
 * ADVISORY GLUE, not a scheduler. No automatic migration of a live group.
 * loop_for_device is a static-topology query (sysfs); the router is N
 * independent caller-owned groups + a modulo. Neither touches a group's
 * slots or a sim-nondeterministic source. */

/* Read a drm/accel device's NUMA node from sysfs. `name` is e.g.
 * "renderD128" (class "drm") or "accel0" (class "accel"); the node lives
 * at /sys/class/<cls>/<name>/device/numa_node. Returns the node (>= 0),
 * or -1 if the file is absent/unreadable or reports -1 (single-node). A
 * static host-topology read -- never on a sim hot path. */
static int
batch_device_node(const char *name)
{
#if defined(__linux__)
	const char *cls;
	char path[128];
	char buf[32];
	FILE *f;
	long node;
	char *end;
	if (name == NULL || name[0] == '\0')
		return -1;
	if (strncmp(name, "renderD", 7) == 0)
		cls = "drm";
	else if (strncmp(name, "accel", 5) == 0)
		cls = "accel";
	else
		return -1;
	if ((size_t)snprintf(path, sizeof path,
	    "/sys/class/%s/%s/device/numa_node", cls, name) >= sizeof path)
		return -1;
	f = fopen(path, "r"); /* XTC_BLOCKING_OK: one-shot static topology probe */
	if (f == NULL)
		return -1;
	if (fgets(buf, (int)sizeof buf, f) == NULL) { /* XTC_BLOCKING_OK: one-shot */
		(void)fclose(f);
		return -1;
	}
	(void)fclose(f);
	node = strtol(buf, &end, 10);
	if (end == buf || node < 0)
		return -1;
	return (int)node;
#else
	(void)name;
	return -1;
#endif
}

/* An N-group + modulo router. Borrows the groups array BY COPY; never
 * owns the groups. */
struct xtc_batch_router {
	xtc_batch_t **groups;
	int n;
};
#endif

/* PUBLIC: int xtc_batch_create __P((xtc_loop_t *, const xtc_batch_opts_t *, xtc_batch_t **)); */
int
xtc_batch_create(xtc_loop_t *loop, const xtc_batch_opts_t *o, xtc_batch_t **out)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	xtc_batch_t *b;
	int rc, lane = -1;
	if (loop == NULL || o == NULL || out == NULL || o->size != sizeof(*o))
		return XTC_E_INVAL;
	if (o->version != XTC_BATCH_VERSION)
		return XTC_E_VERSION;
	if (o->capacity == 0 || o->max_active == 0
	    || o->max_active > o->capacity || o->resource_capacity == 0
	    || o->step == NULL || o->complete == NULL)
		return XTC_E_INVAL;
	if (__xtc_current_loop != NULL && __xtc_current_loop != loop)
		return XTC_E_INVAL;
	if (atomic_load_explicit(&loop->sched.phase, memory_order_acquire) != 0)
		return XTC_E_AGAIN;
	if ((rc = batch_class_lane(loop, o->sched_class, &lane)) != XTC_OK)
		return rc;
	if ((rc = __os_calloc(1, sizeof(*b), (void **)&b)) != XTC_OK)
		return rc;
	b->loop = loop;
	b->opts = *o;
	b->class_index = lane;
	return batch_finish_create(b, loop, lane, out);
#else
	(void)loop;
	(void)o;
	(void)out;
	return XTC_E_NOSYS;
#endif
}

/* PUBLIC: int xtc_batch_create_async __P((xtc_loop_t *, const xtc_batch_async_opts_t *, xtc_batch_t **)); */
int
xtc_batch_create_async(xtc_loop_t *loop, const xtc_batch_async_opts_t *o,
    xtc_batch_t **out)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	xtc_batch_t *b;
	int rc, lane = -1;
	if (loop == NULL || o == NULL || out == NULL || o->size != sizeof(*o))
		return XTC_E_INVAL;
	if (o->version != XTC_BATCH_VERSION)
		return XTC_E_VERSION;
	if (o->capacity == 0 || o->max_active == 0
	    || o->max_active > o->capacity || o->resource_capacity == 0
	    || o->exec_timeout_ns < 0 || o->submit == NULL || o->collect == NULL
	    || o->complete == NULL)
		return XTC_E_INVAL;
	if (__xtc_current_loop != NULL && __xtc_current_loop != loop)
		return XTC_E_INVAL;
	if (atomic_load_explicit(&loop->sched.phase, memory_order_acquire) != 0)
		return XTC_E_AGAIN;
	if ((rc = batch_class_lane(loop, o->sched_class, &lane)) != XTC_OK)
		return rc;
	if ((rc = __os_calloc(1, sizeof(*b), (void **)&b)) != XTC_OK)
		return rc;
	b->loop = loop;
	/* Map the overlapping fields into the classic opts so every shared
	 * path (hint, stats, close, destroy, admission) reads them unchanged;
	 * the async-only fields live beside them. step stays NULL -- the async
	 * driver takes the submit/collect path on b->async. */
	b->opts.size = sizeof(b->opts);
	b->opts.version = XTC_BATCH_VERSION;
	b->opts.capacity = o->capacity;
	b->opts.max_active = o->max_active;
	b->opts.resource_capacity = o->resource_capacity;
	b->opts.sched_class = o->sched_class;
	b->opts.complete = o->complete;
	b->opts.user = o->user;
	b->async = 1;
	b->exec_timeout_ns = o->exec_timeout_ns;
	b->mt_cap_req = o->mt_queue_cap;
	b->submit = o->submit;
	b->collect = o->collect;
	b->cancel_hint = o->cancel_hint;
	b->class_index = lane;
	return batch_finish_create(b, loop, lane, out);
#else
	(void)loop;
	(void)o;
	(void)out;
	return XTC_E_NOSYS;
#endif
}

/* PUBLIC: int xtc_batch_submit __P((xtc_batch_t *, void *, uint64_t, uint64_t *)); */
int
xtc_batch_submit(xtc_batch_t *b, void *request, uint64_t charge, uint64_t *out)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	struct batch_slot *s;
	int rc;
	__xtc_unsafe_enter();
	rc = batch_mutable(b);
	if (rc == XTC_OK) {
		if (out == NULL || charge == 0)
			rc = XTC_E_INVAL;
		else if (charge > b->opts.resource_capacity
		         || atomic_load_explicit(&b->handle_ctr,
		                memory_order_relaxed) == UINT64_MAX)
			rc = XTC_E_RANGE;
		else if (b->closed || b->live == b->opts.capacity
		         || b->mt.n_held != 0)
			rc = XTC_E_AGAIN; /* held hand-ins are older: FCFS */
		if (rc != XTC_OK)
			batch_inc(&b->stats.rejections);
		else {
			s = &b->slots[b->l_free.head];
			b->last_handle = batch_next_handle(b);
			s->handle = b->last_handle;
			s->request = request;
			s->charge = charge;
			batch_move(b, s, B_WAITING);
			batch_map_put(b, s->handle, (uint32_t)(s - b->slots));
			b->live++;
			*out = b->last_handle;
#if !XTC_DST_BUG(XTC_DST_BUG_BATCHWAKE)
			(void)xtc_notify_signal(b->notify);
#endif
		}
	}
	__xtc_unsafe_leave();
	return rc;
#else
	(void)b;
	(void)request;
	(void)charge;
	(void)out;
	return XTC_E_NOSYS;
#endif
}

/* PUBLIC: int xtc_batch_cancel __P((xtc_batch_t *, uint64_t)); */
int
xtc_batch_cancel(xtc_batch_t *b, uint64_t handle)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	struct batch_slot *s;
	int rc;
	__xtc_unsafe_enter();
	rc = batch_mutable(b);
	if (rc == XTC_OK) {
		s = batch_find(b, handle);
		if (s == NULL)
			rc = XTC_E_INVAL;
		else {
			/* A committed terminal result wins; never rewrite it.
			 */
			if (s->state != B_TERMINAL && s->state != B_DELIVERING)
				batch_mark_cancel(b, s);
			else
				s->cancel = 1;
			(void)xtc_notify_signal(b->notify);
			/* In flight: wake the await so cancel_hint fires now. */
			if (s->state == B_INFLIGHT)
				(void)xtc_proc_wake(b->driver);
		}
	}
	__xtc_unsafe_leave();
	return rc;
#else
	(void)b;
	(void)handle;
	return XTC_E_NOSYS;
#endif
}

/* PUBLIC: int xtc_batch_resume __P((xtc_batch_t *, uint64_t)); */
int
xtc_batch_resume(xtc_batch_t *b, uint64_t handle)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	struct batch_slot *s;
	int rc;
	__xtc_unsafe_enter();
	rc = batch_mutable(b);
	if (rc == XTC_OK) {
		s = batch_find(b, handle);
		if (s == NULL || (s->state != B_BLOCKED && s->state != B_READY))
			rc = XTC_E_INVAL;
		else {
			batch_move(b, s, B_READY);
			batch_hint(b);
			(void)xtc_notify_signal(b->notify);
		}
	}
	__xtc_unsafe_leave();
	return rc;
#else
	(void)b;
	(void)handle;
	return XTC_E_NOSYS;
#endif
}

/* PUBLIC: int xtc_batch_submit_mt __P((xtc_batch_t *, void *, uint64_t, uint64_t *)); */
int
xtc_batch_submit_mt(xtc_batch_t *b, void *request, uint64_t charge,
    uint64_t *out)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	struct batch_mt_rec *rec;
	uint64_t handle;
	/* Callable from ANY loop or plain thread: touches only the lock-free
	 * ring + free-pool + atomic handle counter, never b->slots. No unsafe
	 * bracket, no batch_mutable. */
	if (b == NULL || out == NULL)
		return XTC_E_INVAL;
	if (b->mt.ring == NULL)
		return XTC_E_NOSYS; /* mt_queue_cap == 0: not enabled */
	if (charge == 0 || charge > b->opts.resource_capacity)
		return XTC_E_RANGE;
	/* Closed (or closing) group: refused here, before any handle is
	 * issued, so an XTC_OK always means a completion will follow. */
	if (batch_mt_enter(b, 1, 1) != XTC_OK)
		return XTC_E_AGAIN;
	rec = batch_mt_claim(b);
	if (rec == NULL) {
		batch_mt_leave(b, 0);
		return XTC_E_AGAIN;
	}
	handle = batch_next_handle(b);
	if (handle == 0) {
		batch_mt_release(b, rec);
		batch_mt_leave(b, 0);
		return XTC_E_RANGE; /* handle space exhausted */
	}
	rec->op = MT_SUBMIT;
	rec->handle = handle;
	rec->request = request;
	rec->charge = charge;
	if (batch_mt_push(b, rec) != XTC_OK) {
		/* Ring full (push released the record). The handle is burned
		 * -- handles are never reused, so this only advances the
		 * counter; no slot was created and no leak occurs. */
		batch_mt_leave(b, 0);
		return XTC_E_AGAIN;
	}
	batch_mt_leave(b, 1);
	*out = handle;
	return XTC_OK;
#else
	(void)b;
	(void)request;
	(void)charge;
	(void)out;
	return XTC_E_NOSYS;
#endif
}

/* Shared body of cancel_mt/resume_mt: push a control record onto the SAME ring
 * as the submits, so the driver drains it after the SUBMIT for the same
 * handle (see batch_mt_control) and applies it to the slot or held record.
 * A control for a handle that has already completed is discarded. Accepted
 * until the driver has finished (a cancel during DRAIN still matters). */
#ifdef XTC_ENABLE_SCHED_POLICY
static int
batch_mt_control_submit(xtc_batch_t *b, uint64_t handle, enum batch_mt_op op)
{
	struct batch_mt_rec *rec;
	if (b == NULL)
		return XTC_E_INVAL;
	if (b->mt.ring == NULL)
		return XTC_E_NOSYS;
	if (handle == 0)
		return XTC_E_INVAL;
	if (batch_mt_enter(b, 2, 0) != XTC_OK)
		return XTC_E_INVAL; /* driver finished: the handle is stale */
	rec = batch_mt_claim(b);
	if (rec == NULL) {
		batch_mt_leave(b, 0);
		return XTC_E_AGAIN;
	}
	rec->op = op;
	rec->handle = handle;
	rec->request = NULL;
	rec->charge = 0;
	if (batch_mt_push(b, rec) != XTC_OK) {
		batch_mt_leave(b, 0);
		return XTC_E_AGAIN;
	}
	/* A FENCE await parks on the fd, not the notify: wake it so a cancel
	 * of an in-flight member reaches cancel_hint at once. Still inside the
	 * gate, so the driver (and its pid) cannot be gone yet. */
	if (op == MT_CANCEL)
		(void)xtc_proc_wake(b->driver);
	batch_mt_leave(b, 1);
	return XTC_OK;
}
#endif

/* PUBLIC: int xtc_batch_cancel_mt __P((xtc_batch_t *, uint64_t)); */
int
xtc_batch_cancel_mt(xtc_batch_t *b, uint64_t handle)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	return batch_mt_control_submit(b, handle, MT_CANCEL);
#else
	(void)b;
	(void)handle;
	return XTC_E_NOSYS;
#endif
}

/* PUBLIC: int xtc_batch_resume_mt __P((xtc_batch_t *, uint64_t)); */
int
xtc_batch_resume_mt(xtc_batch_t *b, uint64_t handle)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	return batch_mt_control_submit(b, handle, MT_RESUME);
#else
	(void)b;
	(void)handle;
	return XTC_E_NOSYS;
#endif
}

/* PUBLIC: int xtc_batch_close __P((xtc_batch_t *, enum xtc_batch_close_mode)); */
int
xtc_batch_close(xtc_batch_t *b, enum xtc_batch_close_mode mode)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	int rc;
	__xtc_unsafe_enter();
	rc = batch_mutable(b);
	if (rc == XTC_OK) {
		if (mode != XTC_BATCH_DRAIN && mode != XTC_BATCH_CANCEL)
			rc = XTC_E_INVAL;
		else {
			b->closed = 1;
			b->cancelling |= mode == XTC_BATCH_CANCEL;
			if (b->mt.ring != NULL
			    && atomic_load_explicit(&b->mt.gate,
			           memory_order_relaxed) == 0)
				atomic_store_explicit(&b->mt.gate, 1,
				    memory_order_seq_cst);
			/* The notify wakes an idle driver and a FUTURE await;
			 * the proc wake ends a FENCE await, which then sees
			 * b->cancelling and reports ABORTED. */
			(void)xtc_notify_signal(b->notify);
			if (mode == XTC_BATCH_CANCEL && b->inflight.valid)
				(void)xtc_proc_wake(b->driver);
		}
	}
	__xtc_unsafe_leave();
	return rc;
#else
	(void)b;
	(void)mode;
	return XTC_E_NOSYS;
#endif
}

/* PUBLIC: int xtc_batch_destroy __P((xtc_batch_t *)); */
int
xtc_batch_destroy(xtc_batch_t *b)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	int rc;
	__xtc_unsafe_enter();
	rc = batch_owner(b, 1);
	if (rc == XTC_OK) {
		if (b->callback)
			rc = XTC_E_DEADLK;
		else if (!b->closed || b->live != 0)
			rc = XTC_E_AGAIN;
		else if (__xtc_proc_loop_contains(b->loop, b->driver))
			rc = XTC_E_AGAIN;
		else {
			if (b->class_index >= 0)
				__xtc_sched_batch_detach(b->loop, b->class_index);
			batch_free(b);
			rc = XTC_OK;
		}
	}
	__xtc_unsafe_leave();
	return rc;
#else
	(void)b;
	return XTC_E_NOSYS;
#endif
}

/* PUBLIC: int xtc_batch_get_stats __P((xtc_batch_t *, xtc_batch_stats_t *)); */
int
xtc_batch_get_stats(xtc_batch_t *b, xtc_batch_stats_t *out)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	int rc;
	__xtc_unsafe_enter();
	rc = batch_owner(b, 1);
	if (rc == XTC_OK) {
		if (out == NULL || out->size != sizeof(*out))
			rc = XTC_E_INVAL;
		else if (out->version != XTC_BATCH_VERSION)
			rc = XTC_E_VERSION;
		else {
			*out = b->stats;
			out->size = sizeof(*out);
			out->version = XTC_BATCH_VERSION;
			out->queued = b->mt.n_held + b->n_state[B_WAITING];
			out->ready = b->n_state[B_READY]
			    + b->n_state[B_INFLIGHT];
			out->blocked = b->n_state[B_BLOCKED];
		}
	}
	__xtc_unsafe_leave();
	return rc;
#else
	(void)b;
	(void)out;
	return XTC_E_NOSYS;
#endif
}

/* PUBLIC: int xtc_batch_get_async_stats __P((xtc_batch_t *, xtc_batch_async_stats_t *)); */
int
xtc_batch_get_async_stats(xtc_batch_t *b, xtc_batch_async_stats_t *out)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	int rc;
	__xtc_unsafe_enter();
	rc = batch_owner(b, 1);
	if (rc == XTC_OK) {
		if (out == NULL || out->size != sizeof(*out))
			rc = XTC_E_INVAL;
		else if (out->version != XTC_BATCH_VERSION)
			rc = XTC_E_VERSION;
		else {
			*out = b->astats;
			/* mt_queued/mt_rejections live in b->mt as atomics (the
			 * producer path bumps them off-thread); read them with a
			 * relaxed load rather than from the plain astats mirror. */
			out->mt_queued = atomic_load_explicit(&b->mt.n_queued,
			    memory_order_relaxed);
			out->mt_rejections = atomic_load_explicit(
			    &b->mt.n_rejections, memory_order_relaxed);
			out->size = sizeof(*out);
			out->version = XTC_BATCH_VERSION;
		}
	}
	__xtc_unsafe_leave();
	return rc;
#else
	(void)b;
	(void)out;
	return XTC_E_NOSYS;
#endif
}

/* PUBLIC: int xtc_batch_loop_for_device __P((xtc_exec_t *, const xtc_accel_dev_t *, int *)); */
int
xtc_batch_loop_for_device(xtc_exec_t *exec, const xtc_accel_dev_t *dev,
    int *out_loop_idx)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	int n_loops, dev_node, i;
	if (exec == NULL || dev == NULL || out_loop_idx == NULL)
		return XTC_E_INVAL;
	/* Loop 0 is the documented best-effort fallback for every
	 * absence case (absence is not an error -- matches xtc_accel). */
	*out_loop_idx = 0;
	n_loops = xtc_exec_n_loops(exec);
	if (n_loops <= 1)
		return XTC_OK;
	/* Device NUMA node from sysfs (static topology). -1 => unknown. */
	dev_node = batch_device_node(dev->name);
	if (dev_node < 0 || __os_numa_nnodes() <= 1)
		return XTC_OK;
	/* First loop whose carrier recorded that node wins. A carrier
	 * records its node only when it starts, so a loop that has never
	 * run has no known node (the executor's table reads 0 then, which
	 * would falsely match a node-0 device) and is skipped: before the
	 * executor runs, every call falls back to loop 0. */
	for (i = 0; i < n_loops; ++i) {
		extern int __xtc_exec_loop_node(xtc_exec_t *, int);
		if (atomic_load_explicit(&xtc_exec_loop(exec, i)->sched.phase,
		        memory_order_acquire) == 0)
			continue;
		if (__xtc_exec_loop_node(exec, i) == dev_node) {
			*out_loop_idx = i;
			return XTC_OK;
		}
	}
	return XTC_OK;
#else
	(void)exec;
	(void)dev;
	if (out_loop_idx != NULL)
		*out_loop_idx = 0;
	return XTC_E_NOSYS;
#endif
}

/* PUBLIC: int xtc_batch_router_create __P((xtc_batch_t **, int, xtc_batch_router_t **)); */
int
xtc_batch_router_create(xtc_batch_t **groups, int n, xtc_batch_router_t **out)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	xtc_batch_router_t *r;
	int rc, i;
	if (groups == NULL || n <= 0 || out == NULL)
		return XTC_E_INVAL;
	for (i = 0; i < n; ++i)
		if (groups[i] == NULL)
			return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof(*r), (void **)&r)) != XTC_OK)
		return rc;
	/* Borrow the groups array BY COPY; the router never owns the groups. */
	if ((rc = batch_alloc((size_t)n, sizeof(*r->groups),
	                      (void **)&r->groups)) != XTC_OK) {
		__os_free(r);
		return rc;
	}
	for (i = 0; i < n; ++i)
		r->groups[i] = groups[i];
	r->n = n;
	*out = r;
	return XTC_OK;
#else
	(void)groups;
	(void)n;
	(void)out;
	return XTC_E_NOSYS;
#endif
}

/* PUBLIC: void xtc_batch_router_destroy __P((xtc_batch_router_t *)); */
void
xtc_batch_router_destroy(xtc_batch_router_t *r)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	if (r == NULL)
		return;
	/* Frees ONLY the router. The groups are the caller's to destroy. */
	__os_free(r->groups);
	__os_free(r);
#else
	(void)r;
#endif
}

/* PUBLIC: xtc_batch_t *xtc_batch_route __P((xtc_batch_router_t *, uint64_t)); */
xtc_batch_t *
xtc_batch_route(xtc_batch_router_t *r, uint64_t key)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	if (r == NULL || r->n <= 0)
		return NULL;
	return r->groups[key % (uint64_t)r->n];
#else
	(void)r;
	(void)key;
	return NULL;
#endif
}
