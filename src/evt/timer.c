/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/evt/timer.c
 *	Binary min-heap of pending timers.  A cancel on the loop's own
 *	thread removes the node at once; a cancel from another thread only
 *	marks it, and the owner drops marked nodes when they reach the root
 *	or when a full heap is swept before it would grow.
 *	See M3_CLAIMS.md, Tm5-Tm13.
 *
 *	The heap array holds {deadline, node} slots, so a sift reads keys
 *	from the array instead of two scattered nodes per level, and moves
 *	a hole instead of swapping.  It makes the same comparisons as the
 *	pointer heap it replaced, so equal deadlines (common under DST's
 *	virtual clock) fire in the same order.  bench/bench_timerq.c
 *	compares it with d-ary heaps, a timing wheel, a radix heap and a
 *	QuickHeap.
 */

#include "xtc_int.h"
#include "loop_int.h"
#include "xtc_slab.h"
#include "xtc_probes.h"
#include "xtc_inject.h"   /* timer.heap.sift_up test hook */
#include "preempt_int.h"  /* __xtc_unsafe_enter/leave: heap updates */

#include <stdint.h>
#include <string.h>

/* M11.5b: ensure the loop has a timer slab.  Lazy on first set. */
static int
__timer_slab_ensure(xtc_loop_t *loop)
{
	xtc_slab_opts_t o;
	if (loop->timer_slab != NULL) return XTC_OK;
	o = (xtc_slab_opts_t)XTC_SLAB_OPTS_DEFAULT;
	o.name = "loop.timer";
	o.obj_size = sizeof(xtc_timer_t);
	/* No per-thread magazine: a timer's last reference may be dropped on
	 * any thread (xtc_timer_release, a cross-thread cancel), and a
	 * magazine on a thread other than the one that destroys the slab at
	 * loop_fini would be left holding slots of a freed slab.  Until timers
	 * were reclaimed during the run the magazine was always empty anyway,
	 * so every alloc already took the locked path. */
	o.flags |= XTC_SLAB_NO_MAGAZINE;
	return xtc_slab_create(&o, (struct xtc_slab **)&loop->timer_slab);
}

/* --- Heap manipulation. -------------------------------------------- */

/* XTC_NOALLOC_BEGIN: heap sift helpers (PLAN.md 19.23) -- shared by the
 * insert-side sift-up and the per-tick dequeue-side sift-down; never
 * allocate.  Each places slot x by moving the hole at i; x comes by
 * value so a push or remove does not store a slot only to reload it. */
static void
__sift_up(struct xtc_timer_slot *h, int i, struct xtc_timer_slot x)
{
	while (i > 0) {
		int parent = (i - 1) / 2;
		if (h[parent].deadline_ns <= x.deadline_ns) break;
		h[i] = h[parent];
		h[i].t->heap_idx = i;
		/* Test hook: h[parent] is duplicated until the hole is
		 * filled (test_preempt_p2 /timer_heap_*). */
		XTC_INJECTION_POINT("timer.heap.sift_up");
		i = parent;
	}
	h[i] = x;
	x.t->heap_idx = i;
}

static void
__sift_down(struct xtc_timer_slot *h, int n, int i, struct xtc_timer_slot x)
{
	for (;;) {
		int l = 2 * i + 1;
		int r = l + 1;
		int c;
		if (l >= n) break;
		/* The smaller child, the left on a tie: the old swap-based
		 * sift's choice, so ties pop in the same order. */
		c = r < n && h[r].deadline_ns < h[l].deadline_ns ? r : l;
		if (h[c].deadline_ns >= x.deadline_ns) break;
		h[i] = h[c];
		h[i].t->heap_idx = i;
		i = c;
	}
	h[i] = x;
	x.t->heap_idx = i;
}
/* XTC_NOALLOC_END */

static void __sweep_cancelled(xtc_loop_t *loop);

static int
__grow(xtc_loop_t *loop)
{
	int new_cap = loop->cap_timers == 0 ? 8 : loop->cap_timers * 2;
	void *q = NULL;
	int rc = __os_realloc(loop->timers,
	    sizeof(*loop->timers) * (size_t)new_cap, &q);
	if (rc != XTC_OK) return rc;
	loop->timers = q;
	loop->cap_timers = new_cap;
	return XTC_OK;
}

