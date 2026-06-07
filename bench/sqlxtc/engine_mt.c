/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * bench/sqlxtc/engine_mt.c
 *	Multi-threaded A/B: the same random point-operation SQL workload
 *	run by T OS threads against the SAME storage, differing only in
 *	the engine underneath:
 *
 *	  --engine xstore : the libxtc-native engine (btree.c over the
 *	                    cooling buffer pool bufmgr.c), MVCC, reached
 *	                    via the xstore virtual table.  One shared bt;
 *	                    each thread drives it through its own in-memory
 *	                    SQLite connection.  The buffer-manager latch
 *	                    (xtc_arwlock) serves off-loop waiters on a
 *	                    condvar, so plain OS threads exercise the
 *	                    engine's shared-structure concurrency.
 *	  --engine sqlite : SQLite's own btree + pager, WAL mode; each
 *	                    thread opens its own connection to the shared
 *	                    file with a busy timeout.
 *
 *	Both get an EQUAL cache budget and a working set sized larger than
 *	it.  The DB is loaded once, then T threads start together (barrier)
 *	and each runs OPS random point ops; we report aggregate throughput
 *	and merged p50/p95/p99/p99.9/max as one JSON line.
 *
 *	HONEST SCOPE: this measures the OFF-LOOP path -- concurrent MVCC
 *	reads (which scale) and serialized synchronous commits (writes on
 *	both engines fsync; xstore off-loop has no group-commit writer
 *	proc, SQLite WAL has a single writer).  It does NOT measure
 *	sqlxtc's loop-based group commit / fiber scheduling (its design
 *	peak for writes and networked load); mvcc_bench and the networked
 *	server measure that.  Read-heavy mixes are the fair shared-engine
 *	concurrency signal.
 *
 * usage: engine_mt --engine xstore|sqlite --threads T [--rows N]
 *        [--cache-kb K] [--ops O] [--read-pct P] [--row-bytes B]
 *        [--label TAG]
 */

#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sqlite3.h"          /* renamed to xsql_* by xsql.h (force-included) */
#include "bufmgr.h"
#include "btree.h"
#include "xstore.h"

static int      g_rows     = 1000000;
static int      g_cache_kb = 65536;
static int      g_ops      = 200000;   /* per thread */
static int      g_read_pct = 95;
static int      g_row      = 200;
static int      g_threads  = 1;
static const char *g_engine = "xstore";
static const char *g_label  = "mt";

static int      g_is_xstore;
static bt_t    *g_bt;                  /* shared (xstore) */
static char     g_dbpath[64];          /* shared file (sqlite) */
static char     g_blob[4096];

static pthread_barrier_t g_start;

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

struct worker {
	int        id;
	uint32_t  *lat;     /* g_ops latencies (ns, clamped) */
	uint64_t   run_ns;  /* this thread's run wall time */
};

/* Open a connection bound to the shared storage. */
static xsql *
open_conn(void)
{
	xsql *db = NULL;
	if (g_is_xstore) {
		if (xsql_open(":memory:", &db) != SQLITE_OK) return NULL;
		xstore_register(db, g_bt);
		xsql_exec(db, "CREATE VIRTUAL TABLE t USING xstore;", 0, 0, 0);
	} else {
		char pragma[64];
		int per = g_cache_kb / (g_threads > 0 ? g_threads : 1);
		if (per < 256) per = 256;          /* fair: total cache ~ xstore's shared pool */
		if (xsql_open(g_dbpath, &db) != SQLITE_OK) return NULL;
		xsql_exec(db, "PRAGMA journal_mode=WAL;", 0, 0, 0);
		xsql_exec(db, "PRAGMA synchronous=NORMAL;", 0, 0, 0);
		xsql_exec(db, "PRAGMA busy_timeout=10000;", 0, 0, 0);
		snprintf(pragma, sizeof pragma, "PRAGMA cache_size=-%d;", per);
		xsql_exec(db, pragma, 0, 0, 0);
	}
	return db;
}

