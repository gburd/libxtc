/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_plan_cache.c
 *	A prepared statement is parsed and planned once (sqlite-audit gap
 *	#7): re-executing it after sx_reset with new binds runs 0 parses
 *	(sql_parse_count) and 0 plan builds (vx_plan_build_count), and
 *	returns the right rows for each bind.  A bind of another storage
 *	class re-plans once (the affinity gate depends on it); DDL bumps
 *	the catalog generation and re-plans (a new index is then used).
 *	Also covers the write path (no re-parse) and a statement with an
 *	uncorrelated subquery (re-planned from the cached AST, no parse).
 *	No daemon; off a loop.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "bufmgr.h"
#include "btree.h"
#include "sql_parse.h"
#include "vexec.h"
#include "xstore.h"
#include "xtc.h"
#include "t_tmp.h"

static int g_fail;
#ifdef CK
#undef CK
#endif
#define CK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	g_fail = 1; } } while (0)

/* Step st to the end; the row count, first column of row 0 in *first. */
static int
drain(sx_stmt *st, int64_t *first)
{
	int n = 0, rc;
	while ((rc = sx_step(st)) == SX_ROW) {
		if (n == 0 && first) *first = sx_column_int64(st, 0);
		n++;
	}
	sx_reset(st);
	return rc == SX_DONE ? n : -1;
}

int
main(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	sx_db *db = NULL;
	sx_stmt *q = NULL, *w = NULL, *sq = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256];
	unsigned long long p0;
	uint64_t b0, v0, l0;
	int64_t first;
	char wal[300];
	int i, n, fd;

	t_tmpl(path, sizeof path, "sqlxtc-pcache");
	fd = mkstemp(path);
	if (fd < 0) return 1;
	close(fd);
	bo.path = path; bo.page_size = 4096; bo.n_frames = 512; bo.cool_pct = 25;
	if (bm_create(&bo, &bm) != XTC_OK || bt_open(bm, &bt) != XTC_OK ||
	    sx_open_bt(bt, &db) != SX_OK) return 1;
	CK(sx_exec(db, "CREATE TABLE t(a INTEGER, b INTEGER, c TEXT, "
	    "PRIMARY KEY(a, b))", NULL) == SX_OK);
	CK(sx_exec(db, "BEGIN", NULL) == SX_OK);
	CK(sx_prepare(db, "INSERT INTO t VALUES(?,?,?)", -1, &w, NULL) == SX_OK);
	for (i = 0; i < 400; i++) {
		char c[16];   /* loop-body scope (allowed) */
		snprintf(c, sizeof c, "c%d", i % 13);
		sx_bind_int64(w, 1, i / 20); sx_bind_int64(w, 2, i % 20);
		sx_bind_text(w, 3, c, -1);
		CK(sx_step(w) == SX_DONE); sx_reset(w);
	}
	CK(sx_exec(db, "COMMIT", NULL) == SX_OK);

	/* A point SELECT re-executed with new INT binds: one plan, no parse. */
	CK(sx_prepare(db, "SELECT b FROM t WHERE a = ? AND b = ?", -1, &q,
	    NULL) == SX_OK);
	sx_bind_int64(q, 1, 3); sx_bind_int64(q, 2, 4);
	CK(drain(q, &first) == 1 && first == 4);
	p0 = sql_parse_count(); b0 = vx_plan_build_count();
	for (i = 0; i < 100; i++) {
		first = -1;
		sx_bind_int64(q, 1, i % 20); sx_bind_int64(q, 2, i % 17);
		CK(drain(q, &first) == 1 && first == i % 17);
	}
	/* A warm statement takes no global catalog lock (the connection's
	 * bt/ctx and the table/index lookups are thread-local copies). */
	l0 = xstore_cat_lock_count();
	sx_bind_int64(q, 1, 1); sx_bind_int64(q, 2, 1);
	CK(drain(q, NULL) == 1);
	printf("  warm point SELECT: catalog-lock acquisitions %llu\n",
	    (unsigned long long)(xstore_cat_lock_count() - l0));
	CK(xstore_cat_lock_count() - l0 == 0);
	printf("  100 re-executions: parses %llu, plan builds %llu\n",
	    sql_parse_count() - p0,
	    (unsigned long long)(vx_plan_build_count() - b0));
	CK(sql_parse_count() - p0 == 0);
	CK(vx_plan_build_count() - b0 == 0);
	/* Still a seek on the reused plan: a point read visits ~1 row. */
	v0 = xstore_rows_visited();
	sx_bind_int64(q, 1, 7); sx_bind_int64(q, 2, 7);
	CK(drain(q, NULL) == 1);
	CK(xstore_rows_visited() - v0 <= 2);

	/* A TEXT bind on an INTEGER column: SQLite applies numeric affinity
	 * ('5' matches 5, 'x' matches nothing).  The bind class changed: one
	 * re-plan, then cached again. */
	b0 = vx_plan_build_count();
	sx_bind_text(q, 1, "5", -1); sx_bind_int64(q, 2, 6);
	CK(drain(q, &first) == 1 && first == 6);
	sx_bind_text(q, 1, "x", -1); sx_bind_int64(q, 2, 6);
	CK(drain(q, NULL) == 0);
	sx_bind_text(q, 1, " 9", -1); sx_bind_int64(q, 2, 1);
	CK(drain(q, &first) == 1 && first == 1);
	CK(vx_plan_build_count() - b0 == 1);
	sx_bind_int64(q, 1, 9); sx_bind_int64(q, 2, 2);   /* back to INT */
	CK(drain(q, &first) == 1 && first == 2);
	CK(vx_plan_build_count() - b0 == 2);
	sx_bind_null(q, 1); sx_bind_int64(q, 2, 2);         /* NULL: no row */
	CK(drain(q, NULL) == 0);

	/* DDL invalidates: a new index on c is used by the re-built plan. */
	sx_finalize(q); q = NULL;
	CK(sx_prepare(db, "SELECT count(*) FROM t WHERE c = ?", -1, &q,
	    NULL) == SX_OK);
	sx_bind_text(q, 1, "c3", -1);
	CK(drain(q, &first) == 1 && first == 31);
	b0 = vx_plan_build_count();
	v0 = xstore_rows_visited();
	CK(drain(q, &first) == 1 && first == 31);
	n = (int)(xstore_rows_visited() - v0);
	CK(vx_plan_build_count() - b0 == 0);
	CK(n >= 400);                                       /* a scan */
	CK(sx_exec(db, "CREATE INDEX t_c ON t(c)", NULL) == SX_OK);
	v0 = xstore_rows_visited();
	CK(drain(q, &first) == 1 && first == 31);
	n = (int)(xstore_rows_visited() - v0);
	printf("  after CREATE INDEX: plan builds %llu, rows visited %d\n",
	    (unsigned long long)(vx_plan_build_count() - b0), n);
	CK(vx_plan_build_count() - b0 == 1);
	CK(n <= 31);                                        /* the index */

	/* Writes run from the cached AST: no parse per execution. */
	p0 = sql_parse_count();
	CK(sx_prepare(db, "UPDATE t SET c = ? WHERE a = ? AND b = ?", -1, &w,
	    NULL) == SX_OK);
	for (i = 0; i < 20; i++) {
		sx_bind_text(w, 1, "u", -1);
		sx_bind_int64(w, 2, i); sx_bind_int64(w, 3, 0);
		CK(sx_step(w) == SX_DONE); sx_reset(w);
	}
	printf("  20 UPDATEs: parses %llu (prepare included)\n",
	    sql_parse_count() - p0);
	CK(sql_parse_count() - p0 <= 2);   /* sx_prepare classify + first run */
	sx_bind_text(q, 1, "u", -1);
	CK(drain(q, &first) == 1 && first == 20);

	/* An uncorrelated subquery: re-planned from the cached AST (its
	 * value is spliced in), never re-parsed. */
	CK(sx_prepare(db, "SELECT b FROM t WHERE a = ? AND b = (SELECT max(b) "
	    "FROM t WHERE a = ? AND b < ?)", -1, &sq, NULL) == SX_OK);
	sx_bind_int64(sq, 1, 2); sx_bind_int64(sq, 2, 2); sx_bind_int64(sq, 3, 9);
	CK(drain(sq, &first) == 1 && first == 8);
	p0 = sql_parse_count();
	for (i = 1; i < 15; i++) {
		sx_bind_int64(sq, 1, 4); sx_bind_int64(sq, 2, 4);
		sx_bind_int64(sq, 3, i + 1);
		CK(drain(sq, &first) == 1 && first == i);
	}
	printf("  14 subquery re-executions: parses %llu\n",
	    sql_parse_count() - p0);
	CK(sql_parse_count() - p0 == 0);

	sx_finalize(q); sx_finalize(w); sx_finalize(sq);
	sx_close(db);
	bt_close(bt); bm_destroy(bm);
	unlink(path);
	snprintf(wal, sizeof wal, "%s-wal", path);
	unlink(wal);
	if (g_fail) return 1;
	printf("test_plan_cache: OK\n");
	return 0;
}
