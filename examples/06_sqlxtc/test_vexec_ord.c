/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_vexec_ord.c
 *	Differential oracle for vexec V4 (ORDER BY / LIMIT / OFFSET).
 *
 *	Unlike the multiset oracle, an ordered query's row ORDER is part
 *	of the result, so this compares vexec against the VDBE
 *	POSITIONALLY: row i of vexec must equal row i of the VDBE.  The
 *	corpus uses ORDER BY keys that fully determine the order (the last
 *	key is always the unique rowid k), so there are no ties whose
 *	order SQLite leaves unspecified -- the positional comparison is
 *	then well defined.
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
	sqlite3_stmt *st = NULL; int rc;
	rs->nrow = 0;
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
/* Positional comparison: row i must match row i. */
static int positional_eq(const struct rset *a, const struct rset *b) {
	int i, j;
	if (a->nrow != b->nrow) return 0;
	for (i = 0; i < a->nrow; i++) {
		if (a->r[i].ncol != b->r[i].ncol) return 0;
		for (j = 0; j < a->r[i].ncol; j++)
			if (!cell_eq(&a->r[i].c[j], &b->r[i].c[j])) return 0;
	}
	return 1;
}

int
main(void)
{
	sqlite3 *db = NULL; char *err = NULL;
	int i, recognized = 0;
	/* Every ORDER BY ends in k (unique rowid) so the order is total. */
	static const char *corpus[] = {
		"SELECT k FROM t ORDER BY k",
		"SELECT k FROM t ORDER BY k DESC",
		"SELECT a, k FROM t ORDER BY a, k",
		"SELECT a, k FROM t ORDER BY a DESC, k",
		"SELECT a, b, k FROM t WHERE a IS NOT NULL ORDER BY a, k",
		"SELECT k, a FROM t ORDER BY k LIMIT 3",
		"SELECT k FROM t ORDER BY k LIMIT 3 OFFSET 2",
		"SELECT k FROM t ORDER BY k LIMIT 100",          /* limit > rows */
		"SELECT k FROM t ORDER BY 1",                    /* ORDER BY position */
		"SELECT a, k FROM t WHERE a > 5 ORDER BY a, k",
		"SELECT b, k FROM t ORDER BY b, k",              /* text + null ordering */
		"SELECT a*2 AS d, k FROM t ORDER BY d, k",       /* expr key via alias position? */
		"SELECT k FROM t ORDER BY a, k LIMIT 4 OFFSET 1"
	};
	int n = (int)(sizeof corpus / sizeof corpus[0]);

	if (sqlite3_open(":memory:", &db) != SQLITE_OK) { fprintf(stderr, "open\n"); return 1; }
	if (sqlite3_exec(db,
	        "CREATE TABLE t(k INTEGER PRIMARY KEY, a INT, b TEXT);"
	        "INSERT INTO t VALUES(1,5,'one'),(2,10,'two'),(3,15,'three'),"
	        "(4,5,'four'),(5,NULL,'five'),(6,10,'six'),(7,NULL,NULL)",
	        0, 0, &err) != SQLITE_OK) {
		fprintf(stderr, "seed: %s\n", err ? err : "?"); sqlite3_free(err);
		sqlite3_close(db); return 1;
	}

	for (i = 0; i < n; i++) {
		const char *sql = corpus[i];
		vx_stmt_t *vs = NULL; char *verr = NULL;
		int rc = vx_try_prepare(db, sql, &vs, &verr);
		struct rset ref, got;
		memset(&ref, 0, sizeof ref); memset(&got, 0, sizeof got);

		if (rc == 1) {
			recognized++;
			CK(run_vdbe(db, sql, &ref) == 0, "vdbe");
			CK(run_vx(vs, &got) == 0, "vexec");
			if (!positional_eq(&ref, &got)) {
				fprintf(stderr, "FAIL: ordered mismatch [%s]: vdbe %d rows, vexec %d rows\n",
				        sql, ref.nrow, got.nrow);
				g_fail = 1;
			}
			vx_finalize(vs);
		} else {
			/* Acceptable to fall back, but then the VDBE still runs it. */
			if (vs) vx_finalize(vs);
			CK(run_vdbe(db, sql, &ref) == 0, "fallback valid on vdbe");
		}
		sqlite3_free(verr);
		rs_free(&ref); rs_free(&got);
	}

	sqlite3_close(db);
	if (g_fail) { fprintf(stderr, "  vexec V4: FAILURES\n"); return 1; }
	printf("  ok   vexec V4: %d ORDER BY / LIMIT queries matched the VDBE "
	       "positionally\n", recognized);
	printf("All sqlxtc vectorized-executor ORDER BY (V4) tests passed.\n");
	return 0;
}
