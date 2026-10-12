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
#include <stdio.h> /* sysfs numa_node read in the placement helper */
#if defined(XTC_DIAGNOSTIC)
#include <stdlib.h> /* abort */
#endif

enum batch_slot_state {
	B_FREE,
	B_WAITING,
	B_READY,
	B_BLOCKED,
	B_INFLIGHT,
	B_TERMINAL,
	B_DELIVERING
};
struct batch_slot {
	uint64_t handle, charge;
	void *request;
	enum batch_slot_state state;
	int outcome, cancel;
};
/* At most one offloaded op per group (one per iteration). The frozen vector
 * is b->items/b->results[0..n); `idx` records which slot each vector member
 * belongs to. `valid` is the single token: set when submit succeeds, cleared
 * when batch_collect_inflight consumes it -- so double-terminalize is
 * structurally impossible.
 *
 * FUTURE kind only: cancel_prom/wait own the edge-triggered cancel path. At
 * the submit->inflight transition the engine makes a cancel promise/future
 * pair and a `wait` future = when_any{device future, cancel future}; the
 * driver parks on `wait`, so a device completion OR an owner cancel wakes it
 * with no periodic re-park. when_any CONSUMES both input future handles, so
 * after it the engine owns exactly two reclaimable handles: `wait` (the
 * combined future) and `cancel_prom` (the write half it resolves/drops). The
 * device read half is owned by when_any's combinator and reclaimed when the
 * app resolves its own promise -- never a pointer into the group, so no
 * cross-reference outlives teardown. The finalizer reclaims BOTH `wait` and
 * `cancel_prom` exactly once on every path. */
struct batch_inflight {
	xtc_batch_exec_t exec;
	xtc_promise_t *cancel_prom;
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
};
/* Early-control set (the re-review's cross-producer lost-cancel fix). A
 * CANCEL/RESUME drained before its target slot exists is parked here; the
 * later SUBMIT for that handle consults it after creating the slot and applies
 * the pending control at once. Bounded by mt_queue_cap; only the driver (owner
 * loop) touches it, so no synchronization is needed. */
