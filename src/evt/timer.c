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
 */

#include "xtc_int.h"
#include "loop_int.h"
#include "xtc_slab.h"

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
	return xtc_slab_create(&o, (struct xtc_slab **)&loop->timer_slab);
}

/* --- Heap manipulation. -------------------------------------------- */

/* XTC_NOALLOC_BEGIN: heap swap helper (PLAN.md 19.23) -- shared by
 * both the insert-side sift-up and the per-tick dequeue-side
 * sift-down; never itself allocates. */
static void
__swap(xtc_timer_t **a, xtc_timer_t **b)
{
	xtc_timer_t *tmp = *a;
	int idx_a = (*a)->heap_idx;
	int idx_b = (*b)->heap_idx;
	*a = *b; (*a)->heap_idx = idx_a;
	*b = tmp; (*b)->heap_idx = idx_b;
}
/* XTC_NOALLOC_END */

static void
__sift_up(xtc_timer_t **h, int i)
{
	while (i > 0) {
		int parent = (i - 1) / 2;
		if (h[parent]->deadline_ns <= h[i]->deadline_ns) break;
		__swap(&h[parent], &h[i]);
		i = parent;
	}
}

static void
__sift_down(xtc_timer_t **h, int n, int i)
{
	for (;;) {
		int l = 2 * i + 1;
		int r = 2 * i + 2;
		int best = i;
		if (l < n && h[l]->deadline_ns < h[best]->deadline_ns) best = l;
		if (r < n && h[r]->deadline_ns < h[best]->deadline_ns) best = r;
		if (best == i) break;
		__swap(&h[i], &h[best]);
		i = best;
	}
}

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

int
__xtc_timer_heap_push(xtc_loop_t *loop, xtc_timer_t *t)
{
	int rc;
	XTC_ASSERT_LOOP_OWNER(loop, "timer min-heap (push)");
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
	t->heap_idx = loop->n_timers;
	loop->timers[loop->n_timers++] = t;
	__sift_up(loop->timers, t->heap_idx);
	return XTC_OK;
}

/*
 * Drop one park-timer reference (see loop_int.h).  refs == 0 marks a
 * non-refcounted timer (xtc_timer_set, owned by all_timers): a no-op.
 * The last of the two owners (heap, task park_timer slot) to release
 * frees the node.  A bounded O(1) free; see the NOALLOC note on the
 * pop-due callers.  Defined ABOVE the NOALLOC region so its __os_free
 * is not flagged -- the callers inside the region carry XTC_NOALLOC_OK.
 */
void
__xtc_timer_park_release(xtc_timer_t *t)
{
	if (t == NULL)
		return;
	if (atomic_load_explicit(&t->refs, memory_order_relaxed) == 0)
		return;   /* not a park timer: freed at loop_fini */
	if (atomic_fetch_sub_explicit(&t->refs, 1, memory_order_acq_rel) == 1)
		__os_free(t);
}

static int
__is_cancelled(xtc_timer_t *t)
{
	return atomic_load_explicit(&t->cancelled, memory_order_acquire);
}

/* Remove the node at heap index i (owner only).  The caller drops the
 * heap's reference. */
static void
__heap_remove_at(xtc_loop_t *loop, int i)
{
	xtc_timer_t **h = loop->timers;
	int last = --loop->n_timers;
	h[i]->heap_idx = -1;
	if (i == last)
		return;
	h[i] = h[last];
	h[i]->heap_idx = i;
	if (i > 0 && h[i]->deadline_ns < h[(i - 1) / 2]->deadline_ns)
		__sift_up(h, i);
	else
		__sift_down(h, loop->n_timers, i);
}

