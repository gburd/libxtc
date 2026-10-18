/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/bench_morsel_balance.c
 *	Morsel balance of the parallel vectorized scan on skewed rowids.
 *
 *	vexec's morsel-parallel scan splits a table's rowid span into ~2
 *	morsels per worker.  1.56 cut the span UNIFORMLY, so a table whose
 *	rowids are sparse or skewed puts most rows in one morsel and the
 *	parallel scan runs at the speed of that one morsel.  bt157 cuts at
 *	the B-tree's sampled separators (bt_sample_keys, the RethinkDB
 *	get_distribution analogue), which are equi-depth by construction.
 *
 *	Two tables of the same row count:
 *	  skew  -- 90% of the rows in the top 1% of the rowid space
 *	  dense -- rowids 1..N (the control: both splits should agree)
 *	For each, prints the rows per morsel under the uniform split and
 *	the sampled split (counted by scanning each morsel's rowid range),
 *	the slowest morsel's serial scan time under each, and the wall time
 *	of "SELECT count(*), sum(a) FROM t" through the parallel path.  The
 *	wall time measures whichever vexec this binary links: build it with
 *	1.56's vexec.c for "before" and the current one for "after".
 *
 *	usage: bench_morsel_balance [rows] [workers] [reps]
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xtc.h"
#include "bufmgr.h"
#include "btree.h"
#include "engine.h"
#include "xstore.h"
#include "t_tmp.h"

#define MAXM 1024

static void
put64(uint8_t *p, int64_t v)
{
	int b;

	for (b = 0; b < 8; b++) p[b] = (uint8_t)((uint64_t)v >> (8 * b));
}

/* The morsel count rule of vexec.c morsel_bounds (and 1.56's stride):
 * ~2 per worker, never below 64K rowids per morsel. */
static int64_t
morsel_target(int64_t span, int workers)
{
	int64_t t = span / (int64_t)(workers * 2);

	return t < 65536 ? 65536 : t;
}

static long
scan_count(bt_t *bt, const char *tab, int64_t lo, int64_t hi)
{
	xstore_scan_t *s = xstore_scan_open(bt, tab, 0, lo, 1, hi - 1, 1);
	int64_t rid;
	const uint8_t *rec;
	int rl;
	long n = 0;

	if (s == NULL) return -1;
	while (xstore_scan_next(s, &rid, &rec, &rl) == 1) n++;
	xstore_scan_close(s);
	return n;
}

static void
report(bt_t *bt, const char *tab, const char *label, const int64_t *b, int nm)
{
	long mn = -1, mx = 0, tot = 0, c;
	int64_t t0, d, dmax = 0;
	int i;

	printf("  %-8s %3d morsels, rows:", label, nm);
	for (i = 0; i < nm; i++) {
		t0 = xtc_clock_mono();
		c = scan_count(bt, tab, b[i], b[i + 1]);
		d = xtc_clock_mono() - t0;
		if (d > dmax) dmax = d;
		if (mn < 0 || c < mn) mn = c;
		if (c > mx) mx = c;
		tot += c;
		if (nm <= 32) printf(" %ld", c);
	}
	printf("\n           total %ld min %ld max %ld max/mean %.2f, "
	    "slowest morsel %.2f ms\n", tot, mn, mx,
	    tot > 0 ? (double)mx * nm / (double)tot : 0.0, (double)dmax / 1e6);
}

static double
par_ms(sx_db *db, const char *tab, int workers, int reps, long *rows)
{
	char sql[128];
	double best = 1e30;
	int r;

	snprintf(sql, sizeof sql, "SELECT count(*), sum(a) FROM %s", tab);
	for (r = 0; r < reps; r++) {
		sx_vx_result *res = NULL;
		int64_t t0 = xtc_clock_mono();
		double ms;

		if (sx_vexec_try(db, sql, workers, &res) != 1) return -1;
		ms = (double)(xtc_clock_mono() - t0) / 1e6;
		*rows = (long)sx_vexec_int64(res, 0, 0);
		sx_vexec_free(res);
		if (ms < best) best = ms;
	}
	return best;
}

static int
load(sx_db *db, bt_t *bt, const char *tab, long rows, int skew, int64_t *hi)
{
	char sql[96];
	uint8_t rec[19];
	uint32_t tid;
	const int64_t S = 100000000;      /* rowid space of the skew table */
	long i, sparse = rows / 10;
	int64_t rid;

	snprintf(sql, sizeof sql,
	    "CREATE TABLE %s(k INTEGER PRIMARY KEY, a INT, g INT)", tab);
	if (sx_exec(db, sql, NULL) != SX_OK || !xstore_table_id(bt, tab, &tid))
		return -1;
	rec[0] = 2;
	rec[1] = 1;
	rec[10] = 1;
	for (i = 0; i < rows; i++) {
		if (!skew)
			rid = i + 1;
		else if (i < sparse)        /* 10%: spread over 99% of the span */
			rid = 1 + i * ((S - S / 100) / sparse);
		else                         /* 90%: packed in the top 1% */
			rid = S - S / 100 + 1 + (i - sparse);
		put64(rec + 2, (int64_t)(i % 1000));
		put64(rec + 11, (int64_t)(i % 64));
		if (xstore_put_rec(bt, tid, rid, rec, (int)sizeof rec) != 0)
			return -1;
	}
	*hi = xstore_max_rowid(bt, tid) + 1;
	return (int)tid;
}

int
main(int argc, char **argv)
{
	long rows = argc > 1 ? atol(argv[1]) : 1000000;
	int workers = argc > 2 ? atoi(argv[2]) : 8;
	int reps = argc > 3 ? atoi(argv[3]) : 5;
	static const char *tabs[2] = { "skew", "dense" };
	char path[64];
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	sx_db *db = NULL;
	int fd, t, i, nm, got, tid;
	int64_t hi, span, target, ub[MAXM + 1], sb[MAXM + 1];
	long prow = 0;
	double ms;

	if (workers < 1) workers = 1;
	t_tmpl(path, sizeof path, "morsel");
	fd = mkstemp(path);
	if (fd >= 0) close(fd);
	bo.path = path;
	bo.page_size = 4096;
	bo.n_frames = 65536;               /* 256 MiB: everything resident */
	if (sx_init() != SX_OK || bm_create(&bo, &bm) != XTC_OK ||
	    bt_open(bm, &bt) != XTC_OK || sx_open_bt(bt, &db) != SX_OK) {
		fprintf(stderr, "setup failed\n");
		return 1;
	}
	printf("rows=%ld workers=%d reps=%d (best of reps)\n", rows, workers,
	    reps);
	for (t = 0; t < 2; t++) {
		int64_t t0 = xtc_clock_mono();

		tid = load(db, bt, tabs[t], rows, t == 0, &hi);
		if (tid < 0) { fprintf(stderr, "load %s\n", tabs[t]); return 1; }
		printf("\n== %s: %ld rows, rowids [1, %lld), loaded in %.1f s\n",
		    tabs[t], rows, (long long)hi,
		    (double)(xtc_clock_mono() - t0) / 1e9);
		span = hi - 1;
		target = morsel_target(span, workers);
		nm = (int)((span + target - 1) / target);
		if (nm > MAXM) nm = MAXM;
		ub[0] = sb[0] = 1;
		for (i = 1; i < nm; i++) ub[i] = 1 + (int64_t)i * target;
		ub[nm] = sb[nm] = hi;
		t0 = xtc_clock_mono();
		got = nm > 1 ? xstore_rowid_split_points(bt, (uint32_t)tid, 1, hi,
		    nm - 1, sb + 1) : 0;
		printf("  sampling: %d of %d cut points in %.1f us\n", got,
		    nm - 1, (double)(xtc_clock_mono() - t0) / 1e3);
		if (got != nm - 1)
			memcpy(sb, ub, sizeof(int64_t) * (size_t)(nm + 1));
		report(bt, tabs[t], "uniform", ub, nm);
		report(bt, tabs[t], "sampled", sb, nm);
		ms = par_ms(db, tabs[t], workers, reps, &prow);
		printf("  parallel SELECT count(*), sum(a) at %d workers: %.2f ms "
		    "(count %ld)\n", workers, ms, prow);
		ms = par_ms(db, tabs[t], 1, reps, &prow);
		printf("  serial   SELECT count(*), sum(a):              %.2f ms\n",
		    ms);
	}
	sx_close(db);
	bt_close(bt);
	bm_destroy(bm);
	unlink(path);
	return 0;
}
