/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * examples/06_sqlxtc/bench_btree_concurrent.c
 *	Cross-loop READ-SCALING benchmark for the shared B-link tree.
 *
 *	The measure-first gate for the Optimistic-Lock-Coupling work
 *	(PLAN.md "sqlxtc concurrency & indexing research track", item 1).
 *	sqlxtc's btree readers descend the tree taking a SHARED
 *	fiber-yielding content latch (xtc_arwlock) at each frame -- and a
 *	shared-latch acquire/release is still a WRITE to the latch word,
 *	so under cross-loop concurrency it can be the read-scalability
 *	wall OLC would remove (readers become version-validated, taking
 *	NO shared write on the descent path).
 *
 *	This does NOT prove OLC is worth it; it MEASURES whether the
 *	premise holds on this fiber runtime.  It warms a tree of N_KEYS,
 *	then runs a fixed wall-clock window of PURE random-key lookups
 *	from R reader fibers spread across L loops, sweeping L = 1, 2, 4,
 *	8 (one loop per core is the intended deployment).  It reports
 *	reads/sec and ns/read at each L.
 *
 *	Read the SCALING, not the absolute number:
 *	  - throughput ~linear in L  => the shared latch is NOT the wall;
 *	    OLC would buy little on this runtime -- do not build it.
 *	  - throughput plateaus / degrades as L grows => reads contend on
 *	    a shared write (the per-frame latch is the prime suspect);
 *	    OLC is worth building, and this is its before-number.
 *	The whole tree is read-only during the window (no writers), so any
 *	non-scaling is contention on the READ path itself, not on splits.
 *
 *	Usage: bench_btree_concurrent [n_keys] [seconds_per_point]
 *	       (default 50000 keys, 2 s per loop-count point)
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "bufmgr.h"
#include "btree.h"

#define PAGE_SZ    4096
#define N_FRAMES   4096          /* large enough to hold the whole warm set */
#define MAX_LOOPS  8
#define READERS_PER_LOOP 4

static bm_t   *g_bm;
static bt_t   *g_bt;
static long    g_n_keys;
static long    g_iters_per_reader;    /* fixed lookup budget per reader */
static atomic_int      g_go;          /* release readers together */
static atomic_llong    g_reads;       /* total lookups performed */
static atomic_llong    g_hits;        /* lookups that found the key */
static atomic_int      g_readers_left;/* readers still running (last stops provider) */

static void
mkkey(long i, char *k)
{
	/* Fixed-width so key comparison cost is uniform across the sweep. */
	snprintf(k, 24, "k%010ld", i);
}

/* Reader fiber: perform a FIXED budget of random-key lookups, then
 * exit.  Wall time is measured by the host around xtc_exec_run, so no
 * on-loop timer fiber is needed (which would be starved by the
 * busy-yielding readers on a saturated loop -- itself a real runtime
 * observation, but not what this bench is measuring).  The last reader
 * to finish stops the buffer-manager provider so the run quiesces. */
static void
reader_proc(void *arg)
{
	uint64_t rng = (uint64_t)(uintptr_t)arg * 0x9E3779B97F4A7C15ull + 1;
	char k[24], buf[40];
	uint16_t vl;
	long long local_reads = 0, local_hits = 0;
	long i;

	while (!atomic_load_explicit(&g_go, memory_order_acquire))
		xtc_yield();

	for (i = 0; i < g_iters_per_reader; i++) {
		long idx;
		rng = rng * 6364136223846793005ull + 1442695040888963407ull;
		idx = (long)((rng >> 33) % (uint64_t)g_n_keys);
		mkkey(idx, k);
		if (bt_lookup(g_bt, k, (uint16_t)strlen(k), buf,
		    sizeof buf, &vl) == XTC_OK)
			local_hits++;
		local_reads++;
		if ((i & 63) == 63)
			xtc_yield();   /* stay cooperative */
	}

	atomic_fetch_add_explicit(&g_reads, local_reads, memory_order_relaxed);
	atomic_fetch_add_explicit(&g_hits, local_hits, memory_order_relaxed);
	if (atomic_fetch_sub_explicit(&g_readers_left, 1,
	    memory_order_acq_rel) == 1)
		bm_provider_stop(g_bm);   /* last reader releases the provider */
}

/* Run one sweep point: `nloops` loops, READERS_PER_LOOP readers each,
 * each reader doing a FIXED lookup budget.  Times the whole run from
 * the host thread and returns reads/sec. */