/* Drop every cancelled node from the heap and rebuild it (owner only). */
static void
__sweep_cancelled(xtc_loop_t *loop)
{
	xtc_timer_t **h = loop->timers;
	int i, n = 0;
	for (i = 0; i < loop->n_timers; i++) {
		xtc_timer_t *t = h[i];
		if (__is_cancelled(t)) {
			t->heap_idx = -1;
			__xtc_timer_park_release(t);   /* the heap's reference */
			continue;
		}
		t->heap_idx = n;
		h[n++] = t;
	}
	loop->n_timers = n;
	for (i = n / 2 - 1; i >= 0; i--)
		__sift_down(h, n, i);
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
	return loop->n_timers == 0 ? NULL : loop->timers[0];
}

static xtc_timer_t *
__heap_pop_unsafe(xtc_loop_t *loop)
{
	xtc_timer_t *top;
	if (loop->n_timers == 0) return NULL;
	top = loop->timers[0];
	top->heap_idx = -1;
	loop->n_timers--;
	if (loop->n_timers > 0) {
		loop->timers[0] = loop->timers[loop->n_timers];
		loop->timers[0]->heap_idx = 0;
		__sift_down(loop->timers, loop->n_timers, 0);
	}
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
		if (__is_cancelled(top)) {
			(void)__heap_pop_unsafe(loop);
			__xtc_timer_park_release(top);  /* XTC_NOALLOC_OK: bounded free of one popped park timer */
			continue;
		}
		if (top->deadline_ns > now_ns) return NULL;
		return __heap_pop_unsafe(loop);
	}
}

int64_t
__xtc_timer_heap_next_deadline(xtc_loop_t *loop)
{
	while (loop->n_timers > 0 && __is_cancelled(loop->timers[0])) {
		xtc_timer_t *dead = loop->timers[0];
		(void)__heap_pop_unsafe(loop);
		__xtc_timer_park_release(dead);  /* XTC_NOALLOC_OK: bounded free of one popped park timer */
	}
	return loop->n_timers == 0 ? -1 : loop->timers[0]->deadline_ns;
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

	if ((rc = __timer_slab_ensure(loop)) != XTC_OK)
		return rc;
	t = xtc_slab_alloc((struct xtc_slab *)loop->timer_slab);
	if (t == NULL) return XTC_E_RESOURCE;
	memset(t, 0, sizeof *t);
	if ((rc = __os_clock_mono(&now_ns)) != XTC_OK) {
		xtc_slab_free((struct xtc_slab *)loop->timer_slab, t);
		return rc;
	}
	t->deadline_ns = now_ns + delay_ns;
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
	/* Splice into all_timers so loop_fini can free it. */
	t->all_next = loop->all_timers;
	loop->all_timers = t;
	if (out_timer) *out_timer = t;
	return XTC_OK;
}

/* PUBLIC: int xtc_timer_cancel __P((xtc_timer_t *)); */
int
xtc_timer_cancel(xtc_timer_t *timer)
{
	xtc_loop_t *loop;
	if (timer == NULL) return XTC_E_INVAL;
	if (atomic_load_explicit(&timer->fired, memory_order_acquire))
		return XTC_OK;     /* Tm8: already fired, no-op */
	atomic_store_explicit(&timer->cancelled, 1, memory_order_release);
	/*
	 * On the loop's own thread, take the node out of the heap now (Tm13):
	 * leaving it until it reached the root let one earlier live timer pin
	 * every cancelled one behind it.  heap_idx is owner-only and -1 once
	 * the node has left the heap.  Off the owner thread only the mark is
	 * safe; the owner drops the node at the root or in the sweep before
	 * the heap grows.  __xtc_current_loop_get is the out-of-line read, so
	 * a fiber that migrated cannot see its previous thread's binding.
	 */
	loop = timer->loop;
	if (loop != NULL && __xtc_current_loop_get() == loop &&
	    timer->heap_idx >= 0) {
		XTC_ASSERT_LOOP_OWNER(loop, "timer min-heap (cancel)");
		__heap_remove_at(loop, timer->heap_idx);
		__xtc_timer_park_release(timer);   /* the heap's reference */
	}
	return XTC_OK;
}
