/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/sim/test_sim_bufmgr.c
 *	DST coverage of the sqlxtc storage engine's CONCURRENCY layer --
 *	the LeanStore-style buffer manager (examples/06_sqlxtc/bufmgr.c) --
 *	driven by the deterministic scheduler xtc_sim_exec_run instead of
 *	the real xtc_exec_run.  This is a scaled-down version of the
 *	multi-threaded stress test (examples/06_sqlxtc/test_bufmgr_mt.c):
 *	the page provider proactively cools + flushes, and N worker procs
 *	across N loops each pin/read/verify a shared set of pages while the
 *	pool (32 frames) evicts to hold >> 32 pages resident.
 *
 *	Under DST the fibers really park: a fix that misses pages in a page
 *	via xtc_aio (io_sim's deferred, SEEDED-latency completion), a busy
 *	pool yields, the provider sleeps on the virtual clock.  So the page
 *	I/O completion order and the fiber interleaving are both part of the
 *	replayable schedule.  I/O faults are ENABLED (a modest seeded
 *	latency, no injected errors -- the bufmgr's page reads must return
 *	the exact bytes written, so an EIO would be a spurious corruption)
 *	so completion order genuinely reorders across runs.
 *
 *	The provider runs on a periodic timer; under sim the virtual clock
 *	drives that timer, so a provider left running would step the clock
 *	forever and the run would never quiesce (XTC_E_AGAIN).  The LAST
 *	worker to finish calls bm_provider_stop, exactly as the mt test
 *	does, so the provider observes the flag, exits, and the executor
 *	reaches a globally idle state.
 *
 *	Asserts:
 *	  (a) xtc_sim_exec_run returns XTC_OK -- QUIESCENCE, no hang.  A
 *	      XTC_E_AGAIN (livelock / clock-spinning provider) or
 *	      XTC_E_DEADLK (lost wakeup) is a real bug.
 *	  (b) data consistency: no torn pages, every pinned read matched
 *	      its canonical content, and a final single-threaded sweep
 *	      finds all pages intact (the same invariant the mt test checks).
 *	  (c) REPLAY: the SAME seed run twice yields the identical result
 *	      hash (verified count folded with the final page contents) AND
 *	      the identical engine state hash.
 *	  (d) a DIFFERENT seed produces a (usually) different schedule but
 *	      still-consistent data.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_async.h"      /* xtc_yield */
#include "xtc_sim.h"
#include "bufmgr.h"

#define PAGE_SZ          4096
#define N_FRAMES         32        /* small resident pool -> eviction churn */
#define N_ROOTS          256       /* >> N_FRAMES: forces the eviction cycle */
#define N_LOOPS          4
#define WORKERS_PER_LOOP 2
#define N_WORKERS        (N_LOOPS * WORKERS_PER_LOOP)
#define ITERS            200       /* fix/verify iterations per worker */
#define HOT_RANGE        32         /* root[0..HOT_RANGE) hammered by ALL */
#define DISJOINT_PER     ((N_ROOTS - HOT_RANGE) / N_WORKERS)

static bm_t       *g_bm;
static bm_pid_t    g_pid[N_ROOTS];

static _Atomic uint64_t g_verified;     /* pinned reads that matched */
static _Atomic uint64_t g_mismatch;     /* content/pid mismatches (bugs) */
static _Atomic uint64_t g_fix_fail;     /* bm_fix returned != XTC_OK */
static _Atomic int      g_workers_done; /* count; last one stops provider */

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
 * Worker process.  Each iteration picks a page id from the shared hot
 * range (concurrent fixes of the SAME page) or from this worker's own
 * disjoint range (distinct pages -> free-list / eviction contention),
 * fixes it, copies + verifies the page while it is still pinned, then
 * unfixes (sometimes dirty, but WITHOUT changing the bytes -- so the
 * canonical content for a pid/k is stable for the whole run).
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

		if (bm_fix_pid(g_bm, g_pid[k], &f) != XTC_OK) {
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
		if ((i & 15) == 0)
			xtc_yield();
	}

	free(local);

done:
	/* The last worker to finish stops the provider so the executor,
	 * once every proc has returned and the provider's timer is gone,
	 * observes a globally idle state and lets xtc_sim_exec_run return. */
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
		if (bm_alloc_pid(g_bm, &f, &g_pid[k]) != XTC_OK)
			return -1;
		fill_page(bm_page(f), g_pid[k], (uint64_t)k);
		bm_unfix(g_bm, f, 1);             /* dirty: must be written out */
	}
	return 0;
}

