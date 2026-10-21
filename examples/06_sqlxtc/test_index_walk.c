/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_index_walk.c
 *	A secondary-index range walk reads its entries a leaf at a time.
 *
 *	W1  forward: one account's 2,000 entries (several leaves) cost
 *	    O(rows / batch) cursor resumes, not one resume per entry.
 *	W2  reverse (ORDER BY ... DESC): the walk reads a leaf's entries
 *	    per descent, not one root-to-leaf descent (bt_seek_lt) per
 *	    entry.  Counted in buffer-pool page fixes, which a per-entry
 *	    descent multiplies by the tree height.
 *	W3  both directions, with and without bounds, LIMIT, an equality
 *	    prefix of 0 or 1 column, and this transaction's buffered
 *	    inserts / updates / deletes merged in, agree with a brute-force
 *	    oracle over the same rows.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "bufmgr.h"
#include "btree.h"
#include "xstore.h"
#include "xtc.h"
#include "t_tmp.h"

#define PAGE_SZ  4096
#define N_FRAMES 512
#define N_ACCT   20
#define N_ROWS   8000        /* 400 per account; account 0 gets 2,000 more */
#define T_CAP    (N_ROWS + 2000 + 200)   /* rowids 1.. incl. buffered inserts */

static int64_t g_acct[T_CAP], g_ts[T_CAP];
static int     g_live[T_CAP];
static int     g_nrow;

/* The k column of the rows `sql` returns, into out (cap); the count. */
static int
run_k(sx_db *db, const char *sql, int64_t *out, int cap)
{
	sx_stmt *st = NULL;
	int n = 0;

	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK) {
		fprintf(stderr, "prepare failed: %s\n", sql);
		return -1;
	}
	while (sx_step(st) == SX_ROW) {
		if (n < cap) out[n] = sx_column_int64(st, 0);
		n++;
	}
	sx_finalize(st);
	return n;
}

/* Oracle: rows with acct == a (a < 0: any) and lo <= ts <= hi, ordered
 * by (acct, ts, k), ascending or descending, at most lim (< 0: all). */
static int
oracle(int64_t a, int64_t lo, int64_t hi, int desc, int lim, int64_t *out)
{
	static int idx[T_CAP];
	int i, j, n = 0;

	for (i = 1; i <= g_nrow; i++)
		if (g_live[i] && (a < 0 || g_acct[i] == a) && g_ts[i] >= lo &&
		    g_ts[i] <= hi)
			idx[n++] = i;
	for (i = 1; i < n; i++) {         /* insertion sort on (acct, ts, k) */
		int x = idx[i];
		for (j = i - 1; j >= 0; j--) {
			int y = idx[j];
			int gt = g_acct[y] != g_acct[x] ? g_acct[y] > g_acct[x] :
			    g_ts[y] != g_ts[x] ? g_ts[y] > g_ts[x] : y > x;
			if (!gt) break;
			idx[j + 1] = y;
		}
		idx[j + 1] = x;
	}
	if (lim < 0 || lim > n) lim = n;
	for (i = 0; i < lim; i++)
		out[i] = desc ? idx[n - 1 - i] : idx[i];
	return lim;
}

static int
same(const int64_t *a, int na, const int64_t *b, int nb)
{
	return na == nb && (na == 0 || memcmp(a, b, sizeof *a * (size_t)na) == 0);
}

