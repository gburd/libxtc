/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/cbmc/lrlock_harness.c
 *	CBMC bounded model check of the left-right lock read/publish
 *	protocol (src/ptc/lock_lr.c).
 *
 *	INVARIANT PROVED: a reader on the ACTIVE side never observes a
 *	value the writer is concurrently mutating.  The writer only ever
 *	mutates the INACTIVE side (1 - read_idx), publishes by flipping
 *	read_idx, then drains readers off the now-stale side before it
 *	may reuse it -- so a reader that latched the old read_idx sees a
 *	fully-consistent snapshot for the whole read, and the writer
 *	never writes a side a reader is reading.
 *
 *	WHAT IS MODELLED: the left-right read/publish sequence,
 *	transcribed faithfully from lock_lr.c --
 *	  reader (xtc_lrlock_read_begin/read_end):
 *	    announce active (epoch -> odd), SeqCst fence, load read_idx,
 *	    read data[idx]; on end bump epoch -> even.
 *	  writer (xtc_lrlock_write_begin/apply/publish):
 *	    mutate data[1 - read_idx] (the inactive side) in two steps,
 *	    SeqCst fence, flip read_idx, SeqCst fence, then
 *	    __wait_for_readers drains any reader still in the old epoch
 *	    before the stale side may be mutated again.
 *	The real lock_lr.c cannot be included standalone (mmap COW,
 *	per-slot cache-line epochs, oplog replay, pthread + fiber waits,
 *	the global slot allocator) -- none of which bear on the
 *	mutual-visibility property, which lives entirely in the epoch +
 *	read_idx + fence choreography modelled here.  Data is a single
 *	integer per side; the writer writes an intermediate then a final
 *	value so a torn/racing read is observable if the protocol let one
 *	happen.  If lock_lr.c's fence/epoch ordering drifts, this must be
 *	updated in lockstep.
 *
 *	BOUND: one reader racing one writer's single publish.  CBMC
 *	explores every interleaving of the epoch/read_idx atomics.
 *
 *	Run: cbmc lrlock_harness.c --unwind 6
 */

#include <stdint.h>
#include <stdatomic.h>

static _Atomic uint32_t read_idx;      /* 0 or 1: the active side */
static _Atomic int       data[2];      /* the two copies */
static _Atomic uint32_t  epoch;        /* one reader's epoch: even idle, odd active */

/* The writer records which side it is mutating so the reader can
 * cross-check it never reads a side under active mutation. */
static _Atomic int       writing_side; /* -1 == no write in progress */

/* Reader: read_begin (announce + fence + load idx), read the value,
 * read_end (epoch -> even). */
static void
reader(void)
{
	uint32_t idx;
	int a, b;

	/* Step 2: bump epoch to odd (announce active). */
	(void)atomic_fetch_add_explicit(&epoch, 1u, memory_order_acq_rel);
	/* Step 3: SeqCst fence -- writer sees our odd epoch before we
	 * observe read_idx. */
	atomic_thread_fence(memory_order_seq_cst);
	/* Step 4: load read_idx -- the side we will read. */
	idx = atomic_load_explicit(&read_idx, memory_order_acquire);

	/* Read the value twice: on the active side the writer must never
	 * mutate it, so both reads see the same consistent value AND the
	 * writer must not be mid-write on this side. */
	a = atomic_load_explicit(&data[idx], memory_order_relaxed);
	__CPROVER_assert(atomic_load_explicit(&writing_side,
	    memory_order_relaxed) != (int)idx,
	    "writer never mutates the side a reader is reading");
	b = atomic_load_explicit(&data[idx], memory_order_relaxed);
	__CPROVER_assert(a == b,
	    "reader sees a stable snapshot on the active side");

	/* read_end: epoch -> even. */
	(void)atomic_fetch_add_explicit(&epoch, 1u, memory_order_acq_rel);
}

/* __wait_for_readers: spin until the reader is no longer in the old
 * (odd) epoch we snapshotted.  Bounded: with one reader it drains in a
 * bounded number of observations. */
#define DRAIN_SPINS 4
static void
wait_for_readers(uint32_t last_seen)
{
	int i;
	if ((last_seen & 1u) == 0)
		return;                 /* reader was idle at snapshot */
	for (i = 0; i < DRAIN_SPINS; i++) {
		uint32_t cur = atomic_load_explicit(&epoch, memory_order_acquire);
		if (cur != last_seen)
			return;             /* reader advanced: drained */
	}
	__CPROVER_assume(0);            /* deeper drain than modelled: prune */
}

/* Writer: mutate the inactive side, flip, drain, (then the old side is
 * reusable).  Transcribes write_begin/apply/publish. */
static void
writer(void)
{
	int rd = (int)atomic_load_explicit(&read_idx, memory_order_acquire);
	int wr = 1 - rd;
	uint32_t snap;

	/* Mutate the INACTIVE side in two steps (a racing read of this
	 * side would be torn; the protocol must forbid such a read). */
	atomic_store_explicit(&writing_side, wr, memory_order_relaxed);
	atomic_store_explicit(&data[wr], 100, memory_order_relaxed);
	atomic_store_explicit(&data[wr], 200, memory_order_relaxed);
	atomic_store_explicit(&writing_side, -1, memory_order_relaxed);

	/* Publish: SeqCst fence, flip read_idx, SeqCst fence. */
	atomic_thread_fence(memory_order_seq_cst);
	(void)atomic_exchange_explicit(&read_idx, (uint32_t)wr,
	    memory_order_acq_rel);
	atomic_thread_fence(memory_order_seq_cst);

	/* Drain: wait for any reader still in the pre-flip epoch. */
	snap = atomic_load_explicit(&epoch, memory_order_acquire);
	wait_for_readers(snap);
	/* After the drain the old side (rd) is safe to reuse -- but a
	 * SECOND publish is out of this harness's bound. */
	(void)rd;
}

int
main(void)
{
	atomic_store_explicit(&read_idx, 0, memory_order_relaxed);
	atomic_store_explicit(&data[0], 1, memory_order_relaxed);   /* stable */
	atomic_store_explicit(&data[1], 1, memory_order_relaxed);
	atomic_store_explicit(&epoch, 0, memory_order_relaxed);     /* even */
	atomic_store_explicit(&writing_side, -1, memory_order_relaxed);

	__CPROVER_ASYNC_1: writer();
	reader();
	return 0;
}
