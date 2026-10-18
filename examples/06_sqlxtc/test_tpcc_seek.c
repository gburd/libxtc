/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_tpcc_seek.c
 *	Seek, not scan, for the TPROC-C statement shapes: every point
 *	statement the Turso / HammerDB drivers issue must visit O(1) base
 *	rows (xstore_rows_visited), not O(table), when driven with BOUND ?
 *	parameters.  A table of N_ROWS rows is far larger than any point
 *	statement should touch; each case asserts the rows the statement
 *	visited stay under a small bound and that it returned the right
 *	answer.  No daemon; off a loop (synchronous bufmgr I/O).
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

#define N_W    4        /* warehouses (rowid-PK table) */
#define N_D    10       /* districts per warehouse */
#define N_C    60       /* customers per district */
#define SMALL  40       /* "O(1)" bound on rows a point statement may visit */

static int g_fail;
#ifdef CK
#undef CK
#endif
#define CK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	g_fail = 1; } } while (0)

/* Run `sql` with integer binds p[0..np-1] (and an optional text bind at
 * position tpos, 0 = none); return the row count (or -1), first column
 * of the first row in *first, rows visited in *visited. */
static int
run(sx_db *db, const char *sql, const int64_t *p, int np, int tpos,
    const char *txt, int64_t *first, uint64_t *visited)
{
	sx_stmt *st = NULL;
	uint64_t v0;
	int i, n = 0, rc;
	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK) return -1;
	for (i = 0; i < np; i++)
		if (i + 1 == tpos) sx_bind_text(st, i + 1, txt, -1);
		else sx_bind_int64(st, i + 1, p[i]);
	v0 = xstore_rows_visited();
	while ((rc = sx_step(st)) == SX_ROW) {
		if (n == 0 && first) *first = sx_column_int64(st, 0);
		n++;
	}
	if (visited) *visited = xstore_rows_visited() - v0;
	sx_finalize(st);
	return rc == SX_DONE ? n : -1;
}

static void
expect(sx_db *db, const char *what, const char *sql, const int64_t *p, int np,
    int tpos, const char *txt, int want_rows, int64_t want_first)
{
	int64_t first = -1;
	uint64_t vis = 0;
	int n = run(db, sql, p, np, tpos, txt, &first, &vis);
	printf("  %-34s rows=%d visited=%llu\n", what, n, (unsigned long long)vis);
	CK(n == want_rows);
	if (want_rows > 0 && want_first != INT64_MIN) CK(first == want_first);
	CK(vis <= SMALL);
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
	int64_t p[8];
	int fd, w, d, c;

	t_tmpl(path, sizeof path, "sqlxtc-tpccseek");
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = 4096; bo.n_frames = 512; bo.cool_pct = 25;
	if (bm_create(&bo, &bm) != XTC_OK || bt_open(bm, &bt) != XTC_OK ||
	    sx_open_bt(bt, &db) != SX_OK) return 1;

	CK(sx_exec(db, "CREATE TABLE warehouse (w_id integer not null, "
	    "w_name text, w_tax real, w_ytd real, primary key (w_id))", NULL) == SX_OK);
	CK(sx_exec(db, "CREATE TABLE item (i_id INTEGER PRIMARY KEY, i_price REAL, "
	    "i_name TEXT)", NULL) == SX_OK);
	CK(sx_exec(db, "BEGIN", NULL) == SX_OK);
	CK(sx_prepare(db, "INSERT INTO warehouse VALUES(?,?,?,?)", -1, &st, NULL) == SX_OK);
	for (w = 1; w <= N_W * 100; w++) {
		sx_bind_int64(st, 1, w); sx_bind_text(st, 2, "wn", -1);
		sx_bind_double(st, 3, 0.1); sx_bind_double(st, 4, 300000.0);
		CK(sx_step(st) == SX_DONE); sx_reset(st);
	}
	sx_finalize(st);
	CK(sx_prepare(db, "INSERT INTO item VALUES(?,?,?)", -1, &st, NULL) == SX_OK);
	for (c = 1; c <= 3000; c++) {
		sx_bind_int64(st, 1, c); sx_bind_double(st, 2, 1.5);
		sx_bind_text(st, 3, "it", -1);
		CK(sx_step(st) == SX_DONE); sx_reset(st);
	}
	sx_finalize(st);
	CK(sx_exec(db, "COMMIT", NULL) == SX_OK);
	(void)d;

	/* Rowid-PK point reads/writes driven by a bound ? (Turso stmts 5, 9,
	 * 10): the read planner's PK bound must honour the bind. */
	p[0] = 77;
	expect(db, "item by i_id=? (stmt 5)",
	    "SELECT i_price, i_name FROM item WHERE i_id = ?", p, 1, 0, NULL, 1, INT64_MIN);
	p[0] = 3001;
	expect(db, "item miss", "SELECT i_name FROM item WHERE i_id = ?", p, 1, 0,
	    NULL, 0, 0);
	{
		uint64_t v0 = xstore_rows_visited();
		sx_stmt *q = NULL;
		CK(sx_prepare(db, "UPDATE item SET i_price = i_price + ? WHERE i_id = ?",
		    -1, &st, NULL) == SX_OK);
		sx_bind_double(st, 1, 12.5); sx_bind_int64(st, 2, 123);
		CK(sx_step(st) == SX_DONE);
		sx_finalize(st);
		printf("  %-34s visited=%llu\n", "UPDATE x = x + ? WHERE pk=? (stmt 9)",
		    (unsigned long long)(xstore_rows_visited() - v0));
		CK(xstore_rows_visited() - v0 <= SMALL);
		CK(sx_prepare(db, "SELECT i_price FROM item WHERE i_id = ?", -1, &q,
		    NULL) == SX_OK);
		sx_bind_int64(q, 1, 123);
		CK(sx_step(q) == SX_ROW);
		CK(sx_column_double(q, 0) == 14.0);
		sx_finalize(q);
	}

	sx_close(db);
	bt_close(bt); bm_destroy(bm);
	unlink(path);
	{ char wal[300]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
	if (g_fail) return 1;
	printf("test_tpcc_seek: OK\n");
	return 0;
}
