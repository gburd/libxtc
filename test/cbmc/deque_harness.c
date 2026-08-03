/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/cbmc/deque_harness.c
 *	CBMC bounded model check of the Chase-Lev work-stealing deque
 *	(src/inc/deque.h) -- the lock-free structure the executor's run
 *	queue is built on, and the documented benign-race site.
 *
 *	INVARIANT PROVED: a single owner (push then pop) racing one thief
 *	(steal) never returns the same element to two actors (no
 *	duplication -- a task is never run twice) and never returns a
 *	value that was not pushed.  This is the core work-stealing safety
 *	contract; the classic Chase-Lev bug is a double-take of the last
 *	element when owner-pop and steal race on it, which this exposes.
 *
 *	BOUND: NITEMS items, one owner, one concurrent thief.  CBMC
 *	explores every interleaving of the atomic operations.  The deque
 *	is modelled at XTC_DEQUE_CAP=4 (the algorithm is capacity-agnostic
 *	-- top/bottom are unbounded indices masked at access -- so a small
 *	cap checks identical logic in a tractable state space) and stores
 *	small integer "task ids" cast through void*, so CBMC's concurrency
 *	reasoning is over integers (sound + fast) rather than pointers.
 *
 *	The harness drives the REAL deque.h push/pop/steal, so any drift
 *	in the shipped algorithm is caught here.
 *
 *	Run: cbmc deque_harness.c -I<repo>/src/inc --unwind 8
 */

#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>

/* deque.h needs only XTC_OK / XTC_E_AGAIN from xtc.h; suppress the real
 * xtc.h (does not parse standalone under CBMC) via its include guard. */
#define XTC_H 1
enum { XTC_OK = 0, XTC_E_AGAIN = -5 };

/* Small capacity: capacity-agnostic algorithm, tractable state space. */
#define XTC_DEQUE_CAP 4
#include "deque.h"

#define NITEMS 1      /* items pushed before the race */

static xtc_deque_t d;

/* Task ids are small integers 1..NITEMS, carried as (void*)(intptr_t)id.
 * id 0 == "nothing" (NULL). */
static _Atomic int claimed[NITEMS + 1];

static void
claim(void *x)
{
	intptr_t id = (intptr_t)x;
	if (id == 0)
		return;
	__CPROVER_assert(id >= 1 && id <= NITEMS,
	    "returned value is a pushed task id (nothing fabricated)");
	int prev = atomic_fetch_add_explicit(&claimed[id], 1,
	    memory_order_relaxed);
	__CPROVER_assert(prev == 0, "no task returned twice (no duplication)");
}

static void
thief(void)
{
	claim(xtc_deque_steal(&d));
}

int
main(void)
{
	int i;

	/* Initialise the deque WITHOUT xtc_deque_init's CAP-length zeroing
	 * loop (incidental to the algorithm under test -- push/pop/steal
	 * only ever read slots they wrote).  Setting the two index fields
	 * is the semantically-relevant part and keeps the model's loop
	 * count tiny. */
	atomic_store_explicit(&d.top, 0, memory_order_relaxed);
	atomic_store_explicit(&d.bottom, 0, memory_order_relaxed);
	for (i = 1; i <= NITEMS; i++)
		(void)xtc_deque_push(&d, (void *)(intptr_t)i);
	for (i = 0; i <= NITEMS; i++)
		atomic_store_explicit(&claimed[i], 0, memory_order_relaxed);

	/* One thief steals concurrently with the owner's pops; CBMC
	 * explores all interleavings. */
	__CPROVER_ASYNC_1: thief();

	for (i = 0; i < NITEMS; i++)
		claim(xtc_deque_pop(&d));

	return 0;
}
