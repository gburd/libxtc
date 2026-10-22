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
 *	Integers that equal a REAL (1 and 1.0) still group together, and
 *	0.0 with -0.0 and 0.
 *
 *	Cost: the hash tables behind GROUP BY, DISTINCT, count(DISTINCT)
 *	and the hash join must spread integer keys.  The numeric cell hash
 *	was a bare multiply of the key's double bits, whose low bits are
 *	zero for every small integer, so all keys shared one bucket and a
 *	20,000-key count(DISTINCT) took ~440 ms (quadratic).  Checked by
 *	time: 40,000 distinct keys through count(DISTINCT), GROUP BY and a
 *	join must finish in well under a second each.
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

#define N_SPREAD 40000

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
	sx_stmt *st = NULL, *st2 = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256], wal[288];
	int64_t t0, ms[3];
	int fd, i;

	t_tmpl(path, sizeof path, "sqlxtc-grpx");
	fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); return 1; }
	close(fd);
	bo.path = path; bo.page_size = 4096; bo.n_frames = 4096; bo.cool_pct = 25;
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
	CK(sx_exec(db, "INSERT INTO g(k,x) VALUES(7,0),(8,0.0),(9,-0.0)", NULL)
	    == SX_OK);
	CK(one_int(db, "SELECT count(DISTINCT x) FROM g") == 5);   /* + one zero */
	CK(nrows(db, "SELECT x FROM g GROUP BY x") == 5);

	/* Spread: N_SPREAD distinct integer keys. */
	CK(sx_exec(db, "CREATE TABLE s(k INTEGER PRIMARY KEY, x INTEGER)", NULL)
	    == SX_OK);
	CK(sx_exec(db, "CREATE TABLE s2(k INTEGER PRIMARY KEY, x INTEGER)", NULL)
	    == SX_OK);
	CK(sx_exec(db, "BEGIN", NULL) == SX_OK);
	CK(sx_prepare(db, "INSERT INTO s(k,x) VALUES(?,?)", -1, &st, NULL) == SX_OK);
	CK(sx_prepare(db, "INSERT INTO s2(k,x) VALUES(?,?)", -1, &st2, NULL) == SX_OK);
	for (i = 1; i <= N_SPREAD; i++) {
		sx_bind_int64(st, 1, i); sx_bind_int64(st, 2, i);
		CK(sx_step(st) == SX_DONE); sx_reset(st);
		sx_bind_int64(st2, 1, i); sx_bind_int64(st2, 2, N_SPREAD + 1 - i);
		CK(sx_step(st2) == SX_DONE); sx_reset(st2);
	}
	sx_finalize(st); sx_finalize(st2);
	CK(sx_exec(db, "COMMIT", NULL) == SX_OK);
	t0 = xtc_clock_mono();
	CK(one_int(db, "SELECT count(DISTINCT x) FROM s") == N_SPREAD);
	ms[0] = (xtc_clock_mono() - t0) / 1000000;
	t0 = xtc_clock_mono();
	CK(nrows(db, "SELECT x, count(*) FROM s GROUP BY x") == N_SPREAD);
	ms[1] = (xtc_clock_mono() - t0) / 1000000;
	t0 = xtc_clock_mono();
	CK(nrows(db, "SELECT s.k FROM s JOIN s2 ON s.x = s2.x") == N_SPREAD);
	ms[2] = (xtc_clock_mono() - t0) / 1000000;
	/* Spread keys: ~20-40 ms each here; one bucket: 0.8-10 s. */
	CK(ms[0] < 500 && ms[1] < 500 && ms[2] < 500);
	printf("  %s   %d distinct integer keys: count(DISTINCT) %lld ms, GROUP BY "
	    "%lld ms, join %lld ms (bound 500 ms each)\n",
	    ms[0] < 500 && ms[1] < 500 && ms[2] < 500 ? "ok" : "FAIL", N_SPREAD,
	    (long long)ms[0], (long long)ms[1], (long long)ms[2]);

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
	    "2^53+1 apart, 1 with 1.0, 0 with 0.0 and -0.0\n"
	    "test_group_exact: ALL PASS\n");
	return 0;
}
