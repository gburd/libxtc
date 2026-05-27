/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/evt/timer.c
 *	Binary min-heap of pending timers.  Lazy deletion: a cancelled
 *	timer is left in the heap and skipped on extraction.
 *	See M3_CLAIMS.md, Tm5-Tm10.
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

static void
__swap(xtc_timer_t **a, xtc_timer_t **b)
{
	xtc_timer_t *tmp = *a;
	int idx_a = (*a)->heap_idx;
	int idx_b = (*b)->heap_idx;
	*a = *b; (*a)->heap_idx = idx_a;
	*b = tmp; (*b)->heap_idx = idx_b;
}

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
	if (loop->n_timers >= loop->cap_timers) {
		if ((rc = __grow(loop)) != XTC_OK) return rc;
	}
	t->heap_idx = loop->n_timers;
	loop->timers[loop->n_timers++] = t;
	__sift_up(loop->timers, t->heap_idx);
	return XTC_OK;
}

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
 * Pop the next timer if it is due.  The caller must NOT free the
 * returned timer; it is owned by loop->all_timers and freed at
 * loop_fini.  See M3_CLAIMS.md Tm8.
 *
 * We skip past cancelled-and-still-on-heap entries silently.
 */
xtc_timer_t *
__xtc_timer_heap_pop_due(xtc_loop_t *loop, int64_t now_ns)
{
	for (;;) {
		xtc_timer_t *top = __heap_top_unsafe(loop);
		if (top == NULL) return NULL;
		if (top->cancelled) {
			(void)__heap_pop_unsafe(loop);
			continue;
		}
		if (top->deadline_ns > now_ns) return NULL;
		return __heap_pop_unsafe(loop);
	}
}

int64_t
__xtc_timer_heap_next_deadline(xtc_loop_t *loop)
{
	while (loop->n_timers > 0 && loop->timers[0]->cancelled)
		(void)__heap_pop_unsafe(loop);
	return loop->n_timers == 0 ? -1 : loop->timers[0]->deadline_ns;
}

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
	if (timer == NULL) return XTC_E_INVAL;
	if (timer->fired) return XTC_OK;     /* Tm8: already fired, no-op */
	timer->cancelled = 1;
	return XTC_OK;
}
