#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_lrlock.h"
#include "xtc_async.h"     /* xtc_yield */
#include "xtc_sim.h"

/*
 * DST coverage of the Left-Right lock (src/ptc/lock_lr.c): wait-free
 * reads over two side-by-side copies with an atomic read_idx, and a
 * single mutex-serialized writer that mutates the stale copy then
 * publishes with a pointer swap.  The primitive is NON-blocking (a
 * reader never parks; a writer takes a plain mutex over a short
 * section), so it is sim-safe as-is -- NO shim.  The seeded scheduler
 * owns the reader-vs-reader and reader-vs-writer interleaving.
 *
 * Reader-slot note: lrlock reader slots are per-OS-thread (a __thread
 * cache), so under the single-thread sim every fiber shares one slot.
 * The read-side (read_begin .. read_end) must therefore NOT span a
 * yield -- a fiber completes a full read-side before returning to the
 * scheduler, so no two fibers ever occupy the shared slot at once
 * (cooperative single-thread execution).  This is the correct sim
 * usage; it exercises the pointer-swap publish + apply/sync
 * determinism + the no-torn-read guarantee at scheduling-boundary
 * granularity, which is exactly what the sim models.
 *
 * Protected data: a pair {hi, lo, sum} the writer always updates
 * TOGETHER on the stale copy before publishing.  A reader takes a
 * consistent snapshot and asserts hi == lo and sum == hi + lo -- a
 * torn publish (a swap exposing a half-updated copy) would break it.
 *
 * INVARIANTS: (a) quiescence (rc == XTC_OK), (b) no reader ever
 * observes a torn value (hi != lo or sum mismatch), (c) the sequence
 * of observed values REPLAYS byte-identically from the seed (an
 * order-sensitive fold + sim state hash), (d) a different seed reorders
 * but stays consistent.  Footprint tiny (few fibers, one lrlock).
 */

#define N_LOOPS 4
#define READERS 6
#define WRITERS 2
#define ITERS   4

struct payload {
	long hi;
	long lo;
	long sum;
};

/* apply_op: op is a "new base value"; set hi=lo=base, sum=2*base. */
static void
pl_apply(void *data, const void *op, size_t op_size)
{
	struct payload *p = data;
	long base = *(const long *)op;
	(void)op_size;
	p->hi = base;
	p->lo = base;
	p->sum = base + base;
}

static void
pl_sync(void *dst, const void *src, size_t data_size)
{
	memcpy(dst, src, data_size);
}

static xtc_lrlock_t *g_lr;
static atomic_int     g_torn;      /* reader saw an inconsistent snapshot */
static atomic_int     g_no_slot;   /* read_begin returned NULL (slot cap) */
static atomic_int     g_done;
static atomic_long    g_hash;      /* order-sensitive fold of reads */

static void
fold(long v)
{
	long h = atomic_load_explicit(&g_hash, memory_order_relaxed);
	h = h * 1000003L + (v + 1);
	atomic_store_explicit(&g_hash, h, memory_order_relaxed);
}

static void
lr_reader(void *arg)
{
	int id = (int)(intptr_t)arg;
	int it;

	for (it = 0; it < ITERS; it++) {
		const struct payload *p;
		/* Full read-side, NO yield inside it (see the slot note). */
		p = xtc_lrlock_read_begin(g_lr);
		if (p == NULL) {
			atomic_fetch_add_explicit(&g_no_slot, 1,
			    memory_order_relaxed);
		} else {
			long hi = p->hi, lo = p->lo, sum = p->sum;
			if (hi != lo || sum != hi + lo)
				atomic_fetch_add_explicit(&g_torn, 1,
				    memory_order_relaxed);
			fold(hi + (long)id);
			xtc_lrlock_read_end(g_lr);
		}
		xtc_yield();                /* interleave BETWEEN read-sides */
	}
	atomic_fetch_add_explicit(&g_done, 1, memory_order_relaxed);
}

