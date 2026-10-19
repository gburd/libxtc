/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_bulk_insert.c
 *	An in-transaction bulk INSERT must cost O(1) per row, not O(rows
 *	already buffered).  The Turso loader inserts whole districts and
 *	the stock table in one transaction into implicit-rowid tables
 *	(every composite-PK table is one), and the auto-rowid lookup
 *	(xstore_max_rowid_txn) used to scan the whole write buffer per
 *	INSERT: O(n^2), a W=10 load ran for over 45 minutes.  Asserts the
 *	buffer entries scanned stay O(rows) (deterministic) and that 4x the
 *	rows takes well under 16x the time.  No daemon; off a loop.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "bufmgr.h"
#include "btree.h"
#include "xstore.h"
#include "xtc.h"
#include "t_tmp.h"

#define N_SMALL 20000
#define N_BIG   (4 * N_SMALL)

static int g_fail;
#ifdef CK
#undef CK
#endif
#define CK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	g_fail = 1; } } while (0)

/* Insert n rows into `table` in one transaction; returns the ns taken
 * and the buffer entries scanned in *scanned. */
static int64_t
bulk(sx_db *db, const char *table, int n, uint64_t *scanned)
{
	sx_stmt *st = NULL;
	char sql[128];
	int64_t t0;
	uint64_t s0;
	int i;
	snprintf(sql, sizeof sql, "INSERT INTO %s VALUES(?,?,?,?)", table);
	CK(sx_exec(db, "BEGIN", NULL) == SX_OK);
	CK(sx_prepare(db, sql, -1, &st, NULL) == SX_OK);
	s0 = xstore_wbuf_scan_count();
	t0 = xtc_clock_mono();
	for (i = 0; i < n; i++) {
		sx_bind_int64(st, 1, i % 7);
		sx_bind_int64(st, 2, i);
		sx_bind_int64(st, 3, i * 3);
		sx_bind_text(st, 4, "stock-data-padding-0123456789", -1);
		CK(sx_step(st) == SX_DONE);
		sx_reset(st);
	}
	t0 = xtc_clock_mono() - t0;
	*scanned = xstore_wbuf_scan_count() - s0;
	sx_finalize(st);
	CK(sx_exec(db, "COMMIT", NULL) == SX_OK);
	return t0;
}

int
main(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	sx_db *db = NULL;
	sx_stmt *st = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256];
	uint64_t sc_small, sc_big;
	int64_t t_small, t_big;
	int fd;

	t_tmpl(path, sizeof path, "sqlxtc-bulkins");
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = 4096; bo.n_frames = 4096; bo.cool_pct = 25;
	if (bm_create(&bo, &bm) != XTC_OK || bt_open(bm, &bt) != XTC_OK ||
	    sx_open_bt(bt, &db) != SX_OK) return 1;
	/* Composite PK: an implicit rowid + UNIQUE autoindex, as Turso's. */
	CK(sx_exec(db, "CREATE TABLE s1(a INTEGER, b INTEGER, q INTEGER, d TEXT, "
	    "PRIMARY KEY(a, b))", NULL) == SX_OK);
	CK(sx_exec(db, "CREATE TABLE s2(a INTEGER, b INTEGER, q INTEGER, d TEXT, "
	    "PRIMARY KEY(a, b))", NULL) == SX_OK);

	t_small = bulk(db, "s1", N_SMALL, &sc_small);
	t_big = bulk(db, "s2", N_BIG, &sc_big);
	printf("  %d rows: %.1f ms, buffer entries scanned %llu\n", N_SMALL,
	    (double)t_small / 1e6, (unsigned long long)sc_small);
	printf("  %d rows: %.1f ms, buffer entries scanned %llu\n", N_BIG,
	    (double)t_big / 1e6, (unsigned long long)sc_big);
	/* O(1) per row: no full-buffer rescan per INSERT (was n^2 / 2). */
	CK(sc_small <= (uint64_t)N_SMALL);
	CK(sc_big <= (uint64_t)N_BIG);
	/* 4x the rows: linear is ~4x, quadratic 16x (the scan count above
	 * is the deterministic check; this one catches other per-row O(n)). */
	CK(t_big < 10 * t_small);

	/* The rows are all there with distinct rowids. */
	CK(sx_prepare(db, "SELECT count(*), count(DISTINCT rowid) FROM s2", -1,
	    &st, NULL) == SX_OK);
	CK(sx_step(st) == SX_ROW);
	CK(sx_column_int64(st, 0) == N_BIG);
	CK(sx_column_int64(st, 1) == N_BIG);
	sx_finalize(st);

	sx_close(db);
	bt_close(bt); bm_destroy(bm);
	unlink(path);
	{ char wal[300]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
	if (g_fail) return 1;
	printf("test_bulk_insert: OK\n");
	return 0;
}
