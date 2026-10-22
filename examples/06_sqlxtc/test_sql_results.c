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
 *	The morsel-parallel aggregate must give the serial answer (par_sql).
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
#include "vexec.h"
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
	  2, { 1, 4 }, 0, "2;2;3" },
	{ "W1b", "WITH c AS (SELECT x FROM g WHERE x > ?) SELECT ?, x FROM c",
	  2, { 2, 9 }, 0, "9|3;9|4" },
	{ "W2", "SELECT x FROM (SELECT x FROM g WHERE x > ? UNION SELECT k "
	  "FROM a)", 1, { 2 }, 0, "1;2;3;4;5;NULL" },
	/* An output GROUP BY key must be the key it names, not the next one. */
	{ "W3a", "SELECT y, x, count(*) FROM g GROUP BY x, y", 0, { 0 }, 0,
	  "10|1|1;11|1|1;1|NULL|1;20|2|1;21|2|1;2|NULL|1;30|3|1;NULL|4|1" },
	{ "W3b", "SELECT x, x, count(*) FROM g GROUP BY x", 0, { 0 }, 0,
	  "1|1|2;2|2|2;3|3|1;4|4|1;NULL|NULL|2" },
	{ "W3c", "SELECT y, sum(x) FROM g GROUP BY x, y", 0, { 0 }, 0,
	  "10|1;11|1;1|NULL;20|2;21|2;2|NULL;30|3;NULL|4" },
	/* HAVING over a computed output. */
	{ "W4", "SELECT x, sum(y)+1 FROM g GROUP BY x HAVING sum(y)+1 > 25",
	  0, { 0 }, 0, "2|42;3|31" },
	/* A derived-table column that is an expression has no affinity. */
	{ "W5a", "SELECT z FROM (SELECT x+1 AS z FROM g) WHERE z = '2'",
	  0, { 0 }, 0, "" },
	{ "W5b", "SELECT z FROM (SELECT x+1 AS z FROM g) WHERE z > '2'",
	  0, { 0 }, 0, "" },
	{ "W5c", "SELECT n FROM (SELECT count(*) AS n FROM g) WHERE n = '8'",
	  0, { 0 }, 0, "" },
	{ "W5d", "SELECT m FROM (SELECT max(x) AS m FROM g) WHERE m = '4'",
	  0, { 0 }, 0, "" },
	/* HAVING applies the key column's affinity. */
	{ "W6", "SELECT x, count(*) FROM g GROUP BY x HAVING x = '2'",
	  0, { 0 }, 0, "2|2" },
	/* GROUP BY 1 is the first output column (here an aggregate). */
	{ "W7", "SELECT count(*) FROM g GROUP BY 1", 0, { 0 }, 0, "ERR" },
	/* A correlated subquery inside an aggregate query. */
	{ "W8a", "SELECT count(*) FROM g WHERE y > (SELECT min(y) FROM g AS h "
	  "WHERE h.x = g.x)", 0, { 0 }, 0, "2" },
	{ "W8b", "SELECT sum((SELECT count(*) FROM a WHERE a.k = g.x)) FROM g",
	  0, { 0 }, 0, "7" },
	/* A bare column of a join / INTERSECT / EXCEPT keeps its affinity
	 * when read as a derived table. */
	{ "W9a", "SELECT k FROM (SELECT a.k FROM a JOIN b ON a.k = b.k) WHERE "
	  "k = '1'", 0, { 0 }, 0, "1;1;1;1" },
	{ "W9b", "SELECT w FROM (SELECT b.w FROM a JOIN b ON a.k = b.k) WHERE "
	  "w > '150'", 0, { 0 }, 0, "200" },
	{ "W9c", "SELECT q FROM (SELECT k AS q FROM a INTERSECT SELECT k FROM b) "
	  "WHERE q = '1'", 0, { 0 }, 0, "1" },
	{ "W9d", "SELECT q FROM (SELECT k AS q FROM a EXCEPT SELECT k FROM b) "
	  "WHERE q = '3'", 0, { 0 }, 0, "3" },
	/* +x and a CASE carry no affinity: no conversion against '1'. */
	{ "W10a", "SELECT x FROM g WHERE +x = '1'", 0, { 0 }, 0, "" },
	{ "W10b", "SELECT x FROM g WHERE CASE WHEN x > 0 THEN x END = '1'",
	  0, { 0 }, 0, "" },
	/* A 2-table join ordered by a column it does not output. */
	{ "J1", "SELECT a.v FROM a JOIN b ON a.k = b.k ORDER BY b.w, a.v",
	  0, { 0 }, 0, "a1;a1;a1b;a1b;a2;a5" },
	/* Shapes that were declined: a join side with no named column, a
	 * table read for no column, DISTINCT over a join. */
	{ "D1", "SELECT count(*) FROM a, b", 0, { 0 }, 0, "36" },
	{ "D2", "SELECT a.k, count(*) FROM a, b GROUP BY a.k", 0, { 0 }, 0,
	  "1|12;2|6;3|6;5|6;NULL|6" },
	{ "D3", "SELECT 1, 'x' FROM g", 0, { 0 }, 0,
	  "1|x;1|x;1|x;1|x;1|x;1|x;1|x;1|x" },
	{ "D4", "SELECT DISTINCT a.k FROM a JOIN b ON a.k = b.k", 0, { 0 }, 0,
	  "1;2;5" },
	{ "D5", "SELECT count(*) FROM a, b, g", 0, { 0 }, 0, "288" },
	/* Aggregates over a derived table and over a join (were errors). */
	{ "B1", "SELECT count(*) FROM (SELECT x FROM g GROUP BY x HAVING "
	  "count(*) = 2)", 0, { 0 }, 0, "3" },
	{ "B2", "SELECT a.k, count(*) FROM a JOIN b ON a.k = b.k GROUP BY a.k",
	  0, { 0 }, 0, "1|4;2|1;5|1" },
};

