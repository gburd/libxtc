/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_vexec.c
 *	Differential oracle for the vectorized executor (V0/V1).
 *
 *	Seed a table, then for each query in a corpus:
 *	  - run it through the VDBE (the reference) and collect the rows;
 *	  - ask vexec to recognize it (vx_try_prepare);
 *	  - if recognized, run it through vexec and collect the rows;
 *	  - compare the two result sets as MULTISETS (V0 has no ORDER BY,
 *	    so row order is unspecified): same row count, and every VDBE
 *	    row has a matching vexec row of identical column types+values.
 *	The corpus mixes P1 queries (must be recognized and match) with
 *	queries vexec must NOT recognize (must fall back, asserted by
 *	vx_try_prepare returning 0); the fallback queries are still run on
 *	the VDBE to confirm they are valid SQL.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vexec.h"
#include "sqlite3.h"

static int g_fail;
#define CK(c, msg) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
	g_fail = 1; } } while (0)

/* A captured cell, value-typed for multiset comparison. */
struct cell { int type; int64_t i; double r; char *txt; int n; };
struct row  { int ncol; struct cell c[8]; };
struct rset { int nrow; struct row r[256]; };

static void
rset_free(struct rset *rs)
{
	int i, j;
	for (i = 0; i < rs->nrow; i++)
		for (j = 0; j < rs->r[i].ncol; j++)
			free(rs->r[i].c[j].txt);
	rs->nrow = 0;
}

/* Collect a VDBE result set. */
static int
run_vdbe(sqlite3 *db, const char *sql, struct rset *rs)
{
	sqlite3_stmt *st = NULL;
	int rc;
	rs->nrow = 0;
	if (sqlite3_prepare_v2(db, sql, -1, &st, 0) != SQLITE_OK) return -1;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW && rs->nrow < 256) {
		struct row *row = &rs->r[rs->nrow];
		int nc = sqlite3_column_count(st), j;
		row->ncol = nc < 8 ? nc : 8;
		for (j = 0; j < row->ncol; j++) {
			int t = sqlite3_column_type(st, j);
			row->c[j].type = t; row->c[j].txt = NULL; row->c[j].n = 0;
			if (t == SQLITE_INTEGER) row->c[j].i = sqlite3_column_int64(st, j);
			else if (t == SQLITE_FLOAT) row->c[j].r = sqlite3_column_double(st, j);
			else if (t == SQLITE_TEXT || t == SQLITE_BLOB) {
				const void *p = (t == SQLITE_TEXT)
				    ? (const void *)sqlite3_column_text(st, j)
				    : sqlite3_column_blob(st, j);
				int n = sqlite3_column_bytes(st, j);
				row->c[j].txt = (char *)malloc((size_t)n + 1);
				if (row->c[j].txt) { if (n) memcpy(row->c[j].txt, p, (size_t)n); row->c[j].txt[n] = '\0'; }
				row->c[j].n = n;
			}
		}
		rs->nrow++;
	}
	sqlite3_finalize(st);
	return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? 0 : -1;
}

/* Collect a vexec result set (mapping vx types onto SQLITE_ codes). */
static int
run_vexec(vx_stmt_t *st, struct rset *rs)
{
	int rc;
	rs->nrow = 0;
	while ((rc = vx_step(st)) == SQLITE_ROW && rs->nrow < 256) {
		struct row *row = &rs->r[rs->nrow];
		int nc = vx_column_count(st), j;
		row->ncol = nc < 8 ? nc : 8;
		for (j = 0; j < row->ncol; j++) {
			vx_type_t t = vx_column_type(st, j);
			row->c[j].txt = NULL; row->c[j].n = 0;
			switch (t) {
			case VX_NULL: row->c[j].type = SQLITE_NULL; break;
			case VX_INT:  row->c[j].type = SQLITE_INTEGER; row->c[j].i = vx_column_int64(st, j); break;
			case VX_REAL: row->c[j].type = SQLITE_FLOAT; row->c[j].r = vx_column_double(st, j); break;
			case VX_TEXT:
			case VX_BLOB: {
				const void *p = vx_column_blob(st, j);
				int n = vx_column_bytes(st, j);
				row->c[j].type = (t == VX_TEXT) ? SQLITE_TEXT : SQLITE_BLOB;
				row->c[j].txt = (char *)malloc((size_t)n + 1);
				if (row->c[j].txt) { if (n) memcpy(row->c[j].txt, p, (size_t)n); row->c[j].txt[n] = '\0'; }
				row->c[j].n = n;
				break;
			}
			}
		}
		rs->nrow++;
	}
	return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? 0 : -1;
}

