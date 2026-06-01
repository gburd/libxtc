/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * bench/sqlxtc/engine_ab.c
 *	Larger-than-RAM A/B: the SAME SQL workload through the SAME VDBE,
 *	differing only in the storage engine underneath:
 *
 *	  --engine xstore : the libxtc-native engine (btree.c over the
 *	                    cooling buffer pool bufmgr.c), MVCC, reached
 *	                    via the xstore virtual table.
 *	  --engine sqlite : SQLite's own btree + pager, a file-backed
 *	                    rowid table.
 *
 *	Both are given an EQUAL cache budget (--cache-kb) and a working
 *	set sized far larger than it (--rows x ~row), so both page to
 *	disk.  The harness loads the rows, then runs a read/write mix of
 *	random point operations, timing each, and prints one JSON line
 *	(throughput + p50/p95/p99/p999/max).  Single-threaded embedded --
 *	the fair baseline that isolates the storage engine.
 *
 *	Honest caveats (see README.md): xstore is MULTI-version (every
 *	update writes a new version; GC is not yet wired into the SQL
 *	path), so write-heavy mixes accumulate versions and cost more
 *	than SQLite's update-in-place -- read-heavy is the fair storage
 *	read-path comparison.  The xstore scan re-descends per row
 *	(latch released across the vtable boundary), so scans favor
 *	SQLite; this measures POINT operations.
 *
 * usage: engine_ab --engine xstore|sqlite [--rows N] [--cache-kb K]
 *                  [--ops O] [--read-pct P] [--row-bytes B] [--label TAG]
 */

#include <inttypes.h>
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

static int      g_rows     = 200000;
static int      g_cache_kb = 4096;     /* 4 MB cache, both engines */
static int      g_ops      = 200000;
static int      g_read_pct = 95;
static int      g_row      = 200;
static const char *g_engine = "xstore";
static const char *g_label  = "ab";

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