static void *
worker_main(void *arg)
{
	struct worker *w = arg;
	xsql *db = open_conn();
	xsql_stmt *sel = NULL, *upd = NULL;
	uint64_t rng = 0x9e3779b97f4a7c15ull ^ ((uint64_t)w->id << 32);
	uint64_t t0, t1;
	int i;

	if (db == NULL) { fprintf(stderr, "conn %d failed\n", w->id); return NULL; }
	xsql_prepare_v2(db, "SELECT v FROM t WHERE k=?", -1, &sel, 0);
	xsql_prepare_v2(db, "UPDATE t SET v=? WHERE k=?", -1, &upd, 0);

	pthread_barrier_wait(&g_start);
	t0 = now_ns();
	for (i = 0; i < g_ops; i++) {
		uint32_t k;
		int is_read;
		uint64_t a, b;
		rng = rng * 6364136223846793005ull + 1442695040888963407ull;
		k = (uint32_t)((rng >> 33) % (uint32_t)g_rows) + 1;
		is_read = ((int)((rng >> 17) % 100) < g_read_pct);
		a = now_ns();
		if (is_read) {
			xsql_bind_int64(sel, 1, k);
			while (xsql_step(sel) == SQLITE_ROW) { /* drain */ }
			xsql_reset(sel);
		} else {
			xsql_bind_blob(upd, 1, g_blob, g_row, NULL);
			xsql_bind_int64(upd, 2, k);
			(void)xsql_step(upd);     /* SQLITE_BUSY tolerated under WAL */
			xsql_reset(upd);
		}
		b = now_ns();
		w->lat[i] = (b - a) > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)(b - a);
	}
	t1 = now_ns();
	w->run_ns = t1 - t0;
	xsql_finalize(sel); xsql_finalize(upd);
	xsql_close(db);
	return NULL;
}