static int
cell_eq(const struct cell *a, const struct cell *b)
{
	if (a->type != b->type) return 0;
	switch (a->type) {
	case SQLITE_NULL:    return 1;
	case SQLITE_INTEGER: return a->i == b->i;
	case SQLITE_FLOAT:   return a->r == b->r;
	default:             return a->n == b->n &&
	                            (a->n == 0 || memcmp(a->txt, b->txt, (size_t)a->n) == 0);
	}
}

static int
row_eq(const struct row *a, const struct row *b)
{
	int j;
	if (a->ncol != b->ncol) return 0;
	for (j = 0; j < a->ncol; j++)
		if (!cell_eq(&a->c[j], &b->c[j])) return 0;
	return 1;
}

/* Multiset compare: each row of `a` has a distinct matching row in `b`. */
static int
rset_multiset_eq(const struct rset *a, const struct rset *b)
{
	int used[256] = {0};
	int i, k;
	if (a->nrow != b->nrow) return 0;
	for (i = 0; i < a->nrow; i++) {
		int found = 0;
		for (k = 0; k < b->nrow; k++) {
			if (used[k]) continue;
			if (row_eq(&a->r[i], &b->r[k])) { used[k] = 1; found = 1; break; }
		}
		if (!found) return 0;
	}
	return 1;
}

