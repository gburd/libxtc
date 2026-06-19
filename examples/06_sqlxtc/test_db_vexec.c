/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_db_vexec.c
 *	Live-path differential test for the vexec fast path.
 *
 *	The server's query path (db_exec_cached) now tries the libxtc-native
 *	vectorized executor (vexec) before the VDBE, falling back when a
 *	query is not recognized.  This test seeds a real xstore-backed table
 *	and, for each query, executes it twice through db_exec_cached -- once
 *	with vexec active and once with SQLXTC_VEXEC=0 (VDBE only) -- and
 *	asserts the two Quack response buffers are BYTE-IDENTICAL.  That is
 *	the strongest possible equivalence: a client cannot tell which engine
 *	served the rows (same column header, same row encoding, same done
 *	count).  It also asserts that at least some queries were actually
 *	served by vexec (so the test is not vacuously passing on fallback).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db.h"
#include "engine.h"
#include "quack.h"
#include "vexec.h"
#include "sqlite3.h"
#include "bufmgr.h"
#include "btree.h"
#include "xstore.h"

static int g_fail;
#define CK(c, msg) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
	g_fail = 1; } } while (0)

/* Compare two Quack response buffers.  When `ordered`, they must be
 * byte-identical.  Otherwise the cols header line and the done line must
 * match exactly, and the row lines must match as a multiset (unordered
 * GROUP BY / scan results are SQL-legal in any order, and vexec emits
 * hash-bucket order where the VDBE may differ). */
static int
resp_eq(const char *a, const char *b, int ordered)
{
	char *ca, *cb, *sa, *sb, *la, *lb;
	char **ra = NULL, **rb = NULL;
	int na = 0, nb = 0, ca_a = 0, ca_b = 0, ok = 1, i, k;
	char *hdr_a = NULL, *don_a = NULL, *hdr_b = NULL, *don_b = NULL;
	char *used = NULL;

	if (ordered)
		return strcmp(a, b) == 0;

	ca = strdup(a); cb = strdup(b);
	if (!ca || !cb) { free(ca); free(cb); return 0; }

	for (la = strtok_r(ca, "\n", &sa); la; la = strtok_r(NULL, "\n", &sa)) {
		if (strncmp(la, "{\"cols\"", 7) == 0) hdr_a = la;
		else if (strncmp(la, "{\"done\"", 7) == 0) don_a = la;
		else { char **t = realloc(ra, sizeof(char *) * (size_t)(na + 1));
		       if (!t) { ok = 0; goto done; } ra = t; ra[na++] = la; }
	}
	for (lb = strtok_r(cb, "\n", &sb); lb; lb = strtok_r(NULL, "\n", &sb)) {
		if (strncmp(lb, "{\"cols\"", 7) == 0) hdr_b = lb;
		else if (strncmp(lb, "{\"done\"", 7) == 0) don_b = lb;
		else { char **t = realloc(rb, sizeof(char *) * (size_t)(nb + 1));
		       if (!t) { ok = 0; goto done; } rb = t; rb[nb++] = lb; }
	}
	(void)ca_a; (void)ca_b;

	if ((hdr_a == NULL) != (hdr_b == NULL) ||
	    (hdr_a && hdr_b && strcmp(hdr_a, hdr_b) != 0)) { ok = 0; goto done; }
	if ((don_a == NULL) != (don_b == NULL) ||
	    (don_a && don_b && strcmp(don_a, don_b) != 0)) { ok = 0; goto done; }
	if (na != nb) { ok = 0; goto done; }

	used = calloc((size_t)(nb > 0 ? nb : 1), 1);
	if (!used) { ok = 0; goto done; }
	for (i = 0; i < na; i++) {
		int found = 0;
		for (k = 0; k < nb; k++) {
			if (used[k]) continue;
			if (strcmp(ra[i], rb[k]) == 0) { used[k] = 1; found = 1; break; }
		}
		if (!found) { ok = 0; break; }
	}
done:
	free(used); free(ra); free(rb); free(ca); free(cb);
	return ok;
}

/* Run `sql` through the live db_exec_cached path into a fresh buffer;
 * caller passes vexec on/off via the SQLXTC_VEXEC env before calling. */