int
main(int argc, char **argv)
{
	xsql *db = NULL;
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	xsql_stmt *ins = NULL, *sel = NULL, *upd = NULL;
	char dbpath[] = "/tmp/sqlxtc-ab-XXXXXX";
	char btpath[] = "/tmp/sqlxtc-ab-bt-XXXXXX";
	char *blob;
	uint32_t *lat;
	uint64_t rng = 0x1234, t0, t1, load0, load1;
	double load_s, run_s;
	int i, fd, is_xstore;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--engine") && i + 1 < argc) g_engine = argv[++i];
		else if (!strcmp(argv[i], "--rows") && i + 1 < argc) g_rows = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--cache-kb") && i + 1 < argc) g_cache_kb = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--ops") && i + 1 < argc) g_ops = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--read-pct") && i + 1 < argc) g_read_pct = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--row-bytes") && i + 1 < argc) g_row = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--label") && i + 1 < argc) g_label = argv[++i];
	}
	is_xstore = !strcmp(g_engine, "xstore");
	blob = malloc((size_t)g_row);
	memset(blob, 'x', (size_t)g_row);
	lat = calloc((size_t)g_ops, sizeof *lat);
	if (blob == NULL || lat == NULL) { fprintf(stderr, "oom\n"); return 1; }

	/* In-memory schema + (for xstore) our own on-disk B-tree, OR a
	 * file-backed SQLite DB whose page cache equals our pool budget. */
	if (is_xstore) {
		bm_opts_t bo = BM_OPTS_DEFAULT;
		fd = mkstemp(btpath); if (fd < 0) return 1; close(fd);
		bo.path = btpath; bo.page_size = 4096;
		bo.n_frames = (uint32_t)((g_cache_kb * 1024) / 4096);
		if (bo.n_frames < 4) bo.n_frames = 4;
		bo.cool_pct = 20;
		if (bm_create(&bo, &bm) != XTC_OK || bt_open(bm, &bt) != XTC_OK) {
			fprintf(stderr, "bm/bt\n"); return 1;
		}
		if (xsql_open(":memory:", &db) != SQLITE_OK) return 1;
		xstore_register(db, bt);
		xsql_exec(db, "CREATE VIRTUAL TABLE t USING xstore;", 0, 0, 0);
	} else {
		char pragma[64];
		fd = mkstemp(dbpath); if (fd < 0) return 1; close(fd);
		if (xsql_open(dbpath, &db) != SQLITE_OK) return 1;
		xsql_exec(db, "PRAGMA journal_mode=WAL;", 0, 0, 0);
		xsql_exec(db, "PRAGMA synchronous=NORMAL;", 0, 0, 0);
		snprintf(pragma, sizeof pragma, "PRAGMA cache_size=-%d;", g_cache_kb);
		xsql_exec(db, pragma, 0, 0, 0);     /* negative = KB, == our pool */
		xsql_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v BLOB);", 0, 0, 0);
	}

	/* ---- load ---- */
	xsql_prepare_v2(db, "INSERT INTO t(k,v) VALUES(?,?)", -1, &ins, 0);
	load0 = now_ns();
	xsql_exec(db, "BEGIN;", 0, 0, 0);
	for (i = 1; i <= g_rows; i++) {
		xsql_bind_int64(ins, 1, i);
		xsql_bind_blob(ins, 2, blob, g_row, NULL /* SQLITE_STATIC */);
		xsql_step(ins);
		xsql_reset(ins);
		if ((i % 5000) == 0) { xsql_exec(db, "COMMIT;", 0, 0, 0); xsql_exec(db, "BEGIN;", 0, 0, 0); }
	}
	xsql_exec(db, "COMMIT;", 0, 0, 0);
	xsql_finalize(ins);
	load1 = now_ns();

	/* ---- run: random point read/write mix ---- */
	xsql_prepare_v2(db, "SELECT v FROM t WHERE k=?", -1, &sel, 0);
	xsql_prepare_v2(db, "UPDATE t SET v=? WHERE k=?", -1, &upd, 0);
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
			xsql_bind_blob(upd, 1, blob, g_row, NULL);
			xsql_bind_int64(upd, 2, k);
			xsql_step(upd);
			xsql_reset(upd);
		}
		b = now_ns();
		lat[i] = (b - a) > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)(b - a);
	}
	t1 = now_ns();
	xsql_finalize(sel); xsql_finalize(upd);
	xsql_close(db);
	if (is_xstore) { bt_close(bt); bm_destroy(bm); unlink(btpath);
		{ char w[80]; snprintf(w, sizeof w, "%s-wal", btpath); unlink(w); } }
	else { unlink(dbpath);
		{ char w[96]; snprintf(w, sizeof w, "%s-wal", dbpath); unlink(w);
		  snprintf(w, sizeof w, "%s-shm", dbpath); unlink(w); } }

	load_s = (double)(load1 - load0) / 1e9;
	run_s = (double)(t1 - t0) / 1e9;
	qsort(lat, (size_t)g_ops, sizeof *lat, cmp_u32);
#define PCT(p) ((double)lat[(size_t)((double)(p)/100.0*(double)(g_ops-1))]/1000.0)
	printf("{\"label\":\"%s\",\"engine\":\"%s\",\"rows\":%d,\"row_bytes\":%d,"
	    "\"cache_kb\":%d,\"working_set_kb\":%lld,\"read_pct\":%d,\"ops\":%d,"
	    "\"load_s\":%.3f,\"load_kops\":%.1f,\"run_s\":%.3f,\"kops_per_sec\":%.1f,"
	    "\"p50_us\":%.2f,\"p95_us\":%.2f,\"p99_us\":%.2f,\"p999_us\":%.2f,"
	    "\"max_us\":%.2f}\n",
	    g_label, g_engine, g_rows, g_row, g_cache_kb,
	    (long long)((int64_t)g_rows * g_row / 1024), g_read_pct, g_ops,
	    load_s, (double)g_rows / load_s / 1000.0, run_s,
	    (double)g_ops / run_s / 1000.0, PCT(50), PCT(95), PCT(99), PCT(99.9),
	    (double)lat[g_ops - 1] / 1000.0);
#undef PCT
	free(blob); free(lat);
	return 0;
}