int
main(void)
{
	sqlite3 *db = NULL;
	char *err = NULL;
	int i;
	int recognized = 0, fellback = 0;

	struct { const char *sql; int expect_recognized; } corpus[] = {
		/* ---- P1: must be recognized AND match the VDBE ---- */
		{ "SELECT a, b FROM t", 1 },
		{ "SELECT * FROM t", 1 },
		{ "SELECT b, a, k FROM t", 1 },
		{ "SELECT a FROM t WHERE a > 10", 1 },
		{ "SELECT a, b FROM t WHERE a = 5", 1 },
		{ "SELECT k, b FROM t WHERE a >= 5", 1 },
		{ "SELECT a FROM t WHERE a < 100", 1 },
		{ "SELECT a, b FROM t WHERE b = 'three'", 1 },
		{ "SELECT a FROM t WHERE a <> 5", 1 },
		{ "SELECT * FROM t WHERE k <= 3", 1 },
		{ "SELECT a FROM t WHERE a = 999999", 1 },   /* empty result */
		{ "SELECT * FROM t WHERE a > 5", 1 },         /* star + filter (col by name) */
		{ "SELECT t.a, t.b FROM t WHERE t.a = 5", 1 }, /* qualified columns */
		{ "SELECT a FROM t WHERE a = 5", 1 },          /* matches rows 1 and 4 */
		{ "SELECT b FROM t WHERE b <> 'two'", 1 },     /* text inequality */

		/* ---- P2: scalar expressions + functions (V1) ---- */
		{ "SELECT a+1 FROM t", 1 },
		{ "SELECT a*2, a-1 FROM t", 1 },
		{ "SELECT k, a+k FROM t", 1 },
		{ "SELECT a FROM t WHERE a > 1 AND a < 20", 1 },
		{ "SELECT a, b FROM t WHERE a >= 5 AND a <= 10", 1 },
		{ "SELECT a FROM t WHERE NOT (a = 5)", 1 },
		{ "SELECT abs(a) FROM t", 1 },
		{ "SELECT length(b) FROM t", 1 },
		{ "SELECT upper(b), lower(b) FROM t", 1 },
		{ "SELECT b || '!' FROM t", 1 },
		{ "SELECT coalesce(a, -1) FROM t", 1 },
		{ "SELECT ifnull(a, 0) FROM t", 1 },
		{ "SELECT a FROM t WHERE a IS NULL", 1 },
		{ "SELECT a FROM t WHERE a IS NOT NULL", 1 },
		{ "SELECT a FROM t WHERE a > 4 OR b = 'two'", 1 },
		{ "SELECT a/2 FROM t WHERE a IS NOT NULL", 1 },

		/* ---- must fall back ---- */
		{ "SELECT DISTINCT a FROM t", 0 },
		{ "SELECT a.a FROM t a JOIN t b ON a.k=b.k", 1 },  /* self-join (V5) */
		{ "SELECT a FROM t WHERE a IN (1,2)", 0 },          /* IN */
		{ "SELECT substr(b,1,2) FROM t", 0 },               /* unsupported func */

		/* ---- P3: aggregation + GROUP BY (V3) ---- */
		{ "SELECT count(*) FROM t", 1 },
		{ "SELECT count(a) FROM t", 1 },
		{ "SELECT sum(a) FROM t", 1 },
		{ "SELECT total(a) FROM t", 1 },
		{ "SELECT avg(a) FROM t", 1 },
		{ "SELECT min(a), max(a) FROM t", 1 },
		{ "SELECT count(*), sum(a), min(k), max(k) FROM t", 1 },
		{ "SELECT a, count(*) FROM t GROUP BY a", 1 },
		{ "SELECT a FROM t GROUP BY a", 1 },
		{ "SELECT b, count(*), sum(a) FROM t GROUP BY b", 1 },
		{ "SELECT a, count(*) FROM t WHERE a > 4 GROUP BY a", 1 },
		{ "SELECT count(DISTINCT a) FROM t", 0 },           /* DISTINCT agg: fallback */
		{ "SELECT a, count(*) FROM t GROUP BY a HAVING count(*) > 1", 0 }, /* HAVING */

		{ "SELECT k FROM t WHERE a = 'x'", 0 },             /* INT col vs text lit: affinity */
		{ "SELECT k FROM t WHERE b = 5", 0 },               /* TEXT col vs int lit: affinity */
		{ "SELECT k FROM t WHERE b = '5'", 1 },             /* TEXT col vs text lit: safe */
	};
	int n = (int)(sizeof corpus / sizeof corpus[0]);

	if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
		fprintf(stderr, "FAIL: open db\n"); return 1;
	}
	if (sqlite3_exec(db,
	        "CREATE TABLE t(k INTEGER PRIMARY KEY, a INT, b TEXT);"
	        "INSERT INTO t VALUES(1,5,'one'),(2,10,'two'),(3,15,'three'),"
	        "(4,5,'four'),(5,NULL,'five')",
	        0, 0, &err) != SQLITE_OK) {
		fprintf(stderr, "FAIL: seed: %s\n", err ? err : "?");
		sqlite3_free(err); sqlite3_close(db); return 1;
	}

	for (i = 0; i < n; i++) {
		const char *sql = corpus[i].sql;
		vx_stmt_t *vs = NULL;
		char *verr = NULL;
		int rc = vx_try_prepare(db, sql, &vs, &verr);

		if (corpus[i].expect_recognized) {
			CK(rc == 1, sql);   /* must be recognized */
			if (rc == 1) {
				struct rset ref, got;
				recognized++;
				memset(&ref, 0, sizeof ref); memset(&got, 0, sizeof got);
				CK(run_vdbe(db, sql, &ref) == 0, "vdbe run");
				CK(run_vexec(vs, &got) == 0, "vexec run");
				if (!rset_multiset_eq(&ref, &got)) {
					fprintf(stderr, "FAIL: result mismatch for [%s]: "
					        "vdbe %d rows, vexec %d rows\n",
					        sql, ref.nrow, got.nrow);
					g_fail = 1;
				}
				rset_free(&ref); rset_free(&got);
			}
			if (vs) vx_finalize(vs);
		} else {
			CK(rc == 0, sql);   /* must fall back */
			if (rc == 0) {
				struct rset ref;
				fellback++;
				memset(&ref, 0, sizeof ref);
				/* Still valid SQL: the VDBE accepts it. */
				CK(run_vdbe(db, sql, &ref) == 0, "fallback query valid on vdbe");
				rset_free(&ref);
			}
			if (vs) vx_finalize(vs);   /* defensive: should be NULL */
		}
		sqlite3_free(verr);
	}

	sqlite3_close(db);

	if (g_fail) { fprintf(stderr, "  vexec: FAILURES\n"); return 1; }
	printf("  ok   vexec V1: %d P1/P2 queries recognized and matched the VDBE; "
	       "%d queries correctly fell back\n", recognized, fellback);
	printf("All sqlxtc vectorized-executor (V0/V1) tests passed.\n");
	return 0;
}
