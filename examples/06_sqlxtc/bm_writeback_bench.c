/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/bm_writeback_bench.c
 *	A/B writeback benchmark.  Drives a sustained dirty-page workload
 *	against the buffer manager (working set >> pool, so eviction +
 *	writeback runs continuously) for a fixed duration, with the page
 *	store opened DIRECT both times and adaptive writeback toggled, to
 *	measure whether the genetic trickler tuner helps or hurts.
 *
 *	usage: bm_writeback_bench <adaptive 0|1> <seconds> <path>
 *	                          [frames] [n_pages]
 *	Reports ops/s and the buffer-manager flush/evict counters.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bufmgr.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

static bm_t       *g_bm;
static int         g_n_pages;
static int64_t     g_deadline_ns;
static _Atomic long g_ops;
static _Atomic long g_syncs;
static int         g_sync_every;     /* fdatasync every N ops (0 = never) */
static int         g_locality;       /* 0=random 1=sequential 2=clustered */
#define BM_CLUSTER 64                /* run length for clustered locality */

static bm_pid_t   *g_pid;

static int64_t
now_ns(void)
{
	int64_t ns = 0;
	ns = xtc_clock_mono();   /* portable monotonic clock */
	return ns;
}

static void
worker_proc(void *arg)
{
	bm_frame_t *f;
	uint64_t rng = 0x12345678ULL;
	int k;
	(void)arg;

	/* Pre-allocate the working set (>> pool): forces eviction. */
	for (k = 0; k < g_n_pages; k++) {
		if (bm_alloc_pid(g_bm, &f, &g_pid[k]) != XTC_OK)
			break;
		((uint64_t *)bm_page(f))[0] = (uint64_t)k;
		bm_unfix(g_bm, f, 1);
	}

	/* Sustained dirtying loop until the deadline. */
	uint64_t seqcur = 0, clbase = 0;
	int clrem = 0;
	while (now_ns() < g_deadline_ns) {
		int i;
		for (i = 0; i < 256; i++) {     /* batch before re-checking time */
			int idx;
			long n;
			if (g_locality == 1) {              /* sequential */
				idx = (int)(seqcur % (uint64_t)g_n_pages);
				seqcur++;
			} else if (g_locality == 2) {       /* clustered runs */
				if (clrem == 0) {
					rng ^= rng << 13; rng ^= rng >> 7;
					rng ^= rng << 17;
					clbase = rng % (uint64_t)g_n_pages;
					clrem = BM_CLUSTER;
				}
				idx = (int)((clbase + (uint64_t)(BM_CLUSTER - clrem))
				    % (uint64_t)g_n_pages);
				clrem--;
			} else {                            /* random */
				rng ^= rng << 13; rng ^= rng >> 7;
				rng ^= rng << 17;
				idx = (int)(rng % (uint64_t)g_n_pages);
			}
			if (bm_fix_pid(g_bm, g_pid[idx], &f) != XTC_OK)
				continue;
			((uint64_t *)bm_page(f))[1] += 1;   /* mutate */
			bm_unfix(g_bm, f, 1);               /* dirty */
			n = atomic_fetch_add_explicit(&g_ops, 1,
			    memory_order_relaxed) + 1;
			/* Durability barrier: force data to the device so the
			 * buffered mode cannot "lie" by deferring writeback. */
			if (g_sync_every > 0 && (n % g_sync_every) == 0) {
				(void)bm_sync(g_bm);
				atomic_fetch_add_explicit(&g_syncs, 1,
				    memory_order_relaxed);
			}
		}
	}
	bm_provider_stop(g_bm);
	bm_trickler_stop(g_bm);
}