/*
 * Final single-threaded consistency sweep: every page must still read
 * back its canonical content after the concurrent storm.  Folds each
 * page's (pid,k) into a content hash so replay equality covers the
 * data, not just the counters.  Returns the number of bad pages, or -1
 * on a fix failure; *out_hash gets the content fold.
 */
static int
final_check(uint64_t *out_hash)
{
	bm_frame_t *f;
	uint64_t h = 0xCBF29CE484222325ull;   /* FNV-1a basis */
	int k, bad = 0;

	for (k = 0; k < N_ROOTS; k++) {
		if (bm_fix_pid(g_bm, g_pid[k], &f) != XTC_OK)
			return -1;
		if (bm_frame_pid(f) != g_pid[k] ||
		    !check_page(bm_page(f), g_pid[k], (uint64_t)k))
			bad++;
		else {
			h ^= (uint64_t)g_pid[k];
			h *= 0x100000001B3ull;
			h ^= (uint64_t)k;
			h *= 0x100000001B3ull;
		}
		bm_unfix(g_bm, f, 0);
	}
	*out_hash = h;
	return bad;
}

/*
 * One full run under the deterministic scheduler.  Returns the
 * xtc_sim_exec_run rc; fills the observable outputs so main() can assert
 * quiescence, consistency, and replay.
 */
static int
run_once(uint64_t seed, uint64_t *out_verified, uint64_t *out_mismatch,
    uint64_t *out_fix_fail, int *out_bad, uint64_t *out_content,
    uint64_t *out_state)
{
	xtc_exec_t *exec = NULL;
	xtc_proc_opts_t opts = { 0 };
	bm_opts_t bo = BM_OPTS_DEFAULT;
	xtc_pid_t pp, w;
	char path[] = "/tmp/sim_bufmgr_XXXXXX";
	int fd, i, rc;

	atomic_store(&g_verified, 0);
	atomic_store(&g_mismatch, 0);
	atomic_store(&g_fix_fail, 0);
	atomic_store(&g_workers_done, 0);

	fd = mkstemp(path);
	if (fd < 0)
		return -1;
	close(fd);

	bo.path = path;
	bo.page_size = PAGE_SZ;
	bo.n_frames = N_FRAMES;
	bo.cool_pct = 25;
	if (bm_create(&bo, &g_bm) != XTC_OK) {
		unlink(path);
		return -1;
	}

	if (setup_roots() != 0) {
		bm_destroy(g_bm);
		unlink(path);
		return -1;
	}

	if (xtc_exec_init(&exec, N_LOOPS) != XTC_OK) {
		bm_destroy(g_bm);
		unlink(path);
		return -1;
	}

	/* Deferred, seeded-latency page-I/O completions -- but NO injected
	 * faults (fault_pct 0): the bufmgr must read back exactly what it
	 * wrote, so a short read / EIO would be a spurious "corruption".
	 * The latency alone reorders completions across runs, making I/O
	 * ordering part of the replayable schedule. */
	xtc_sim_io_faults_enable(50 * 1000LL, 500 * 1000LL, 0);

	/* Page-provider on loop 0: proactively cools + flushes (1ms). */
	if (bm_provider_spawn(g_bm, xtc_exec_loop(exec, 0), 1LL * 1000 * 1000,
	    &pp) != XTC_OK) {
		xtc_sim_io_faults_disable();
		(void)xtc_exec_fini(exec);
		bm_destroy(g_bm);
		unlink(path);
		return -1;
	}

	for (i = 0; i < N_WORKERS; i++) {
		int loop_idx = i % N_LOOPS;
		g_warg[i].id = i;
		g_warg[i].seed = 0x9e3779b9u ^ (unsigned)(i * 2654435761u + 1);
		opts.name = "bm-worker";
		if (xtc_proc_spawn(xtc_exec_loop(exec, loop_idx), worker_proc,
		    &g_warg[i], &opts, &w) != XTC_OK) {
			/* Best-effort drain of whatever we started. */
			bm_provider_stop(g_bm);
			(void)xtc_sim_exec_run(exec, seed, 20000000);
			xtc_sim_io_faults_disable();
			(void)xtc_exec_fini(exec);
			bm_destroy(g_bm);
			unlink(path);
			return -1;
		}
	}

	rc = xtc_sim_exec_run(exec, seed, 20000000);

	*out_verified = atomic_load(&g_verified);
	*out_mismatch = atomic_load(&g_mismatch);
	*out_fix_fail = atomic_load(&g_fix_fail);
	if (out_state)
		*out_state = xtc_sim_state_hash(exec);

	xtc_sim_io_faults_disable();
	(void)xtc_exec_fini(exec);

	/* Final single-threaded consistency sweep (sim now inactive). */
	*out_bad = final_check(out_content);

	bm_destroy(g_bm);
	g_bm = NULL;
	unlink(path);
	return rc;
}

