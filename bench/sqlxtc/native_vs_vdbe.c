/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * bench/sqlxtc/native_vs_vdbe.c
 *	End-to-end A/B: the SAME SQL workload over the SAME on-disk xstore
 *	B-tree, executed two ways --
 *	  - NATIVE   : the from-scratch sqlxtc engine (Lime parser ->
 *	               sx_classify -> vexec / native write path), no VDBE;
 *	  - VDBE     : SQLite's bytecode engine over the xstore vtab
 *	               (SQLXTC_NATIVE_DRIVER=0).
 *	Both run in-process through db_exec, so the only variable is the
 *	execution engine.  Reports per-query wall-clock and the native
 *	speedup.  Used for the SQLite-vs-sqlxtc comparison report.
 *
 *	usage: native_vs_vdbe [--rows N] [--iters M]
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "engine.h"
#include "db.h"
#include "quack.h"

static double
now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* The read workload: a spread of shapes the report cares about. */
static const char *const QUERIES[] = {
	"SELECT count(*) FROM t",
	"SELECT count(a), sum(a), min(a), max(a) FROM t",
	"SELECT b, count(*), sum(a) FROM t GROUP BY b",
	"SELECT k, a, b FROM t WHERE a > 5000",
	"SELECT k, a FROM t WHERE a > 1000 AND a < 9000 ORDER BY a LIMIT 50",
	"SELECT k, b FROM t WHERE b LIKE 'r5%'",
	"SELECT a FROM t WHERE a IN (SELECT a FROM t WHERE a > 9900)",
};
#define NQ ((int)(sizeof QUERIES / sizeof QUERIES[0]))

static double
run_pass(sx_db *h, int iters, int *out_rows)
{
	quack_buf_t buf;
	double t0, t1;
	int q, it, rows = 0;
	if (quack_buf_init(&buf, 4096) != 0) return -1;
	t0 = now_s();
	for (it = 0; it < iters; it++)
		for (q = 0; q < NQ; q++) {
			int64_t nr = 0; char *err = NULL;
			quack_buf_reset(&buf);
			(void)db_exec(h, QUERIES[q], -1, &buf, &nr, &err);
			if (err) free(err);
			rows += (int)nr;
		}
	t1 = now_s();
	quack_buf_free(&buf);
	if (out_rows) *out_rows = rows;
	return t1 - t0;
}

/* Time ONE query shape over `iters` runs. */
static double
run_one(sx_db *h, const char *q, int iters)
{
	quack_buf_t buf;
	double t0, t1;
	int it;
	if (quack_buf_init(&buf, 4096) != 0) return -1;
	t0 = now_s();
	for (it = 0; it < iters; it++) {
		int64_t nr = 0; char *err = NULL;
		quack_buf_reset(&buf);
		(void)db_exec(h, q, -1, &buf, &nr, &err);
		if (err) free(err);
	}
	t1 = now_s();
	quack_buf_free(&buf);
	return t1 - t0;
}

