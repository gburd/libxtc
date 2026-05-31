/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_bufmgr_mt.c
 *	MULTI-THREADED stress test of the LeanStore-style buffer
 *	manager.  Unlike the single-loop test_bufmgr.c, this drives the
 *	bufmgr from many OS threads at once: the L2 multi-loop executor
 *	owns N loops, each on its own thread, and we pin several worker
 *	processes to each loop.  Because procs are pinned to their loop,
 *	workers on distinct loops run truly in parallel -- so the
 *	bufmgr's atomics, free-list mutex, and CAS-based Swip state
 *	transitions (HOT/COOL/EVICTED) are exercised under real
 *	concurrency, with a page-provider proc cooling and flushing
 *	alongside.
 *
 *	The page-content pattern is written ONCE at setup and never
 *	mutated thereafter (re-touching a page marks it dirty but does
 *	not change the bytes), so the canonical content for a given
 *	pid/k is STABLE for the whole run.  A worker therefore must
 *	always read back exactly what was written; any mismatch is a
 *	real corruption.  Verification is done on a COPY taken while the
 *	frame is still pinned, so the comparison never races eviction.
 *
 *	No network, no daemon.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bufmgr.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_exec.h"
#include "xtc_async.h"      /* xtc_yield */

#define PAGE_SZ          4096
#define N_FRAMES         32        /* small resident pool -> eviction churn */
#define N_ROOTS          1024      /* >> N_FRAMES: forces the swizzle cycle */
#define N_LOOPS          4         /* executor loops == OS threads */
#define WORKERS_PER_LOOP 4
#define N_WORKERS        (N_LOOPS * WORKERS_PER_LOOP)
#define ITERS            2000      /* fix/verify iterations per worker */
#define HOT_RANGE        64        /* root[0..HOT_RANGE) hammered by ALL */
#define DISJOINT_PER     ((N_ROOTS - HOT_RANGE) / N_WORKERS)

static bm_t       *g_bm;
static bm_swip_t   g_root[N_ROOTS];     /* the "parents" holding the swips */
static bm_pid_t    g_pid[N_ROOTS];

static _Atomic uint64_t g_verified;     /* pinned reads that matched */
static _Atomic uint64_t g_mismatch;     /* content/pid mismatches (bugs) */
static _Atomic uint64_t g_fix_fail;     /* bm_fix returned != XTC_OK */
static _Atomic int      g_workers_done; /* count, last one stops provider */

struct warg { int id; unsigned seed; };
static struct warg g_warg[N_WORKERS];

/* page content: [u64 pid][u64 k][fill byte at 100][fill byte at end]. */
static void
fill_page(void *p, bm_pid_t pid, uint64_t k)
{
	uint64_t *u = p;
	u[0] = pid;
	u[1] = k;
	((unsigned char *)p)[100] = (unsigned char)(k & 0xff);
	((unsigned char *)p)[PAGE_SZ - 1] = (unsigned char)((k >> 8) & 0xff);
}
static int
check_page(const void *p, bm_pid_t pid, uint64_t k)
{
	const uint64_t *u = p;
	return u[0] == pid && u[1] == k &&
	    ((const unsigned char *)p)[100] == (unsigned char)(k & 0xff) &&
	    ((const unsigned char *)p)[PAGE_SZ - 1] ==
	        (unsigned char)((k >> 8) & 0xff);
}

/*
 * Worker process.  Each iteration picks a root either from the shared
 * hot range (concurrent fixes of the SAME swip across threads -> HOT
 * hits + COOL rescues + races on one slot) or from this worker's own
 * disjoint range (distinct pages -> free-list / eviction contention),
 * fixes it, copies + verifies the page while it is still pinned, then
 * unfixes (sometimes dirty, but WITHOUT changing the bytes).
 */
