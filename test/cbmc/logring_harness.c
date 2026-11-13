/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/cbmc/logring_harness.c
 *
 *	(src/ptc/log.c: __log_claim, __log_drop_oldest, __log_take and the
 *	publish / give-back stores -- the xtc_log record ring.)
 *
 *	Writers on any thread append records to a bounded ring; one drainer
 *	(xtc_log_drain holds drain_lock) prints them.  Each slot carries a
 *	sequence word (D. Vyukov's bounded queue): a writer claims position
 *	pos only when its slot reads seq == pos, fills it, and publishes
 *	seq = pos + 1; the drainer takes position h only when its slot reads
 *	seq == h + 1 and gives it back with seq = h + n_slots.  On a full
 *	ring a writer consumes the oldest record unprinted (counted), or
 *	drops its own record when the oldest one is mid-write or mid-read.
 *
 *	INVARIANTS, two writers (two records and one) racing the drainer on
 *	a two-slot ring (so it can overflow), over every interleaving:
 *	  - the drainer never takes a slot a writer has claimed but not
 *	    filled (report 09: before 2.2 it read every slot in
 *	    [head, tail) and printed empty or stale records);
 *	  - every record taken is one a writer wrote, taken once;
 *	  - each writer's records are taken in the order it wrote them;
 *	  - once everything is drained, taken + dropped == written.
 *
 *	WHAT IS MODELLED: the statements between BEGIN/END SOURCE are the
 *	real ones (check_drift.sh).  The ring and the logger are globals
 *	(`log` and __log_slot are macros over them, so no pointer to shared
 *	state crosses a thread), a record is two ints the writer sets to one
 *	token, and the formatting and sink are left out.  CBMC is
 *	sequentially consistent: the acquire/release pairs the source uses
 *	are not checked here (TSan covers them).  Planted bugs, each must
 *	FAIL:
 *	  -DPLANT_TAKE_TO_TAIL    the drainer takes any position below tail
 *	                          (the 2.1 drain);
 *	  -DPLANT_DROP_UNWRITTEN  a full-ring writer drops the oldest record
 *	                          without checking it is written.
 */
#include "xtc_atomic.h"
#include <stddef.h>
#include <stdint.h>

#define NS 2u /* ring slots */
#define NW 2  /* writers */
#define NR 2  /* records per writer (writer 1 writes one) */

struct log_slot {
	uint64_t seq;
	int a, b; /* the record: both set to the writer's token */
};

typedef struct xtc_log {
	uint64_t head;
	uint64_t tail;
	uint64_t n_slots;
	int dropped;
} xtc_log_t;

static xtc_log_t L;
/* Two slot globals, not an array: CBMC encodes a store through a
 * computed array index as a store to the whole array, so two threads
 * writing different slots would race on one variable. */
static struct log_slot SL0, SL1;
static int done[NW];
static int taken[NW * NR], ntaken;

#define log (&L)
#define __log_slot(lg, pos) ((pos) % NS != 0 ? &SL1 : &SL0)
#define __log_drop_oldest(lg, pos) drop_oldest(pos)

static int
drop_oldest(uint64_t pos)
{
	/* BEGIN SOURCE src/ptc/log.c __log_drop_oldest */
	uint64_t h = pos - log->n_slots;
	struct log_slot *s = __log_slot(log, h);

	if (xtc_atomic_load_u64(&log->head, xtc_mo_relaxed) != h ||
#ifndef PLANT_DROP_UNWRITTEN
	    xtc_atomic_load_u64(&s->seq, xtc_mo_acquire) != h + 1 ||
#endif
	    !xtc_atomic_cas_u64(&log->head, &h, h + 1, xtc_mo_relaxed,
	        xtc_mo_relaxed))
		return 0;
	xtc_atomic_store_u64(&s->seq, h + log->n_slots, xtc_mo_release);
	xtc_atomic_fetch_add_i32(&log->dropped, 1, xtc_mo_relaxed);
	return 1;
	/* END SOURCE */
}

static struct log_slot *
claim(uint64_t *out_pos)
{
	/* BEGIN SOURCE src/ptc/log.c __log_claim */
	uint64_t pos, seq, was;
	struct log_slot *s;

	pos = xtc_atomic_load_u64(&log->tail, xtc_mo_relaxed);
	for (;;) {
		s = __log_slot(log, pos);
		seq = xtc_atomic_load_u64(&s->seq, xtc_mo_acquire);
		if (seq == pos) {
			if (xtc_atomic_cas_weak_u64(&log->tail, &pos, pos + 1,
			        xtc_mo_relaxed, xtc_mo_relaxed))
				break;
		} else if (seq > pos) {
			pos = xtc_atomic_load_u64(&log->tail, xtc_mo_relaxed);
		} else if (!__log_drop_oldest(log, pos)) {
			was = pos;
			pos = xtc_atomic_load_u64(&log->tail, xtc_mo_relaxed);
			seq = xtc_atomic_load_u64(&s->seq, xtc_mo_acquire);
			if (pos == was && seq < pos) {
				xtc_atomic_fetch_add_i32(&log->dropped, 1,
				    xtc_mo_relaxed);
				return NULL;
			}
		}
	}
	*out_pos = pos;
	return s;
	/* END SOURCE */
}

static struct log_slot *
take(uint64_t *out_h)
{
#ifdef PLANT_TAKE_TO_TAIL
	uint64_t h = xtc_atomic_load_u64(&log->head, xtc_mo_relaxed);
	if (h >= xtc_atomic_load_u64(&log->tail, xtc_mo_relaxed))
		return NULL;
	xtc_atomic_store_u64(&log->head, h + 1, xtc_mo_relaxed);
	*out_h = h;
	return __log_slot(log, h);
#else
	/* BEGIN SOURCE src/ptc/log.c __log_take */
	uint64_t h, seq;
	struct log_slot *s;

	for (;;) {
		h = xtc_atomic_load_u64(&log->head, xtc_mo_relaxed);
		s = __log_slot(log, h);
		seq = xtc_atomic_load_u64(&s->seq, xtc_mo_acquire);
		if (seq < h + 1)
			return NULL;
		if (seq == h + 1 &&
		    xtc_atomic_cas_u64(&log->head, &h, h + 1, xtc_mo_relaxed,
		        xtc_mo_relaxed))
			break;
	}
	*out_h = h;
	return s;
	/* END SOURCE */
#endif
}

static void
writer(int w)
{
	struct log_slot *s;
	uint64_t pos;
	int k;

	for (k = 0; k < NR - w; k++) {
		if ((s = claim(&pos)) == NULL)
			continue;
		/* One atomic section per plain store: CBMC encodes a store
		 * through a pointer that may name either slot as a
		 * read-modify-write of both, which another thread could
		 * interleave.  The window between the two stores stays open,
		 * so a torn record is still observable. */
		__CPROVER_atomic_begin();
		s->a = (w + 1) * 10 + k + 1;
		__CPROVER_atomic_end();
		__CPROVER_atomic_begin();
		s->b = (w + 1) * 10 + k + 1;
		__CPROVER_atomic_end();
		/* BEGIN SOURCE src/ptc/log.c publish */
		xtc_atomic_store_u64(&s->seq, pos + 1, xtc_mo_release);
		/* END SOURCE */
	}
	xtc_atomic_store_i32(&done[w], 1, xtc_mo_seq_cst);
}

/* One xtc_log_drain step: take, check the record, give the slot back. */
static int
drain_one(void)
{
	struct log_slot *s;
	uint64_t h;
	int tok, i;

	if ((s = take(&h)) == NULL)
		return 0;
	tok = s->a;
	__CPROVER_assert(tok != 0 && tok == s->b,
	    "the drainer takes only a record a writer finished writing");
	for (i = 0; i < ntaken && i < NW * NR; i++)
		__CPROVER_assert(taken[i] != tok, "no record taken twice");
	for (i = 0; i < ntaken && i < NW * NR; i++)
		if (taken[i] / 10 == tok / 10)
			__CPROVER_assert(taken[i] < tok,
			    "a writer's records are taken in write order");
	if (ntaken < NW * NR)
		taken[ntaken] = tok;
	ntaken++;
	/* BEGIN SOURCE src/ptc/log.c drain give-back */
	xtc_atomic_store_u64(&s->seq, h + log->n_slots, xtc_mo_release);
	/* END SOURCE */
	return 1;
}

static void
writer0(void)
{
	writer(0);
}

static void
writer1(void)
{
	writer(1);
}

int
main(void)
{
	int k;

	L.n_slots = NS;
	xtc_atomic_store_u64(&SL0.seq, 0, xtc_mo_seq_cst);
	xtc_atomic_store_u64(&SL1.seq, 1, xtc_mo_seq_cst);

	__CPROVER_ASYNC_1: writer0();
	__CPROVER_ASYNC_2: writer1();

	(void)drain_one();

	/* Writers finished: drain the rest, then account for every write. */
	__CPROVER_assume(xtc_atomic_load_i32(&done[0], xtc_mo_seq_cst) == 1);
	__CPROVER_assume(xtc_atomic_load_i32(&done[1], xtc_mo_seq_cst) == 1);
	for (k = 0; k < (int)NS; k++)
		(void)drain_one();
	__CPROVER_assert(drain_one() == 0, "the ring is empty after a drain");
	__CPROVER_assert(ntaken + xtc_atomic_load_i32(&L.dropped,
	    xtc_mo_seq_cst) == NW * NR - 1, "taken + dropped == written");
	return 0;
}