int
main(int argc, char **argv)
{
	long rows = 20000, iters = 200;
	char store[80], dbf[80];
	sx_db *h = NULL;
	quack_buf_t b;
	int i, seedrows = 0;
	double t_native, t_vdbe;
	int r_native = 0, r_vdbe = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--rows") && i + 1 < argc) rows = atol(argv[++i]);
		else if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atol(argv[++i]);
	}

	(void)snprintf(store, sizeof store, "/tmp/sqlxtc-bench-%ld.xstore", (long)getpid());
	(void)snprintf(dbf, sizeof dbf, "/tmp/sqlxtc-bench-%ld.db", (long)getpid());
	unlink(store); unlink(dbf);

	if (sx_init() != SX_OK) { fprintf(stderr, "sx_init\n"); return 1; }
	if (sx_storage_open(store, 4096) != SX_OK) { fprintf(stderr, "storage\n"); return 1; }

	/* Seed once through the native path (CREATE TABLE + INSERT). */
	sx_native_conn(0);   /* legacy handle: lets us A/B the driver in-process */
	if (sx_open(dbf, &h) != SX_OK) { fprintf(stderr, "open\n"); return 1; }
	if (quack_buf_init(&b, 4096) != 0) return 1;

	/* Create the table as an xstore VTAB (driver off): this registers it
	 * in BOTH SQLite's schema (so the VDBE can read it) AND the xstore
	 * catalog (so the native vexec can read it) -- the only way to A/B
	 * the two engines over the SAME rows in one process. */
	sx_native_driver(0);
	{
		int64_t nr; char *err = NULL;
		quack_buf_reset(&b);
		(void)db_exec(h, "CREATE VIRTUAL TABLE t USING xstore(k, a INT, b TEXT)",
		    -1, &b, &nr, &err);
		if (err) { fprintf(stderr, "create: %s\n", err); free(err); return 1; }
	}
	/* Insert `rows` rows (driver off -> VDBE/vtab, visible to both),
	 * wrapped in one transaction for speed. */
	{
		long r; int64_t nr; char *err = NULL;
		quack_buf_reset(&b); (void)db_exec(h, "BEGIN", -1, &b, &nr, &err); if (err) free(err);
		for (r = 1; r <= rows; r++) {
			char sql[160];
			err = NULL;
			(void)snprintf(sql, sizeof sql,
			    "INSERT INTO t(k,a,b) VALUES(%ld,%ld,'r%ld')",
			    r, (r * 2654435761UL) % 10000, (r * 2654435761UL) % 1000);
			quack_buf_reset(&b);
			if (db_exec(h, sql, -1, &b, &nr, &err) != 0) {
				fprintf(stderr, "insert %ld: %s\n", r, err ? err : "?");
				if (err) free(err); return 1;
			}
			if (err) free(err);
			seedrows++;
		}
		err = NULL;
		quack_buf_reset(&b); (void)db_exec(h, "COMMIT", -1, &b, &nr, &err); if (err) free(err);
	}
	quack_buf_free(&b);
	printf("seeded %d rows into %s\n", seedrows, store);

	/* Warm both engines once (page cache, plan compile). */
	sx_native_driver(1); (void)run_pass(h, 2, NULL);
	sx_native_driver(0); (void)run_pass(h, 2, NULL);

	/* Timed A/B. */
	sx_native_driver(1);
	t_native = run_pass(h, (int)iters, &r_native);
	sx_native_driver(0);
	t_vdbe = run_pass(h, (int)iters, &r_vdbe);

	printf("\n=== native_vs_vdbe : %ld rows, %ld iters x %d queries ===\n",
	    rows, iters, NQ);
	printf("  rows returned: native=%d vdbe=%d %s\n",
	    r_native, r_vdbe, r_native == r_vdbe ? "(match)" : "(MISMATCH!)");
	printf("  NATIVE (sqlxtc vexec) : %8.3f s  (%.0f q/s)\n",
	    t_native, (double)(iters * NQ) / t_native);
	printf("  VDBE   (SQLite)       : %8.3f s  (%.0f q/s)\n",
	    t_vdbe, (double)(iters * NQ) / t_vdbe);
	printf("  speedup (vdbe/native) : %.2fx\n", t_vdbe / t_native);

	/* Per-query breakdown. */
	printf("\n  per-query (iters=%ld):  native    vdbe   vdbe/native\n", iters);
	for (i = 0; i < NQ; i++) {
		double dn, dv;
		sx_native_driver(1); dn = run_one(h, QUERIES[i], (int)iters);
		sx_native_driver(0); dv = run_one(h, QUERIES[i], (int)iters);
		printf("    %-50.50s %6.0fus %6.0fus  %.2fx\n",
		    QUERIES[i], dn / iters * 1e6, dv / iters * 1e6, dv / dn);
	}

	sx_close(h);
	sx_storage_close();
	unlink(store); unlink(dbf);
	{ char wal[96]; (void)snprintf(wal, sizeof wal, "%s-wal", store); unlink(wal); }
	return 0;
}