static void
worker_proc(void *arg)
{
	struct warg *wa = arg;
	unsigned seed = wa->seed;
	int id = wa->id;
	int lo = HOT_RANGE + id * DISJOINT_PER;   /* disjoint range [lo,hi) */
	int hi = lo + DISJOINT_PER;
	unsigned char *local;
	int i;

	local = malloc(PAGE_SZ);                  /* off the fiber stack */
	if (local == NULL) {
		atomic_fetch_add(&g_fix_fail, 1);
		goto done;
	}

	for (i = 0; i < ITERS; i++) {
		bm_frame_t *f = NULL;
		bm_pid_t fpid;
		int k, dirty;

		if ((rand_r(&seed) & 1) == 0)
			k = (int)((unsigned)rand_r(&seed) % HOT_RANGE);
		else
			k = lo + (int)((unsigned)rand_r(&seed) % (unsigned)(hi - lo));

		if (bm_fix(g_bm, &g_root[k], &f) != XTC_OK) {
			atomic_fetch_add(&g_fix_fail, 1);
			continue;
		}

		/* Verify WHILE PINNED: snapshot the page + pid before any
		 * unfix, so eviction cannot recycle the frame under us. */
		memcpy(local, bm_page(f), PAGE_SZ);
		fpid = bm_frame_pid(f);

		dirty = ((i & 7) == 0);           /* occasionally re-touch */
		bm_unfix(g_bm, f, dirty);         /* bytes unchanged: still canonical */

		if (fpid != g_pid[k] ||
		    !check_page(local, g_pid[k], (uint64_t)k))
			atomic_fetch_add(&g_mismatch, 1);
		else
			atomic_fetch_add(&g_verified, 1);

		/* Yield periodically so peers on the same loop and the
		 * page-provider interleave with us. */
		if ((i & 63) == 0)
			xtc_yield();
	}

	free(local);

done:
	/* The last worker to finish stops the provider so the executor,
	 * once every proc has returned and the provider's timer is gone,
	 * observes a globally idle state and lets xtc_exec_run return. */
	if (atomic_fetch_add(&g_workers_done, 1) + 1 == N_WORKERS)
		bm_provider_stop(g_bm);
}

/*
 * Pre-allocate every root from the main thread (off any loop), writing
 * the canonical pattern and unfixing dirty.  Allocating N_ROOTS pages
 * into N_FRAMES frames already forces cool/flush/evict during setup.
 */
static int
setup_roots(void)
{
	bm_frame_t *f;
	int k;

	for (k = 0; k < N_ROOTS; k++) {
		if (bm_alloc(g_bm, &g_root[k], &f, &g_pid[k]) != XTC_OK) {
			fprintf(stderr, "FAIL: bm_alloc(%d)\n", k);
			return -1;
		}
		fill_page(bm_page(f), g_pid[k], (uint64_t)k);
		bm_unfix(g_bm, f, 1);             /* dirty: must be written out */
	}
	return 0;
}

/* Final single-threaded consistency sweep: every page must still read
 * back its canonical content after the concurrent storm. */
static int
final_check(void)
{
	bm_frame_t *f;
	int k, bad = 0;

	for (k = 0; k < N_ROOTS; k++) {
		if (bm_fix(g_bm, &g_root[k], &f) != XTC_OK) {
			fprintf(stderr, "FAIL: final bm_fix(%d)\n", k);
			return -1;
		}
		if (bm_frame_pid(f) != g_pid[k] ||
		    !check_page(bm_page(f), g_pid[k], (uint64_t)k))
			bad++;
		bm_unfix(g_bm, f, 0);
	}
	return bad;
}

