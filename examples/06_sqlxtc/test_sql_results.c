/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_sql_results.c
 *	In-process result oracle for SELECT shapes that returned a WRONG
 *	answer or a spurious error: each case runs through the native sx_*
 *	API and its rows must equal what SQLite returns for the same data
 *	(the expected strings were produced by SQLite 3.46.1; every case
 *	gives the same answer on 3.53.4).
 *
 *	Rows render as "c1|c2|..." (INTEGER %lld, REAL %.15g with ".0" when
 *	integral, TEXT raw, NULL as NULL), sorted, joined by ';'.  "ERR"
 *	means SQLite rejects the statement, so sqlxtc must too.
 *
 *	A case marked OPEN is a known, not yet fixed defect: it is reported
 *	but does not fail the run, and it FAILS the run once it passes (so
 *	the mark is removed with the fix).  --strict treats every case as a
 *	hard check.
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

static const char *setup[] = {
	"CREATE TABLE g(x INT, y INT)",
	"INSERT INTO g VALUES(1,10),(1,11),(2,20),(2,21),(3,30),(NULL,1),"
	"(NULL,2),(4,NULL)",
	"CREATE TABLE a(k INT, v TEXT)",
	"CREATE TABLE b(k INT, w INT)",
	"INSERT INTO a VALUES(1,'a1'),(2,'a2'),(3,'a3'),(NULL,'an'),(5,'a5'),"
	"(1,'a1b')",
	"INSERT INTO b VALUES(1,100),(1,101),(2,200),(4,400),(NULL,0),(5,NULL)",
	"CREATE TABLE e(k INT, w INT)",
	NULL
};

struct rcase {
	const char *id;
	const char *sql;
	int         nbind;
	int64_t     bind[4];
	int         open;     /* 1: known defect, not yet fixed */
	const char *want;
};

static const struct rcase cases[] = {
	/* Derived table / CTE: the inner SELECT lost the statement's binds. */
	{ "W1", "SELECT x FROM (SELECT x FROM g WHERE x > ?) WHERE x < ?",
	  2, { 1, 4 }, 1, "2;2;3" },
	{ "W1b", "WITH c AS (SELECT x FROM g WHERE x > ?) SELECT ?, x FROM c",
	  2, { 2, 9 }, 1, "9|3;9|4" },
	{ "W2", "SELECT x FROM (SELECT x FROM g WHERE x > ? UNION SELECT k "
	  "FROM a)", 1, { 2 }, 1, "1;2;3;4;5;NULL" },
	/* An output GROUP BY key must be the key it names, not the next one. */
	{ "W3a", "SELECT y, x, count(*) FROM g GROUP BY x, y", 0, { 0 }, 1,
	  "10|1|1;11|1|1;1|NULL|1;20|2|1;21|2|1;2|NULL|1;30|3|1;NULL|4|1" },
	{ "W3b", "SELECT x, x, count(*) FROM g GROUP BY x", 0, { 0 }, 1,
	  "1|1|2;2|2|2;3|3|1;4|4|1;NULL|NULL|2" },
	{ "W3c", "SELECT y, sum(x) FROM g GROUP BY x, y", 0, { 0 }, 1,
	  "10|1;11|1;1|NULL;20|2;21|2;2|NULL;30|3;NULL|4" },
	/* HAVING over a computed output. */
	{ "W4", "SELECT x, sum(y)+1 FROM g GROUP BY x HAVING sum(y)+1 > 25",
	  0, { 0 }, 1, "2|42;3|31" },
	/* A derived-table column that is an expression has no affinity. */
	{ "W5a", "SELECT z FROM (SELECT x+1 AS z FROM g) WHERE z = '2'",
	  0, { 0 }, 1, "" },
	{ "W5b", "SELECT z FROM (SELECT x+1 AS z FROM g) WHERE z > '2'",
	  0, { 0 }, 1, "" },
	{ "W5c", "SELECT n FROM (SELECT count(*) AS n FROM g) WHERE n = '8'",
	  0, { 0 }, 1, "" },
	{ "W5d", "SELECT m FROM (SELECT max(x) AS m FROM g) WHERE m = '4'",
	  0, { 0 }, 1, "" },
	/* HAVING applies the key column's affinity. */
	{ "W6", "SELECT x, count(*) FROM g GROUP BY x HAVING x = '2'",
	  0, { 0 }, 1, "2|2" },
	/* GROUP BY 1 is the first output column (here an aggregate). */
	{ "W7", "SELECT count(*) FROM g GROUP BY 1", 0, { 0 }, 1, "ERR" },
	/* A correlated subquery inside an aggregate query. */
	{ "W8a", "SELECT count(*) FROM g WHERE y > (SELECT min(y) FROM g AS h "
	  "WHERE h.x = g.x)", 0, { 0 }, 1, "2" },
	{ "W8b", "SELECT sum((SELECT count(*) FROM a WHERE a.k = g.x)) FROM g",
	  0, { 0 }, 1, "7" },
	/* A bare column of a join / INTERSECT / EXCEPT keeps its affinity
	 * when read as a derived table. */
	{ "W9a", "SELECT k FROM (SELECT a.k FROM a JOIN b ON a.k = b.k) WHERE "
	  "k = '1'", 0, { 0 }, 1, "1;1;1;1" },
	{ "W9b", "SELECT w FROM (SELECT b.w FROM a JOIN b ON a.k = b.k) WHERE "
	  "w > '150'", 0, { 0 }, 1, "200" },
	{ "W9c", "SELECT q FROM (SELECT k AS q FROM a INTERSECT SELECT k FROM b) "
	  "WHERE q = '1'", 0, { 0 }, 1, "1" },
	{ "W9d", "SELECT q FROM (SELECT k AS q FROM a EXCEPT SELECT k FROM b) "
	  "WHERE q = '3'", 0, { 0 }, 1, "3" },
	/* +x and a CASE carry no affinity: no conversion against '1'. */
	{ "W10a", "SELECT x FROM g WHERE +x = '1'", 0, { 0 }, 1, "" },
	{ "W10b", "SELECT x FROM g WHERE CASE WHEN x > 0 THEN x END = '1'",
	  0, { 0 }, 1, "" },
	/* Aggregates over a derived table and over a join (were errors). */
	{ "B1", "SELECT count(*) FROM (SELECT x FROM g GROUP BY x HAVING "
	  "count(*) = 2)", 0, { 0 }, 1, "3" },
	{ "B2", "SELECT a.k, count(*) FROM a JOIN b ON a.k = b.k GROUP BY a.k",
	  0, { 0 }, 1, "1|4;2|1;5|1" },
};

