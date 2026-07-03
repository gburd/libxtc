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
#include "xtc_lwlock.h"
#include "xtc_async.h"     /* xtc_yield */
#include "xtc_sim.h"

/*
 * DST coverage of the lightweight lock (src/ptc/lock_lw.c): a
 * multi-reader / single-writer lock over an atomic state word with a
 * pthread_cond_wait slow path.  A fiber-park path was added -- PURELY
 * ADDITIVE and gated on __xtc_current_task() != NULL, mirroring the
 * xtc_amutex / semaphore / lock-manager discipline: a caller blocking
 * on a contended acquire from INSIDE a fiber arms a waker, enqueues on
 * the lock's FIFO fiber wait queue, drops wait_mu, and xtc_yield()s to
 * the loop, then re-CASes on wake (wake-and-recheck).  A release wakes
 * both a thread condvar-waiter (broadcast) and every queued fiber.  A
 * caller NOT on a loop (cur == NULL: OS threads, the blocking pool,
 * tooling) takes the ORIGINAL pthread_cond_wait path byte for byte, so
 * test/m13/test_lwlock (the thread-path suite) still passes unchanged.
 *
 * Two workloads across N loops under xtc_sim_exec_run:
 *
 *   excl   -- W worker fibers each take the lock EXCLUSIVE, bump a
 *            shared protected counter across a yield, and release.
 *            INVARIANT: mutual exclusion -- at most one holder at a
 *            time (a peak-holder counter must never exceed 1), and the
 *            final counter equals the number of exclusive sections
 *            (no lost update -- a torn read/write would corrupt it).
 *
 *   rw     -- a mix of readers and writers on one lock.  A writer takes
 *            EXCLUSIVE, writes two halves of a "checked" value across a
 *            yield (so a torn read is observable), and releases; a
 *            reader takes SHARED, reads the value across a yield, and
 *            asserts the two halves agree (NO TORN READ under a shared
 *            latch while a writer is excluded), and that multiple
 *            readers may hold concurrently.
 *
 * Each asserts (a) quiescence (rc == XTC_OK -- no hang, so the
 * fiber-park re-grant works), (b) the invariant (mutual exclusion / no
 * torn read / no lost update), (c) byte-identical replay from the seed
 * (app hash + sim state hash), (d) a different seed reorders but stays
 * consistent.  Footprint is tiny (few fibers, one lock, per-run free).
 */

#define N_LOOPS 4

/* ================= exclusive: mutual exclusion + no lost update ======= */

#define EXCL_WORK  10
#define EXCL_ITERS 3

static xtc_lwlock_t g_ex_lk;
static atomic_int    g_ex_live;      /* current exclusive holders */
static atomic_int    g_ex_peak;      /* max holders seen (must stay 1) */
static long          g_ex_counter;   /* protected, non-atomic on purpose */
static atomic_int    g_ex_done;
static atomic_long   g_ex_hash;

static void
ex_fold(long v)
{
	long h = atomic_load_explicit(&g_ex_hash, memory_order_relaxed);
	h = h * 1000003L + (v + 1);
	atomic_store_explicit(&g_ex_hash, h, memory_order_relaxed);
}

static void
ex_worker(void *arg)
{
	int id = (int)(intptr_t)arg;
	int it;

	for (it = 0; it < EXCL_ITERS; it++) {
		int pk, live;
		(void)xtc_lwlock_acquire(&g_ex_lk, XTC_LW_EXCLUSIVE);
		live = atomic_fetch_add_explicit(&g_ex_live, 1,
		    memory_order_relaxed) + 1;
		do {
			pk = atomic_load_explicit(&g_ex_peak,
			    memory_order_relaxed);
			if (live <= pk) break;
		} while (!atomic_compare_exchange_weak_explicit(&g_ex_peak,
		    &pk, live, memory_order_relaxed, memory_order_relaxed));
		/* Protected critical section: read-modify-write a plain long
		 * across a yield.  Under mutual exclusion this is exact; a
		 * broken lock would lose updates. */
		{
			long v = g_ex_counter;
			xtc_yield();
			g_ex_counter = v + 1;
			ex_fold(id);
		}
		atomic_fetch_sub_explicit(&g_ex_live, 1, memory_order_relaxed);
		xtc_lwlock_release(&g_ex_lk);
		xtc_yield();
	}
	atomic_fetch_add_explicit(&g_ex_done, 1, memory_order_relaxed);
}