/* Fold the observable app outputs into one replay hash. */
static uint64_t
result_hash(uint64_t verified, uint64_t content)
{
	uint64_t h = 0xCBF29CE484222325ull;
	h ^= verified; h *= 0x100000001B3ull;
	h ^= content;  h *= 0x100000001B3ull;
	return h;
}

int
main(void)
{
	uint64_t v1 = 0, v2 = 0, mm1 = 0, mm2 = 0, ff1 = 0, ff2 = 0;
	uint64_t c1 = 0, c2 = 0, s1 = 0, s2 = 0;
	int bad1 = 0, bad2 = 0, rc;
	uint64_t rh1, rh2;

	/* --- same seed twice: quiescence + consistency + replay --- */
	rc = run_once(0x5B10, &v1, &mm1, &ff1, &bad1, &c1, &s1);
	if (rc != XTC_OK) {
		printf("FAIL: bufmgr run did not quiesce (rc=%d) -- a "
		    "clock-spinning provider (AGAIN) or a lost page-I/O "
		    "wakeup (DEADLK)?\n", rc);
		return 1;
	}
	rc = run_once(0x5B10, &v2, &mm2, &ff2, &bad2, &c2, &s2);
	if (rc != XTC_OK) {
		printf("FAIL: bufmgr replay run did not quiesce (rc=%d)\n", rc);
		return 1;
	}

	rh1 = result_hash(v1, c1);
	rh2 = result_hash(v2, c2);

	printf("run1: verified=%llu mismatch=%llu fix_fail=%llu bad=%d "
	    "content=%016llx state=%016llx\n",
	    (unsigned long long)v1, (unsigned long long)mm1,
	    (unsigned long long)ff1, bad1,
	    (unsigned long long)c1, (unsigned long long)s1);
	printf("run2: verified=%llu mismatch=%llu fix_fail=%llu bad=%d "
	    "content=%016llx state=%016llx\n",
	    (unsigned long long)v2, (unsigned long long)mm2,
	    (unsigned long long)ff2, bad2,
	    (unsigned long long)c2, (unsigned long long)s2);

	/* (b) consistency */
	if (mm1 != 0 || bad1 != 0) {
		printf("FAIL: content corruption (concurrent mismatches=%llu, "
		    "final-sweep bad=%d)\n", (unsigned long long)mm1, bad1);
		return 1;
	}
	if (ff1 != 0) {
		printf("FAIL: %llu bm_fix calls failed\n",
		    (unsigned long long)ff1);
		return 1;
	}
	if (v1 == 0) {
		printf("FAIL: no reads were verified\n");
		return 1;
	}

	/* (c) replay */
	if (v1 != v2 || c1 != c2 || s1 != s2 || rh1 != rh2 ||
	    mm1 != mm2 || ff1 != ff2 || bad1 != bad2) {
		printf("FAIL: bufmgr run did not replay "
		    "(result %016llx/%016llx state %016llx/%016llx)\n",
		    (unsigned long long)rh1, (unsigned long long)rh2,
		    (unsigned long long)s1, (unsigned long long)s2);
		return 1;
	}

	/* (d) a different seed: (usually) different schedule, still
	 * consistent data.  We do not require the state hash to differ (a
	 * small workload can coincide), only that consistency still holds. */
	rc = run_once(0xA17E, &v2, &mm2, &ff2, &bad2, &c2, &s2);
	if (rc != XTC_OK) {
		printf("FAIL: alt-seed bufmgr run did not quiesce (rc=%d)\n",
		    rc);
		return 1;
	}
	printf("altseed: verified=%llu mismatch=%llu fix_fail=%llu bad=%d "
	    "content=%016llx state=%016llx\n",
	    (unsigned long long)v2, (unsigned long long)mm2,
	    (unsigned long long)ff2, bad2,
	    (unsigned long long)c2, (unsigned long long)s2);
	if (mm2 != 0 || ff2 != 0 || bad2 != 0 || v2 == 0) {
		printf("FAIL: alt-seed run inconsistent "
		    "(mismatch=%llu fix_fail=%llu bad=%d verified=%llu)\n",
		    (unsigned long long)mm2, (unsigned long long)ff2, bad2,
		    (unsigned long long)v2);
		return 1;
	}

	printf("OK: sqlxtc buffer manager (storage concurrency layer) runs "
	    "under DST -- %d workers x %d loops fixed/verified %llu pinned "
	    "reads over %d pages / %d frames, reached quiescence (no hang), "
	    "data consistent, and replays byte-identically from seed\n",
	    N_WORKERS, N_LOOPS, (unsigned long long)v1, N_ROOTS, N_FRAMES);
	return 0;
}