struct batch_mt_early {
	uint64_t handle;
	enum batch_mt_op op;
};
struct batch_mt {
	xtc_chan_mpsc_t *ring;      /* producers -> driver, control records */
	xtc_chan_mpmc_t *pool;      /* free record pointers (claim/release) */
	struct batch_mt_rec *recs;  /* preallocated record storage */
	struct batch_mt_early *early;
	uint32_t cap;               /* mt_queue_cap (recs/early length) */
	uint32_t n_early;           /* live entries in `early` */
	/* Producer-side stat counters. submit_mt/cancel_mt/resume_mt run on
	 * ANY thread and bump these concurrently, so they are atomic; the
	 * driver-only astats fields (offloads/offload_*) stay plain. A relaxed
	 * fetch-add is enough: these are monotonic totals, not ordering
	 * barriers, and a wrap at 2^64 on a lifetime counter is a non-issue. */
	_Atomic uint64_t n_queued;
	_Atomic uint64_t n_rejections;
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
	uint32_t live, active;
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

/* Publish only committed membership. Called under the owner's short bracket. */
static void
batch_hint(xtc_batch_t *b)
{
	uint32_t i, ready = 0;
	if (b->class_index < 0)
		return;
	for (i = 0; i < b->opts.capacity; ++i)
		ready += b->slots[i].state == B_READY;
	__xtc_sched_batch_hint(b->loop, b->class_index, ready, b->active);
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
	uint64_t prev =
	    atomic_fetch_add_explicit(&b->handle_ctr, 1, memory_order_relaxed);
	if (prev == UINT64_MAX) {
		/* Wrapped: restore so a second attempt also sees exhaustion. */
		atomic_store_explicit(&b->handle_ctr, UINT64_MAX,
		    memory_order_relaxed);
		return 0;
	}
	return prev + 1;
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

static struct batch_slot *
batch_find(xtc_batch_t *b, uint64_t handle)
{
	uint32_t i;
	if (handle == 0)
		return NULL;
	for (i = 0; i < b->opts.capacity; ++i)
		if (b->slots[i].state != B_FREE
#if !XTC_DST_BUG(XTC_DST_BUG_BATCHSTALE)
		    && b->slots[i].handle == handle
#endif
		    )
			return &b->slots[i];
	return NULL;
}

static void
batch_terminal(xtc_batch_t *b, struct batch_slot *s, int outcome)
{
	if (s->state == B_READY || s->state == B_BLOCKED
	    || s->state == B_INFLIGHT) {
		b->active--;
#if !XTC_DST_BUG(XTC_DST_BUG_BATCHRES)
		b->stats.reserved_units -= s->charge;
#endif
	}
	s->state = B_TERMINAL;
	s->outcome = outcome;
	batch_hint(b);
}

static void
batch_retire(xtc_batch_t *b, struct batch_slot *s)
{
	if (s->outcome == XTC_OK)
		batch_inc(&b->stats.completed);
	else if (s->outcome == XTC_E_ABORTED)
		batch_inc(&b->stats.cancelled);
	else
		batch_inc(&b->stats.failed);
	memset(s, 0, sizeof(*s));
	--b->live;
}

static void
batch_deliver(xtc_batch_t *b)
{
	struct batch_slot *s;
	uint32_t i;
	for (i = 0; i < b->opts.capacity; ++i) {
		(void)xtc_mask_enter();
		__xtc_unsafe_enter();
		s = &b->slots[i];
		if (s->state != B_TERMINAL) {
			__xtc_unsafe_leave();
			(void)xtc_mask_leave();
			continue;
		}
		/* Persistent delivery-started survives an explicit callback
		 * exit. */
		s->state = B_DELIVERING;
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

/* THE SINGLE FINALIZER for an offloaded op. collect() runs exactly once; each
 * in-flight member is finalized by exactly one call to this. Reached from (1)
 * the driver after the token wait returns and (2) batch_cleanup on forced
 * teardown (exec_status XTC_BATCH_EXEC_ABORTED), only when inflight.valid.
 * Caller must NOT hold the unsafe bracket (collect is user code and may yield).
 * The single-token inflight.valid flag, cleared here, makes a second
 * terminalize of the same member structurally impossible. */
static void
batch_collect_inflight(xtc_batch_t *b, int exec_status)
{
	struct batch_slot *s;
	xtc_batch_result_t *r;
	size_t n, j;
	int rc, aborted;
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
		if (aborted) {
			if (b->cancel_hint != NULL)
				b->cancel_hint(s->handle, b->opts.user);
			batch_terminal(b, s, XTC_E_ABORTED);
		} else if (rc != XTC_OK)
			batch_terminal(b, s,
			               batch_error(rc) ? rc : XTC_E_INTERNAL);
		else if (r->state == XTC_BATCH_DONE)
			batch_terminal(b, s, XTC_OK);
		else if (r->state == XTC_BATCH_BLOCKED)
			s->state = B_BLOCKED;
		else if (r->state == XTC_BATCH_CONTINUE)
			s->state = B_READY; /* stays active; re-enters next vector */
		else if (r->state == XTC_BATCH_ERROR)
			batch_terminal(b, s,
			               batch_error(r->error) ? r->error :
			                                       XTC_E_INTERNAL);
		else
			batch_terminal(b, s, XTC_E_INTERNAL);
	}
	if (exec_status == XTC_BATCH_EXEC_TIMEOUT)
		batch_inc(&b->astats.offload_timeouts);
	else if (exec_status != XTC_BATCH_EXEC_OK
	         && exec_status != XTC_BATCH_EXEC_ABORTED)
		batch_inc(&b->astats.offload_errors);
	/* Reclaim the engine-owned FUTURE handles exactly once (see
	 * struct batch_inflight). `wait` is the when_any combined future;
	 * batch_await nulls it on consume, so this discards it only on a kill
	 * before/through the await. `cancel_prom` is the write half the engine
	 * owns: drop it (a dropped promise aborts the cancel cell) so when_any's
	 * combinator retires its ref and frees -- idempotent because set/drop
	 * nulls it. The device read half is owned by when_any, not the engine. */
	if (b->inflight.wait != NULL) {
		__xtc_future_discard(b->inflight.wait);
		b->inflight.wait = NULL;
	}
	if (b->inflight.cancel_prom != NULL) {
		xtc_promise_drop(b->inflight.cancel_prom);
		b->inflight.cancel_prom = NULL;
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
	for (i = 0; i < b->opts.capacity; ++i) {
		s = &b->slots[i];
		if (s->state == B_DELIVERING)
			batch_retire(b, s);
		else if (s->state == B_INFLIGHT)
			continue; /* owned by the finalizer; never here now */
		else if (s->state != B_FREE && s->state != B_TERMINAL)
			batch_terminal(b, s,
			               b->cancelling || s->cancel ?
			                   XTC_E_ABORTED :
			                   XTC_E_INTERNAL);
	}
	__xtc_unsafe_leave();
	batch_deliver(b);
}

static size_t
batch_vector(xtc_batch_t *b)
{
	struct batch_slot *s, *first;
	uint32_t i;
	size_t n = 0, j;
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
	for (i = 0; i < b->opts.capacity; ++i) {
		s = &b->slots[i];
		if ((s->state == B_WAITING || s->state == B_READY
		     || s->state == B_BLOCKED)
		    && (b->cancelling || s->cancel))
			batch_terminal(b, s, XTC_E_ABORTED);
	}
	/* Bounded O(capacity * max_active) FCFS scan; a queue would add another
	 * linked invariant and is only worth it if measured large-capacity
	 * workloads justify it. */
#if XTC_DST_BUG(XTC_DST_BUG_BATCHCAP)
	while (b->active < b->opts.capacity) {
#else
	while (b->active < b->opts.max_active) {
#endif
		first = NULL;
		for (i = 0; i < b->opts.capacity; ++i) {
			s = &b->slots[i];
			if (s->state == B_WAITING
			    && (first == NULL || s->handle < first->handle))
				first = s;
		}
		if (first == NULL
		    || first->charge > b->opts.resource_capacity
		                           - b->stats.reserved_units)
			break;
		first->state = B_READY;
		b->stats.reserved_units += first->charge;
		b->active++;
	}
	for (i = 0; i < b->opts.capacity; ++i) {
		s = &b->slots[i];
		if (s->state != B_READY)
			continue;
		/* Slot reuse must not change stable admission order in the
		 * vector. */
		for (j = n; j > 0 && b->items[j - 1].handle > s->handle; --j) {
			b->indices[j] = b->indices[j - 1];
			b->items[j] = b->items[j - 1];
		}
		b->indices[j] = i;
		b->items[j].handle = s->handle;
		b->items[j].request = s->request;
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
 * ONLY the lock-free ring + free-pool + atomic handle counter (+ a wake of
 * the owner loop). They NEVER read or write b->slots, b->active, the early-
 * control set, or any other driver-owned state. The driver (owner loop) is
 * the only mutator of those, so there is no data race between a producer and
 * the driver. */

static int batch_alloc(size_t count, size_t size, void **out);

/* Create the ring, free-pool, record storage and early-control set, and seed
 * the free-pool with every record pointer. Charged through the group's loop
 * res. mt stays disabled (ring == NULL) when cap == 0. */
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
	    || (rc = batch_alloc(cap, sizeof(*b->mt.early),
	                         (void **)&b->mt.early))
	           != XTC_OK
	    || (rc = xtc_chan_mpsc_create(res, cap, &b->mt.ring)) != XTC_OK
	    || (rc = xtc_chan_mpmc_create(res, cap, &b->mt.pool)) != XTC_OK)
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
	__os_free(b->mt.early);
	b->mt.early = NULL;
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
 * full here.  A try_send can therefore only fail transiently (e.g. a DST
 * buggify spurious-full); retry until it lands so a record is never lost
 * to circulation, which would slowly starve the pool.  Bounded in
 * practice because the slot provably exists; the diagnostic assert pins
 * the invariant rather than looping unboundedly on a real bug. */
static void
batch_mt_release(xtc_batch_t *b, struct batch_mt_rec *rec)
{
	int rc, tries = 0;
	(void)tries;
	for (;;) {
		rc = xtc_chan_mpmc_try_send(b->mt.pool, rec);
		if (rc == XTC_OK)
			return;
		if (++tries >= 1 << 20) {
#if defined(XTC_DIAGNOSTIC)
			/* The pool has a free slot by the conservation
			 * invariant; a persistent failure is a bug, not
			 * backpressure. */
			abort();
#else
			return;   /* give up rather than hang a release */
#endif
		}
	}
}

/* Producer side: push a filled record onto the ring, wake the driver. On a
 * full ring the record is returned to the pool and XTC_E_AGAIN is reported.
 * Note the ring is NOT closed by xtc_batch_close (only batch_mt_fini closes
 * it, at destroy), so a hand-in that races close is accepted here and then
 * dropped at the next driver drain (the b->closed check in batch_mt_drain) --
 * this is best-effort admission, not an AGAIN-on-closed guarantee. Freedom
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

/* Driver side: apply a CANCEL/RESUME to handle H's slot if it exists, else
 * park it in the early-control set to be applied when H's SUBMIT is later
 * drained. Only the driver calls this. */
static void
batch_mt_control(xtc_batch_t *b, uint64_t handle, enum batch_mt_op op)
{
	struct batch_slot *s = batch_find(b, handle);
	if (s != NULL) {
		if (op == MT_CANCEL) {
			s->cancel = 1;
		} else if (op == MT_RESUME
		    && (s->state == B_BLOCKED || s->state == B_READY)) {
			s->state = B_READY;
			batch_hint(b);
		}
		return;
	}
	/* No slot yet: a cross-producer cancel/resume reached the ring before
	 * its submit. Park it (bounded by cap; a duplicate handle just records
	 * the latest op, which is correct for a cancel-then-resume ordering is
	 * not possible since handles are never reused and a producer issues one
	 * op per handle after submit). */
#ifndef XTC_NO_EARLY_CONTROL
	if (b->mt.n_early < b->mt.cap) {
		b->mt.early[b->mt.n_early].handle = handle;
		b->mt.early[b->mt.n_early].op = op;
		b->mt.n_early++;
	}
#endif
}

/* Driver side: after creating a SUBMIT's slot, apply any early-control entry
 * pending for its handle and remove it. A pending cancel aborts the member
 * before it ever runs; a pending resume is a no-op on a fresh WAITING slot. */
static void
batch_mt_apply_early(xtc_batch_t *b, struct batch_slot *s)
{
	uint32_t i;
	for (i = 0; i < b->mt.n_early; ++i) {
		if (b->mt.early[i].handle != s->handle)
			continue;
		if (b->mt.early[i].op == MT_CANCEL)
			s->cancel = 1;
		/* RESUME on a fresh WAITING slot is a no-op. */
		b->mt.n_early--;
		b->mt.early[i] = b->mt.early[b->mt.n_early];
		return;
	}
}

/* Driver side: drain the whole admission ring at an iteration boundary, before
 * the next vector is built. SUBMIT creates a B_WAITING slot (the thread-safe
 * equivalent of the owner-loop submit body) then consults the early-control
 * set; CANCEL/RESUME apply to the slot or park early. Each drained record is
 * released back to the free-pool. Caller holds the unsafe bracket. */
static void
batch_mt_drain(xtc_batch_t *b)
{
	struct batch_mt_rec *rec;
	struct batch_slot *s;
	void *p;
	uint32_t i;
	if (b->mt.ring == NULL)
		return;
	while (xtc_chan_mpsc_try_recv(b->mt.ring, &p) == XTC_OK) {
		rec = p;
		if (rec->op == MT_SUBMIT) {
			/* A hand-in that passed the ring is admitted into a
			 * WAITING slot unless the group is closed or full --
			 * identical to the owner-loop submit body, but the
			 * handle was already stamped by the producer. */
			if (b->closed || b->live == b->opts.capacity) {
				batch_inc(&b->stats.rejections);
				batch_mt_release(b, rec);
				continue;
			}
			for (i = 0; i < b->opts.capacity; ++i)
				if (b->slots[i].state == B_FREE)
					break;
			s = &b->slots[i];
			s->handle = rec->handle;
			s->request = rec->request;
			s->charge = rec->charge;
			s->state = B_WAITING;
			s->cancel = 0;
			b->live++;
			batch_mt_apply_early(b, s);
		} else {
			batch_mt_control(b, rec->handle, rec->op);
		}
		batch_mt_release(b, rec);
	}
}

/* FUTURE kind: build the edge-triggered cancel path for the op just launched.
 * Makes a cancel promise/future pair owned solely by the inflight record and
 * sets b->inflight.wait = when_any{device future, cancel future}, so a device
 * completion OR a cancel-close resolution wakes the single wait. when_any
 * consumes BOTH input future handles, so the engine afterward owns exactly
 * `wait` (combined) and `cancel_prom` (write half), reclaimed once by the
 * finalizer. On OOM it degrades to parking directly on the device future (no
 * early cancel wake, completion still wakes); `wait` then IS the device future
 * and the finalizer discards it the same way. Called on the driver fiber. */
static void
batch_future_arm(xtc_batch_t *b)
{
	xtc_promise_t *cp = NULL;
	xtc_future_t *cf = NULL, *combined = NULL;
	xtc_future_t *inputs[2];
	if (xtc_future_new_pair(&cp, &cf) != XTC_OK) {
		b->inflight.wait = b->inflight.exec.future; /* degrade */
		b->inflight.exec.future = NULL;
		return;
	}
	inputs[0] = b->inflight.exec.future;
	inputs[1] = cf;
	if (xtc_future_when_any(inputs, 2, &combined) != XTC_OK) {
		/* when_any did NOT consume on failure: both handles still ours. */
		xtc_promise_drop(cp);
		__xtc_future_discard(cf);
		b->inflight.wait = b->inflight.exec.future; /* degrade */
		b->inflight.exec.future = NULL;
		return;
	}
	/* when_any consumed both device + cancel read halves. */
	b->inflight.exec.future = NULL;
	b->inflight.cancel_prom = cp;
	b->inflight.wait = combined;
}

/* Await the offload token. Returns the exec_status to hand collect: OK on
 * completion, TIMEOUT if the budget elapsed, ABORTED if the group was cancel-
 * closed while the op was outstanding. Not reached for SYNC (no park).
 *
 * The engine OWNS the FUTURE token's lifetime: submit builds a cancel
 * promise/future pair and parks on when_any{device future, cancel future}.
 * A device completion OR a cancel-close resolution wakes the single combined
 * wait with NO periodic re-park -- edge-triggered, not polled. The app keeps
 * the DEVICE promise and resolves/drops it; the engine keeps the CANCEL
 * promise (b->inflight.cancel_prom) and drops it in the finalizer. */
static int
batch_await(xtc_batch_t *b)
{
	intptr_t val;
	int rc, result = 0;
	switch (b->inflight.exec.kind) {
	case XTC_BATCH_EXEC_FENCE:
		/* The fd park (xtc_proc_wait_fd) is interruptible and mailbox-
		 * wakeable; its stack waiter is runtime-owned, so a kill unwind
		 * through batch_cleanup is safe. The app owns fence_fd. */
		rc = xtc_accel_wait_fence(b->inflight.exec.fence_fd,
		    b->exec_timeout_ns > 0 ? b->exec_timeout_ns : -1);
		return rc == XTC_E_AGAIN ? XTC_BATCH_EXEC_TIMEOUT :
		                           XTC_BATCH_EXEC_OK;
	case XTC_BATCH_EXEC_FUTURE:
		/* Edge-triggered: one wait on the when_any combined future.
		 * The app resolves the DEVICE promise on completion and the
		 * owner-loop close(CANCEL) path resolves the CANCEL promise on
		 * teardown -- either wakes this wait immediately, so there is
		 * no 1ms re-park and cancel is noticed with completion-wake
		 * latency, not a slice floor. b->cancelling (set by close
		 * CANCEL under the owner bracket before the cancel promise is
		 * resolved) tells cancel apart from completion after the wake.
		 * when_any already consumed the device/cancel read halves at
		 * submit; the combined future (b->inflight.wait) is the only
		 * handle this park touches. The wait runs masked so its stack
		 * waiter is unlinked before any kill can unwind; xtc_future_wait
		 * CONSUMES the combined future on completion and NOT on timeout,
		 * so a timeout leaves it for the finalizer to discard. */
		rc = XTC_BATCH_EXEC_OK;
		(void)xtc_mask_enter();
		{
			int wrc = xtc_future_wait(b->inflight.wait, &val,
			    b->exec_timeout_ns > 0 ? b->exec_timeout_ns : -1);
			if (wrc == XTC_E_AGAIN)
				rc = XTC_BATCH_EXEC_TIMEOUT;
			else {
				/* consumed by the wait */
				b->inflight.wait = NULL;
				if (b->cancelling)
					rc = XTC_BATCH_EXEC_ABORTED;
			}
		}
		(void)xtc_mask_leave();
		return rc;
	case XTC_BATCH_EXEC_BLOCKING:
		/* The pool call runs to completion; no engine timeout applies
		 * to a blocking closure (it holds a pool thread for its whole
		 * run). */
		(void)xtc_blocking_run(b->inflight.exec.run,
		                       b->inflight.exec.arg, &result);
		return XTC_BATCH_EXEC_OK;
	default:
		return XTC_BATCH_EXEC_OK;
	}
}

static void
batch_driver(void *arg)
{
	xtc_batch_t *b = arg;
	struct batch_slot *s;
	xtc_batch_result_t *r;
	size_t n, j;
	uint32_t i;
	int rc, finished, pending, exec_status;
	xtc_batch_exec_t exec;
	if (xtc_proc_at_exit(batch_cleanup, b) != XTC_OK) {
		batch_cleanup(b);
		return;
	}
	for (;;) {
		__xtc_unsafe_enter();
		/* Drain cross-loop hand-ins FIRST: create WAITING slots for
		 * SUBMITs and apply/park CANCEL/RESUME control records, so an
		 * early cancel is honored in the same cancel-consume scan below
		 * and before batch_vector exposes any member. */
		batch_mt_drain(b);
		for (i = 0; i < b->opts.capacity; ++i) {
			s = &b->slots[i];
			if ((s->state == B_WAITING || s->state == B_READY
			     || s->state == B_BLOCKED)
			    && (b->cancelling || s->cancel))
				batch_terminal(b, s, XTC_E_ABORTED);
		}
		__xtc_unsafe_leave();
		batch_deliver(b);
		(void)xtc_mask_enter();
		__xtc_unsafe_enter();
		n = batch_vector(b);
		finished = b->closed && b->live == 0;
		pending = 0;
		for (i = 0; i < b->opts.capacity; ++i)
			pending |= b->slots[i].state == B_TERMINAL;
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
			if (!finished && !pending)
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
				b->slots[b->indices[j]].state = B_INFLIGHT;
			b->inflight.exec = exec;
			b->inflight.idx = b->indices; /* frozen vector order */
			b->inflight.n = n;
			b->inflight.valid = 1;
			batch_inc(&b->astats.offloads);
			b->callback = 0;
			__xtc_unsafe_leave();
			(void)xtc_mask_leave();
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
				s->state = B_READY;
#else
				batch_terminal(b, s, XTC_OK);
#endif
			else if (r->state == XTC_BATCH_BLOCKED)
				s->state = B_BLOCKED;
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

/* Shared tail of create / create_async: allocate per-group storage, attach the
 * class, and spawn the pinned driver. b->opts and the async fields must be set
 * by the caller. On any failure b is freed and *out is untouched. */
static int
batch_finish_create(xtc_batch_t *b, xtc_loop_t *loop, int lane,
    xtc_batch_t **out)
{
	xtc_proc_opts_t po = { 0 };
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
	    || (rc = xtc_notify_create(&b->notify)) != XTC_OK
	    || (rc = batch_mt_init(b, b->mt_cap_req)) != XTC_OK) {
		batch_free(b);
		return rc;
	}
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
 * with a CALLER-CHOSEN handle (bypassing the atomic stamp), so a test can
 * force the exact cross-producer ring order -- e.g. a CANCEL for handle H
 * BEFORE H's SUBMIT -- and prove the early-control set never loses a cancel.
 * Returns XTC_OK, XTC_E_AGAIN (ring/pool full), or XTC_E_NOSYS (mt disabled).
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
	uint32_t i;
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
		else if (b->closed || b->live == b->opts.capacity)
			rc = XTC_E_AGAIN;
		if (rc != XTC_OK)
			batch_inc(&b->stats.rejections);
		else {
			for (i = 0; i < b->opts.capacity; ++i)
				if (b->slots[i].state == B_FREE)
					break;
			b->last_handle = batch_next_handle(b);
			b->slots[i].handle = b->last_handle;
			b->slots[i].request = request;
			b->slots[i].charge = charge;
			b->slots[i].state = B_WAITING;
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
			s->cancel = 1;
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
			s->state = B_READY;
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
	rec = batch_mt_claim(b);
	if (rec == NULL) {
		batch_inc_atomic(&b->mt.n_rejections);
		return XTC_E_AGAIN;
	}
	handle = batch_next_handle(b);
	if (handle == 0) {
		batch_mt_release(b, rec);
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
		batch_inc_atomic(&b->mt.n_rejections);
		return XTC_E_AGAIN;
	}
	batch_inc_atomic(&b->mt.n_queued);
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
 * so FIFO order to the single driver is preserved. The driver applies it to
 * the handle's slot, or parks it in the early-control set if the slot does not
 * yet exist (a cross-producer control that reached the ring before its
 * submit) -- so a cancel is never lost regardless of producer/ring order. */
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
	rec = batch_mt_claim(b);
	if (rec == NULL) {
		batch_inc_atomic(&b->mt.n_rejections);
		return XTC_E_AGAIN;
	}
	rec->op = op;
	rec->handle = handle;
	rec->request = NULL;
	rec->charge = 0;
	if (batch_mt_push(b, rec) != XTC_OK) {
		batch_inc_atomic(&b->mt.n_rejections);
		return XTC_E_AGAIN;
	}
	batch_inc_atomic(&b->mt.n_queued);
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
			/* Edge-triggered abort of an outstanding FUTURE op:
			 * resolve the engine-owned cancel promise so the driver's
			 * when_any wait wakes IMMEDIATELY (no 1ms slice). Set
			 * consumes the handle, so null it; the finalizer then has
			 * nothing left to drop. b->cancelling is already set above
			 * so batch_await maps the wake to ABORTED. Runs on the
			 * owner loop, same as the driver, so no cross-thread race
			 * with the park. */
			if (mode == XTC_BATCH_CANCEL && b->inflight.valid
			    && b->inflight.cancel_prom != NULL) {
				(void)xtc_promise_set(b->inflight.cancel_prom, 0,
				    XTC_E_ABORTED);
				b->inflight.cancel_prom = NULL;
			}
			(void)xtc_notify_signal(b->notify);
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
	uint32_t i;
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
			for (i = 0; i < b->opts.capacity; ++i) {
				if (b->slots[i].state == B_WAITING)
					out->queued++;
				if (b->slots[i].state == B_READY
				    || b->slots[i].state == B_INFLIGHT)
					out->ready++;
				if (b->slots[i].state == B_BLOCKED)
					out->blocked++;
			}
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
	/* First loop whose carrier recorded that node wins. A loop whose
	 * node is unknown (-1, carrier not yet run) never matches, so a
	 * pre-run call falls through to loop 0. */
	for (i = 0; i < n_loops; ++i) {
		extern int __xtc_exec_loop_node(xtc_exec_t *, int);
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
