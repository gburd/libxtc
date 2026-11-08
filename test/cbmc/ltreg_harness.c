/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/cbmc/ltreg_harness.c
 *
 *	(src/ptc/proc.c: the loop registry -- __lt_find, the lock-free
 *	seqlock reader on every send/wake, against __lt_remove_locked,
 *	which rebuilds the open-addressed hash IN PLACE: it clears every
 *	slot, then re-inserts the survivors.)
 *
 *	Two lock-free readers, both modelled: __lt_find (loop -> table) and,
 *	since 2.2, __lt_find_id (pid.loop_id -> loop and table, the by-id
 *	hash __resolve probes on every send), which the same rebuild empties
 *	and refills.
 *
 *	INVARIANT: a reader looking up a loop (or a loop id) that stays
 *	registered always finds its table (and, by id, that loop), even
 *	while another loop is removed and both hashes are emptied and
 *	refilled under it; a lookup of the loop being removed returns its
 *	own table (and loop) or nothing, never another loop's, and a by-id
 *	reader never pairs one loop with another loop's table.
 *
 *	WHAT IS MODELLED: three loops whose hashes all collide (one probe
 *	chain, the case a rebuild disturbs most); loop 1 is removed while a
 *	reader looks up loop 1, 2 or 3, by pointer or by id.  Loops and
 *	tables are indices (CBMC 5.x rejects pointer atomics across threads;
 *	NULL is 0); loop l has id 100 + l.  The statements between BEGIN/END
 *	SOURCE are the real ones (check_drift.sh).  CBMC is sequentially
 *	consistent: the seqlock's fences and orders are NOT checked here --
 *	the protocol is.  -DPLANT_NO_RETRY returns either probe's result
 *	without validating the sequence and must FAIL.
 */
#include "xtc_atomic.h"
/* The modelled pointers are int indexes (CBMC 5.x rejects pointers shared
 * between threads), so the source's _ptr operations are the _i32 ones
 * here; the transcribed blocks keep the source's spelling. */
#undef xtc_atomic_load_ptr
#undef xtc_atomic_store_ptr
#undef xtc_atomic_exchange_ptr
#undef xtc_atomic_cas_ptr
#undef xtc_atomic_cas_weak_ptr
#define xtc_atomic_load_ptr xtc_atomic_load_i32
#define xtc_atomic_store_ptr xtc_atomic_store_i32
#define xtc_atomic_exchange_ptr xtc_atomic_exchange_i32
#define xtc_atomic_cas_ptr xtc_atomic_cas_i32
#define xtc_atomic_cas_weak_ptr xtc_atomic_cas_weak_i32
#include <stdint.h>

#define NULL		0
#define LT_HCAP		4u
#define LOOP_TABLE_MAX	4

typedef int xtc_loop_t;			/* a loop is its index; 0 = none */
struct lt_entry {
	int loop;
	int tbl;
};
static struct lt_entry __lt[LOOP_TABLE_MAX];
static int __lt_n;
static struct lt_entry __lt_h[LT_HCAP];
struct lt_id_entry {
	unsigned id;
	int loop;
	int tbl;
};
static struct lt_id_entry __lt_ih[LT_HCAP];
static unsigned __lt_seq;
static int reader_done;

static unsigned
__lt_hash(int loop)
{
	(void)loop;
	return 0;			/* every loop collides */
}

static unsigned
__lt_loop_id(int loop)
{
	return 100u + (unsigned)loop;
}

static unsigned
__lt_id_hash(unsigned id)
{
	(void)id;
	return 0; /* every id collides */
}

static void
__lt_write_begin(void)
{
	xtc_atomic_store_u32(&__lt_seq,
	    xtc_atomic_load_u32(&__lt_seq, xtc_mo_relaxed) + 1u,
	    xtc_mo_relaxed);
	xtc_atomic_thread_fence(xtc_mo_release);
}
static void
__lt_write_end(void)
{
	xtc_atomic_store_u32(&__lt_seq,
	    xtc_atomic_load_u32(&__lt_seq, xtc_mo_relaxed) + 1u,
	    xtc_mo_release);
}