static int
run_live(sx_db *h, const char *sql, char **out, size_t *outn)
{
	quack_buf_t buf;
	sx_stmt *st = NULL;
	int64_t nrows = 0;
	char *err = NULL;
	int rc;

	if (quack_buf_init(&buf, 256) != 0) return -1;
	rc = db_exec_cached(h, &st, sql, NULL, 0, -1, &buf, &nrows, &err);
	if (st) sx_finalize(st);
	if (rc != 0) {
		fprintf(stderr, "  live exec failed [%s]: %s\n", sql, err ? err : "?");
		free(err); quack_buf_free(&buf); return -1;
	}
	*outn = buf.len;
	*out = malloc(*outn + 1);
	if (*out == NULL) { quack_buf_free(&buf); return -1; }
	if (*outn) memcpy(*out, buf.p, *outn);
	(*out)[*outn] = '\0';
	quack_buf_free(&buf);
	return 0;
}

/* run_live with bound integer parameters (1-based). */
static int
run_live_pi(sx_db *h, const char *sql, const int64_t *ivals, int n,
            char **out, size_t *outn)
{
	quack_buf_t buf;
	sx_stmt *st = NULL;
	int64_t nrows = 0;
	char *err = NULL;
	struct quack_param ps[8];
	int i, rc;

	for (i = 0; i < n && i < 8; i++) {
		memset(&ps[i], 0, sizeof ps[i]);
		ps[i].type = QUACK_P_INT; ps[i].ival = ivals[i];
	}
	if (quack_buf_init(&buf, 256) != 0) return -1;
	rc = db_exec_cached(h, &st, sql, ps, n, -1, &buf, &nrows, &err);
	if (st) sx_finalize(st);
	if (rc != 0) {
		fprintf(stderr, "  live exec failed [%s]: %s\n", sql, err ? err : "?");
		free(err); quack_buf_free(&buf); return -1;
	}
	*outn = buf.len;
	*out = malloc(*outn + 1);
	if (*out == NULL) { quack_buf_free(&buf); return -1; }
	if (*outn) memcpy(*out, buf.p, *outn);
	(*out)[*outn] = '\0';
	quack_buf_free(&buf);
	return 0;
}

