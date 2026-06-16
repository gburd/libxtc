/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_vexec_join.c
 *	Differential oracle for vexec V5 (INNER hash join).
 *
 *	Two tables, several equi-join shapes; results compared against the
 *	VDBE as MULTISETS (no top-level ORDER BY, so row order is
 *	unspecified -- the hash join may emit in any order).  Covers joins
 *	on the PK (where SQLite nested-loops with a rowid seek) and on a
 *	non-indexed column (where SQLite nested-loops a full scan), plus
 *	multi-match keys, projections drawing from both sides, and a WHERE
 *	over the joined row.  vexec runs all of them as a hash join and
 *	must still match the VDBE row-for-row (as a multiset).
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

struct cell { int type; int64_t i; double r; char *txt; int n; };
struct row  { int ncol; struct cell c[8]; };
struct rset { int nrow; struct row r[512]; };

static void rs_free(struct rset *rs) {
	int i, j;
	for (i = 0; i < rs->nrow; i++)
		for (j = 0; j < rs->r[i].ncol; j++) free(rs->r[i].c[j].txt);
	rs->nrow = 0;
}
static void cap_bytes(struct cell *c, const void *p, int n) {
	c->txt = malloc((size_t)n + 1);
	if (c->txt) { if (n) memcpy(c->txt, p, (size_t)n); c->txt[n] = '\0'; }
	c->n = n;
}
static int run_vdbe(sqlite3 *db, const char *sql, struct rset *rs) {
	sqlite3_stmt *st = NULL; int rc; rs->nrow = 0;
	if (sqlite3_prepare_v2(db, sql, -1, &st, 0) != SQLITE_OK) return -1;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW && rs->nrow < 512) {
		struct row *row = &rs->r[rs->nrow]; int nc = sqlite3_column_count(st), j;
		row->ncol = nc < 8 ? nc : 8;
		for (j = 0; j < row->ncol; j++) {
			int t = sqlite3_column_type(st, j);
			row->c[j].type = t; row->c[j].txt = NULL; row->c[j].n = 0;
			if (t == SQLITE_INTEGER) row->c[j].i = sqlite3_column_int64(st, j);
			else if (t == SQLITE_FLOAT) row->c[j].r = sqlite3_column_double(st, j);
			else if (t == SQLITE_TEXT) cap_bytes(&row->c[j], sqlite3_column_text(st, j), sqlite3_column_bytes(st, j));
			else if (t == SQLITE_BLOB) cap_bytes(&row->c[j], sqlite3_column_blob(st, j), sqlite3_column_bytes(st, j));
		}
		rs->nrow++;
	}
	sqlite3_finalize(st);
	return rc == SQLITE_DONE ? 0 : -1;
}
static int run_vx(vx_stmt_t *st, struct rset *rs) {
	int rc; rs->nrow = 0;
	while ((rc = vx_step(st)) == SQLITE_ROW && rs->nrow < 512) {
		struct row *row = &rs->r[rs->nrow]; int nc = vx_column_count(st), j;
		row->ncol = nc < 8 ? nc : 8;
		for (j = 0; j < row->ncol; j++) {
			vx_type_t t = vx_column_type(st, j);
			row->c[j].txt = NULL; row->c[j].n = 0;
			switch (t) {
			case VX_NULL: row->c[j].type = SQLITE_NULL; break;
			case VX_INT:  row->c[j].type = SQLITE_INTEGER; row->c[j].i = vx_column_int64(st, j); break;
			case VX_REAL: row->c[j].type = SQLITE_FLOAT; row->c[j].r = vx_column_double(st, j); break;
			case VX_TEXT: row->c[j].type = SQLITE_TEXT; cap_bytes(&row->c[j], vx_column_text(st, j), vx_column_bytes(st, j)); break;
			case VX_BLOB: row->c[j].type = SQLITE_BLOB; cap_bytes(&row->c[j], vx_column_blob(st, j), vx_column_bytes(st, j)); break;
			}
		}
		rs->nrow++;
	}
	return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? 0 : -1;
}
static int cell_eq(const struct cell *a, const struct cell *b) {
	if (a->type != b->type) return 0;
	switch (a->type) {
	case SQLITE_NULL: return 1;
	case SQLITE_INTEGER: return a->i == b->i;
	case SQLITE_FLOAT: return a->r == b->r;
	default: return a->n == b->n && (a->n == 0 || memcmp(a->txt, b->txt, (size_t)a->n) == 0);
	}
}
static int row_eq(const struct row *a, const struct row *b) {
	int j; if (a->ncol != b->ncol) return 0;
	for (j = 0; j < a->ncol; j++) if (!cell_eq(&a->c[j], &b->c[j])) return 0;
	return 1;
}
static int multiset_eq(const struct rset *a, const struct rset *b) {
	char used[512] = {0}; int i, k;
	if (a->nrow != b->nrow) return 0;
	for (i = 0; i < a->nrow; i++) {
		int found = 0;
		for (k = 0; k < b->nrow; k++) { if (used[k]) continue;
			if (row_eq(&a->r[i], &b->r[k])) { used[k] = 1; found = 1; break; } }
		if (!found) return 0;
	}
	return 1;
}