static void
__lt_hash_put(int loop, int t)
{
	/* BEGIN SOURCE src/ptc/proc.c __lt_hash_put */
	unsigned h = __lt_hash(loop);
	while (xtc_atomic_load_ptr(&__lt_h[h].loop, xtc_mo_relaxed) != NULL)
		h = (h + 1u) & (LT_HCAP - 1u);
	xtc_atomic_store_ptr(&__lt_h[h].tbl, t, xtc_mo_release);
	xtc_atomic_store_ptr(&__lt_h[h].loop, loop, xtc_mo_release);
	/* END SOURCE */
}

static void
__lt_id_put(int loop, int t)
{
	/* BEGIN SOURCE src/ptc/proc.c __lt_id_put */
	uint32_t id = __lt_loop_id(loop);
	unsigned h = __lt_id_hash(id);
	while (xtc_atomic_load_ptr(&__lt_ih[h].loop, xtc_mo_relaxed) != NULL)
		h = (h + 1u) & (LT_HCAP - 1u);
	xtc_atomic_store_u32(&__lt_ih[h].id, id, xtc_mo_release);
	xtc_atomic_store_ptr(&__lt_ih[h].tbl, t, xtc_mo_release);
	xtc_atomic_store_ptr(&__lt_ih[h].loop, loop, xtc_mo_release);
	/* END SOURCE */
}

static int
__lt_find_id(uint32_t id, int *out_tbl)
{
	unsigned seq, h, k;
	int l, found;
	int t;
	/* BEGIN SOURCE src/ptc/proc.c __lt_find_id */
	for (;;) {
		seq = xtc_atomic_load_u32(&__lt_seq, xtc_mo_acquire);
		if (seq & 1u)
			continue;
		found = NULL;
		t = NULL;
		for (h = __lt_id_hash(id), k = 0; k < LT_HCAP;
		     h = (h + 1u) & (LT_HCAP - 1u), k++) {
			l = xtc_atomic_load_ptr(&__lt_ih[h].loop,
			    xtc_mo_acquire);
			if (l == NULL)
				break;
			if (xtc_atomic_load_u32(&__lt_ih[h].id,
			        xtc_mo_acquire) == id) {
				found = l;
				t = xtc_atomic_load_ptr(&__lt_ih[h].tbl,
				    xtc_mo_acquire);
				break;
			}
		}
		xtc_atomic_thread_fence(xtc_mo_acquire);
#ifdef PLANT_NO_RETRY
		*out_tbl = t;
		return found;
#endif
		if (xtc_atomic_load_u32(&__lt_seq, xtc_mo_relaxed) == seq) {
			*out_tbl = t;
			return found;
		}
	}
	/* END SOURCE */
}

static int
__lt_find(int loop)
{
	unsigned seq, h;
	int l;
	int t;
	/* BEGIN SOURCE src/ptc/proc.c __lt_find */
	for (;;) {
		seq = xtc_atomic_load_u32(&__lt_seq, xtc_mo_acquire);
		if (seq & 1u)
			continue;              /* a writer is mid-change */
		t = NULL;
		for (h = __lt_hash(loop); ; h = (h + 1u) & (LT_HCAP - 1u)) {
			l = xtc_atomic_load_ptr(&__lt_h[h].loop,
			    xtc_mo_acquire);
			if (l == NULL)
				break;
			if (l == loop) {
				t = xtc_atomic_load_ptr(&__lt_h[h].tbl,
				    xtc_mo_acquire);
				break;
			}
		}
		xtc_atomic_thread_fence(xtc_mo_acquire);
#ifdef PLANT_NO_RETRY
		return t;
#endif
		if (xtc_atomic_load_u32(&__lt_seq, xtc_mo_relaxed) == seq)
			return t;
	}
	/* END SOURCE */
}

/* __lt_remove_locked for loop 1 (the dense-array swap and the rebuild of
 * both hashes). */
