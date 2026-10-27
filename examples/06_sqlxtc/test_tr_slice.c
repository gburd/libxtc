/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * The trickler's pass is bounded like the page provider's tick: each one
 * visits at most TR_SLICE frames whatever the pool size, resuming where
 * the last stopped, and dirty pages anywhere in the pool are still
 * written back.  It walked all n_frames every 5 ms on loop 0: at a 4 GiB
 * pool (1048576 frames) that was 40% of an in-process TPC-C profile.
 * Runs the real trickler proc on a loop over a pool with dirty frames
 * scattered across it, and checks frames-visited per pass and that every
 * dirty frame got written.
 */
#include "bufmgr.h"
#include "bufmgr.c"
#include "xtc_loop.h"
#include "t_tmp.h"

static bm_t *g_bm;
static int g_fail;

static void
driver(void *arg)
{
	bm_stats_t st;
	uint32_t n = g_bm->n_frames, i, passes;
	uint64_t want_passes = (n + TR_SLICE - 1) / TR_SLICE + 2;
	(void)arg;
	/* Wait until the trickler has written every dirty page (it takes
	 * `batch` per pass, so ndirty/batch passes at least) -- or 20 s. */
	for (passes = 0; passes < 20000; passes++) {
		uint32_t d = 0;
		bm_get_stats(g_bm, &st);
		for (i = 0; i < n; i++)
			d += atomic_load(&g_bm->frames[i].dirty) ? 1 : 0;
		if (d == 0 && st.tr_passes >= want_passes)
			break;
		(void)xtc_proc_sleep(1LL * 1000 * 1000);
	}
	bm_trickler_stop(g_bm);
	bm_get_stats(g_bm, &st);
	printf("  n_frames=%u passes=%llu visited=%llu (%.0f per pass) trickled=%llu\n",
	    n, (unsigned long long)st.tr_passes, (unsigned long long)st.tr_visited,
	    st.tr_passes ? (double)st.tr_visited / (double)st.tr_passes : 0.0,
	    (unsigned long long)st.trickled);
	if (st.tr_passes == 0 || st.tr_visited > st.tr_passes * (uint64_t)TR_SLICE) {
		fprintf(stderr, "FAIL: a pass visited more than %u frames\n", TR_SLICE);
		g_fail = 1;
	}
	for (i = 0; i < n; i++)
		if (atomic_load(&g_bm->frames[i].dirty)) {
			fprintf(stderr, "FAIL: frame %u still dirty\n", i);
			g_fail = 1;
			break;
		}
}

static int
run(uint32_t n_frames, uint32_t ndirty)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256];
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t po = { .size = sizeof(xtc_proc_opts_t), .name = "drv" };
	xtc_pid_t pid;
	uint32_t i;
	int fd;
	t_tmpl(path, sizeof path, "sqlxtc-tr-slice");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	bo.path = path;
	bo.n_frames = n_frames;
	if (bm_create(&bo, &g_bm) != XTC_OK) { unlink(path); return 1; }
	/* ndirty pages, spread over the whole pool */
	for (i = 0; i < ndirty; i++) {
		bm_frame_t *f = NULL;
		bm_pid_t pid;
		if (bm_alloc_pid(g_bm, &f, &pid) != XTC_OK) { g_fail = 1; break; }
		memset(bm_page(f), (int)(i & 0xFF), g_bm->page_size);
		bm_unfix(g_bm, f, 1);
	}
	if (xtc_loop_init(&loop) != XTC_OK) return 1;
	(void)bm_trickler_spawn(g_bm, loop, 1LL * 1000 * 1000, NULL);
	(void)xtc_proc_spawn(loop, driver, NULL, &po, &pid);
	(void)xtc_loop_run(loop);
	(void)xtc_loop_fini(loop);
	bm_destroy(g_bm);
	unlink(path);
	return 0;
}

int
main(void)
{
	if (run(4096, 400) || run(TR_SLICE * 4 + 7, 300)) return 1;
	if (g_fail) { fprintf(stderr, "FAIL: test_tr_slice\n"); return 1; }
	printf("  ok   trickler pass bounded to %u frames; every dirty frame written\n",
	    TR_SLICE);
	return 0;
}