static double
run_point(int nloops)
{
	xtc_exec_t *e = NULL;
	int readers = nloops * READERS_PER_LOOP;
	int64_t t0, t1;
	int i;

	atomic_store(&g_go, 0);
	atomic_store(&g_reads, 0);
	atomic_store(&g_hits, 0);
	atomic_store(&g_readers_left, readers);

	if (xtc_exec_init(&e, nloops) != XTC_OK)
		return -1;
	/* The buffer-manager provider proc lives on loop 0. */
	if (bm_provider_spawn(g_bm, xtc_exec_loop(e, 0), 1LL * 1000 * 1000,
	    NULL) != XTC_OK)
		return -1;
	for (i = 0; i < readers; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, i % nloops), reader_proc,
		    (void *)(intptr_t)(i + 1), NULL, NULL);

	t0 = xtc_clock_mono();
	atomic_store_explicit(&g_go, 1, memory_order_release);   /* release */
	(void)xtc_exec_run(e);                                   /* blocks */
	t1 = xtc_clock_mono();
	xtc_exec_fini(e);

	{
		long long reads = atomic_load(&g_reads);
		double secs = (double)(t1 - t0) / 1e9;
		return secs > 0 ? (double)reads / secs : 0.0;
	}
}

int
main(int argc, char **argv)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256] = "/tmp/sqlxtc-btree-bench-XXXXXX";
	int fd, i;
	long n = argc > 1 ? atol(argv[1]) : 50000;
	long iters = argc > 2 ? atol(argv[2]) : 2000000;
	const int sweep[] = { 1, 2, 4, 8 };
	double base = 0.0;

	g_n_keys = n < 1 ? 1 : n;
	g_iters_per_reader = iters < 1 ? 1 : iters;

	fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); return 1; }
	close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = N_FRAMES;
	bo.cool_pct = 25;
	if (bm_create(&bo, &g_bm) != XTC_OK) { fprintf(stderr, "bm_create\n"); return 1; }
	if (bt_open(g_bm, &g_bt) != XTC_OK) { fprintf(stderr, "bt_open\n"); return 1; }

	/* Warm the tree: insert every key (single-threaded, off a loop --
	 * bt_insert works standalone; no provider needed for the warm-up
	 * because the small warm set stays resident). */
	{
		char k[24], v[32];
		for (i = 0; i < g_n_keys; i++) {
			mkkey(i, k);
			snprintf(v, sizeof v, "v%010d", i);
			if (bt_insert(g_bt, k, (uint16_t)strlen(k), v,
			    (uint16_t)strlen(v)) != XTC_OK) {
				fprintf(stderr, "warm insert failed at %d\n", i);
				return 1;
			}
		}
	}

	{
		bt_stats_t ts;
		bt_get_stats(g_bt, &ts);
		printf("# warm tree: %ld keys, height=%llu\n", g_n_keys,
		    (unsigned long long)ts.height);
	}
	printf("# read-scaling sweep (%ld lookups/reader, %d readers/loop, "
	    "pure random lookups, read-only tree)\n", g_iters_per_reader,
	    READERS_PER_LOOP);
	printf("# loops   reads/sec      ns/read   scaling(x vs 1 loop)  "
	    "efficiency\n");

	for (i = 0; i < (int)(sizeof sweep / sizeof sweep[0]); i++) {
		int L = sweep[i];
		double rps;
		if (L > MAX_LOOPS) break;
		rps = run_point(L);
		if (rps < 0) { fprintf(stderr, "run_point(%d) failed\n", L); return 1; }
		if (i == 0) base = rps;
		printf("  %4d  %12.0f  %10.2f  %14.2fx  %8.0f%%\n",
		    L, rps, rps > 0 ? 1e9 / rps : 0.0,
		    base > 0 ? rps / base : 0.0,
		    base > 0 ? 100.0 * (rps / base) / L : 0.0);
	}

	printf("#\n# READ THE SCALING COLUMN: efficiency near 100%% at L=8 "
	    "means reads scale\n# (the shared latch is not the wall -- OLC "
	    "would buy little here).  Efficiency\n# collapsing toward 100/L%% "
	    "means reads contend on a shared write (the per-frame\n# latch is "
	    "the prime suspect) -- OLC is worth building, and this is its "
	    "before-number.\n");

	bt_close(g_bt);
	bm_destroy(g_bm);
	unlink(path);
	return 0;
}