static int
row_cmp(const void *pa, const void *pb)
{
	return strcmp(*(char *const *)pa, *(char *const *)pb);
}

/* Run `rc` and render its rows into out (cap bytes); returns 0, or -1 if
 * prepare or step reported an error. */
static int
run_case(sx_db *db, const struct rcase *rc, char *out, size_t cap)
{
	sx_stmt *st = NULL;
	char *rows[256], line[1024];
	int nrow = 0, i, s, err = 0;
	size_t off = 0;

	out[0] = '\0';
	if (sx_prepare(db, rc->sql, -1, &st, NULL) != SX_OK || st == NULL)
		return -1;
	for (i = 0; i < rc->nbind; i++)
		sx_bind_int64(st, i + 1, rc->bind[i]);
	while ((s = sx_step(st)) == SX_ROW && nrow < 256) {
		int c, nc = sx_column_count(st);
		size_t lo = 0;
		line[0] = '\0';
		for (c = 0; c < nc; c++) {
			char cell[256];
			switch (sx_column_type(st, c)) {
			case SX_NULL:
				snprintf(cell, sizeof cell, "NULL");
				break;
			case SX_INTEGER:
				snprintf(cell, sizeof cell, "%lld",
				    (long long)sx_column_int64(st, c));
				break;
			case SX_FLOAT:
				snprintf(cell, sizeof cell, "%.15g",
				    sx_column_double(st, c));
				if (strpbrk(cell, ".en") == NULL)
					strcat(cell, ".0");
				break;
			default:
				snprintf(cell, sizeof cell, "%s",
				    sx_column_text(st, c) ? sx_column_text(st, c) : "");
				break;
			}
			lo += (size_t)snprintf(line + lo, sizeof line - lo, "%s%s",
			    c ? "|" : "", cell);
			if (lo >= sizeof line) lo = sizeof line - 1;
		}
		rows[nrow] = strdup(line);
		if (rows[nrow] == NULL) { err = 1; break; }
		nrow++;
	}
	if (s != SX_ROW && s != SX_DONE) err = 1;
	sx_finalize(st);
	qsort(rows, (size_t)nrow, sizeof rows[0], row_cmp);
	for (i = 0; i < nrow; i++) {
		off += (size_t)snprintf(out + off, cap - off, "%s%s", i ? ";" : "",
		    rows[i]);
		if (off >= cap) off = cap - 1;
		free(rows[i]);
	}
	return err ? -1 : 0;
}

int
main(int argc, char **argv)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	sx_db *db = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256], wal[288], got[4096];
	int strict = (argc > 1 && strcmp(argv[1], "--strict") == 0);
	int fd, i, nfail = 0, nopen = 0, nok = 0;

	t_tmpl(path, sizeof path, "sqlxtc-sqlres");
	fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); return 1; }
	close(fd);
	bo.path = path; bo.page_size = 4096; bo.n_frames = 1024; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(sx_open_bt(bt, &db) == SX_OK);
	for (i = 0; setup[i] != NULL; i++)
		CK(sx_exec(db, setup[i], NULL) == SX_OK);
	if (g_fail) { fprintf(stderr, "test_sql_results: setup FAILED\n"); return 1; }

	for (i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++) {
		const struct rcase *c = &cases[i];
		int r = run_case(db, c, got, sizeof got);
		int pass = (strcmp(c->want, "ERR") == 0) ? (r != 0)
		    : (r == 0 && strcmp(got, c->want) == 0);
		if (pass && c->open && !strict) {
			printf("  XPASS %-5s %s  (fixed: drop its OPEN mark)\n", c->id,
			    c->sql);
			nfail++;
		} else if (pass) {
			nok++;
		} else if (c->open && !strict) {
			printf("  open  %-5s %s\n        got  %s\n        want %s\n",
			    c->id, c->sql, r ? "ERR" : got, c->want);
			nopen++;
		} else {
			printf("  FAIL  %-5s %s\n        got  %s\n        want %s\n",
			    c->id, c->sql, r ? "ERR" : got, c->want);
			nfail++;
		}
	}

	sx_close(db);
	bt_close(bt);
	bm_destroy(bm);
	unlink(path);
	snprintf(wal, sizeof wal, "%s-wal", path);
	unlink(wal);
	printf("test_sql_results: %d ok, %d open (known), %d FAIL\n", nok, nopen,
	    nfail);
	return nfail ? 1 : 0;
}