/* SQLite's answers for three successive binds of the one ? . */
static const struct {
	const char *sql;
	int64_t     b[3], want[3];
} rebind[] = {
	{ "SELECT count(*) FROM g WHERE y > ?", { 5, 20, 100 }, { 5, 2, 0 } },
	{ "SELECT count(*) FROM a JOIN b ON a.k = b.k WHERE b.w > ?",
	  { 0, 100, 1000 }, { 5, 3, 0 } },
	{ "SELECT sum(b.w) FROM a JOIN b ON a.k = b.k GROUP BY a.k HAVING a.k = ?",
	  { 1, 2, 3 }, { 402, 200, -1 } },
	{ "SELECT count(*) FROM (SELECT x FROM g WHERE y > ?)", { 5, 20, 100 },
	  { 5, 2, 0 } },
};

/* Aggregates the morsel-parallel path must run like the serial one. */
static const char *const par_sql[] = {
	"SELECT x, count(*), sum(y), min(y), max(y), avg(y) FROM p GROUP BY x",
	"SELECT x, sum(y) + count(*), max(y) - min(y) FROM p GROUP BY x",
	"SELECT x, count(*) FROM p GROUP BY x HAVING sum(y) > 0",
	"SELECT count(*), sum(y), count(DISTINCT x) FROM p WHERE y > 100",
	"SELECT y % 3, count(*) FROM p GROUP BY y % 3 HAVING count(*) > 10",
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

/* Run `sql` through the vectorized seam at `nw` workers and render it
 * like run_case (sorted rows).  *nworkers: the workers it ran on. */
static int
vexec_render(sx_db *db, const char *sql, int nw, char *out, size_t cap,
             int *nworkers)
{
	sx_vx_result *r = NULL;
	char *rows[256], line[512];
	int nrow, i, c, n = 0;
	size_t off = 0;

	out[0] = '\0';
	if (sx_vexec_try(db, sql, nw, &r) != 1) return -1;
	/* sx_vx_result is the engine's name for a vx_result_t. */
	if (nworkers) *nworkers = vx_result_nworkers((const vx_result_t *)r);
	nrow = sx_vexec_nrow(r);
	for (i = 0; i < nrow && n < 256; i++) {
		size_t lo = 0;
		for (c = 0; c < sx_vexec_ncol(r); c++) {
			switch (sx_vexec_type(r, i, c)) {
			case SX_NULL:
				lo += (size_t)snprintf(line + lo, sizeof line - lo, "%sNULL",
				    c ? "|" : "");
				break;
			case SX_INTEGER:
				lo += (size_t)snprintf(line + lo, sizeof line - lo, "%s%lld",
				    c ? "|" : "", (long long)sx_vexec_int64(r, i, c));
				break;
			default:
				lo += (size_t)snprintf(line + lo, sizeof line - lo, "%s%.6f",
				    c ? "|" : "", sx_vexec_double(r, i, c));
				break;
			}
			if (lo >= sizeof line) lo = sizeof line - 1;
		}
		rows[n] = strdup(line);
		if (rows[n] == NULL) break;
		n++;
	}
	sx_vexec_free(r);
	qsort(rows, (size_t)n, sizeof rows[0], row_cmp);
	for (i = 0; i < n; i++) {
		off += (size_t)snprintf(out + off, cap - off, "%s%s", i ? ";" : "", rows[i]);
		if (off >= cap) off = cap - 1;
		free(rows[i]);
	}
	return 0;
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

	/* A prepared aggregate re-run with new binds (the plan is cached and
	 * rebound): over a base table, a join and a derived table. */
	for (i = 0; i < (int)(sizeof rebind / sizeof rebind[0]); i++) {
		sx_stmt *st = NULL;
		int j;
		if (sx_prepare(db, rebind[i].sql, -1, &st, NULL) != SX_OK) {
			printf("  FAIL  rebind prepare %s\n", rebind[i].sql); nfail++; continue;
		}
		for (j = 0; j < 3; j++) {
			int64_t v = -1;
			sx_bind_int64(st, 1, rebind[i].b[j]);
			if (sx_step(st) == SX_ROW) v = sx_column_int64(st, 0);
			while (sx_step(st) == SX_ROW)
				;
			sx_reset(st);
			if (v != rebind[i].want[j]) {
				printf("  FAIL  rebind %s with %lld: got %lld want %lld\n",
				    rebind[i].sql, (long long)rebind[i].b[j], (long long)v,
				    (long long)rebind[i].want[j]);
				nfail++;
			} else
				nok++;
		}
		sx_finalize(st);
	}

	/* The morsel-parallel aggregate (4 workers) must give the serial
	 * answer, including HAVING and outputs computed over aggregates. */
	CK(sx_exec(db, "CREATE TABLE p(k INTEGER PRIMARY KEY, x INT, y INT)", NULL)
	    == SX_OK);
	CK(sx_exec(db, "BEGIN", NULL) == SX_OK);
	for (i = 1; i <= 20000; i++) {
		char ins[96];
		snprintf(ins, sizeof ins, "INSERT INTO p VALUES(%d, %d, %d)", i, i % 37,
		    i % 11 == 0 ? -i : i);
		CK(sx_exec(db, ins, NULL) == SX_OK);
	}
	CK(sx_exec(db, "COMMIT", NULL) == SX_OK);
	for (i = 0; i < (int)(sizeof par_sql / sizeof par_sql[0]); i++) {
		char s1[8192], s4[8192];
		int nw = 0;
		if (vexec_render(db, par_sql[i], 1, s1, sizeof s1, NULL) != 0 ||
		    vexec_render(db, par_sql[i], 4, s4, sizeof s4, &nw) != 0 ||
		    strcmp(s1, s4) != 0 || nw < 2) {
			printf("  FAIL  par   %s (workers %d)\n        1 worker  %.200s\n"
			    "        4 workers %.200s\n", par_sql[i], nw, s1, s4);
			nfail++;
		} else
			nok++;
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
