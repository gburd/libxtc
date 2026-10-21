/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * The page provider and the trickler back off when the pool is idle.
 * They woke every 1 ms / 5 ms and walked a slice whatever the load: an
 * idle sqlxtc-server with a 1048576-frame pool spent 15% of a core on
 * loop 0 (262144 frames: 11%).  This runs both real procs on a loop over
 * an untouched pool for IDLE_MS and counts their passes, then dirties a
 * page and checks the trickler still writes it back promptly (the first
 * busy pass returns to the base interval), and that stopping a
 * backed-off proc does not wait out its long sleep.
 *
 *   ./test_bg_idle            the check (fails on the fixed-cadence procs)
 *
 * Includes the implementation for the pass counters.
 */
#define BM_PP_TICK_TEST_HOOK(bm) ((void)atomic_fetch_add(&g_pp_ticks, 1))
#include <stdatomic.h>
static _Atomic unsigned g_pp_ticks;
#include "bufmgr.h"
#include "bufmgr.c"
#include "xtc_loop.h"
#include "t_tmp.h"

#ifndef BG_IDLE_MAX_NS                 /* the pre-backoff bufmgr.c */
#define BG_IDLE_MAX_NS (64LL * 1000 * 1000)
#endif

#define N_FRAMES  4096u
#define IDLE_MS   600
#define PP_IV_MS  1
#define TR_IV_MS  1

static bm_t *g_bm;
static int64_t g_wb_ms = -1, g_stop_ms = -1;
static unsigned g_pp_idle, g_tr_idle;

static int64_t
ms(void)
{
	return xtc_clock_mono() / 1000000;
}

static void
driver(void *arg)
{
	bm_stats_t st;
	bm_frame_t *f = NULL;
	bm_pid_t pid;
	unsigned p0;
	uint64_t t0;
	int64_t a;
	(void)arg;
	(void)xtc_proc_sleep(50LL * 1000 * 1000);      /* let both start */
	p0 = atomic_load(&g_pp_ticks);
	bm_get_stats(g_bm, &st);
	t0 = st.tr_passes;
	(void)xtc_proc_sleep((int64_t)IDLE_MS * 1000 * 1000);
	g_pp_idle = atomic_load(&g_pp_ticks) - p0;
	bm_get_stats(g_bm, &st);
	g_tr_idle = (unsigned)(st.tr_passes - t0);

	/* Work arrives: the trickler must write it back without waiting
	 * out a full idle sleep series. */
	if (bm_alloc_pid(g_bm, &f, &pid) == XTC_OK) {
		memset(bm_page(f), 0x5a, g_bm->page_size);
		bm_unfix(g_bm, f, 1);
		a = ms();
		while (atomic_load(&f->dirty) && ms() - a < 2000)
			(void)xtc_proc_sleep(1LL * 1000 * 1000);
		if (!atomic_load(&f->dirty))
			g_wb_ms = ms() - a;
	}
	/* Let both back off again, then stop them. */
	(void)xtc_proc_sleep((int64_t)IDLE_MS * 1000 * 1000);
	a = ms();
	bm_provider_stop(g_bm);
	bm_trickler_stop(g_bm);
	g_stop_ms = ms() - a;
}

int
main(void)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256];
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t po = { .name = "drv" };
	xtc_pid_t pid;
	int fd, bad = 0;
	/* Fixed cadence would be IDLE_MS / interval passes; allow a quarter. */
	unsigned pp_cap = IDLE_MS / PP_IV_MS / 4, tr_cap = IDLE_MS / TR_IV_MS / 4;

	t_tmpl(path, sizeof path, "sqlxtc-bg-idle");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	bo.path = path;
	bo.n_frames = N_FRAMES;
	if (bm_create(&bo, &g_bm) != XTC_OK) { unlink(path); return 1; }
	if (xtc_loop_init(&loop) != XTC_OK) return 1;
	if (bm_provider_spawn(g_bm, loop, (int64_t)PP_IV_MS * 1000 * 1000, NULL) != XTC_OK ||
	    bm_trickler_spawn(g_bm, loop, (int64_t)TR_IV_MS * 1000 * 1000, NULL) != XTC_OK ||
	    xtc_proc_spawn(loop, driver, NULL, &po, &pid) != XTC_OK)
		return 1;
	(void)xtc_loop_run(loop);
	(void)xtc_loop_fini(loop);
	bm_destroy(g_bm);
	unlink(path);

	printf("  idle %d ms: provider passes=%u (cap %u), trickler passes=%u "
	    "(cap %u); write-back after idle %lld ms; stop %lld ms\n",
	    IDLE_MS, g_pp_idle, pp_cap, g_tr_idle, tr_cap,
	    (long long)g_wb_ms, (long long)g_stop_ms);
	if (g_pp_idle > pp_cap) {
		fprintf(stderr, "FAIL: idle provider ran %u passes in %d ms\n",
		    g_pp_idle, IDLE_MS);
		bad = 1;
	}
	if (g_tr_idle > tr_cap) {
		fprintf(stderr, "FAIL: idle trickler ran %u passes in %d ms\n",
		    g_tr_idle, IDLE_MS);
		bad = 1;
	}
	if (g_pp_idle == 0 || g_tr_idle == 0) {
		fprintf(stderr, "FAIL: an idle proc stopped running entirely\n");
		bad = 1;
	}
	/* Snap-back: within the cap's sleep plus a few base passes. */
	if (g_wb_ms < 0 || g_wb_ms > BG_IDLE_MAX_NS / 1000000 + 50) {
		fprintf(stderr, "FAIL: dirty page written back after %lld ms\n",
		    (long long)g_wb_ms);
		bad = 1;
	}
	if (g_stop_ms < 0 || g_stop_ms > 20) {
		fprintf(stderr, "FAIL: stopping backed-off procs took %lld ms\n",
		    (long long)g_stop_ms);
		bad = 1;
	}
	if (bad) { fprintf(stderr, "FAIL: test_bg_idle\n"); return 1; }
	printf("  ok   idle provider/trickler back off; work and stop are prompt\n");
	return 0;
}