static int
check_all(sx_db *db, const char *tag)
{
	static int64_t got[T_CAP], want[T_CAP];
	char sql[256];
	int a, d, l, ng, nw, bad = 0, cases = 0;
	const int64_t accts[] = { 0, 3, 19, 99 };
	const int lims[] = { -1, 1, 20, 700 };

	for (a = 0; a < 4; a++)
		for (d = 0; d < 2; d++)
			for (l = 0; l < 4; l++) {
				int b;
				for (b = 0; b < 3; b++) {
					int64_t lo = b == 0 ? INT64_MIN : b == 1 ? 100 : 1500;
					int64_t hi = b == 0 ? INT64_MAX : b == 1 ? 1900 : 1500;
					char lim[32] = "", rng[96] = "";
					if (lims[l] >= 0)
						snprintf(lim, sizeof lim, " LIMIT %d", lims[l]);
					if (b == 1)
						snprintf(rng, sizeof rng,
						    " AND ts >= 100 AND ts <= 1900");
					else if (b == 2)
						snprintf(rng, sizeof rng, " AND ts = 1500");
					snprintf(sql, sizeof sql, "SELECT k FROM t WHERE acct = "
					    "%lld%s ORDER BY ts %s, k %s%s", (long long)accts[a],
					    rng, d ? "DESC" : "ASC", d ? "DESC" : "ASC", lim);
					ng = run_k(db, sql, got, T_CAP);
					nw = oracle(accts[a], lo, hi, d, lims[l], want);
					cases++;
					if (!same(got, ng, want, nw)) {
						if (bad < 5)
							fprintf(stderr, "  %s: %s: got %d rows, want %d\n",
							    tag, sql, ng, nw);
						bad++;
					}
				}
			}
	/* No equality prefix: a range on the leading column. */
	for (d = 0; d < 2; d++) {
		snprintf(sql, sizeof sql, "SELECT k FROM t WHERE acct >= 2 AND "
		    "acct <= 4 ORDER BY acct %s, ts %s, k %s", d ? "DESC" : "ASC",
		    d ? "DESC" : "ASC", d ? "DESC" : "ASC");
		ng = run_k(db, sql, got, T_CAP);
		nw = 0;
		for (a = 2; a <= 4; a++)
			nw += oracle(a, INT64_MIN, INT64_MAX, 0, -1, want + nw);
		if (d)
			for (l = 0; l < nw / 2; l++) {
				int64_t t = want[l];
				want[l] = want[nw - 1 - l];
				want[nw - 1 - l] = t;
			}
		cases++;
		if (!same(got, ng, want, nw)) {
			fprintf(stderr, "  %s: %s: got %d rows, want %d\n", tag, sql,
			    ng, nw);
			bad++;
		}
	}
	CK(bad == 0);
	printf("  %s   W3 %s: %d range queries agree with the oracle (%d wrong)\n",
	    bad ? "FAIL" : "ok", tag, cases, bad);
	return bad;
}

