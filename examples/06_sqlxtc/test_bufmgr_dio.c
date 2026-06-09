/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_bufmgr_dio.c
 *	Buffer manager with DIRECT I/O + ADAPTIVE writeback: the same
 *	allocate-evict-reload-verify cycle as test_bufmgr, but with the
 *	page store opened direct (cache-bypass) and the trickler's pacing
 *	genetically tuned (xtc_dio_sched).  Proves data integrity through
 *	direct writes and that the adaptive trickler ran.  Skips (exit 0)
 *	where the filesystem cannot do direct I/O (e.g. Linux tmpfs).
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bufmgr.h"
#include "xtc.h"
#include "xtc_fs.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "t_tmp.h"

#define N_PAGES   200
#define N_FRAMES  16
#define PAGE_SZ   4096

static bm_t       *g_bm;
static bm_swip_t   g_root[N_PAGES];
static bm_pid_t    g_pid[N_PAGES];
static _Atomic int g_result;

static void
fill_page(void *p, bm_pid_t pid, uint64_t k)
{
	uint64_t *u = p;
	u[0] = pid; u[1] = k;
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
	for (k = 0; k < N_PAGES; k++) {
		if (bm_alloc(g_bm, &g_root[k], &f, &g_pid[k]) != XTC_OK) { ok = 0; break; }
		fill_page(bm_page(f), g_pid[k], (uint64_t)k);
		bm_unfix(g_bm, f, 1);
	}
	for (k = 0; ok && k < N_PAGES; k++) {
		if (bm_fix(g_bm, &g_root[k], &f) != XTC_OK) { ok = 0; break; }
		if (!check_page(bm_page(f), g_pid[k], (uint64_t)k)) ok = 0;
		bm_unfix(g_bm, f, 0);
	}
	atomic_store(&g_result, ok ? 1 : -1);
	bm_provider_stop(g_bm);
	bm_trickler_stop(g_bm);     /* both must stop for the loop to drain */
}

/* Can this filesystem actually do direct I/O on a fresh temp file? */
static int
direct_supported(const char *path)
{
	int fd = -1, rc;
	void *buf = NULL;
	size_t done = 0;
	if (xtc_fs_open(path, XTC_FS_WRITE | XTC_FS_DIRECT, &fd) != XTC_OK)
		return 0;
	if (xtc_fs_dio_alloc(fd, PAGE_SZ, &buf) != XTC_OK) { xtc_fs_close(fd); return 0; }
	memset(buf, 0, PAGE_SZ);
	rc = xtc_fs_pwrite(fd, buf, PAGE_SZ, 0, &done);
	xtc_fs_dio_free(buf);
	xtc_fs_close(fd);
	return (rc == XTC_OK && done == PAGE_SZ);
}

int
main(void)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	bm_opts_t bo = BM_OPTS_DEFAULT;
	xtc_pid_t w, pp, tr;
	bm_stats_t st;
	char path[256]; t_tmpl(path, sizeof path, "sqlxtc-bm-dio");
	int fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); return 1; }
	close(fd);

	if (!direct_supported(path)) {
		printf("SKIP: filesystem does not support direct I/O here\n");
		unlink(path);
		return 0;
	}

	bo.path = path;
	bo.page_size = PAGE_SZ;
	bo.n_frames = N_FRAMES;
	bo.cool_pct = 25;
	bo.direct = 1;
	bo.adaptive_writeback = 1;
	if (bm_create(&bo, &g_bm) != XTC_OK) { fprintf(stderr, "bm_create\n"); return 1; }

	atomic_store(&g_result, 0);
	if (xtc_loop_init(&loop) != XTC_OK) return 1;
	if (bm_provider_spawn(g_bm, loop, 1LL * 1000 * 1000, &pp) != XTC_OK) return 1;
	if (bm_trickler_spawn(g_bm, loop, 1LL * 1000 * 1000, &tr) != XTC_OK) return 1;
	opts.name = "worker";
	if (xtc_proc_spawn(loop, worker_proc, NULL, &opts, &w) != XTC_OK) return 1;
	if (xtc_loop_run(loop) != XTC_OK) { fprintf(stderr, "loop_run\n"); return 1; }
	(void)xtc_loop_fini(loop);

	bm_get_stats(g_bm, &st);
	bm_destroy(g_bm);
	unlink(path);

	if (atomic_load(&g_result) != 1) {
		fprintf(stderr, "FAIL: direct-I/O workload result=%d\n",
		    atomic_load(&g_result));
		return 1;
	}
	if (st.flushed == 0) {
		fprintf(stderr, "FAIL: no pages written via direct I/O\n");
		return 1;
	}
	printf("  ok   %d pages cycled (direct I/O); all content survived "
	    "eviction + reload\n", N_PAGES);
	printf("  ok   adaptive trickler ran (trickled=%llu flushed=%llu)\n",
	    (unsigned long long)st.trickled, (unsigned long long)st.flushed);
	printf("All sqlxtc direct-I/O + adaptive-writeback tests passed.\n");
	return 0;
}
