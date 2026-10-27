/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * Stopping a background proc that has already been killed must not wait.
 * The page provider, the trickler, the metrics proc and the checkpoint
 * proc each clear an "alive" flag that their stop function polls -- but
 * only on a normal return, so a proc that left any other way
 * (xtc_exit_pid, an xtc_exit_self deep in a callee) left the flag set
 * and its stop spun out its whole bound: bm_provider_stop /
 * bm_trickler_stop 100000 x 200 us (about 20 s each), metrics_stop and
 * sx_ckpt_stop 2000 x 1 ms.  A drain that hit that held SIGTERM for
 * over 40 s.  The flag is now cleared from an xtc_proc_at_exit hook,
 * which runs on every exit path.
 *
 * Part 1 spawns the provider, trickler and metrics procs on a loop,
 * kills each with xtc_exit_pid, waits until they are out of the proc
 * table, then times their three stops.  Part 2 runs the storage engine's
 * procs (sx_storage_run), kills the checkpoint proc,
 * and times sx_storage_quiesce (which stops the checkpoint proc first).
 */
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bufmgr.h"
#include "engine.h"
#include "xtc.h"
#include "xtc_inspect.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_res.h"
#include "t_tmp.h"

extern int  metrics_spawn(xtc_loop_t *loop, xtc_res_t *res,
                          _Atomic int *conn_count,
                          _Atomic int64_t *queries_total, xtc_pid_t *out_pid);
extern void metrics_stop(void);

static bm_t *g_bm;
static xtc_pid_t g_pp, g_tr, g_mx, g_ck;
static int64_t g_stop_ms = -1, g_gone_ms = -1, g_q_ms = -1;
static int g_ck_found;

static int64_t
ms(void)
{
	return xtc_clock_mono() / 1000000;
}

static int
gone(xtc_pid_t p)
{
	xtc_proc_info_t pi = XTC_PROC_INFO_INIT;
	return xtc_proc_info(p, &pi) != XTC_OK || !pi.alive;
}

/* Part 2: kill the engine's checkpoint proc, then stop the engine. */
static void
storage_driver(void *arg)
{
	int64_t a;
	(void)arg;
	(void)xtc_proc_sleep(20LL * 1000 * 1000);
	if (!(g_ck_found = sx_storage_ckpt_pid(&g_ck)))
		return;
	(void)xtc_exit_pid(g_ck, 9);
	a = ms();
	while (!gone(g_ck) && ms() - a < 2000)
		(void)xtc_proc_sleep(1LL * 1000 * 1000);
	a = ms();
	sx_storage_quiesce();
	g_q_ms = ms() - a;
}

static void
driver(void *arg)
{
	int64_t a;
	(void)arg;
	(void)xtc_proc_sleep(20LL * 1000 * 1000);      /* let all three park */
	(void)xtc_exit_pid(g_pp, 9);
	(void)xtc_exit_pid(g_tr, 9);
	(void)xtc_exit_pid(g_mx, 9);
	a = ms();
	while (!(gone(g_pp) && gone(g_tr) && gone(g_mx)) && ms() - a < 2000)
		(void)xtc_proc_sleep(1LL * 1000 * 1000);
	g_gone_ms = ms() - a;
	a = ms();
	bm_provider_stop(g_bm);
	bm_trickler_stop(g_bm);
	metrics_stop();
	g_stop_ms = ms() - a;
}

int
main(void)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256];
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t po = { .size = sizeof(xtc_proc_opts_t), .name = "drv" };
	_Atomic int conns = 0;
	xtc_pid_t pid;
	int fd;

	t_tmpl(path, sizeof path, "sqlxtc-bg-kill");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	bo.path = path;
	bo.n_frames = 1024;
	if (bm_create(&bo, &g_bm) != XTC_OK) { unlink(path); return 1; }
	if (xtc_loop_init(&loop) != XTC_OK) return 1;
	CK(bm_provider_spawn(g_bm, loop, 1LL * 1000 * 1000, &g_pp) == XTC_OK);
	CK(bm_trickler_spawn(g_bm, loop, 1LL * 1000 * 1000, &g_tr) == XTC_OK);
	CK(metrics_spawn(loop, NULL, &conns, NULL, &g_mx) == XTC_OK);
	CK(xtc_proc_spawn(loop, driver, NULL, &po, &pid) == XTC_OK);
	(void)xtc_loop_run(loop);
	(void)xtc_loop_fini(loop);
	bm_destroy(g_bm);
	unlink(path);
	printf("  killed provider, trickler, metrics: gone after %lld ms; "
	    "their three stops took %lld ms\n",
	    (long long)g_gone_ms, (long long)g_stop_ms);
	CK(g_gone_ms >= 0 && g_gone_ms < 2000);
	CK(g_stop_ms >= 0 && g_stop_ms < 100);

	/* Part 2: the engine's checkpoint proc. */
	t_tmpl(path, sizeof path, "sqlxtc-bg-kill2");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	CK(sx_init() == SX_OK);
	CK(sx_storage_open(path, 256) == SX_OK);
	if (xtc_loop_init(&loop) != XTC_OK) return 1;
	CK(sx_storage_run(loop) == SX_OK);
	po.name = "drv2";
	CK(xtc_proc_spawn(loop, storage_driver, NULL, &po, &pid) == XTC_OK);
	(void)xtc_loop_run(loop);
	(void)xtc_loop_fini(loop);
	(void)sx_storage_close();
	(void)sx_shutdown();
	printf("  killed the checkpoint proc (found=%d): sx_storage_quiesce "
	    "took %lld ms\n", g_ck_found, (long long)g_q_ms);
	unlink(path);
	snprintf(path + strlen(path), sizeof path - strlen(path), "-wal");
	unlink(path);
	CK(g_ck_found);
	CK(g_q_ms >= 0 && g_q_ms < 100);
	if (g_fail) { fprintf(stderr, "FAIL: test_bg_kill\n"); return 1; }
	printf("  ok   stopping an already-killed background proc does not wait\n");
	return 0;
}