/*
 * The heap is single-owner state that the scheduler on this thread also
 * mutates (pop_due), and fibers push and cancel: every fiber-reachable
 * update holds preemption off, or a tick mid-sift lets another fiber's
 * push land in a half-sifted heap and be overwritten (test_preempt_p2
 * /timer_heap_lost_timer).
 */
static int
__heap_push(xtc_loop_t *loop, xtc_timer_t *t)
{
	struct xtc_timer_slot x;
	int rc;
	if (loop->n_timers >= loop->cap_timers) {
		/* Full: first drop the nodes cancelled from other threads, and
		 * grow only if the heap is still more than half full, so the
		 * O(n) sweep runs at most once per n/2 pushes and the heap stays
		 * within a small factor of the live timer count. */
		if (loop->n_timers > 0)
			__sweep_cancelled(loop);
		if ((loop->cap_timers == 0 ||
		    loop->n_timers > loop->cap_timers / 2) &&
		    (rc = __grow(loop)) != XTC_OK)
			return rc;
	}
	/* The DST late-fire path re-pushes a node its pop already claimed;
	 * make it cancellable again. */
	if (atomic_load_explicit(&t->fired, memory_order_relaxed) ==
	    XTC_TIMER_FIRED)
		atomic_store_explicit(&t->fired, 0, memory_order_relaxed);
	x.deadline_ns = t->deadline_ns;
	x.t = t;
	__sift_up(loop->timers, loop->n_timers++, x);
	return XTC_OK;
}

int
__xtc_timer_heap_push(xtc_loop_t *loop, xtc_timer_t *t)
{
	int rc;
	XTC_ASSERT_LOOP_OWNER(loop, "timer min-heap (push)");
	__xtc_unsafe_enter();
	rc = __heap_push(loop, t);
	__xtc_unsafe_leave();
	return rc;
}

/*
 * Drop one timer reference (see loop_int.h).  Every timer has two owners:
 * the heap, and the task's park_timer slot (park timers) or the caller's
 * handle (xtc_timer_set).  The last to let go frees the node, to the
 * loop's timer slab or the heap allocator it came from.  A bounded O(1)
 * free; see the NOALLOC note on the pop-due callers.  Defined ABOVE the
 * NOALLOC region so its frees are not flagged -- the callers inside the
 * region carry XTC_NOALLOC_OK.
 */
void
__xtc_timer_park_release(xtc_timer_t *t)
{
	if (t == NULL)
		return;
	if (atomic_load_explicit(&t->refs, memory_order_relaxed) == 0)
		return;   /* defensive: no owner left */
	if (atomic_fetch_sub_explicit(&t->refs, 1, memory_order_acq_rel) != 1)
		return;
	if (t->from_slab)
		xtc_slab_free((struct xtc_slab *)t->loop->timer_slab, t);
	else
		__os_free(t);
}

static int
__is_cancelled(xtc_timer_t *t)
{
	return atomic_load_explicit(&t->fired, memory_order_acquire) ==
	    XTC_TIMER_CANCELLED;
}

/* Remove the node at heap index i (owner only).  The caller drops the
 * heap's reference. */
static void
__heap_remove_at(xtc_loop_t *loop, int i)
{
	struct xtc_timer_slot *h = loop->timers;
	int last = --loop->n_timers;
	h[i].t->heap_idx = -1;
	if (i == last)
		return;
	if (i > 0 && h[last].deadline_ns < h[(i - 1) / 2].deadline_ns)
		__sift_up(h, i, h[last]);
	else
		__sift_down(h, loop->n_timers, i, h[last]);
}

/* Drop every cancelled node from the heap and rebuild it (owner only). */
static void
__sweep_cancelled(xtc_loop_t *loop)
{
	struct xtc_timer_slot *h = loop->timers;
	int i, n = 0;
	for (i = 0; i < loop->n_timers; i++) {
		xtc_timer_t *t = h[i].t;
		if (__is_cancelled(t)) {
			t->heap_idx = -1;
			__xtc_timer_park_release(t);   /* the heap's reference */
			continue;
		}
		t->heap_idx = n;
		h[n++] = h[i];
	}
	loop->n_timers = n;
	for (i = n / 2 - 1; i >= 0; i--)
		__sift_down(h, n, i, h[i]);
}