int
main(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	sx_db *db = NULL;
	sx_stmt *st = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bt_stats_t s0, s1;
	bm_stats_t b0, b1;
	static int64_t got[4000];
	char path[256], wal[288], sql[160];
	uint64_t resumes, fixes;
	int fd, i, n;

	t_tmpl(path, sizeof path, "sqlxtc-iwalk");
	fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); return 1; }
	close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = N_FRAMES;
	bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(sx_open_bt(bt, &db) == SX_OK);
	CK(sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, acct INTEGER, "
	    "ts INTEGER, pad TEXT)", NULL) == SX_OK);
	CK(sx_exec(db, "CREATE INDEX t_acct_ts ON t(acct, ts)", NULL) == SX_OK);
	CK(sx_prepare(db, "INSERT INTO t(k,acct,ts,pad) VALUES(?,?,?,'p')", -1,
	    &st, NULL) == SX_OK);
	CK(sx_exec(db, "BEGIN", NULL) == SX_OK);
	for (i = 1; i <= N_ROWS + 2000; i++) {
		g_acct[i] = i <= N_ROWS ? i % N_ACCT : 0;
		g_ts[i] = (i * 7919) % 2000;       /* duplicates: ties on ts */
		g_live[i] = 1;
		sx_bind_int64(st, 1, i);
		sx_bind_int64(st, 2, g_acct[i]);
		sx_bind_int64(st, 3, g_ts[i]);
		CK(sx_step(st) == SX_DONE);
		sx_reset(st);
	}
	g_nrow = N_ROWS + 2000;
	CK(sx_exec(db, "COMMIT", NULL) == SX_OK);
	sx_finalize(st);
	st = NULL;

	/* W1: forward, account 0 (2,400 entries). */
	n = run_k(db, "SELECT k FROM t WHERE acct = 0 ORDER BY ts", got, 4000);
	bt_get_stats(bt, &s0);
	n = run_k(db, "SELECT k FROM t WHERE acct = 0 ORDER BY ts", got, 4000);
	bt_get_stats(bt, &s1);
	resumes = s1.resumes - s0.resumes;
	CK(n == 2400);
	CK(resumes <= (uint64_t)(n / 32));
	printf("  %s   W1 forward: %d index entries, %llu cursor resumes "
	    "(one per entry would be %d)\n", resumes <= (uint64_t)(n / 32) ?
	    "ok" : "FAIL", n, (unsigned long long)resumes, n);

	/* W2: reverse.  A descent per entry fixes height pages per entry,
	 * plus the row fetch; a batched walk fixes about one page per row
	 * (the fetch) plus a descent per batch. */
	n = run_k(db, "SELECT k FROM t WHERE acct = 0 ORDER BY ts DESC", got, 4000);
	bm_get_stats(bm, &b0);
	bt_get_stats(bt, &s0);
	n = run_k(db, "SELECT k FROM t WHERE acct = 0 ORDER BY ts DESC", got, 4000);
	bm_get_stats(bm, &b1);
	bt_get_stats(bt, &s1);
	fixes = (b1.hits + b1.rescues + b1.loads) - (b0.hits + b0.rescues + b0.loads);
	CK(n == 2400);
	CK(s1.height >= 3);
	CK(fixes <= (uint64_t)n * (s1.height + 1) + (uint64_t)n / 4);
	printf("  %s   W2 reverse: %d index entries, %llu page fixes, height "
	    "%llu (a descent per entry: >= %llu)\n",
	    fixes <= (uint64_t)n * (s1.height + 1) + (uint64_t)n / 4 ? "ok" : "FAIL",
	    n, (unsigned long long)fixes, (unsigned long long)s1.height,
	    (unsigned long long)((uint64_t)n * (2 * s1.height)));

	/* W3: committed only, then with buffered writes merged in. */
	check_all(db, "committed");
	CK(sx_exec(db, "BEGIN", NULL) == SX_OK);
	for (i = 1; i <= 120; i++) {           /* buffered inserts */
		int k = g_nrow + 1;
		if (k >= T_CAP) break;
		g_acct[k] = i % 4 == 0 ? 3 : 0;
		g_ts[k] = (i * 37) % 2000;
		g_live[k] = 1;
		g_nrow = k;
		snprintf(sql, sizeof sql, "INSERT INTO t(k,acct,ts,pad) VALUES(%d,"
		    "%lld,%lld,'b')", k, (long long)g_acct[k], (long long)g_ts[k]);
		CK(sx_exec(db, sql, NULL) == SX_OK);
	}
	for (i = 20; i <= N_ROWS; i += 97) {   /* buffered moves and deletes */
		if (i % 2) {
			g_ts[i] = (g_ts[i] + 500) % 2000;
			snprintf(sql, sizeof sql, "UPDATE t SET ts=%lld WHERE k=%d",
			    (long long)g_ts[i], i);
		} else {
			g_live[i] = 0;
			snprintf(sql, sizeof sql, "DELETE FROM t WHERE k=%d", i);
		}
		CK(sx_exec(db, sql, NULL) == SX_OK);
	}
	check_all(db, "in a txn with buffered writes");
	CK(sx_exec(db, "COMMIT", NULL) == SX_OK);
	check_all(db, "after commit");

	sx_close(db);
	bt_close(bt);
	bm_destroy(bm);
	unlink(path);
	snprintf(wal, sizeof wal, "%s-wal", path);
	unlink(wal);
	if (g_fail) {
		fprintf(stderr, "test_index_walk: FAILED\n");
		return 1;
	}
	printf("test_index_walk: ALL PASS\n");
	return 0;
}