int
main(int argc, char **argv)
{
	bm_t *bm = NULL;
	xsql *db = NULL;
	xsql_stmt *ins = NULL;
	char btpath[] = "/tmp/sqlxtc-mt-bt-XXXXXX";
	char dbtmpl[] = "/tmp/sqlxtc-mt-XXXXXX";
	struct worker *ws;
	pthread_t *th;
	uint32_t *all;
	uint64_t load0, load1, wall0, wall1, maxrun = 0;
	double load_s, wall_s;
	long total_ops;
	int i, t, fd;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--engine") && i + 1 < argc) g_engine = argv[++i];
		else if (!strcmp(argv[i], "--threads") && i + 1 < argc) g_threads = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--rows") && i + 1 < argc) g_rows = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--cache-kb") && i + 1 < argc) g_cache_kb = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--ops") && i + 1 < argc) g_ops = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--read-pct") && i + 1 < argc) g_read_pct = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--row-bytes") && i + 1 < argc) g_row = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--label") && i + 1 < argc) g_label = argv[++i];
	}
	if (g_threads < 1) g_threads = 1;
	if (g_row > (int)sizeof g_blob) g_row = sizeof g_blob;
	g_is_xstore = !strcmp(g_engine, "xstore");
	memset(g_blob, 'x', sizeof g_blob);

	/* ---- shared storage + load ---- */
	if (g_is_xstore) {
		bm_opts_t bo = BM_OPTS_DEFAULT;
		fd = mkstemp(btpath); if (fd < 0) return 1; close(fd);
		bo.path = btpath; bo.page_size = 4096;
		bo.n_frames = (uint32_t)((g_cache_kb * 1024) / 4096);
		if (bo.n_frames < 16) bo.n_frames = 16;
		bo.cool_pct = 20;
		if (bm_create(&bo, &bm) != XTC_OK || bt_open(bm, &g_bt) != XTC_OK) return 1;
		if (xsql_open(":memory:", &db) != SQLITE_OK) return 1;
		xstore_register(db, g_bt);
		xsql_exec(db, "CREATE VIRTUAL TABLE t USING xstore;", 0, 0, 0);
	} else {
		char pragma[64];
		fd = mkstemp(dbtmpl); if (fd < 0) return 1; close(fd);
		snprintf(g_dbpath, sizeof g_dbpath, "%s", dbtmpl);
		if (xsql_open(g_dbpath, &db) != SQLITE_OK) return 1;
		xsql_exec(db, "PRAGMA journal_mode=WAL;", 0, 0, 0);
		xsql_exec(db, "PRAGMA synchronous=NORMAL;", 0, 0, 0);
		snprintf(pragma, sizeof pragma, "PRAGMA cache_size=-%d;", g_cache_kb);
		xsql_exec(db, pragma, 0, 0, 0);
		xsql_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v BLOB);", 0, 0, 0);
	}
	xsql_prepare_v2(db, "INSERT INTO t(k,v) VALUES(?,?)", -1, &ins, 0);
	load0 = now_ns();
	xsql_exec(db, "BEGIN;", 0, 0, 0);
	for (i = 1; i <= g_rows; i++) {
		xsql_bind_int64(ins, 1, i);
		xsql_bind_blob(ins, 2, g_blob, g_row, NULL);
		xsql_step(ins); xsql_reset(ins);
		if ((i % 5000) == 0) { xsql_exec(db, "COMMIT;", 0, 0, 0); xsql_exec(db, "BEGIN;", 0, 0, 0); }
	}
	xsql_exec(db, "COMMIT;", 0, 0, 0);
	xsql_finalize(ins);
	load1 = now_ns();
	xsql_close(db);                    /* loader connection; workers open their own */

	/* ---- run ---- */
	ws = calloc((size_t)g_threads, sizeof *ws);
	th = calloc((size_t)g_threads, sizeof *th);
	pthread_barrier_init(&g_start, NULL, (unsigned)g_threads);
	for (t = 0; t < g_threads; t++) {
		ws[t].id = t;
		ws[t].lat = calloc((size_t)g_ops, sizeof(uint32_t));
		pthread_create(&th[t], NULL, worker_main, &ws[t]);
	}
	wall0 = now_ns();
	for (t = 0; t < g_threads; t++) pthread_join(th[t], NULL);
	wall1 = now_ns();
	pthread_barrier_destroy(&g_start);

	for (t = 0; t < g_threads; t++) if (ws[t].run_ns > maxrun) maxrun = ws[t].run_ns;
	total_ops = (long)g_ops * g_threads;
	all = calloc((size_t)total_ops, sizeof *all);
	for (t = 0; t < g_threads; t++)
		memcpy(all + (size_t)t * g_ops, ws[t].lat, (size_t)g_ops * sizeof(uint32_t));
	qsort(all, (size_t)total_ops, sizeof *all, cmp_u32);

	load_s = (double)(load1 - load0) / 1e9;
	wall_s = (double)(wall1 - wall0) / 1e9;     /* spawn..join wall */
#define PCT(p) ((double)all[(size_t)((double)(p)/100.0*(double)(total_ops-1))]/1000.0)
	printf("{\"label\":\"%s\",\"engine\":\"%s\",\"threads\":%d,\"rows\":%d,"
	    "\"row_bytes\":%d,\"cache_kb\":%d,\"working_set_kb\":%lld,\"read_pct\":%d,"
	    "\"ops_per_thread\":%d,\"total_ops\":%ld,\"load_s\":%.3f,"
	    "\"wall_s\":%.3f,\"kops_per_sec\":%.1f,"
	    "\"p50_us\":%.2f,\"p95_us\":%.2f,\"p99_us\":%.2f,\"p999_us\":%.2f,\"max_us\":%.2f}\n",
	    g_label, g_engine, g_threads, g_rows, g_row, g_cache_kb,
	    (long long)((int64_t)g_rows * g_row / 1024), g_read_pct, g_ops, total_ops,
	    load_s, wall_s, (double)total_ops / wall_s / 1000.0,
	    PCT(50), PCT(95), PCT(99), PCT(99.9), (double)all[total_ops - 1] / 1000.0);
#undef PCT

	if (g_is_xstore) { bt_close(g_bt); bm_destroy(bm); unlink(btpath);
		{ char w[80]; snprintf(w, sizeof w, "%s-wal", btpath); unlink(w); } }
	else { unlink(g_dbpath);
		{ char w[96]; snprintf(w, sizeof w, "%s-wal", g_dbpath); unlink(w);
		  snprintf(w, sizeof w, "%s-shm", g_dbpath); unlink(w); } }
	return 0;
}