static void
remove_loop(int loop)
{
	int i, j;

	for (i = 0; i < __lt_n; i++)
		if (xtc_atomic_load_i32(&__lt[i].loop, xtc_mo_relaxed) == loop)
			break;
	/* BEGIN SOURCE src/ptc/proc.c __lt_remove_locked */
	__lt_write_begin();
	/* Dense array: move the last entry into the hole. */
	__lt_n--;
	xtc_atomic_store_ptr(&__lt[i].loop,
	    xtc_atomic_load_ptr(&__lt[__lt_n].loop, xtc_mo_relaxed),
	    xtc_mo_release);
	xtc_atomic_store_ptr(&__lt[i].tbl,
	    xtc_atomic_load_ptr(&__lt[__lt_n].tbl, xtc_mo_relaxed),
	    xtc_mo_release);
	xtc_atomic_store_ptr(&__lt[__lt_n].loop, NULL, xtc_mo_release);
	xtc_atomic_store_ptr(&__lt[__lt_n].tbl, NULL, xtc_mo_release);
	/* Both hashes: rebuild from the dense array (no tombstones). */
	for (j = 0; j < (int)LT_HCAP; j++) {
		xtc_atomic_store_ptr(&__lt_h[j].loop, NULL, xtc_mo_relaxed);
		xtc_atomic_store_ptr(&__lt_h[j].tbl, NULL, xtc_mo_relaxed);
		xtc_atomic_store_ptr(&__lt_ih[j].loop, NULL, xtc_mo_relaxed);
		xtc_atomic_store_ptr(&__lt_ih[j].tbl, NULL, xtc_mo_relaxed);
		xtc_atomic_store_u32(&__lt_ih[j].id, 0, xtc_mo_relaxed);
	}
	for (j = 0; j < __lt_n; j++) {
		__lt_hash_put(xtc_atomic_load_ptr(&__lt[j].loop,
		                  xtc_mo_relaxed),
		    xtc_atomic_load_ptr(&__lt[j].tbl, xtc_mo_relaxed));
		__lt_id_put(xtc_atomic_load_ptr(&__lt[j].loop, xtc_mo_relaxed),
		    xtc_atomic_load_ptr(&__lt[j].tbl, xtc_mo_relaxed));
	}
	__lt_write_end();
	/* END SOURCE */
}

int nondet_int(void);
static int want, by_id, got, got_tbl;

static void
reader(void)
{
	if (by_id)
		got = __lt_find_id(__lt_loop_id(want), &got_tbl);
	else
		got_tbl = __lt_find(want);
	xtc_atomic_store_i32(&reader_done, 1, xtc_mo_seq_cst);
}

int
main(void)
{
	int i;

	/* Loops 1, 2, 3 with tables 11, 12, 13, registered in order. */
	for (i = 0; i < 3; i++) {
		xtc_atomic_store_i32(&__lt[i].loop, i + 1, xtc_mo_seq_cst);
		xtc_atomic_store_i32(&__lt[i].tbl, 11 + i, xtc_mo_seq_cst);
		__lt_hash_put(i + 1, 11 + i);
		__lt_id_put(i + 1, 11 + i);
	}
	__lt_n = 3;
	want = nondet_int();
	__CPROVER_assume(want == 1 || want == 2 || want == 3);
	by_id = nondet_int() != 0;

	__CPROVER_ASYNC_1: reader();
	remove_loop(1);

	__CPROVER_assume(
	    xtc_atomic_load_i32(&reader_done, xtc_mo_seq_cst) == 1);
	if (want == 1)
		__CPROVER_assert(got_tbl == 11 || got_tbl == NULL,
		    "the loop being removed: its own table or none");
	else
		__CPROVER_assert(got_tbl == 10 + want,
		    "a loop that stays registered is always found");
	if (by_id)
		__CPROVER_assert(got_tbl == NULL ? got == NULL : got == want,
		    "by id: the table's own loop, never another");
	return 0;
}
