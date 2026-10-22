/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/xs_ctr.h
 *	A striped statistics counter: XS_CTR_SLOTS cache-line-padded slots,
 *	each OS thread adding to its own slot, the slots summed on read.
 *
 *	A plain _Atomic counter bumped on a hot path is one cache line
 *	written by every thread: each add steals the line from the other
 *	cores.  Measured on the buffer pool's hit counter (one add per page
 *	fix), it was 20.8% of all samples with 8 readers and removing it
 *	took ledger history at 8 connections from 105k to 131k txn/s.  A
 *	striped add touches a line shared only with the threads hashed to
 *	the same slot, and the read (a statistics call, rare) pays the sum.
 *
 *	The value is exact: every add is an atomic add to one slot, and the
 *	sum reads every slot, so a quiescent counter reads the true total
 *	(a read concurrent with adds sees some prefix of them, like any
 *	relaxed counter).  It is a statistic, not a synchronization point:
 *	relaxed ordering throughout.
 *
 *	Usage:
 *	    xs_ctr_t hits;              (zeroed: static, calloc, or
 *	                                 xs_ctr_init)
 *	    xs_ctr_add(&hits, 1);
 *	    total = xs_ctr_sum(&hits);
 *	The struct is (XS_CTR_SLOTS + 1) * 64 bytes (~1 KiB): embed it where
 *	a few hot counters live, not in per-row objects.  The slots' values
 *	are 64 bytes apart and the struct leads with a 64-byte pad, so each
 *	value sits on a cache line of its own whatever the struct's
 *	alignment: no over-aligned allocation is needed.
 *
 *	The slot is chosen once per thread from the address of a
 *	thread-local, so the hot path is a thread-local load, an index and
 *	one relaxed atomic add.  No clock, no RNG: deterministic.
 */
#ifndef SQLXTC_XS_CTR_H
#define SQLXTC_XS_CTR_H

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#ifndef XS_CTR_SLOTS
#define XS_CTR_SLOTS 16          /* a power of two */
#endif
#define XS_CTR_LINE  64

typedef struct xs_ctr {
	char lead[XS_CTR_LINE];      /* slot 0 shares no line with a neighbour */
	struct {
		_Atomic uint64_t v;
		char pad[XS_CTR_LINE - sizeof(_Atomic uint64_t)];
	} slot[XS_CTR_SLOTS];
} xs_ctr_t;

/* This thread's slot: assigned on first use from a process-wide
 * round-robin, so the first XS_CTR_SLOTS threads get distinct slots. */
static inline unsigned
xs_ctr_slot(void)
{
	static _Atomic unsigned next;
	static _Thread_local unsigned mine;   /* 0 = unassigned, else slot + 1 */

	if (mine == 0)
		mine = 1 + (atomic_fetch_add_explicit(&next, 1,
		    memory_order_relaxed) & (XS_CTR_SLOTS - 1));
	return mine - 1;
}

static inline void
xs_ctr_init(xs_ctr_t *c)
{
	memset(c, 0, sizeof *c);
}

static inline void
xs_ctr_add(xs_ctr_t *c, uint64_t n)
{
	atomic_fetch_add_explicit(&c->slot[xs_ctr_slot()].v, n,
	    memory_order_relaxed);
}

static inline uint64_t
xs_ctr_sum(const xs_ctr_t *c)
{
	uint64_t s = 0;
	int i;

	for (i = 0; i < XS_CTR_SLOTS; i++)
		s += atomic_load_explicit(&((xs_ctr_t *)c)->slot[i].v,
		    memory_order_relaxed);
	return s;
}

#endif /* SQLXTC_XS_CTR_H */