int
main(void)
{
	sqlite3 *db = NULL; char *err = NULL;
	int i, recognized = 0;
	static struct { const char *sql; int expect; } corpus[] = {
		/* PK join (SQLite uses a rowid seek; vexec hash-joins). */
		{ "SELECT t.a, u.c FROM t JOIN u ON t.k = u.j", 1 },
		{ "SELECT u.c, t.a FROM t JOIN u ON u.j = t.k", 1 },
		/* join on a non-indexed column with MULTIPLE matches per key. */
		{ "SELECT t.k, u.j FROM t JOIN u ON t.a = u.d", 1 },
		/* projection expression over the joined row + WHERE. */
		{ "SELECT t.a + u.c FROM t JOIN u ON t.k = u.j WHERE t.a > 5", 1 },
		{ "SELECT t.k, u.c FROM t JOIN u ON t.k = u.j WHERE u.c IS NOT NULL", 1 },
		/* self-join. */
		{ "SELECT x.a, y.a FROM t x JOIN t y ON x.a = y.a", 1 },
		/* function over a joined column. */
		{ "SELECT t.k, length(u.e) FROM t JOIN u ON t.k = u.j", 1 },
		/* must fall back: LEFT join (V5 is INNER only), non-equi ON. */
		{ "SELECT t.a FROM t LEFT JOIN u ON t.k = u.j", 0 },
		{ "SELECT t.a FROM t JOIN u ON t.k < u.j", 0 },
		{ "SELECT t.a FROM t, u, t v", 0 }   /* three tables */
	};
	int n = (int)(sizeof corpus / sizeof corpus[0]);

	if (sqlite3_open(":memory:", &db) != SQLITE_OK) { fprintf(stderr, "open\n"); return 1; }
	if (sqlite3_exec(db,
	        "CREATE TABLE t(k INTEGER PRIMARY KEY, a INT);"
	        "CREATE TABLE u(j INTEGER PRIMARY KEY, c INT, d INT, e TEXT);"
	        "INSERT INTO t VALUES(1,5),(2,10),(3,5),(4,15),(5,NULL);"
	        "INSERT INTO u VALUES(1,100,5,'aa'),(2,200,10,'bbb'),(3,NULL,5,'c'),"
	        "(6,300,5,'dddd'),(7,400,99,NULL)",
	        0, 0, &err) != SQLITE_OK) {
		fprintf(stderr, "seed: %s\n", err ? err : "?"); sqlite3_free(err);
		sqlite3_close(db); return 1;
	}

	for (i = 0; i < n; i++) {
		const char *sql = corpus[i].sql;
		vx_stmt_t *vs = NULL; char *verr = NULL;
		int rc = vx_try_prepare(db, sql, &vs, &verr);
		struct rset ref, got;
		memset(&ref, 0, sizeof ref); memset(&got, 0, sizeof got);

		if (corpus[i].expect) {
			CK(rc == 1, sql);
			if (rc == 1) {
				recognized++;
				CK(run_vdbe(db, sql, &ref) == 0, "vdbe");
				CK(run_vx(vs, &got) == 0, "vexec");
				if (!multiset_eq(&ref, &got)) {
					fprintf(stderr, "FAIL: join mismatch [%s]: vdbe %d, vexec %d rows\n",
					        sql, ref.nrow, got.nrow);
					g_fail = 1;
				}
				vx_finalize(vs);
			} else if (vs) vx_finalize(vs);
		} else {
			CK(rc == 0, sql);
			if (vs) vx_finalize(vs);
			CK(run_vdbe(db, sql, &ref) == 0, "fallback valid on vdbe");
		}
		sqlite3_free(verr);
		rs_free(&ref); rs_free(&got);
	}

	sqlite3_close(db);
	if (g_fail) { fprintf(stderr, "  vexec V5: FAILURES\n"); return 1; }
	printf("  ok   vexec V5: %d INNER hash joins matched the VDBE (multiset)\n", recognized);
	printf("All sqlxtc vectorized-executor join (V5) tests passed.\n");
	return 0;
}