static int
run_excl(uint64_t seed, int *out_done, int *out_peak, long *out_counter,
    long *out_hash, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	atomic_store(&g_ex_live, 0);
	atomic_store(&g_ex_peak, 0);
	g_ex_counter = 0;
	atomic_store(&g_ex_done, 0);
	atomic_store(&g_ex_hash, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	if (xtc_lwlock_init(&g_ex_lk, 1) != XTC_OK) {
		(void)xtc_exec_fini(e); return -1;
	}
	for (i = 0; i < EXCL_WORK; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(i % N_LOOPS)),
		    ex_worker, (void *)(intptr_t)i, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_done = atomic_load(&g_ex_done);
	*out_peak = atomic_load(&g_ex_peak);
	*out_counter = g_ex_counter;
	*out_hash = atomic_load(&g_ex_hash);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	xtc_lwlock_destroy(&g_ex_lk);
	(void)xtc_exec_fini(e);
	return rc;
}

/* ================= rw: no torn read under shared / exclusive ========= */

#define RW_READERS 6
#define RW_WRITERS 3
#define RW_ITERS   3

static xtc_lwlock_t g_rw_lk;
static atomic_long   g_rw_hi;        /* two halves that must always agree */
static atomic_long   g_rw_lo;
static atomic_int     g_rw_torn;     /* reader saw hi != lo (bug) */
static atomic_int     g_rw_rd_live;  /* concurrent shared holders */
static atomic_int     g_rw_rd_peak;  /* max concurrent readers */
static atomic_int     g_rw_wr_conflict; /* writer saw a peer writer (bug) */
static atomic_int     g_rw_wr_live;  /* concurrent exclusive holders */
static atomic_int     g_rw_done;
static atomic_long    g_rw_hash;

static void
rw_writer(void *arg)
{
	int id = (int)(intptr_t)arg;
	int it;

	for (it = 0; it < RW_ITERS; it++) {
		long nv = (long)((id + 1) * 100 + it);
		int live;
		(void)xtc_lwlock_acquire(&g_rw_lk, XTC_LW_EXCLUSIVE);
		live = atomic_fetch_add_explicit(&g_rw_wr_live, 1,
		    memory_order_relaxed) + 1;
		if (live > 1)
			atomic_fetch_add_explicit(&g_rw_wr_conflict, 1,
			    memory_order_relaxed);
		/* Write the two halves with a yield BETWEEN them, so a reader
		 * that saw the update without exclusion would catch a torn
		 * (hi != lo) state. */
		atomic_store_explicit(&g_rw_hi, nv, memory_order_relaxed);
		xtc_yield();
		atomic_store_explicit(&g_rw_lo, nv, memory_order_relaxed);
		atomic_fetch_sub_explicit(&g_rw_wr_live, 1,
		    memory_order_relaxed);
		xtc_lwlock_release(&g_rw_lk);
		xtc_yield();
	}
	atomic_fetch_add_explicit(&g_rw_done, 1, memory_order_relaxed);
}

static void
rw_reader(void *arg)
{
	int id = (int)(intptr_t)arg;
	int it;

	for (it = 0; it < RW_ITERS; it++) {
		long hi, lo;
		int live, pk;
		(void)xtc_lwlock_acquire(&g_rw_lk, XTC_LW_SHARED);
		live = atomic_fetch_add_explicit(&g_rw_rd_live, 1,
		    memory_order_relaxed) + 1;
		do {
			pk = atomic_load_explicit(&g_rw_rd_peak,
			    memory_order_relaxed);
			if (live <= pk) break;
		} while (!atomic_compare_exchange_weak_explicit(&g_rw_rd_peak,
		    &pk, live, memory_order_relaxed, memory_order_relaxed));
		hi = atomic_load_explicit(&g_rw_hi, memory_order_relaxed);
		xtc_yield();                /* hold shared across a yield */
		lo = atomic_load_explicit(&g_rw_lo, memory_order_relaxed);
		if (hi != lo)
			atomic_fetch_add_explicit(&g_rw_torn, 1,
			    memory_order_relaxed);
		{
			long h = atomic_load_explicit(&g_rw_hash,
			    memory_order_relaxed);
			h = h * 1000003L + (hi + 1) + (long)id;
			atomic_store_explicit(&g_rw_hash, h,
			    memory_order_relaxed);
		}
		atomic_fetch_sub_explicit(&g_rw_rd_live, 1,
		    memory_order_relaxed);
		xtc_lwlock_release(&g_rw_lk);
		xtc_yield();
	}
	atomic_fetch_add_explicit(&g_rw_done, 1, memory_order_relaxed);
}

static int
run_rw(uint64_t seed, int *out_done, int *out_torn, int *out_wr_conflict,
    int *out_rd_peak, long *out_hash, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc, next = 0;

	atomic_store(&g_rw_hi, 0);
	atomic_store(&g_rw_lo, 0);
	atomic_store(&g_rw_torn, 0);
	atomic_store(&g_rw_rd_live, 0);
	atomic_store(&g_rw_rd_peak, 0);
	atomic_store(&g_rw_wr_conflict, 0);
	atomic_store(&g_rw_wr_live, 0);
	atomic_store(&g_rw_done, 0);
	atomic_store(&g_rw_hash, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	if (xtc_lwlock_init(&g_rw_lk, 2) != XTC_OK) {
		(void)xtc_exec_fini(e); return -1;
	}
	for (i = 0; i < RW_WRITERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(next++ %
		    N_LOOPS)), rw_writer, (void *)(intptr_t)i, NULL, NULL);
	for (i = 0; i < RW_READERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(next++ %
		    N_LOOPS)), rw_reader, (void *)(intptr_t)i, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_done = atomic_load(&g_rw_done);
	*out_torn = atomic_load(&g_rw_torn);
	*out_wr_conflict = atomic_load(&g_rw_wr_conflict);
	*out_rd_peak = atomic_load(&g_rw_rd_peak);
	*out_hash = atomic_load(&g_rw_hash);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	xtc_lwlock_destroy(&g_rw_lk);
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(void)
{
	int rc;

	/* ---- exclusive: mutual exclusion + no lost update + replay ---- */
	{
		int d1 = 0, pk1 = 0, d2 = 0, pk2 = 0, d3 = 0, pk3 = 0;
		long c1 = 0, c2 = 0, c3 = 0, h1 = 0, h2 = 0, h3 = 0;
		uint64_t s1 = 0, s2 = 0, s3 = 0;
		long want = (long)EXCL_WORK * EXCL_ITERS;

		rc = run_excl(0x1B01, &d1, &pk1, &c1, &h1, &s1);
		if (rc != XTC_OK) {
			printf("FAIL: lwlock excl rc=%d (hang?)\n", rc);
			return 1;
		}
		(void)run_excl(0x1B01, &d2, &pk2, &c2, &h2, &s2);
		rc = run_excl(0x2C02, &d3, &pk3, &c3, &h3, &s3);
		if (rc != XTC_OK) {
			printf("FAIL: lwlock excl diff-seed rc=%d\n", rc);
			return 1;
		}
		printf("excl run1: done=%d peak=%d counter=%ld (want %ld) "
		    "hash=%ld state=%016llx\n", d1, pk1, c1, want, h1,
		    (unsigned long long)s1);
		if (d1 != EXCL_WORK) {
			printf("FAIL: not all excl workers finished "
			    "(done=%d want %d)\n", d1, EXCL_WORK); return 1;
		}
		if (pk1 != 1) {
			printf("FAIL: lwlock EXCLUSIVE not mutually exclusive "
			    "(peak holders=%d)\n", pk1); return 1;
		}
		if (c1 != want) {
			printf("FAIL: lost update under exclusive lock "
			    "(counter=%ld want %ld)\n", c1, want); return 1;
		}
		if (d1 != d2 || pk1 != pk2 || c1 != c2 || h1 != h2 ||
		    s1 != s2) {
			printf("FAIL: lwlock excl did not replay "
			    "(counter %ld/%ld hash %ld/%ld state "
			    "%016llx/%016llx)\n", c1, c2, h1, h2,
			    (unsigned long long)s1,
			    (unsigned long long)s2); return 1;
		}
		if (d3 != EXCL_WORK || pk3 != 1 || c3 != want) {
			printf("FAIL: lwlock excl diff-seed inconsistent "
			    "(done=%d peak=%d counter=%ld)\n", d3, pk3, c3);
			return 1;
		}
	}

	/* ---- rw: no torn read, writer-exclusive, readers concurrent ---- */
	{
		int d1 = 0, t1 = 0, wc1 = 0, rp1 = 0;
		int d2 = 0, t2 = 0, wc2 = 0, rp2 = 0;
		int d3 = 0, t3 = 0, wc3 = 0, rp3 = 0;
		long h1 = 0, h2 = 0, h3 = 0;
		uint64_t s1 = 0, s2 = 0, s3 = 0;
		int want = RW_READERS + RW_WRITERS;

		rc = run_rw(0x3D03, &d1, &t1, &wc1, &rp1, &h1, &s1);
		if (rc != XTC_OK) {
			printf("FAIL: lwlock rw rc=%d (hang?)\n", rc);
			return 1;
		}
		(void)run_rw(0x3D03, &d2, &t2, &wc2, &rp2, &h2, &s2);
		rc = run_rw(0x4E04, &d3, &t3, &wc3, &rp3, &h3, &s3);
		if (rc != XTC_OK) {
			printf("FAIL: lwlock rw diff-seed rc=%d\n", rc);
			return 1;
		}
		printf("rw   run1: done=%d torn=%d wr-conflict=%d rd-peak=%d "
		    "hash=%ld state=%016llx\n", d1, t1, wc1, rp1, h1,
		    (unsigned long long)s1);
		if (d1 != want) {
			printf("FAIL: not all rw fibers finished "
			    "(done=%d want %d)\n", d1, want); return 1;
		}
		if (t1 != 0) {
			printf("FAIL: TORN READ under shared latch (%d "
			    "readers saw hi != lo)\n", t1); return 1;
		}
		if (wc1 != 0) {
			printf("FAIL: two writers held EXCLUSIVE at once "
			    "(%d conflicts)\n", wc1); return 1;
		}
		if (d1 != d2 || t1 != t2 || wc1 != wc2 || rp1 != rp2 ||
		    h1 != h2 || s1 != s2) {
			printf("FAIL: lwlock rw did not replay "
			    "(hash %ld/%ld state %016llx/%016llx)\n",
			    h1, h2, (unsigned long long)s1,
			    (unsigned long long)s2); return 1;
		}
		if (d3 != want || t3 != 0 || wc3 != 0) {
			printf("FAIL: lwlock rw diff-seed inconsistent "
			    "(done=%d torn=%d wr-conflict=%d)\n", d3, t3, wc3);
			return 1;
		}
	}

	printf("OK: lwlock fiber-park -- EXCLUSIVE mutual exclusion + no "
	    "lost update, SHARED no torn read + writer-exclusive, replayed; "
	    "different seed reorders and stays consistent\n");
	return 0;
}
