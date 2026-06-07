/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * bench/sqlxtc/engine_loop.c
 *	Loop/proc A/B: the same random point-op SQL workload as engine_mt,
 *	but driven the way sqlxtc is DESIGNED to run -- as libxtc processes
 *	on a multi-loop executor, with the storage engine's group-commit
 *	WAL writer spawned.  Each worker runs the VDBE through its own
 *	xstore connection; a write commit goes through wal_commit, which
 *	parks the fiber on the writer's acknowledgement, so the loop runs
 *	other workers meanwhile and the writer coalesces many commits into
 *	ONE fsync (group commit).  This is the path engine_mt's off-loop OS
 *	threads cannot use (off-loop, each commit fsyncs synchronously,
 *	serialized).
 *
 *	`--loops L` sets the executor's loop count (= cores used);
 *	`--procs P` the worker fibers per loop (oversubscription that feeds
 *	the group-commit writer).  Total committers W = L*P.  Reports
 *	aggregate throughput over the concurrent window and merged
 *	p50/p95/p99/p99.9/max as one JSON line.
 *
 * usage: engine_loop [--loops L] [--procs P] [--rows N] [--cache-kb K]
 *        [--ops O] [--read-pct P] [--row-bytes B] [--label TAG]
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_exec.h"
#include "engine.h"

static int g_loops    = 4;
static int g_procs    = 4;        /* worker fibers per loop */
static int g_rows     = 500000;
static int g_cache_kb = 262144;
static int g_ops      = 80000;    /* per worker */
static int g_read_pct = 95;
static int g_row      = 200;
static const char *g_label = "loop";

static char g_blob[4096];
static _Atomic int g_left;        /* workers remaining; last one quiesces */

struct wk {
	int        id;
	uint32_t  *lat;
	uint64_t   start_ns, end_ns;
};

static uint64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return (x > y) - (x < y);
}

static void
worker_proc(void *arg)
{
	struct wk *w = arg;
	sx_db *h = NULL;
	sx_stmt *sel = NULL, *upd = NULL;
	uint64_t rng = 0x9e3779b97f4a7c15ull ^ ((uint64_t)w->id << 32);
	int i;

	if (sx_open(":memory:", &h) != SX_OK) goto done;
	(void)sx_exec(h, "CREATE VIRTUAL TABLE t USING xstore;", NULL);
	sx_prepare(h, "SELECT v FROM t WHERE k=?", -1, &sel, NULL);
	sx_prepare(h, "UPDATE t SET v=? WHERE k=?", -1, &upd, NULL);

	w->start_ns = now_ns();
	for (i = 0; i < g_ops; i++) {
		uint32_t k;
		int is_read;
		uint64_t a, b;
		rng = rng * 6364136223846793005ull + 1442695040888963407ull;
		k = (uint32_t)((rng >> 33) % (uint32_t)g_rows) + 1;
		is_read = ((int)((rng >> 17) % 100) < g_read_pct);
		a = now_ns();
		if (is_read) {
			sx_bind_int64(sel, 1, k);
			while (sx_step(sel) == SX_ROW) { /* drain */ }
			sx_reset(sel);
		} else {
			sx_bind_blob(upd, 1, g_blob, g_row);
			sx_bind_int64(upd, 2, k);
			(void)sx_step(upd);
			sx_reset(upd);
		}
		b = now_ns();
		w->lat[i] = (b - a) > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)(b - a);
	}
	w->end_ns = now_ns();
	sx_finalize(sel); sx_finalize(upd);
	sx_close(h);
done:
	/* Last worker out stops the background procs so the executor drains. */
	if (atomic_fetch_sub(&g_left, 1) == 1)
		sx_storage_quiesce();
}