/* XTC_NOALLOC_BEGIN: per-tick timer dequeue/peek path (PLAN.md 19.23) --
 * __xtc_loop_step calls __xtc_timer_heap_pop_due and
 * __xtc_timer_heap_next_deadline every scheduler tick; none of
 * __heap_top_unsafe / __heap_pop_unsafe / __sift_down (reused from
 * the insert side above, itself allocation-free) / the two public
 * pop/peek entry points may allocate.  __grow and
 * __xtc_timer_heap_push (the INSERT side, called from xtc_timer_set,
 * not from the per-tick step) are deliberately left OUTSIDE this
 * region -- __grow calls __os_realloc to grow the heap array. */
static xtc_timer_t *
__heap_top_unsafe(xtc_loop_t *loop)
{
	return loop->n_timers == 0 ? NULL : loop->timers[0].t;
}

static xtc_timer_t *
__heap_pop_unsafe(xtc_loop_t *loop)
{
	xtc_timer_t *top;
	if (loop->n_timers == 0) return NULL;
	top = loop->timers[0].t;
	top->heap_idx = -1;
	loop->n_timers--;
	if (loop->n_timers > 0)
		__sift_down(loop->timers, loop->n_timers, 0,
		    loop->timers[loop->n_timers]);
	return top;
}

/*
 * Pop the next timer if it is due, dropping the heap reference of any
 * cancelled park timer it skips.  Freeing a fired/cancelled park timer
 * here (and in the drain that pops due ones) is a bounded O(1)
 * deallocation, not an allocation -- the NOALLOC invariant guards the
 * insert path against allocator growth/contention, so the release is
 * marked XTC_NOALLOC_OK rather than deferred.  A non-refcounted timer is
 * owned by all_timers and freed at loop_fini.  See M3_CLAIMS.md Tm8.
 */
xtc_timer_t *
__xtc_timer_heap_pop_due(xtc_loop_t *loop, int64_t now_ns)
{
	for (;;) {
		xtc_timer_t *top = __heap_top_unsafe(loop);
		if (top == NULL) return NULL;
		int expect = 0;
		if (!__is_cancelled(top)) {
			if (loop->timers[0].deadline_ns > now_ns) return NULL;
			/* Claim it to fire; a concurrent cancel that got there
			 * first wins, and the node is dropped below. */
			if (atomic_compare_exchange_strong_explicit(&top->fired,
			    &expect, XTC_TIMER_FIRED, memory_order_acq_rel,
			    memory_order_acquire))
				return __heap_pop_unsafe(loop);
		}
		(void)__heap_pop_unsafe(loop);
		__xtc_timer_park_release(top);  /* XTC_NOALLOC_OK: bounded free of one popped timer */
	}
}

int64_t
__xtc_timer_heap_next_deadline(xtc_loop_t *loop)
{
	while (loop->n_timers > 0 && __is_cancelled(loop->timers[0].t)) {
		xtc_timer_t *dead = loop->timers[0].t;
		(void)__heap_pop_unsafe(loop);
		__xtc_timer_park_release(dead);  /* XTC_NOALLOC_OK: bounded free of one popped timer */
	}
	return loop->n_timers == 0 ? -1 : loop->timers[0].deadline_ns;
}
/* XTC_NOALLOC_END */

/* --- Public API. --------------------------------------------------- */

