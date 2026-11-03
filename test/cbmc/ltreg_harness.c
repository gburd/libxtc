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
 *	INVARIANT: a reader looking up a loop that stays registered always
 *	finds its table, even while another loop is removed (and the hash
 *	is emptied and refilled under it); a lookup of the loop being
 *	removed returns its table or NULL, never another loop's table.
 *
 *	WHAT IS MODELLED: three loops whose hashes all collide (one probe
 *	chain, the case a rebuild disturbs most); loop 1 is removed while a
 *	reader looks up loop 2 or 1.  Loops and tables are indices (CBMC 5.x
 *	rejects pointer atomics across threads; NULL is 0).  The statements
 *	between BEGIN/END SOURCE are the real ones (check_drift.sh).  CBMC
 *	is sequentially consistent: the seqlock's fences and orders are NOT
 *	checked here -- the protocol is.  -DPLANT_NO_RETRY returns the probe
 *	result without validating the sequence and must FAIL.
 */
#include <stdatomic.h>

#define NULL		0
#define LT_HCAP		4u
#define LOOP_TABLE_MAX	4

typedef int xtc_loop_t;			/* a loop is its index; 0 = none */
struct lt_entry {
	_Atomic int loop;
	_Atomic int tbl;
};
static struct lt_entry __lt[LOOP_TABLE_MAX];
static int __lt_n;
static struct lt_entry __lt_h[LT_HCAP];
static _Atomic unsigned __lt_seq;
static _Atomic int reader_done;

static unsigned
__lt_hash(int loop)
{
	(void)loop;
	return 0;			/* every loop collides */
}

static void
__lt_write_begin(void)
{
	atomic_store_explicit(&__lt_seq,
	    atomic_load_explicit(&__lt_seq, memory_order_relaxed) + 1u,
	    memory_order_relaxed);
	atomic_thread_fence(memory_order_release);
}
static void
__lt_write_end(void)
{
	atomic_store_explicit(&__lt_seq,
	    atomic_load_explicit(&__lt_seq, memory_order_relaxed) + 1u,
	    memory_order_release);
}

static void
__lt_hash_put(int loop, int t)
{
	/* BEGIN SOURCE src/ptc/proc.c __lt_hash_put */
	unsigned h = __lt_hash(loop);
	while (atomic_load_explicit(&__lt_h[h].loop,
	    memory_order_relaxed) != NULL)
		h = (h + 1u) & (LT_HCAP - 1u);
	atomic_store_explicit(&__lt_h[h].tbl, t, memory_order_release);
	atomic_store_explicit(&__lt_h[h].loop, loop, memory_order_release);
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
		seq = atomic_load_explicit(&__lt_seq, memory_order_acquire);
		if (seq & 1u)
			continue;              /* a writer is mid-change */
		t = NULL;
		for (h = __lt_hash(loop); ; h = (h + 1u) & (LT_HCAP - 1u)) {
			l = atomic_load_explicit(&__lt_h[h].loop,
			    memory_order_acquire);
			if (l == NULL)
				break;
			if (l == loop) {
				t = atomic_load_explicit(&__lt_h[h].tbl,
				    memory_order_acquire);
				break;
			}
		}
		atomic_thread_fence(memory_order_acquire);
#ifdef PLANT_NO_RETRY
		return t;
#endif
		if (atomic_load_explicit(&__lt_seq,
		    memory_order_relaxed) == seq)
			return t;
	}
	/* END SOURCE */
}

/* __lt_remove_locked for loop 1 (the dense-array swap and the hash
 * rebuild; the by-id index is a separate, directly-read array). */
static void
remove_loop(int loop)
{
	int i, j;

	for (i = 0; i < __lt_n; i++)
		if (atomic_load_explicit(&__lt[i].loop,
		    memory_order_relaxed) == loop)
			break;
	/* BEGIN SOURCE src/ptc/proc.c __lt_remove_locked */
	__lt_write_begin();
	/* Dense array: move the last entry into the hole. */
	__lt_n--;
	atomic_store_explicit(&__lt[i].loop, atomic_load_explicit(
	    &__lt[__lt_n].loop, memory_order_relaxed), memory_order_release);
	atomic_store_explicit(&__lt[i].tbl, atomic_load_explicit(
	    &__lt[__lt_n].tbl, memory_order_relaxed), memory_order_release);
	atomic_store_explicit(&__lt[__lt_n].loop, NULL, memory_order_release);
	atomic_store_explicit(&__lt[__lt_n].tbl, NULL, memory_order_release);
	/* Hash: rebuild from the dense array (no tombstones). */
	for (j = 0; j < (int)LT_HCAP; j++) {
		atomic_store_explicit(&__lt_h[j].loop, NULL,
		    memory_order_relaxed);
		atomic_store_explicit(&__lt_h[j].tbl, NULL,
		    memory_order_relaxed);
	}
	for (j = 0; j < __lt_n; j++)
		__lt_hash_put(atomic_load_explicit(&__lt[j].loop,
		    memory_order_relaxed), atomic_load_explicit(&__lt[j].tbl,
		    memory_order_relaxed));
	/* END SOURCE */
	__lt_write_end();
}

int nondet_int(void);
static int want, got;

static void
reader(void)
{
	got = __lt_find(want);
	atomic_store(&reader_done, 1);
}

int
main(void)
{
	int i;

	/* Loops 1, 2, 3 with tables 11, 12, 13, registered in order. */
	for (i = 0; i < 3; i++) {
		atomic_store(&__lt[i].loop, i + 1);
		atomic_store(&__lt[i].tbl, 11 + i);
		__lt_hash_put(i + 1, 11 + i);
	}
	__lt_n = 3;
	want = nondet_int();
	__CPROVER_assume(want == 1 || want == 2 || want == 3);

	__CPROVER_ASYNC_1: reader();
	remove_loop(1);

	__CPROVER_assume(atomic_load(&reader_done) == 1);
	if (want == 1)
		__CPROVER_assert(got == 11 || got == NULL,
		    "the loop being removed: its own table or none");
	else
		__CPROVER_assert(got == 10 + want,
		    "a loop that stays registered is always found");
	return 0;
}
