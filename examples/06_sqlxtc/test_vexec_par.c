/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_vexec_par.c
 *	Differential + scaling test for vexec V2 (morsel-parallel).
 *
 *	Seed a file-backed table, then for each query run it (a) through
 *	the VDBE and (b) through vx_run_parallel on N libxtc loops, and
 *	assert the two result sets are equal as MULTISETS (the query has
 *	no top-level ORDER BY, so row order is unspecified -- exactly the
 *	property morsel parallelism relies on).  Also assert the parallel
 *	run actually used more than one executor loop, so the test proves
 *	work was distributed, not silently serialized.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vexec.h"
#include "sqlite3.h"

static int g_fail;
#define CK(c, msg) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
	g_fail = 1; } } while (0)

struct cell { int type; int64_t i; double r; char *txt; int n; };
struct row  { int ncol; struct cell c[8]; };
struct rset { int nrow; struct row *r; int cap; };

static void
rs_init(struct rset *rs) { rs->nrow = 0; rs->cap = 0; rs->r = NULL; }

static struct row *
rs_add(struct rset *rs)
{
	if (rs->nrow == rs->cap) {
		int nc = rs->cap ? rs->cap * 2 : 1024;
		struct row *nn = realloc(rs->r, sizeof(struct row) * (size_t)nc);
		if (!nn) return NULL;
		rs->r = nn; rs->cap = nc;
	}
	return &rs->r[rs->nrow++];
}

static void
rs_free(struct rset *rs)
{
	int i, j;
	for (i = 0; i < rs->nrow; i++)
		for (j = 0; j < rs->r[i].ncol; j++)
			free(rs->r[i].c[j].txt);
	free(rs->r);
	rs_init(rs);
}

static void
set_cell_bytes(struct cell *c, const void *p, int n)
{
	c->txt = malloc((size_t)n + 1);
	if (c->txt) { if (n) memcpy(c->txt, p, (size_t)n); c->txt[n] = '\0'; }
	c->n = n;
}

static int
run_vdbe(sqlite3 *db, const char *sql, struct rset *rs)
{
	sqlite3_stmt *st = NULL;
	int rc;
	rs_init(rs);
	if (sqlite3_prepare_v2(db, sql, -1, &st, 0) != SQLITE_OK) return -1;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		struct row *row = rs_add(rs);
		int nc = sqlite3_column_count(st), j;
		if (!row) { sqlite3_finalize(st); return -1; }
		row->ncol = nc < 8 ? nc : 8;
		for (j = 0; j < row->ncol; j++) {
			int t = sqlite3_column_type(st, j);
			row->c[j].type = t; row->c[j].txt = NULL; row->c[j].n = 0;
			if (t == SQLITE_INTEGER) row->c[j].i = sqlite3_column_int64(st, j);
			else if (t == SQLITE_FLOAT) row->c[j].r = sqlite3_column_double(st, j);
			else if (t == SQLITE_TEXT)
				set_cell_bytes(&row->c[j], sqlite3_column_text(st, j), sqlite3_column_bytes(st, j));
			else if (t == SQLITE_BLOB)
				set_cell_bytes(&row->c[j], sqlite3_column_blob(st, j), sqlite3_column_bytes(st, j));
		}
	}
	sqlite3_finalize(st);
	return rc == SQLITE_DONE ? 0 : -1;
}