/* PUBLIC: int xtc_timer_set __P((xtc_loop_t *, int64_t, xtc_timer_fn, void *, xtc_timer_t **)); */
int
xtc_timer_set(xtc_loop_t *loop, int64_t delay_ns, xtc_timer_fn fn, void *user,
              xtc_timer_t **out_timer)
{
	xtc_timer_t *t;
	int64_t now_ns;
	int rc;

	if (loop == NULL || delay_ns < 0)
		return XTC_E_INVAL;

	/* Lazy create: two fibers racing it would each store a slab. */
	__xtc_unsafe_enter();
	rc = __timer_slab_ensure(loop);
	__xtc_unsafe_leave();
	if (rc != XTC_OK)
		return rc;
	t = xtc_slab_alloc((struct xtc_slab *)loop->timer_slab);
	if (t == NULL) return XTC_E_RESOURCE;
	memset(t, 0, sizeof *t);
	if ((rc = __os_clock_mono(&now_ns)) != XTC_OK) {
		xtc_slab_free((struct xtc_slab *)loop->timer_slab, t);
		return rc;
	}
	t->deadline_ns = now_ns + delay_ns;
	/* Owners: the heap, plus the caller's handle if one is returned.
	 * With no handle the timer is freed as soon as it fires. */
	atomic_store_explicit(&t->refs, out_timer != NULL ? 2 : 1,
	    memory_order_relaxed);
	t->cb = fn;
	t->user = user;
	t->waiter = NULL;
	t->cancelled = 0;
	t->fired = 0;
	t->heap_idx = -1;
	t->loop = loop;
	t->from_slab = 1;

	if ((rc = __xtc_timer_heap_push(loop, t)) != XTC_OK) {
		xtc_slab_free((struct xtc_slab *)loop->timer_slab, t);
		return rc;
	}
	XTC_PROBE_TIMER_ARM((uintptr_t)t, t->deadline_ns, (uintptr_t)0);
	/* Not on all_timers: a handle that is never released is freed with
	 * the timer slab at loop_fini. */
	if (out_timer) *out_timer = t;
	return XTC_OK;
}

/*
 * Cancel `timer` if it has not fired.  Returns 1 if this call stopped the
 * fire (the callback will never run), 0 if the timer had fired, was
 * firing, or was already cancelled.  `fired` is the arbiter: it leaves 0
 * exactly once, to FIRED (the owner's pop claimed it) or to CANCELLED.
 * The caller must hold a reference (a handle or a park slot).
 */
int
__xtc_timer_try_cancel(xtc_timer_t *timer)
{
	xtc_loop_t *loop = timer->loop;
	int expect = 0;
	if (!atomic_compare_exchange_strong_explicit(&timer->fired, &expect,
	    XTC_TIMER_CANCELLED, memory_order_acq_rel, memory_order_acquire))
		return 0;
	atomic_store_explicit(&timer->cancelled, 1, memory_order_release);
	XTC_PROBE_TIMER_CANCEL((uintptr_t)timer);
	/*
	 * On the loop's own thread, take the node out of the heap now (Tm13):
	 * leaving it until it reached the root let one earlier live timer pin
	 * every cancelled one behind it.  heap_idx is owner-only and -1 once
	 * the node has left the heap.  Off the owner thread only the mark is
	 * safe; the owner drops the node at the root or in the sweep before
	 * the heap grows.  __xtc_current_loop_get is the out-of-line read, so
	 * a fiber that migrated cannot see its previous thread's binding.
	 */
	__xtc_unsafe_enter();   /* check through removal: see the push */
	if (__xtc_current_loop_get() == loop && timer->heap_idx >= 0) {
		XTC_ASSERT_LOOP_OWNER(loop, "timer min-heap (cancel)");
		__heap_remove_at(loop, timer->heap_idx);
		__xtc_timer_park_release(timer);   /* the heap's reference */
	}
	__xtc_unsafe_leave();
	return 1;
}

/* PUBLIC: int xtc_timer_cancel __P((xtc_timer_t *)); */
int
xtc_timer_cancel(xtc_timer_t *timer)
{
	if (timer == NULL) return XTC_E_INVAL;
	(void)__xtc_timer_try_cancel(timer);   /* Tm8: after fire, a no-op */
	return XTC_OK;
}

/* PUBLIC: int xtc_timer_release __P((xtc_timer_t *)); */
int
xtc_timer_release(xtc_timer_t *timer)
{
	/* Only xtc_timer_set hands out handles (from_slab); a park timer is
	 * never the caller's to release. */
	if (timer == NULL || !timer->from_slab) return XTC_E_INVAL;
	__xtc_timer_park_release(timer);   /* the handle's reference */
	return XTC_OK;
}
