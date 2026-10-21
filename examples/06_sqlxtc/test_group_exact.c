/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_group_exact.c
 *	Grouping and DISTINCT compare two INTEGER keys exactly, as SQLite
 *	does.  The executor's key equality went through double, so integers
 *	above 2^53 that round to the same double (9007199254740992 and
 *	9007199254740993) collapsed into one group / one distinct value.
 *	Integers that equal a REAL (1 and 1.0) still group together.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "bufmgr.h"
#include "btree.h"
#include "xtc.h"
#include "t_tmp.h"

static int64_t
one_int(sx_db *db, const char *sql)
{
	sx_stmt *st = NULL;
	int64_t v = -1;

	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK)
		return -2;
	if (sx_step(st) == SX_ROW)
		v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

static int
nrows(sx_db *db, const char *sql)
{
	sx_stmt *st = NULL;
	int n = 0;

	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK)
		return -1;
	while (sx_step(st) == SX_ROW)
		n++;
	sx_finalize(st);
	return n;
}

int
main(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	sx_db *db = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256], wal[288];
	int fd;

	t_tmpl(path, sizeof path, "sqlxtc-grpx");
	fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); return 1; }
	close(fd);
	bo.path = path; bo.page_size = 4096; bo.n_frames = 64; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(sx_open_bt(bt, &db) == SX_OK);
	CK(sx_exec(db, "CREATE TABLE g(k INTEGER PRIMARY KEY, x)", NULL) == SX_OK);
	CK(sx_exec(db, "INSERT INTO g(k,x) VALUES(1,9007199254740992),"
	    "(2,9007199254740993),(3,9007199254740993),(4,1),(5,1.0),(6,2)",
	    NULL) == SX_OK);

	/* 2^53, 2^53+1, and {1, 1.0} and 2: four groups (SQLite: 4). */
	CK(nrows(db, "SELECT x, count(*) FROM g GROUP BY x") == 4);
	CK(one_int(db, "SELECT count(*) FROM g WHERE x = 9007199254740993") == 2);
	CK(nrows(db, "SELECT x FROM g GROUP BY x HAVING count(*) = 2") == 2);
	                                       /* 2^53+1 twice; 1 and 1.0 */
	CK(nrows(db, "SELECT DISTINCT x FROM g") == 4);
	CK(one_int(db, "SELECT count(DISTINCT x) FROM g") == 4);

	sx_close(db);
	bt_close(bt);
	bm_destroy(bm);
	unlink(path);
	snprintf(wal, sizeof wal, "%s-wal", path);
	unlink(wal);
	if (g_fail) {
		fprintf(stderr, "test_group_exact: FAILED\n");
		return 1;
	}
	printf("  ok   GROUP BY / DISTINCT / count(DISTINCT) keep 2^53 and "
	    "2^53+1 apart and 1 with 1.0\ntest_group_exact: ALL PASS\n");
	return 0;
}