int
main(int argc, char **argv)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	bm_opts_t bo = BM_OPTS_DEFAULT;
	xtc_pid_t w, pp, tr;
	bm_stats_t st;
	int adaptive = argc > 1 ? atoi(argv[1]) : 0;
	int secs     = argc > 2 ? atoi(argv[2]) : 60;
	const char *path = argc > 3 ? argv[3] : "/tmp/bm-wb-bench.dat";
	int frames   = argc > 4 ? atoi(argv[4]) : 256;
	double elapsed;
	int64_t t0;
	long ops;
	int direct = 1;
	/* argv[1] may be a mode letter: A=direct+fixed, B=direct+adaptive,
	 * C=buffered+fixed (OS page cache + OS I/O scheduling). */
	if (argc > 1) {
		char m = argv[1][0];
		if (m == 'C' || m == 'c') { direct = 0; adaptive = 0; }
		else if (m == 'B' || m == 'b') { direct = 1; adaptive = 1; }
		else if (m == 'A' || m == 'a') { direct = 1; adaptive = 0; }
		/* else numeric: adaptive already parsed; keep direct=1 */
	}

	g_n_pages = argc > 5 ? atoi(argv[5]) : 65536;   /* working set */
	g_sync_every = argc > 6 ? atoi(argv[6]) : 1000; /* fdatasync cadence */
	g_locality = argc > 7 ? atoi(argv[7]) : 0;      /* 0=rand 1=seq 2=clustered */
	g_pid  = calloc((size_t)g_n_pages, sizeof *g_pid);
	if (!g_pid) { fprintf(stderr, "oom\n"); return 1; }

	unlink(path);
	bo.path = path;
	bo.page_size = 4096;
	bo.n_frames = (uint32_t)frames;
	bo.cool_pct = 20;
	bo.direct = direct;
	bo.adaptive_writeback = adaptive ? 1 : 0;
	if (bm_create(&bo, &g_bm) != XTC_OK) { fprintf(stderr, "bm_create\n"); return 1; }

	atomic_store(&g_ops, 0);
	atomic_store(&g_syncs, 0);
	g_deadline_ns = now_ns() + (int64_t)secs * 1000000000LL;
	t0 = now_ns();

	if (xtc_loop_init(&loop) != XTC_OK) return 1;
	if (bm_provider_spawn(g_bm, loop, 1LL * 1000 * 1000, &pp) != XTC_OK) return 1;
	if (bm_trickler_spawn(g_bm, loop, 2LL * 1000 * 1000, &tr) != XTC_OK) return 1;
	opts.name = "wb-worker";
	if (xtc_proc_spawn(loop, worker_proc, NULL, &opts, &w) != XTC_OK) return 1;
	if (xtc_loop_run(loop) != XTC_OK) { fprintf(stderr, "loop_run\n"); return 1; }
	(void)xtc_loop_fini(loop);

	elapsed = (double)(now_ns() - t0) / 1e9;
	ops = atomic_load(&g_ops);
	bm_get_stats(g_bm, &st);
	bm_destroy(g_bm);
	unlink(path);
	free(g_pid);

	printf("DIRECT=%d ADAPTIVE=%d secs=%.1f frames=%d pages=%d sync_every=%d "
	    "locality=%s\n",
	    direct, adaptive, elapsed, frames, g_n_pages, g_sync_every,
	    g_locality == 1 ? "seq" : (g_locality == 2 ? "clustered" : "random"));
	printf("  ops=%ld  ops_per_sec=%.0f  fdatasyncs=%ld\n", ops,
	    elapsed > 0 ? (double)ops / elapsed : 0.0,
	    atomic_load(&g_syncs));
	printf("  flushed=%llu trickled=%llu evicted=%llu loads=%llu\n",
	    (unsigned long long)st.flushed, (unsigned long long)st.trickled,
	    (unsigned long long)st.evicted, (unsigned long long)st.loads);
	printf("  trickler_writes=%llu pages_per_write=%.2f\n",
	    (unsigned long long)st.tr_writes,
	    st.tr_writes ? (double)st.trickled / (double)st.tr_writes : 0.0);
	printf("  evict_flushes=%llu (%.1f%% of evictions forced a sync write) "
	    "dirty_backlog=%llu\n",
	    (unsigned long long)st.evict_flushes,
	    st.evicted ? 100.0 * (double)st.evict_flushes / (double)st.evicted : 0.0,
	    (unsigned long long)st.dirty_backlog);
	printf("  trickler_passes=%llu adaptive_passes=%llu (%.1f%% adaptive)\n",
	    (unsigned long long)st.tr_passes, (unsigned long long)st.tr_adaptive,
	    st.tr_passes ? 100.0 * (double)st.tr_adaptive / (double)st.tr_passes : 0.0);
	return 0;
}