static void
lr_writer(void *arg)
{
	int id = (int)(intptr_t)arg;
	int it;

	for (it = 0; it < ITERS; it++) {
		long base = (long)((id + 1) * 1000 + it);
		/* apply_op updates the stale copy atomically w.r.t. readers;
		 * publish swaps the read_idx.  No reader is mid-read-side
		 * across this (they never yield inside one). */
		(void)xtc_lrlock_write_begin(g_lr);
		xtc_lrlock_apply_op(g_lr, &base, sizeof base);
		xtc_lrlock_publish(g_lr);
		xtc_lrlock_write_end(g_lr);
		xtc_yield();
	}
	atomic_fetch_add_explicit(&g_done, 1, memory_order_relaxed);
}

static int
run_lr(uint64_t seed, int *out_done, int *out_torn, int *out_no_slot,
    long *out_hash, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	struct payload *w;
	int i, rc, next = 0;

	atomic_store(&g_torn, 0);
	atomic_store(&g_no_slot, 0);
	atomic_store(&g_done, 0);
	atomic_store(&g_hash, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	if (xtc_lrlock_create(sizeof(struct payload), pl_apply, pl_sync,
	    "lr.dst", &g_lr) != XTC_OK) {
		(void)xtc_exec_fini(e); return -1;
	}
	/* Initialise both copies to a consistent zero payload, then mark
	 * ready so the first publish need not full-sync from garbage. */
	w = xtc_lrlock_write_data(g_lr);
	w->hi = w->lo = w->sum = 0;
	xtc_lrlock_publish_full_sync(g_lr);
	w = xtc_lrlock_write_data(g_lr);
	w->hi = w->lo = w->sum = 0;
	xtc_lrlock_mark_ready(g_lr);

	for (i = 0; i < WRITERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(next++ %
		    N_LOOPS)), lr_writer, (void *)(intptr_t)i, NULL, NULL);
	for (i = 0; i < READERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(next++ %
		    N_LOOPS)), lr_reader, (void *)(intptr_t)i, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_done = atomic_load(&g_done);
	*out_torn = atomic_load(&g_torn);
	*out_no_slot = atomic_load(&g_no_slot);
	*out_hash = atomic_load(&g_hash);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	xtc_lrlock_destroy(g_lr);
	g_lr = NULL;
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(void)
{
	int rc;
	int d1 = 0, t1 = 0, n1 = 0, d2 = 0, t2 = 0, n2 = 0;
	int d3 = 0, t3 = 0, n3 = 0;
	long h1 = 0, h2 = 0, h3 = 0;
	uint64_t s1 = 0, s2 = 0, s3 = 0;
	int want = READERS + WRITERS;

	rc = run_lr(0x1EF7, &d1, &t1, &n1, &h1, &s1);
	if (rc != XTC_OK) {
		printf("FAIL: lrlock rc=%d (hang?)\n", rc);
		return 1;
	}
	(void)run_lr(0x1EF7, &d2, &t2, &n2, &h2, &s2);
	rc = run_lr(0x2FA8, &d3, &t3, &n3, &h3, &s3);
	if (rc != XTC_OK) {
		printf("FAIL: lrlock diff-seed rc=%d\n", rc);
		return 1;
	}
	printf("lrlock run1: done=%d torn=%d no-slot=%d hash=%ld "
	    "state=%016llx\n", d1, t1, n1, h1, (unsigned long long)s1);
	if (d1 != want) {
		printf("FAIL: not all lrlock fibers finished (done=%d "
		    "want %d)\n", d1, want); return 1;
	}
	if (t1 != 0) {
		printf("FAIL: TORN READ under wait-free read (%d readers saw "
		    "an inconsistent snapshot)\n", t1); return 1;
	}
	if (n1 != 0) {
		printf("FAIL: reader-slot exhaustion (%d NULL read_begin) -- "
		    "unexpected on one sim thread\n", n1); return 1;
	}
	if (d1 != d2 || t1 != t2 || h1 != h2 || s1 != s2) {
		printf("FAIL: lrlock did not replay "
		    "(hash %ld/%ld state %016llx/%016llx)\n", h1, h2,
		    (unsigned long long)s1, (unsigned long long)s2);
		return 1;
	}
	if (d3 != want || t3 != 0) {
		printf("FAIL: lrlock diff-seed inconsistent (done=%d "
		    "torn=%d)\n", d3, t3); return 1;
	}

	printf("OK: lrlock wait-free reads never torn under a concurrent "
	    "writer, publish is atomic, replayed; different seed reorders "
	    "and stays consistent\n");
	return 0;
}