int
main(void)
{
	xtc_exec_t *exec = NULL;
	xtc_proc_opts_t opts = { 0 };
	bm_opts_t bo = BM_OPTS_DEFAULT;
	xtc_pid_t pp, w;
	bm_stats_t st;
	char path[] = "/tmp/sqlxtc-bm-mt-XXXXXX";
	int fd, i, bad;

	fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); return 1; }
	close(fd);

	bo.path = path;
	bo.page_size = PAGE_SZ;
	bo.n_frames = N_FRAMES;
	bo.cool_pct = 25;
	if (bm_create(&bo, &g_bm) != XTC_OK) {
		fprintf(stderr, "FAIL: bm_create\n");
		unlink(path);
		return 1;
	}

	atomic_store(&g_verified, 0);
	atomic_store(&g_mismatch, 0);
	atomic_store(&g_fix_fail, 0);
	atomic_store(&g_workers_done, 0);

	if (setup_roots() != 0) { bm_destroy(g_bm); unlink(path); return 1; }

	/* Multi-loop executor: N_LOOPS loops, each its own OS thread. */
	if (xtc_exec_init(&exec, N_LOOPS) != XTC_OK) {
		fprintf(stderr, "FAIL: xtc_exec_init\n");
		bm_destroy(g_bm); unlink(path); return 1;
	}

	/* Page-provider on loop 0: proactively cools + flushes pages. */
	if (bm_provider_spawn(g_bm, xtc_exec_loop(exec, 0), 1LL * 1000 * 1000,
	    &pp) != XTC_OK) {
		fprintf(stderr, "FAIL: bm_provider_spawn\n");
		(void)xtc_exec_fini(exec); bm_destroy(g_bm); unlink(path);
		return 1;
	}

	/* Pin WORKERS_PER_LOOP workers to each loop (round-robin). */
	for (i = 0; i < N_WORKERS; i++) {
		int loop_idx = i % N_LOOPS;
		g_warg[i].id = i;
		g_warg[i].seed = 0x9e3779b9u ^ (unsigned)(i * 2654435761u + 1);
		opts.name = "bm-worker";
		if (xtc_proc_spawn(xtc_exec_loop(exec, loop_idx), worker_proc,
		    &g_warg[i], &opts, &w) != XTC_OK) {
			fprintf(stderr, "FAIL: spawn worker %d\n", i);
			/* Drain whatever we started, then bail. */
			bm_provider_stop(g_bm);
			(void)xtc_exec_run(exec);
			(void)xtc_exec_fini(exec);
			bm_destroy(g_bm); unlink(path);
			return 1;
		}
	}

	/* Blocks until all workers return AND the provider has been
	 * stopped (so the loops drain to idle). */
	if (xtc_exec_run(exec) != XTC_OK) {
		fprintf(stderr, "FAIL: xtc_exec_run\n");
		(void)xtc_exec_fini(exec); bm_destroy(g_bm); unlink(path);
		return 1;
	}
	(void)xtc_exec_fini(exec);

	/* Final single-threaded consistency sweep. */
	bad = final_check();
	if (bad < 0) { bm_destroy(g_bm); unlink(path); return 1; }

	bm_get_stats(g_bm, &st);
	bm_destroy(g_bm);
	unlink(path);

	/* ---- assertions ---- */
	if (atomic_load(&g_mismatch) != 0 || bad != 0) {
		fprintf(stderr, "FAIL: content corruption "
		    "(concurrent mismatches=%llu, final-sweep mismatches=%d)\n",
		    (unsigned long long)atomic_load(&g_mismatch), bad);
		return 1;
	}
	if (atomic_load(&g_fix_fail) != 0) {
		fprintf(stderr, "FAIL: %llu bm_fix calls failed\n",
		    (unsigned long long)atomic_load(&g_fix_fail));
		return 1;
	}
	if (atomic_load(&g_verified) == 0) {
		fprintf(stderr, "FAIL: no reads were verified\n");
		return 1;
	}
	if (st.resident > N_FRAMES) {
		fprintf(stderr, "FAIL: resident %llu > pool %d\n",
		    (unsigned long long)st.resident, N_FRAMES);
		return 1;
	}
	if (st.evicted == 0 || st.loads == 0 || st.cooled == 0) {
		fprintf(stderr, "FAIL: eviction cycle did not run "
		    "(cooled=%llu evicted=%llu loads=%llu)\n",
		    (unsigned long long)st.cooled,
		    (unsigned long long)st.evicted,
		    (unsigned long long)st.loads);
		return 1;
	}

	printf("  ok   %d workers on %d loops (%d OS threads) did %d fixes "
	    "each over %d roots / %d frames; %llu pinned reads verified, "
	    "0 mismatches\n",
	    N_WORKERS, N_LOOPS, N_LOOPS, ITERS, N_ROOTS, N_FRAMES,
	    (unsigned long long)atomic_load(&g_verified));
	printf("  ok   final single-threaded sweep: all %d pages intact\n",
	    N_ROOTS);
	printf("  ok   concurrent swizzle/cool/evict cycle "
	    "(hits=%llu rescues=%llu loads=%llu cooled=%llu flushed=%llu "
	    "evicted=%llu resident=%llu free=%llu)\n",
	    (unsigned long long)st.hits, (unsigned long long)st.rescues,
	    (unsigned long long)st.loads, (unsigned long long)st.cooled,
	    (unsigned long long)st.flushed, (unsigned long long)st.evicted,
	    (unsigned long long)st.resident,
	    (unsigned long long)st.free_frames);
	printf("All sqlxtc buffer-manager multi-threaded stress tests "
	    "passed.\n");
	return 0;
}
