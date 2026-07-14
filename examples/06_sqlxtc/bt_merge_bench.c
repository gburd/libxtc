/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/bt_merge_bench.c
 *	Delete-heavy B-tree throughput benchmark.  Several worker procs
 *	insert then delete a sliding key window concurrently for a fixed
 *	duration, so most deletes drive the merge SMO (bt_merge) under
 *	sustained contention on bt->smo -- the workload
 *	.agent/M_SQLXTC_BTREE_MERGE.md section 8 flags as the place the
 *	always-on merge could add contention, and section "Phase 5"
 *	calls for measuring before tuning BT_MERGE_NUM/DEN.
 *
 *	NOTE ON "ON vs OFF": the merge SMO used to have a runtime
 *	opt-out (bt_set_merge_enabled / merge_on).  That flag was
 *	deliberately REMOVED once the concurrent-merge race was closed
 *	(see the doc's "DONE" record) -- merge is now unconditional, by
 *	design, so there is no live "off" mode to A/B against without
 *	reverting a correctness fix.  This benchmark instead reports the
 *	COST of the always-on merge directly: throughput plus the
 *	fraction of deletes that triggered a merge pass and the
 *	resulting page-reclaim rate, so a footprint-vs-throughput
 *	tradeoff is visible without a synthetic "off" build.
 *
 *	usage: bt_merge_bench <seconds> <path> [workers] [frames] [window]
 *	Reports ops/s, merges/s, reclaim rate, and final tree height.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "btree.h"
#include "bufmgr.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"

static bt_t        *g_bt;
static bm_t        *g_bm;
static int64_t      g_deadline_ns;
static int           g_window;
static _Atomic long g_ops;
static _Atomic int  g_workers_left;

static int64_t
now_ns(void)
{
	return xtc_clock_mono();
}

static void
mkkv(long w, int idx, char *k, char *v)
{
	(void)snprintf(k, 24, "w%02ld-%08d", w, idx);
	(void)snprintf(v, 32, "val-%08d-payload", idx);
}

/*
 * Each worker owns a private sliding window of KEYS: insert idx, then
 * (once the window is full) delete the oldest still-live idx before
 * inserting the next -- so every worker sustains a steady insert +
 * delete rate against its own disjoint key range, the delete-heavy
 * pattern that keeps driving bt_merge underflow passes for the whole
 * run, while multiple workers contend on the single per-tree bt->smo
 * lock the merge and split SMOs share.
 */
static void
worker_proc(void *arg)
{
	long w = (long)arg;
	int next = 0, oldest = 0;
	char k[24], v[32];

	/* Prime the window. */
	for (; next < g_window; next++) {
		mkkv(w, next, k, v);
		(void)bt_insert(g_bt, k, (uint16_t)strlen(k), v,
		    (uint16_t)strlen(v));
	}

	while (now_ns() < g_deadline_ns) {
		int i;
		for (i = 0; i < 64; i++) {
			mkkv(w, oldest, k, v);
			(void)bt_delete(g_bt, k, (uint16_t)strlen(k));
			oldest++;
			mkkv(w, next, k, v);
			(void)bt_insert(g_bt, k, (uint16_t)strlen(k), v,
			    (uint16_t)strlen(v));
			next++;
			atomic_fetch_add_explicit(&g_ops, 2,
			    memory_order_relaxed);
		}
	}

	if (atomic_fetch_sub(&g_workers_left, 1) == 1)
		bm_provider_stop(g_bm);
}

int
main(int argc, char **argv)
{
	xtc_exec_t *exec = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	xtc_loop_t *l0;
	bt_stats_t ts;
	bm_stats_t bs;
	int secs      = argc > 1 ? atoi(argv[1]) : 10;
	const char *path = argc > 2 ? argv[2] : "/tmp/bt-merge-bench.dat";
	int workers   = argc > 3 ? atoi(argv[3]) : 4;
	int frames    = argc > 4 ? atoi(argv[4]) : 256;
	int n_loops   = workers < 4 ? workers : 4;
	double elapsed;
	int64_t t0;
	long ops, w;

	g_window = argc > 5 ? atoi(argv[5]) : 2000;

	unlink(path);
	bo.path = path;
	bo.page_size = 4096;
	bo.n_frames = (uint32_t)frames;
	bo.cool_pct = 20;
	if (bm_create(&bo, &g_bm) != XTC_OK) {
		fprintf(stderr, "bm_create\n");
		return 1;
	}
	if (bt_open(g_bm, &g_bt) != XTC_OK) {
		fprintf(stderr, "bt_open\n");
		return 1;
	}

	atomic_store(&g_ops, 0);
	atomic_store(&g_workers_left, workers);

	if (xtc_exec_init(&exec, n_loops) != XTC_OK) {
		fprintf(stderr, "exec_init\n");
		return 1;
	}
	l0 = xtc_exec_loop(exec, 0);
	if (bm_provider_spawn(g_bm, l0, 1LL * 1000 * 1000, NULL) != XTC_OK)
		return 1;

	g_deadline_ns = now_ns() + (int64_t)secs * 1000000000LL;
	t0 = now_ns();

	for (w = 0; w < workers; w++) {
		xtc_loop_t *lp = xtc_exec_loop(exec, (int)(w % n_loops));
		xtc_proc_opts_t opts = { .name = "mbw" };
		if (xtc_proc_spawn(lp, worker_proc, (void *)w, &opts, NULL) !=
		    XTC_OK) {
			fprintf(stderr, "proc_spawn %ld\n", w);
			return 1;
		}
	}

	if (xtc_exec_run(exec) != XTC_OK) {
		fprintf(stderr, "exec_run\n");
		return 1;
	}
	bm_provider_stop(g_bm);
	(void)xtc_exec_fini(exec);

	elapsed = (double)(now_ns() - t0) / 1e9;
	ops = atomic_load(&g_ops);
	bt_get_stats(g_bt, &ts);
	bm_get_stats(g_bm, &bs);
	bt_close(g_bt);
	bm_destroy(g_bm);
	unlink(path);
	{
		char wal[300];
		(void)snprintf(wal, sizeof wal, "%s-wal", path);
		unlink(wal);
	}

	printf("secs=%.1f workers=%d frames=%d window=%d\n", elapsed, workers,
	    frames, g_window);
	printf("  ops=%ld  ops_per_sec=%.0f\n", ops,
	    elapsed > 0 ? (double)ops / elapsed : 0.0);
	printf("  inserts=%llu deletes(~)=%llu splits=%llu merges=%llu "
	    "reclaimed=%llu height=%llu\n",
	    (unsigned long long)ts.inserts, (unsigned long long)ops / 2,
	    (unsigned long long)ts.splits, (unsigned long long)ts.merges,
	    (unsigned long long)ts.reclaimed, (unsigned long long)ts.height);
	printf("  merges_per_sec=%.1f  reclaimed_per_sec=%.1f  "
	    "merge_rate=%.1f%% (of deletes that triggered a merge pass)\n",
	    elapsed > 0 ? (double)ts.merges / elapsed : 0.0,
	    elapsed > 0 ? (double)ts.reclaimed / elapsed : 0.0,
	    ops > 0 ? 100.0 * (double)ts.merges / ((double)ops / 2.0) : 0.0);
	printf("  bufmgr: loads=%llu evicted=%llu flushed=%llu freed=%llu "
	    "reissued=%llu resident=%llu\n",
	    (unsigned long long)bs.loads, (unsigned long long)bs.evicted,
	    (unsigned long long)bs.flushed, (unsigned long long)bs.freed,
	    (unsigned long long)bs.reissued, (unsigned long long)bs.resident);
	return 0;
}