int
main(int argc, char **argv)
{
	xtc_exec_t *exec = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t pid;
	sx_db *h = NULL;
	sx_stmt *ins = NULL;
	char path[64];
	struct wk *ws;
	uint32_t *all;
	uint64_t load0, load1, t_min = ~0ull, t_max = 0;
	long total_ops;
	double load_s, win_s;
	int i, t, nworkers, fd;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--loops") && i + 1 < argc) g_loops = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--procs") && i + 1 < argc) g_procs = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--rows") && i + 1 < argc) g_rows = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--cache-kb") && i + 1 < argc) g_cache_kb = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--ops") && i + 1 < argc) g_ops = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--read-pct") && i + 1 < argc) g_read_pct = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--row-bytes") && i + 1 < argc) g_row = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--label") && i + 1 < argc) g_label = argv[++i];
	}
	if (g_loops < 1) g_loops = 1;
	if (g_procs < 1) g_procs = 1;
	if (g_row > (int)sizeof g_blob) g_row = sizeof g_blob;
	nworkers = g_loops * g_procs;
	memset(g_blob, 'x', sizeof g_blob);

	snprintf(path, sizeof path, "/tmp/sqlxtc-loop-XXXXXX");
	fd = mkstemp(path); if (fd < 0) return 1; close(fd); unlink(path);

	if (sx_init() != SX_OK) return 1;
	if (sx_storage_open(path, (unsigned)(g_cache_kb * 1024 / 4096)) != SX_OK) return 1;

	/* load (off loop, before the writer is spawned) */
	if (sx_open(":memory:", &h) != SX_OK) return 1;
	(void)sx_exec(h, "CREATE VIRTUAL TABLE t USING xstore;", NULL);
	sx_prepare(h, "INSERT INTO t(k,v) VALUES(?,?)", -1, &ins, NULL);
	load0 = now_ns();
	sx_exec(h, "BEGIN;", NULL);
	for (i = 1; i <= g_rows; i++) {
		sx_bind_int64(ins, 1, i);
		sx_bind_blob(ins, 2, g_blob, g_row);
		sx_step(ins); sx_reset(ins);
		if ((i % 5000) == 0) { sx_exec(h, "COMMIT;", NULL); sx_exec(h, "BEGIN;", NULL); }
	}
	sx_exec(h, "COMMIT;", NULL);
	sx_finalize(ins); sx_close(h);
	load1 = now_ns();

	/* executor + the engine's background procs (incl. group-commit writer) */
	if (xtc_exec_init(&exec, g_loops) != XTC_OK) return 1;
	if (sx_storage_run(xtc_exec_loop(exec, 0)) != SX_OK) return 1;

	ws = calloc((size_t)nworkers, sizeof *ws);
	atomic_store(&g_left, nworkers);
	for (t = 0; t < nworkers; t++) {
		ws[t].id = t;
		ws[t].lat = calloc((size_t)g_ops, sizeof(uint32_t));
		opts.name = "loadgen";
		if (xtc_proc_spawn(xtc_exec_loop(exec, t % g_loops), worker_proc,
		    &ws[t], &opts, &pid) != XTC_OK) return 1;
	}
	if (xtc_exec_run(exec) != XTC_OK) return 1;
	(void)xtc_exec_fini(exec);
	sx_storage_close();
	(void)sx_shutdown();

	/* aggregate over the concurrent window [min start, max end] */
	total_ops = (long)g_ops * nworkers;
	all = calloc((size_t)total_ops, sizeof *all);
	for (t = 0; t < nworkers; t++) {
		if (ws[t].start_ns && ws[t].start_ns < t_min) t_min = ws[t].start_ns;
		if (ws[t].end_ns > t_max) t_max = ws[t].end_ns;
		memcpy(all + (size_t)t * g_ops, ws[t].lat, (size_t)g_ops * sizeof(uint32_t));
	}
	qsort(all, (size_t)total_ops, sizeof *all, cmp_u32);
	load_s = (double)(load1 - load0) / 1e9;
	win_s = (t_max > t_min) ? (double)(t_max - t_min) / 1e9 : 0.0;
#define PCT(p) ((double)all[(size_t)((double)(p)/100.0*(double)(total_ops-1))]/1000.0)
	printf("{\"label\":\"%s\",\"engine\":\"xstore-loop\",\"loops\":%d,\"procs\":%d,"
	    "\"workers\":%d,\"rows\":%d,\"row_bytes\":%d,\"cache_kb\":%d,\"read_pct\":%d,"
	    "\"ops_per_worker\":%d,\"total_ops\":%ld,\"load_s\":%.3f,\"window_s\":%.3f,"
	    "\"kops_per_sec\":%.1f,\"p50_us\":%.2f,\"p95_us\":%.2f,\"p99_us\":%.2f,"
	    "\"p999_us\":%.2f,\"max_us\":%.2f}\n",
	    g_label, g_loops, g_procs, nworkers, g_rows, g_row, g_cache_kb, g_read_pct,
	    g_ops, total_ops, load_s, win_s,
	    win_s > 0 ? (double)total_ops / win_s / 1000.0 : 0.0,
	    PCT(50), PCT(95), PCT(99), PCT(99.9), (double)all[total_ops - 1] / 1000.0);
#undef PCT
	unlink(path);
	{ char w[96]; snprintf(w, sizeof w, "%s-wal", path); unlink(w);
	  snprintf(w, sizeof w, "%s.dwb", path); unlink(w); }
	return 0;
}
