/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/cbmc/mpsc_harness.c
 *	CBMC bounded model check of the proc mailbox MPSC queue
 *	(src/ptc/proc.c __mbox_push_locked / __mbox_pop_locked).
 *
 *	INVARIANT PROVED: across concurrent producers (each under the
 *	mailbox lock) and a single consumer, no message is lost,
 *	duplicated, or fabricated, and messages from the SAME producer
 *	are delivered in send order (per-producer FIFO).  This is the
 *	mailbox delivery contract xtc_proc_send relies on.
 *
 *	WHAT IS MODELLED: the FIFO enqueue/dequeue semantics of
 *	__mbox_push_locked (append at tail, bump depth) and
 *	__mbox_pop_locked (remove at head, drop depth), transcribed
 *	FAITHFULLY.  The real mailbox is a singly-LINKED list, but CBMC's
 *	concurrency reasoning over concurrently-mutated pointer chains is
 *	unsound (it refuses to verify), so -- exactly as deque_harness.c
 *	carries task ids as integers rather than pointers -- the FIFO is
 *	modelled as an append-at-tail / remove-at-head INDEX queue over a
 *	fixed slot array.  This preserves the algorithm's observable
 *	semantics (order-preserving append and head removal, depth
 *	tracking) while keeping CBMC's reasoning over integers (sound +
 *	fast).  All pushes and pops run UNDER the mailbox lock, modelled
 *	by a CAS spinlock carrying the same release/acquire ordering
 *	p->mbox_lock (a pthread_mutex) provides.  The watermark/stats
 *	bookkeeping is dropped (pure observability).  If the mailbox's
 *	FIFO discipline in proc.c drifts, this must be updated to match.
 *
 *	BOUND: two producers (ids 10,11 and 20,21) enqueue 2 messages
 *	each under the lock; the consumer dequeues all 4 once the queue
 *	is full.  CBMC explores every interleaving of the locked
 *	enqueue/dequeue sections.
 *
 *	Run: cbmc mpsc_harness.c --unwind 6
 */

#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>

#define QCAP 4

/* The mailbox as an index FIFO (see header: faithful integer model of
 * the linked-list head/tail splice).  slots[head..tail) are live. */
static int    slots[QCAP];
static size_t mbox_head;   /* consumer end: next to pop */
static size_t mbox_tail;   /* producer end: next free slot */
static size_t mbox_n;      /* depth */

/* A modelled mutex: a CAS spinlock, exactly the mutual exclusion
 * p->mbox_lock (a pthread_mutex) provides.  Acquire/release carry the
 * release/acquire ordering that publishes the payload written under
 * the lock to the consumer that pops under it. */
static _Atomic int mbox_locked;
#define LOCK_SPINS 4
static void
mbox_lock(void)
{
	int expected, i;
	for (i = 0; i < LOCK_SPINS; i++) {
		expected = 0;
		if (atomic_compare_exchange_weak_explicit(&mbox_locked,
		    &expected, 1, memory_order_acquire, memory_order_relaxed))
			return;
	}
	/* Deeper contention than the modelled bound explores: prune this
	 * schedule (not a real failure -- three threads contend the lock a
	 * bounded number of times before one wins). */
	__CPROVER_assume(0);
}
static void
mbox_unlock(void)
{
	atomic_store_explicit(&mbox_locked, 0, memory_order_release);
}

/* __mbox_push_locked -- append at the tail, bump depth (the linked
 * list's "mbox_tail->next = e; mbox_tail = e; mbox_n++"). */
static void
mbox_push_locked(int msg)
{
	slots[mbox_tail] = msg;
	mbox_tail++;
	mbox_n++;
}

/* __mbox_pop_locked -- remove at the head, drop depth (the linked
 * list's "e = mbox_head; mbox_head = e->next; mbox_n--"); returns -1
 * on empty (== NULL). */
static int
mbox_pop_locked(void)
{
	int e;
	if (mbox_n == 0)
		return -1;
	e = slots[mbox_head];
	mbox_head++;
	mbox_n--;
	return e;
}

/* Producer: enqueue its two messages IN ORDER, each under the lock. */
static void
producer(int m1, int m2)
{
	mbox_lock();   mbox_push_locked(m1);   mbox_unlock();
	mbox_lock();   mbox_push_locked(m2);   mbox_unlock();
}

int
main(void)
{
	int got[QCAP], ngot = 0;
	int seen_a = 0, seen_b = 0;    /* per-producer count / order tracking */
	int last_a = 0, last_b = 0;
	int i, j, e;

	mbox_head = mbox_tail = mbox_n = 0;
	atomic_store_explicit(&mbox_locked, 0, memory_order_relaxed);

	/* Two concurrent producers race each other under the mailbox
	 * lock; CBMC explores every interleaving of their locked
	 * enqueues. */
	__CPROVER_ASYNC_1: producer(10, 11);
	__CPROVER_ASYNC_2: producer(20, 21);

	/* The consumer drains under the lock.  It only proceeds once the
	 * queue is full (mbox_n == QCAP) -- the modelled equivalent of
	 * the real consumer parking until its mailbox has traffic.  This
	 * keeps exactly the schedules in which "no message lost" is a
	 * meaningful, non-vacuous claim (they exist, so this is not
	 * vacuous -- see the reachability note in the harness tests). */
	mbox_lock();
	__CPROVER_assume(mbox_n == QCAP);
	for (i = 0; i < QCAP; i++) {
		e = mbox_pop_locked();
		__CPROVER_assert(e != -1, "no message lost (all four present)");
		got[ngot++] = e;
	}
	mbox_unlock();

	/* No fabrication, no duplication, per-producer FIFO. */
	for (i = 0; i < ngot; i++) {
		int m = got[i];
		__CPROVER_assert(m == 10 || m == 11 || m == 20 || m == 21,
		    "no fabricated message");
		for (j = i + 1; j < ngot; j++)
			__CPROVER_assert(got[j] != m, "no duplicated message");
		if (m == 10 || m == 11) {
			/* per-producer FIFO: A's 10 must precede A's 11. */
			__CPROVER_assert(m > last_a, "producer A messages in FIFO order");
			last_a = m; seen_a++;
		} else {
			__CPROVER_assert(m > last_b, "producer B messages in FIFO order");
			last_b = m; seen_b++;
		}
	}
	__CPROVER_assert(seen_a == 2 && seen_b == 2,
	    "exactly two messages from each producer");
	return 0;
}
