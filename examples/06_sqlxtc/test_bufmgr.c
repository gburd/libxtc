/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_bufmgr.c
 *	In-process test of the LeanStore-style buffer manager.  A worker
 *	process allocates far more pages than the resident pool holds --
 *	forcing the swizzle -> cool -> write-out -> evict cycle -- while
 *	the page-provider process concurrently cools and flushes.  Every
 *	page is then fixed back and its content verified, proving that
 *	evicted pages were written and reloaded intact and that the
 *	concurrent state machine never corrupts a frame.  No daemon.
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

#define N_PAGES   200
#define N_FRAMES  16
#define PAGE_SZ   4096

static bm_t       *g_bm;
static bm_swip_t   g_root[N_PAGES];     /* the "parents" holding swips */
static bm_pid_t    g_pid[N_PAGES];
static _Atomic int g_result;            /* 0 = not run, 1 = pass, -1 = fail */

/* page content: [u64 pid][u64 k][fill byte at 100..] */
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
	    ((const unsigned char *)p)[PAGE_SZ - 1] == (unsigned char)((k >> 8) & 0xff);
}

static void
worker_proc(void *arg)
{
	int k, ok = 1;
	bm_frame_t *f;
	(void)arg;

	/* Allocate N_PAGES pages into a pool of N_FRAMES: forces eviction. */
	for (k = 0; k < N_PAGES; k++) {
		if (bm_alloc(g_bm, &g_root[k], &f, &g_pid[k]) != XTC_OK) { ok = 0; break; }
		fill_page(bm_page(f), g_pid[k], (uint64_t)k);
		bm_unfix(g_bm, f, 1);                 /* dirty */
	}

	/* Fix every page back -- most were evicted, so they reload from the
	 * backing file -- and verify content survived the round trip. */
	for (k = 0; ok && k < N_PAGES; k++) {
		if (bm_fix(g_bm, &g_root[k], &f) != XTC_OK) { ok = 0; break; }
		if (!check_page(bm_page(f), g_pid[k], (uint64_t)k)) ok = 0;
		bm_unfix(g_bm, f, 0);
	}

	/* Re-touch a hot working set repeatedly: exercises HOT hits and
	 * COOL rescues against the concurrent provider. */
	for (k = 0; ok && k < 500; k++) {
		int idx = k % 12;
		if (bm_fix(g_bm, &g_root[idx], &f) != XTC_OK) { ok = 0; break; }
		if (!check_page(bm_page(f), g_pid[idx], (uint64_t)idx)) ok = 0;
		bm_unfix(g_bm, f, (k & 1));            /* sometimes dirty */
	}

	atomic_store(&g_result, ok ? 1 : -1);
	bm_provider_stop(g_bm);                        /* let the loop drain */
}

int
main(void)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	bm_opts_t bo = BM_OPTS_DEFAULT;
	xtc_pid_t w, pp;
	bm_stats_t st;
	char path[] = "/tmp/sqlxtc-bm-test-XXXXXX";
	int fd;

	fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); return 1; }
	close(fd);

	bo.path = path;
	bo.page_size = PAGE_SZ;
	bo.n_frames = N_FRAMES;
	bo.cool_pct = 25;
	if (bm_create(&bo, &g_bm) != XTC_OK) { fprintf(stderr, "bm_create\n"); return 1; }

	atomic_store(&g_result, 0);
	if (xtc_loop_init(&loop) != XTC_OK) return 1;
	if (bm_provider_spawn(g_bm, loop, 1LL * 1000 * 1000, &pp) != XTC_OK) return 1;
	opts.name = "worker";
	if (xtc_proc_spawn(loop, worker_proc, NULL, &opts, &w) != XTC_OK) return 1;
	if (xtc_loop_run(loop) != XTC_OK) { fprintf(stderr, "loop_run\n"); return 1; }
	(void)xtc_loop_fini(loop);

	bm_get_stats(g_bm, &st);
	bm_destroy(g_bm);
	unlink(path);

	if (atomic_load(&g_result) != 1) {
		fprintf(stderr, "FAIL: workload result=%d\n", atomic_load(&g_result));
		return 1;
	}
	if (st.resident > N_FRAMES) {
		fprintf(stderr, "FAIL: resident %llu > pool %d\n",
		    (unsigned long long)st.resident, N_FRAMES);
		return 1;
	}
	if (st.evicted == 0 || st.cooled == 0 || st.loads == 0) {
		fprintf(stderr, "FAIL: eviction cycle did not run "
		    "(cooled=%llu evicted=%llu loads=%llu)\n",
		    (unsigned long long)st.cooled, (unsigned long long)st.evicted,
		    (unsigned long long)st.loads);
		return 1;
	}
	if (st.flushed == 0) {
		fprintf(stderr, "FAIL: no dirty pages were written before eviction\n");
		return 1;
	}

	printf("  ok   %d pages cycled through a %d-frame pool; all content "
	    "survived eviction + reload\n", N_PAGES, N_FRAMES);
	printf("  ok   swizzling + cooling-stage eviction "
	    "(hits=%llu rescues=%llu loads=%llu cooled=%llu flushed=%llu "
	    "evicted=%llu resident=%llu)\n",
	    (unsigned long long)st.hits, (unsigned long long)st.rescues,
	    (unsigned long long)st.loads, (unsigned long long)st.cooled,
	    (unsigned long long)st.flushed, (unsigned long long)st.evicted,
	    (unsigned long long)st.resident);
	printf("All sqlxtc buffer-manager tests passed.\n");
	return 0;
}