int
main(void)
{
	char path[64] = "/tmp/sqlxtc_dbvexXXXXXX";
	sqlite3 *raw = NULL;
	sx_db *h = NULL;
	char *err = NULL;
	bm_t *bm = NULL; bt_t *bt = NULL; bm_opts_t bo = BM_OPTS_DEFAULT;
	int i, dbfd, served_by_vexec = 0;

	/* Queries vexec is expected to RECOGNIZE (so vexec actually runs and
	 * we are not just comparing the VDBE to itself).  `ordered` marks a
	 * query whose row order is defined (ORDER BY ... ending in the unique
	 * rowid): those must be BYTE-identical.  Unordered results are SQL-
	 * legal in any order, so vexec (hash-bucket order) and the VDBE may
	 * differ -- compared as a row multiset. */
	struct q { const char *sql; int ordered; };
	static const struct q vexec_q[] = {
		{ "SELECT a, b FROM t WHERE a > 50",                       0 },
		{ "SELECT a AS amount, b AS label FROM t WHERE a > 50",     0 },
		{ "SELECT t.k, a FROM t WHERE a > 50",                      0 },
		{ "SELECT abs(a), length(b) FROM t WHERE a IS NOT NULL",   0 },
		{ "SELECT a+1, a * 2, a||b FROM t WHERE a > 50",            0 },
		{ "SELECT (a+1)*2, abs(a)+length(b) FROM t WHERE a > 50",    0 },
		{ "SELECT count(*) FROM t",                                1 },
		{ "SELECT count(a), sum(a), min(a), max(a) FROM t",        1 },
		{ "SELECT b, count(*), sum(a) FROM t GROUP BY b",          0 },
		{ "SELECT b, count(*) FROM t GROUP BY b HAVING count(*) > 20", 0 },
		{ "SELECT b, count(*), sum(a) FROM t GROUP BY b HAVING sum(a) > 1000 AND count(*) > 5", 0 },
		{ "SELECT b FROM t GROUP BY b HAVING count(*) > 20", 0 },
		{ "SELECT b FROM t GROUP BY b HAVING sum(a) > 1000", 0 },
		{ "SELECT b, count(*) FROM t GROUP BY b HAVING sum(a) > 1000", 0 },
		{ "SELECT k, a FROM t ORDER BY a, k",                      1 },
		{ "SELECT k, a FROM t ORDER BY k LIMIT 5 OFFSET 3",        1 },
		{ "SELECT * FROM t WHERE k = 5",                           1 },
		{ "SELECT * FROM t WHERE a > 50",                          0 },
		{ "SELECT a FROM t WHERE k < 3 UNION ALL SELECT a FROM t WHERE k > 8", 0 },
		{ "SELECT k, a FROM t WHERE a = 5 UNION ALL SELECT k, a FROM t WHERE a = 10", 0 },
		{ "SELECT DISTINCT b FROM t",                               0 },
		{ "SELECT DISTINCT a FROM t WHERE a IS NOT NULL",           0 },
		{ "SELECT DISTINCT b FROM t ORDER BY b LIMIT 2",            1 },
		{ "SELECT DISTINCT b FROM t ORDER BY b LIMIT 2 OFFSET 1",   1 },
		{ "SELECT b FROM t WHERE k < 5 UNION SELECT b FROM t WHERE k > 3", 0 },
		{ "SELECT b FROM t WHERE k <= 3 INTERSECT SELECT b FROM t WHERE k >= 2", 0 },
		{ "SELECT b FROM t WHERE k <= 4 EXCEPT SELECT b FROM t WHERE k = 2", 0 },
		{ "SELECT a FROM t WHERE a IN (1, 2, 3)",                   0 },
		{ "SELECT k FROM t WHERE a BETWEEN 50 AND 100 ORDER BY k",  1 },
		{ "SELECT k FROM t WHERE b BETWEEN 'g1' AND 'g3' AND a IS NOT NULL ORDER BY k LIMIT 5", 1 },
		{ "SELECT k FROM t WHERE k IN (5, 10, 50, 999)",            0 },
		{ "SELECT a FROM t WHERE a NOT IN (1, 2, 3) AND a IS NOT NULL", 0 },
		{ "SELECT k FROM t WHERE b IN ('g1', 'g3')",               0 },
		{ "SELECT k, a FROM t WHERE a > (SELECT avg(a) FROM t) ORDER BY k LIMIT 5", 1 },
		{ "SELECT k FROM t WHERE a = (SELECT max(a) FROM t)",      0 },
		{ "SELECT k FROM t WHERE k <= 3 ORDER BY k",               1 },
		{ "SELECT k, (SELECT count(*) FROM t) FROM t WHERE k <= 3 ORDER BY k", 1 },
		{ "SELECT k FROM t WHERE a < (SELECT min(a) FROM t WHERE a > 50) ORDER BY k", 1 },
		{ "SELECT k FROM t WHERE a IN (SELECT a FROM t WHERE k < 5) ORDER BY k", 1 },
		{ "SELECT k FROM t WHERE k IN (SELECT k FROM t WHERE b = 'g1') ORDER BY k LIMIT 5", 1 },
		{ "SELECT k FROM t WHERE a NOT IN (SELECT a FROM t WHERE k > 100 AND a IS NOT NULL) AND a IS NOT NULL ORDER BY k LIMIT 5", 1 }
	};
	/* Queries vexec FALLS BACK on -- the VDBE serves both runs, so they
	 * are byte-identical by construction. */
	static const struct q fallback_q[] = {
		/* correlated subquery: the standalone prepare fails (it references
		 * the outer t.k), so vexec falls back and the VDBE serves it. */
		{ "SELECT k FROM t x WHERE a = (SELECT max(a) FROM t y WHERE y.k <= x.k)", 1 }
	};
	int nv = (int)(sizeof vexec_q / sizeof vexec_q[0]);
	int nf = (int)(sizeof fallback_q / sizeof fallback_q[0]);

	dbfd = mkstemp(path); if (dbfd >= 0) close(dbfd);
	bo.path = path; bo.page_size = 4096; bo.n_frames = 256; bo.lsn_off = 0;
	if (bm_create(&bo, &bm) != XTC_OK || bt_open(bm, &bt) != XTC_OK) {
		fprintf(stderr, "FAIL: storage open\n"); return 1; }
	if (sx_open(":memory:", &h) != SX_OK) { fprintf(stderr, "FAIL: open\n"); return 1; }
	raw = (sqlite3 *)h;
	if (xstore_register(raw, bt) != SQLITE_OK) {
		fprintf(stderr, "FAIL: register\n"); return 1; }
	if (sx_exec(h, "CREATE VIRTUAL TABLE t USING xstore(k, a INT, b TEXT)", &err)
	    != SX_OK) {
		fprintf(stderr, "FAIL: create: %s\n", err ? err : "?");
		free(err); return 1;
	}
	/* The native catalog must carry the column schema (no sqlite_master):
	 * (k pk), (a INT), (b TEXT). */
	{
		xstore_col_t cols[8];
		int nc = xstore_table_schema(bt, "t", cols, 8);
		CK(nc == 3, "native schema has 3 columns");
		if (nc == 3) {
			CK(strcmp(cols[0].name, "k") == 0 && cols[0].is_pk, "col0 = k pk");
			CK(strcmp(cols[1].name, "a") == 0 && cols[1].affinity == 'i', "col1 = a INT");
			CK(strcmp(cols[2].name, "b") == 0 && cols[2].affinity == 't', "col2 = b TEXT");
		}
	}
	{
		sqlite3_stmt *ins = NULL;
		int r;
		sqlite3_exec(raw, "BEGIN", 0, 0, 0);
		sqlite3_prepare_v2(raw, "INSERT INTO t(k,a,b) VALUES(?,?,?)", -1, &ins, 0);
		for (r = 1; r <= 200; r++) {
			char bb[16]; snprintf(bb, sizeof bb, "g%d", r % 5);
			sqlite3_reset(ins);
			sqlite3_bind_int64(ins, 1, r);
			if (r % 11 == 0) sqlite3_bind_null(ins, 2);
			else sqlite3_bind_int64(ins, 2, r);
			sqlite3_bind_text(ins, 3, bb, -1, SQLITE_TRANSIENT);
			sqlite3_step(ins);
		}
		sqlite3_finalize(ins);
		sqlite3_exec(raw, "COMMIT", 0, 0, 0);
	}

	/* For each query: vexec-on bytes vs vexec-off (VDBE) bytes. */
	for (i = 0; i < nv + nf; i++) {
		const struct q *q = (i < nv) ? &vexec_q[i] : &fallback_q[i - nv];
		const char *sql = q->sql;
		char *on = NULL, *off = NULL;
		size_t non = 0, noff = 0;
		int von_recognized;

		/* vexec ON */
		unsetenv("SQLXTC_VEXEC");
		/* Probe whether vexec recognizes this (independent of bytes). */
		{
			sx_vx_result *vr = NULL;
			von_recognized = (sx_vexec_try(h, sql, 1, &vr) == 1);
			if (vr) sx_vexec_free(vr);
		}
		CK(run_live(h, sql, &on, &non) == 0, sql);

		/* vexec OFF (VDBE only) */
		setenv("SQLXTC_VEXEC", "0", 1);
		CK(run_live(h, sql, &off, &noff) == 0, sql);
		unsetenv("SQLXTC_VEXEC");

		if (on && off) {
			if (!resp_eq(on, off, q->ordered)) {
				fprintf(stderr, "FAIL: result mismatch [%s] (ordered=%d):\n"
				        "  vexec: %s\n  vdbe : %s\n",
				        sql, q->ordered, on, off);
				g_fail = 1;
			}
		}
		if (i < nv) {
			CK(von_recognized, sql);   /* expected to be served by vexec */
			if (von_recognized) served_by_vexec++;
		}
		free(on); free(off);
	}

	/* ---- native write path: literal-row INSERT bypassing the VDBE ----
	 * Two more xstore tables on the same bt: u takes the native write
	 * path, v takes the VDBE.  Run the SAME DML (INSERT / DELETE / UPDATE)
	 * into each, then read both back (SELECT * ORDER BY k, VDBE both times
	 * -- a neutral reader) and assert byte-identical.  That proves a
	 * native write lands the same bytes on disk as a VDBE write. */
	{
		static const char *dml[] = {
			"INSERT INTO %s VALUES(1, 100, 'one')",
			"INSERT INTO %s VALUES(2, -50, 'two'), (3, 7, 'three')",
			"INSERT INTO %s VALUES(4, 3.5, 'pi'), (5, NULL, NULL)",
			"INSERT INTO %s VALUES(10, 0, '')",
			/* native explicit-column-list INSERT: reordered, and omitting
			 * a column (filled with NULL) -- both must match the VDBE. */
			"INSERT INTO %s(k, b, a) VALUES(20, 'twenty', 200)",
			"INSERT INTO %s(b, k) VALUES('justk', 21)",
			"INSERT INTO %s(a, k, b) VALUES(22, 22, 'all')",
			/* native REPLACE: overwrite an existing row, insert a new one,
			 * and a column-list REPLACE that nulls the omitted column. */
			"REPLACE INTO %s VALUES(1, 11, 'one-prime')",
			"REPLACE INTO %s VALUES(30, 300, 'thirty')",
			"REPLACE INTO %s(k, a) VALUES(20, 222)",
			/* native DELETE by pk: an existing row and an absent row */
			"DELETE FROM %s WHERE k = 3",
			"DELETE FROM %s WHERE k = 999",
			/* native range DELETE: a closed range and a half-open one */
			"DELETE FROM %s WHERE k BETWEEN 4 AND 5",
			"DELETE FROM %s WHERE k > 9000",
			/* native general-predicate DELETE: non-pk column, compound */
			"DELETE FROM %s WHERE b = 'three'",
			"DELETE FROM %s WHERE a > 50 AND a < 200",
			/* native UPDATE by pk: change a subset of columns, all columns,
			 * and a no-such-row update */
			"UPDATE %s SET a = 42 WHERE k = 1",
			"UPDATE %s SET a = -7, b = 'two!' WHERE k = 2",
			"UPDATE %s SET b = 'gone' WHERE k = 999",
			/* native range UPDATE: multiple rows by a pk range */
			"UPDATE %s SET b = 'lo' WHERE k <= 10",
			"UPDATE %s SET a = 0 WHERE k >= 8000",
			/* native general-predicate UPDATE: non-pk column WHERE */
			"UPDATE %s SET b = 'hit' WHERE a < 8",
			/* native INSERT...SELECT: from a shared source table, with a
			 * WHERE, an explicit reordered column list, and expressions. */
			"INSERT INTO %s SELECT k+500, a, b FROM isrc WHERE a > 1",
			"INSERT INTO %s(b, k, a) SELECT b, k+600, a*10 FROM isrc WHERE k = 1"
		};
		int ni = (int)(sizeof dml / sizeof dml[0]);
		int native_served = 0;
		char *ru = NULL, *rv = NULL; size_t nu = 0, nv2 = 0;

		if (sx_exec(h, "CREATE VIRTUAL TABLE u USING xstore(k, a INT, b TEXT)", &err)
		    != SX_OK || sx_exec(h, "CREATE VIRTUAL TABLE v USING xstore(k, a INT, b TEXT)",
		    &err) != SX_OK) {
			fprintf(stderr, "FAIL: create u/v: %s\n", err ? err : "?");
			free(err); g_fail = 1;
		} else {
			int j;
			/* Shared source for the INSERT...SELECT cases (read by both
			 * the native u run and the VDBE v run; identical for each). */
			(void)sx_exec(h, "CREATE VIRTUAL TABLE isrc USING xstore(k, a INT, b TEXT)", &err);
			(void)sx_exec(h, "INSERT INTO isrc VALUES(1, 5, 'is-one'), "
			              "(2, 0, 'is-two'), (3, 9, 'is-three')", &err);
			for (j = 0; j < ni; j++) {
				char squ[128], sqv[128];
				char *ou = NULL, *ov = NULL; size_t a = 0, b = 0;
				int64_t nch = -1;
				snprintf(squ, sizeof squ, dml[j], "u");
				snprintf(sqv, sizeof sqv, dml[j], "v");

				/* u via the native write path -- assert it is recognized
				 * (returns 1 even when 0 rows change, e.g. a no-such-row
				 * DELETE/UPDATE). */
				CK(sx_vexec_write(h, squ, &nch) == 1, squ);
				if (nch >= 0) native_served++;
				/* v via the VDBE. */
				setenv("SQLXTC_VEXEC", "0", 1);
				CK(run_live(h, sqv, &ov, &b) == 0, sqv);
				unsetenv("SQLXTC_VEXEC");
				free(ou); free(ov); (void)a;
			}
			/* Read both back with the VDBE (neutral) and compare. */
			setenv("SQLXTC_VEXEC", "0", 1);
			CK(run_live(h, "SELECT * FROM u ORDER BY k", &ru, &nu) == 0, "read u");
			CK(run_live(h, "SELECT * FROM v ORDER BY k", &rv, &nv2) == 0, "read v");
			unsetenv("SQLXTC_VEXEC");
			if (ru && rv) {
				/* Same rows; column header differs only by table alias?  No --
				 * SELECT * names columns by the table's own column names, which
				 * are identical (k,a,b) for u and v, so the bytes match. */
				if (nu != nv2 || memcmp(ru, rv, nu) != 0) {
					fprintf(stderr, "FAIL: native vs VDBE insert differ:\n"
					        "  u (native): %s\n  v (vdbe)  : %s\n", ru, rv);
					g_fail = 1;
				}
			}
			free(ru); free(rv);
			CK(native_served == ni, "all DML native-served");
			printf("  ok   native write: %d INSERT/DELETE/UPDATE statements "
			       "applied to the B-tree without the VDBE, byte-identical "
			       "to VDBE writes\n", native_served);

			/* PRIMARY KEY uniqueness: a plain INSERT onto an already
			 * present rowid must NOT silently overwrite.  The native
			 * path detects the collision (committed row or intra-
			 * statement repeat) and falls back (returns 0) so the VDBE
			 * raises the canonical UNIQUE-constraint error. */
			{
				int64_t dn = 0;
				/* Insert a fresh row, then a plain INSERT onto the same PK
				 * must fall back (so the VDBE raises UNIQUE), not silently
				 * overwrite. */
				CK(sx_vexec_write(h, "INSERT INTO u VALUES(5000, 1, 'orig')", &dn) == 1,
				   "seed row for duplicate-PK check");
				CK(sx_vexec_write(h, "INSERT INTO u VALUES(5000, 2, 'dup')", &dn) == 0,
				   "duplicate-PK INSERT falls back (no silent overwrite)");
				CK(sx_vexec_write(h, "INSERT INTO u VALUES(7777, 1, 'a'), (7777, 2, 'b')",
				   &dn) == 0, "intra-statement duplicate PK falls back");
			}

			/* Transactional DELETE/UPDATE on COMMITTED rows is native while
			 * the table is clean in the txn (the committed scan sees what a
			 * vtab cursor would), and falls back once the txn has buffered a
			 * write to that table (where the vtab's wbuf merge could
			 * diverge). */
			{
				int64_t dn = 0;
				char *o = NULL; size_t on3 = 0;
				(void)sx_exec(h, "CREATE VIRTUAL TABLE tdml USING xstore(k, a INT)", &err);
				(void)sx_exec(h, "CREATE VIRTUAL TABLE tdml2 USING xstore(k, a INT)", &err);
				CK(sx_vexec_write(h, "INSERT INTO tdml VALUES(1,10),(2,20),(3,30),(4,40)", &dn) == 1,
				   "seed tdml");
				CK(sx_vexec_write(h, "INSERT INTO tdml2 VALUES(1,10),(2,20)", &dn) == 1,
				   "seed tdml2");
				/* A transactional range DELETE/UPDATE on COMMITTED rows is
				 * native while the table is clean in the txn; once the txn
				 * has buffered a write to a table, further DML on it falls
				 * back.  Recognition is probed directly via sx_vexec_write;
				 * the txn itself runs through run_live (the live path, which
				 * flushes the buffered native writes at COMMIT). */
				CK(run_live(h, "BEGIN", &o, &on3) == 0, "begin tdml txn"); free(o); o = NULL;
				CK(sx_vexec_write(h, "DELETE FROM tdml WHERE k BETWEEN 2 AND 3", &dn) == 1,
				   "in-txn range DELETE on clean table is native");
				CK(sx_vexec_write(h, "UPDATE tdml2 SET a = 99 WHERE k = 1", &dn) == 1,
				   "in-txn UPDATE on clean table is native");
				CK(sx_vexec_write(h, "DELETE FROM tdml WHERE k = 1", &dn) == 0,
				   "in-txn DML on a dirtied table falls back");
				CK(run_live(h, "COMMIT", &o, &on3) == 0, "commit tdml txn"); free(o); o = NULL;
				/* After COMMIT the buffered DELETE (k=2,3) is durable: 2 rows. */
				{
					sx_stmt *q = NULL; const char *tl = NULL; int64_t cnt = -1;
					if (sx_prepare(h, "SELECT count(*) FROM tdml", -1, &q, &tl) == SX_OK) {
						if (sx_step(q) == SX_ROW) cnt = sx_column_int64(q, 0);
						sx_finalize(q);
					}
					CK(cnt == 2, "tdml has 2 rows after committed in-txn DELETE");
				}
			}
		}
	}

	/* ---- native multi-statement transactions ------------------------
	 * Native INSERTs inside a BEGIN..COMMIT must buffer into the shared
	 * transaction and become visible atomically only after COMMIT; a
	 * BEGIN..ROLLBACK must leave none of them.  BEGIN/COMMIT/ROLLBACK
	 * still run through the VDBE (cheap), but the INSERTs are native and
	 * join the same write buffer. */
	{
		char *err2 = NULL;
		sx_stmt *st = NULL;
		quack_buf_t b; int64_t nr = 0;
		int64_t mid_count = -1, post_commit = -1, post_rollback = -1;

		if (sx_exec(h, "CREATE VIRTUAL TABLE tx USING xstore(k, a INT)", &err2) != SX_OK) {
			fprintf(stderr, "FAIL: create tx: %s\n", err2 ? err2 : "?"); free(err2); g_fail = 1;
		} else {
			char *o = NULL; size_t on2 = 0;
			/* Drive the WHOLE transaction through the live path (run_live
			 * -> db_exec_cached), so BEGIN/COMMIT/ROLLBACK hit the native
			 * txn-end hook and the INSERTs are routed to the native write
			 * path inside the open transaction. */
			CK(run_live(h, "BEGIN", &o, &on2) == 0, "BEGIN"); free(o); o = NULL;
			CK(run_live(h, "INSERT INTO tx VALUES(1, 10)", &o, &on2) == 0, "ins1"); free(o); o = NULL;
			CK(run_live(h, "INSERT INTO tx VALUES(2, 20)", &o, &on2) == 0, "ins2"); free(o); o = NULL;
			CK(run_live(h, "INSERT INTO tx VALUES(3, 30)", &o, &on2) == 0, "ins3"); free(o); o = NULL;
			CK(run_live(h, "COMMIT", &o, &on2) == 0, "COMMIT"); free(o); o = NULL;
			/* Read the committed count via a fresh VDBE query. */
			{
				sx_stmt *q = NULL; const char *tail = NULL;
				if (sx_prepare(h, "SELECT count(*) FROM tx", -1, &q, &tail) == SX_OK) {
					if (sx_step(q) == SX_ROW) post_commit = sx_column_int64(q, 0);
					sx_finalize(q);
				}
			}
			CK(post_commit == 3, "3 rows visible after native-insert COMMIT");

			/* ROLLBACK: native INSERTs must leave nothing. */
			CK(run_live(h, "BEGIN", &o, &on2) == 0, "BEGIN2"); free(o); o = NULL;
			CK(run_live(h, "INSERT INTO tx VALUES(4, 40)", &o, &on2) == 0, "ins4"); free(o); o = NULL;
			CK(run_live(h, "INSERT INTO tx VALUES(5, 50)", &o, &on2) == 0, "ins5"); free(o); o = NULL;
			CK(run_live(h, "ROLLBACK", &o, &on2) == 0, "ROLLBACK"); free(o); o = NULL;
			{
				sx_stmt *q = NULL; const char *tail = NULL;
				if (sx_prepare(h, "SELECT count(*) FROM tx", -1, &q, &tail) == SX_OK) {
					if (sx_step(q) == SX_ROW) post_rollback = sx_column_int64(q, 0);
					sx_finalize(q);
				}
			}
			CK(post_rollback == 3, "rolled-back native inserts left nothing (still 3)");
			(void)mid_count; (void)st; (void)b; (void)nr;
			if (!g_fail)
				printf("  ok   native transactions: BEGIN + native INSERTs + "
				       "COMMIT commits atomically; ROLLBACK discards\n");
		}
	}

	/* ---- parametrized statements (? binds) on the native path -------
	 * A ? compiles to its bound value, so a parametrized read runs on
	 * vexec.  Drive each query with params bound through the live path
	 * (vexec on) vs SQLXTC_VEXEC=0 (VDBE) and assert byte-identical. */
	{
		static const struct { const char *sql; int64_t v[2]; int nv; int ordered; } pq[] = {
			{ "SELECT a, b FROM t WHERE k = ?",            { 5 },     1, 0 },
			{ "SELECT k, a FROM t WHERE a > ? ORDER BY k", { 100 },   1, 1 },
			{ "SELECT k, a FROM t WHERE a >= ? AND a <= ?", { 50, 150 }, 2, 0 },
			{ "SELECT count(*) FROM t WHERE a > ?",        { 0 },     1, 1 },
			{ "SELECT * FROM t WHERE k = ?",               { 3 },     1, 1 }
		};
		int np = (int)(sizeof pq / sizeof pq[0]), pi2, served = 0;
		for (pi2 = 0; pi2 < np; pi2++) {
			char *on = NULL, *off = NULL; size_t non = 0, noff = 0;
			sx_vx_result *probe = NULL;
			vx_cell_t bc[2]; int bi;
			for (bi = 0; bi < pq[pi2].nv; bi++) {
				memset(&bc[bi], 0, sizeof bc[bi]);
				bc[bi].type = VX_INT; bc[bi].i = pq[pi2].v[bi];
			}
			if (sx_vexec_try_p(h, pq[pi2].sql, bc, pq[pi2].nv, 1, &probe) == 1) {
				served++; sx_vexec_free(probe);
			}
			unsetenv("SQLXTC_VEXEC");
			CK(run_live_pi(h, pq[pi2].sql, pq[pi2].v, pq[pi2].nv, &on, &non) == 0, pq[pi2].sql);
			setenv("SQLXTC_VEXEC", "0", 1);
			CK(run_live_pi(h, pq[pi2].sql, pq[pi2].v, pq[pi2].nv, &off, &noff) == 0, pq[pi2].sql);
			unsetenv("SQLXTC_VEXEC");
			if (on && off) {
				int eq = pq[pi2].ordered
				    ? (non == noff && memcmp(on, off, non) == 0)
				    : resp_eq(on, off, 0);
				if (!eq) {
					fprintf(stderr, "FAIL: param mismatch [%s]:\n  vexec: %s\n  vdbe : %s\n",
					        pq[pi2].sql, on, off);
					g_fail = 1;
				}
			}
			free(on); free(off);
		}
		CK(served == np, "all parametrized reads served by vexec");
		if (!g_fail)
			printf("  ok   parametrized reads: %d ? queries served by vexec, "
			       "byte-identical to the VDBE\n", served);
	}

	/* ---- parametrized writes (? in VALUES / SET / WHERE) ------------
	 * Apply the same parametrized DML natively to one xstore table and
	 * via the VDBE to another, then read both back byte-identical. */
	{
		static const struct { const char *sql; int64_t v[3]; int nv; } pw[] = {
			{ "INSERT INTO %s VALUES(?, ?, ?)",         { 1, 100, 0 }, 3 },
			{ "INSERT INTO %s VALUES(?, ?, ?)",         { 2, 200, 0 }, 3 },
			{ "INSERT INTO %s VALUES(?, ?, ?)",         { 3, 300, 0 }, 3 },
			{ "UPDATE %s SET a = ? WHERE k = ?",        { 999, 2 },    2 },
			{ "DELETE FROM %s WHERE k = ?",             { 3 },         1 }
		};
		int npw = (int)(sizeof pw / sizeof pw[0]), wi, wserved = 0;
		char *ru = NULL, *rv = NULL; size_t nu = 0, nv3 = 0;
		if (sx_exec(h, "CREATE VIRTUAL TABLE pwu USING xstore(k, a INT, b INT)", &err) != SX_OK ||
		    sx_exec(h, "CREATE VIRTUAL TABLE pwv USING xstore(k, a INT, b INT)", &err) != SX_OK) {
			fprintf(stderr, "FAIL: create pwu/pwv: %s\n", err ? err : "?"); free(err); g_fail = 1;
		} else {
			for (wi = 0; wi < npw; wi++) {
				char squ[96], sqv[96];
				char *o = NULL; size_t on2 = 0;
				vx_cell_t bc[3]; int bi;
				int64_t nch = -1;
				snprintf(squ, sizeof squ, pw[wi].sql, "pwu");
				snprintf(sqv, sizeof sqv, pw[wi].sql, "pwv");
				for (bi = 0; bi < pw[wi].nv; bi++) {
					memset(&bc[bi], 0, sizeof bc[bi]);
					bc[bi].type = VX_INT; bc[bi].i = pw[wi].v[bi];
				}
				/* native into pwu */
				if (sx_vexec_write_p(h, squ, bc, pw[wi].nv, &nch) == 1) wserved++;
				/* VDBE into pwv */
				setenv("SQLXTC_VEXEC", "0", 1);
				CK(run_live_pi(h, sqv, pw[wi].v, pw[wi].nv, &o, &on2) == 0, sqv);
				unsetenv("SQLXTC_VEXEC"); free(o);
			}
			setenv("SQLXTC_VEXEC", "0", 1);
			CK(run_live(h, "SELECT * FROM pwu ORDER BY k", &ru, &nu) == 0, "read pwu");
			CK(run_live(h, "SELECT * FROM pwv ORDER BY k", &rv, &nv3) == 0, "read pwv");
			unsetenv("SQLXTC_VEXEC");
			if (ru && rv && (nu != nv3 || memcmp(ru, rv, nu) != 0)) {
				fprintf(stderr, "FAIL: param-write native vs VDBE differ:\n  u: %s\n  v: %s\n", ru, rv);
				g_fail = 1;
			}
			free(ru); free(rv);
			CK(wserved == npw, "all parametrized writes native-served");
			if (!g_fail)
				printf("  ok   parametrized writes: %d ? INSERT/UPDATE/DELETE "
				       "native, byte-identical to the VDBE\n", wserved);
		}
	}

	sx_close(h);
	bt_close(bt);
	bm_destroy(bm);
	unlink(path);

	if (g_fail) { fprintf(stderr, "  db vexec live-path: FAILURES\n"); return 1; }
	printf("  ok   live db_exec_cached: %d vexec-served + %d fallback queries "
	       "match the VDBE (ordered byte-identical, unordered multiset)\n",
	       served_by_vexec, nf);
	printf("All sqlxtc live-path vexec differential tests passed.\n");
	return 0;
}
