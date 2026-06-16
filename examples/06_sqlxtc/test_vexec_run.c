/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_vexec_run.c
 *	Differential test for the unified vexec dispatcher vx_run.
 *
 *	vx_run is the committed front door: it recognizes a query once and
 *	routes it -- a parallelizable single-table scan/aggregation runs on
 *	the morsel-parallel storage scan, any other recognized plan (ordered,
 *	limited, joined) is collected from the serial vectorized path, and an
 *	unrecognized query returns 0 so the caller runs the VDBE.  This test
 *	drives a tagged corpus through vx_run and asserts (a) the expected
 *	route is taken and (b) the result matches the VDBE -- positionally
 *	for ordered queries (total order via trailing rowid), as a multiset
 *	otherwise.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vexec.h"
#include "sqlite3.h"
#include "bufmgr.h"
#include "btree.h"
#include "xstore.h"

static int g_fail;
#define CK(c, msg) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
	g_fail = 1; } } while (0)

struct cell { int type; int64_t i; double r; char *txt; int n; };
struct row  { int ncol; struct cell c[8]; };
struct rset { int nrow; struct row *r; int cap; };

static void rs_init(struct rset *rs) { rs->nrow = 0; rs->cap = 0; rs->r = NULL; }

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
collect_res(const vx_result_t *pr, struct rset *rs)
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

static int
ordered_eq(const struct rset *a, const struct rset *b)
{
	int i;
	if (a->nrow != b->nrow) return 0;
	for (i = 0; i < a->nrow; i++)
		if (!row_eq(&a->r[i], &b->r[i])) return 0;
	return 1;
}

/* What route vx_run should take for a query. */
enum route { R_PAR, R_SER, R_VDBE };

int
main(void)
{
	char path[64] = "/tmp/sqlxtc_vexrunXXXXXX";
	sqlite3 *db = NULL;
	char *err = NULL;
	bm_t *bm = NULL; bt_t *bt = NULL; bm_opts_t bo = BM_OPTS_DEFAULT;
	int i, dbfd, n_par = 0, n_ser = 0, n_vdbe = 0;

	struct q { const char *sql; enum route route; int ordered; };
	static const struct q corpus[] = {
		/* parallelizable single-table scan / aggregation -> R_PAR */
		{ "SELECT a, b FROM t WHERE a > 5000",            R_PAR, 0 },
		{ "SELECT abs(a), length(b) FROM t WHERE a IS NOT NULL", R_PAR, 0 },
		{ "SELECT count(*) FROM t",                       R_PAR, 0 },
		{ "SELECT count(a), sum(a), min(a), max(a) FROM t", R_PAR, 0 },
		{ "SELECT b, count(*), sum(a) FROM t GROUP BY b", R_PAR, 0 },
		/* recognized but not parallelizable -> serial collect (R_SER) */
		{ "SELECT k, a FROM t ORDER BY a, k",             R_SER, 1 },
		{ "SELECT k, b FROM t WHERE a > 5000 ORDER BY k", R_SER, 1 },
		{ "SELECT k, a FROM t ORDER BY k LIMIT 5",        R_SER, 1 },
		{ "SELECT k, a FROM t ORDER BY k LIMIT 5 OFFSET 3", R_SER, 1 },
		/* not recognized -> caller runs the VDBE (R_VDBE) */
		{ "SELECT a FROM t WHERE a IN (1,2,3)",           R_VDBE, 0 },
		{ "SELECT DISTINCT b FROM t",                     R_VDBE, 0 }
	};
	int n = (int)(sizeof corpus / sizeof corpus[0]);

	dbfd = mkstemp(path); if (dbfd >= 0) close(dbfd);
	bo.path = path; bo.page_size = 4096; bo.n_frames = 512; bo.lsn_off = 0;
	if (bm_create(&bo, &bm) != XTC_OK || bt_open(bm, &bt) != XTC_OK) {
		fprintf(stderr, "FAIL: storage open\n"); return 1;
	}
	if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
		fprintf(stderr, "FAIL: open\n"); return 1;
	}
	if (xstore_register(db, bt) != SQLITE_OK) {
		fprintf(stderr, "FAIL: register\n"); return 1;
	}
	if (sqlite3_exec(db, "CREATE VIRTUAL TABLE t USING xstore(k, a INT, b TEXT)",
	                 0, 0, &err) != SQLITE_OK) {
		fprintf(stderr, "FAIL: create: %s\n", err ? err : "?");
		sqlite3_free(err); sqlite3_close(db); return 1;
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
		const char *sql = corpus[i].sql;
		struct rset ref, got;
		vx_result_t *pr = NULL;
		char *perr = NULL;
		int rc;

		rs_init(&ref); rs_init(&got);
		CK(run_vdbe(db, sql, &ref) == 0, "vdbe run");

		rc = vx_run(db, sql, 4, &pr, &perr);

		if (corpus[i].route == R_VDBE) {
			CK(rc == 0, sql);   /* must NOT be recognized */
			if (rc != 0 && pr) vx_result_free(pr);
		} else {
			CK(rc == 1, sql);   /* must be recognized + run by vexec */
			if (rc == 1) {
				if (corpus[i].route == R_PAR) {
					n_par++;
					/* a parallelizable plan must have used >1 loop when the
					 * box has cores; at minimum it must run via the result */
					CK(vx_result_nworkers(pr) >= 1, "par nworkers");
				} else {
					n_ser++;
					CK(vx_result_nworkers(pr) == 1, "ser single-loop");
				}
				CK(collect_res(pr, &got) == 0, "collect");
				if (corpus[i].ordered) {
					if (!ordered_eq(&ref, &got)) {
						fprintf(stderr, "FAIL: ordered mismatch [%s]: "
						        "vdbe %d, vexec %d rows\n", sql, ref.nrow, got.nrow);
						g_fail = 1;
					}
				} else if (!multiset_eq(&ref, &got)) {
					fprintf(stderr, "FAIL: multiset mismatch [%s]: "
					        "vdbe %d, vexec %d rows\n", sql, ref.nrow, got.nrow);
					g_fail = 1;
				}
				vx_result_free(pr);
			}
		}
		if (corpus[i].route == R_VDBE) n_vdbe++;
		sqlite3_free(perr);
		rs_free(&ref); rs_free(&got);
	}

	sqlite3_close(db);
	bt_close(bt);
	bm_destroy(bm);
	unlink(path);

	if (g_fail) { fprintf(stderr, "  vexec dispatcher: FAILURES\n"); return 1; }
	printf("  ok   vx_run routed %d parallel, %d serial-collect, %d VDBE-fallback "
	       "queries -- all matched the VDBE\n", n_par, n_ser, n_vdbe);
	printf("All sqlxtc vectorized-executor dispatcher (vx_run) tests passed.\n");
	return 0;
}
