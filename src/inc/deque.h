/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/deque.h
 *	A bounded Chase-Lev work-stealing deque.  Single-owner pushes
 *	and pops on the bottom; multiple thieves CAS-steal from the
 *	top.  See:
 *	  Chase, D. & Lev, Y. "Dynamic Circular Work-Stealing Deque",
 *	  SPAA 2005.
 *
 *	M5 ships a fixed-capacity variant: callers fall back to a
 *	mutex-protected slow-path queue when push fails with
 *	XTC_E_AGAIN.  M5.5 may add CAS-based growth.
 *
 *	The deque stores `void *` and is generic; the run-queue uses
 *	xtc_task_t* by convention.
 */

#ifndef XTC_DEQUE_H
#define XTC_DEQUE_H

#include <stdatomic.h>
#include <stdint.h>

#include "xtc.h"

/*
 * Capacity must be a power of two.  256 is generous for a per-loop
 * run queue; PG-style backends typically have <128 in flight.
 */
#define XTC_DEQUE_CAP 256
#define XTC_DEQUE_MASK (XTC_DEQUE_CAP - 1)

typedef struct xtc_deque {
	/*
	 * top and bottom are 64-bit indices; we mask them by the
	 * capacity at slot access.  Using 64-bit indices avoids any
	 * wrap concern inside the lifetime of a process.
	 *
	 * - `bottom` is owned by the owner thread; only it writes.
	 * - `top` is read by everyone, written via CAS by thieves
	 *   (to claim a slot) and by the owner (when popping the
	 *   last item to break a tie with a thief).
	 */
	_Atomic int64_t  top;
	_Atomic int64_t  bottom;
	_Atomic(void *)  buf[XTC_DEQUE_CAP];
} xtc_deque_t;

static inline void
xtc_deque_init(xtc_deque_t *d)
{
	int i;
	atomic_store_explicit(&d->top, 0, memory_order_relaxed);
	atomic_store_explicit(&d->bottom, 0, memory_order_relaxed);
	for (i = 0; i < XTC_DEQUE_CAP; i++)
		atomic_store_explicit(&d->buf[i], NULL, memory_order_relaxed);
}

/*
 * Owner-side push.  Returns XTC_OK on success, XTC_E_AGAIN if the
 * deque is full (caller must use the slow-path overflow queue).
 */
static inline int
xtc_deque_push(xtc_deque_t *d, void *v)
{
	int64_t b = atomic_load_explicit(&d->bottom, memory_order_relaxed);
	int64_t t = atomic_load_explicit(&d->top,    memory_order_acquire);
	if (b - t >= XTC_DEQUE_CAP) return XTC_E_AGAIN;
	atomic_store_explicit(&d->buf[b & XTC_DEQUE_MASK], v,
	    memory_order_relaxed);
	atomic_thread_fence(memory_order_release);
	atomic_store_explicit(&d->bottom, b + 1, memory_order_relaxed);
	return XTC_OK;
}

/*
 * Owner-side pop (LIFO).  Returns NULL if the deque is empty.
 *
 * The tricky case is a single-element deque: both the owner and a
 * thief can race for it.  We resolve via CAS on `top`: whoever wins
 * gets the item; the loser sees empty.
 */
static inline void *
xtc_deque_pop(xtc_deque_t *d)
{
	int64_t b = atomic_load_explicit(&d->bottom, memory_order_relaxed) - 1;
	int64_t t;
	void *x;

	atomic_store_explicit(&d->bottom, b, memory_order_relaxed);
	atomic_thread_fence(memory_order_seq_cst);
	t = atomic_load_explicit(&d->top, memory_order_relaxed);

	if (t > b) {
		/* Empty. */
		atomic_store_explicit(&d->bottom, t, memory_order_relaxed);
		return NULL;
	}

	x = atomic_load_explicit(&d->buf[b & XTC_DEQUE_MASK],
	    memory_order_relaxed);
	if (t < b) return x;        /* uncontested */

	/* t == b: last element, race with thief. */
	if (!atomic_compare_exchange_strong_explicit(
	        &d->top, &t, t + 1,
	        memory_order_seq_cst, memory_order_relaxed))
		x = NULL;             /* lost the race */
	atomic_store_explicit(&d->bottom, b + 1, memory_order_relaxed);
	return x;
}

/*
 * Thief-side steal (FIFO).  Returns NULL on empty or on losing the
 * CAS race with another thief or with the owner's pop.
 */
static inline void *
xtc_deque_steal(xtc_deque_t *d)
{
	int64_t t = atomic_load_explicit(&d->top, memory_order_acquire);
	int64_t b;
	void *x;

	atomic_thread_fence(memory_order_seq_cst);
	b = atomic_load_explicit(&d->bottom, memory_order_acquire);

	if (t >= b) return NULL;    /* empty */
	x = atomic_load_explicit(&d->buf[t & XTC_DEQUE_MASK],
	    memory_order_relaxed);
	if (!atomic_compare_exchange_strong_explicit(
	        &d->top, &t, t + 1,
	        memory_order_seq_cst, memory_order_relaxed))
		return NULL;          /* lost race */
	return x;
}

/*
 * Approximate length.  May return slightly stale values; useful for
 * stealing heuristics ("which loop is busiest?").
 */
static inline int64_t
xtc_deque_len(const xtc_deque_t *d)
{
	int64_t b = atomic_load_explicit(&d->bottom, memory_order_acquire);
	int64_t t = atomic_load_explicit(&d->top,    memory_order_acquire);
	return b - t;
}

#endif /* XTC_DEQUE_H */