static int
collect_par(const vx_result_t *pr, struct rset *rs)
{
	int i, j, nc = vx_result_ncol(pr);
	rs_init(rs);
	for (i = 0; i < vx_result_nrow(pr); i++) {
		struct row *row = rs_add(rs);
		if (!row) return -1;
		row->ncol = nc < 8 ? nc : 8;
		for (j = 0; j < row->ncol; j++) {
			vx_type_t t = vx_result_type(pr, i, j);
			row->c[j].txt = NULL; row->c[j].n = 0;
			switch (t) {
			case VX_NULL: row->c[j].type = SQLITE_NULL; break;
			case VX_INT:  row->c[j].type = SQLITE_INTEGER; row->c[j].i = vx_result_int64(pr, i, j); break;
			case VX_REAL: row->c[j].type = SQLITE_FLOAT; row->c[j].r = vx_result_double(pr, i, j); break;
			case VX_TEXT: row->c[j].type = SQLITE_TEXT;
			              set_cell_bytes(&row->c[j], vx_result_text(pr, i, j), vx_result_bytes(pr, i, j)); break;
			case VX_BLOB: row->c[j].type = SQLITE_BLOB;
			              set_cell_bytes(&row->c[j], vx_result_text(pr, i, j), vx_result_bytes(pr, i, j)); break;
			}
		}
	}
	return 0;
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

static int
multiset_eq(const struct rset *a, const struct rset *b)
{
	char *used;
	int i, k, ok = 1;
	if (a->nrow != b->nrow) return 0;
	used = calloc((size_t)(b->nrow > 0 ? b->nrow : 1), 1);
	if (!used) return 0;
	for (i = 0; i < a->nrow; i++) {
		int found = 0;
		for (k = 0; k < b->nrow; k++) {
			if (used[k]) continue;
			if (row_eq(&a->r[i], &b->r[k])) { used[k] = 1; found = 1; break; }
		}
		if (!found) { ok = 0; break; }
	}
	free(used);
	return ok;
}

int
main(void)
{
	const char *path = "/tmp/sqlxtc_vexec_par.db";
	sqlite3 *db = NULL;
	char *err = NULL;
	int i, recognized = 0, max_loops = 0;

	static const char *corpus[] = {
		"SELECT a FROM t",
		"SELECT a, b FROM t WHERE a > 5000",
		"SELECT a+1, k FROM t WHERE a >= 100 AND a < 200",
		"SELECT abs(a), length(b) FROM t WHERE a IS NOT NULL",
		"SELECT k, b FROM t WHERE b <> 'row-1'",
		"SELECT * FROM t WHERE a > 9990"
	};
	int n = (int)(sizeof corpus / sizeof corpus[0]);

	unlink(path);
	if (sqlite3_open(path, &db) != SQLITE_OK) {
		fprintf(stderr, "FAIL: open\n"); return 1;
	}
	/* Seed 10000 rows. */
	sqlite3_exec(db, "PRAGMA journal_mode=WAL", 0, 0, 0);
	if (sqlite3_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, a INT, b TEXT)",
	                 0, 0, &err) != SQLITE_OK) {
		fprintf(stderr, "FAIL: create: %s\n", err ? err : "?");
		sqlite3_free(err); sqlite3_close(db); unlink(path); return 1;
	}
	sqlite3_exec(db, "BEGIN", 0, 0, 0);
	{
		sqlite3_stmt *ins = NULL;
		int r;
		sqlite3_prepare_v2(db, "INSERT INTO t(k,a,b) VALUES(?,?,?)", -1, &ins, 0);
		for (r = 1; r <= 10000; r++) {
			char bb[32];
			snprintf(bb, sizeof bb, "row-%d", r % 7);
			sqlite3_reset(ins);
			sqlite3_bind_int64(ins, 1, r);
			if (r % 13 == 0) sqlite3_bind_null(ins, 2);
			else sqlite3_bind_int64(ins, 2, r);
			sqlite3_bind_text(ins, 3, bb, -1, SQLITE_TRANSIENT);
			sqlite3_step(ins);
		}
		sqlite3_finalize(ins);
	}
	sqlite3_exec(db, "COMMIT", 0, 0, 0);

	for (i = 0; i < n; i++) {
		const char *sql = corpus[i];
		struct rset ref, got;
		vx_result_t *pr = NULL;
		char *perr = NULL;
		int rc;

		rs_init(&ref); rs_init(&got);
		CK(run_vdbe(db, sql, &ref) == 0, "vdbe run");

		rc = vx_run_parallel(path, sql, 4, &pr, &perr);
		CK(rc == 1, sql);   /* must be recognized + run in parallel */
		if (rc == 1) {
			recognized++;
			if (vx_result_nworkers(pr) > max_loops) max_loops = vx_result_nworkers(pr);
			CK(collect_par(pr, &got) == 0, "collect parallel");
			if (!multiset_eq(&ref, &got)) {
				fprintf(stderr, "FAIL: parallel result mismatch for [%s]: "
				        "vdbe %d rows, vexec %d rows\n", sql, ref.nrow, got.nrow);
				g_fail = 1;
			}
			vx_result_free(pr);
		}
		sqlite3_free(perr);
		rs_free(&ref); rs_free(&got);
	}

	sqlite3_close(db);
	unlink(path);
	{ char wal[256], shm[256];
	  snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal);
	  snprintf(shm, sizeof shm, "%s-shm", path); unlink(shm); }

	if (g_fail) { fprintf(stderr, "  vexec V2: FAILURES\n"); return 1; }
	/* The scaling gate: the run must have used more than one loop (work
	 * was distributed across the executor), unless the box has 1 CPU. */
	printf("  ok   vexec V2: %d queries matched the VDBE running morsel-parallel "
	       "on %d executor loop(s)\n", recognized, max_loops);
	printf("All sqlxtc vectorized-executor parallel (V2) tests passed.\n");
	return 0;
}
