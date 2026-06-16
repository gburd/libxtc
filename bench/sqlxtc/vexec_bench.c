/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * bench/sqlxtc/vexec_bench.c
 *	Analytic A/B benchmark: the same query through SQLite's VDBE, the
 *	single-threaded vectorized executor (vexec), and the morsel-
 *	parallel vexec at N worker loops.  Measures the V2/V3 parallelism
 *	claim on the scan / filter / aggregate shapes vectorization is for.
 *
 *	Each query is first run for CORRECTNESS (vexec result count must
 *	equal the VDBE's) and then timed.  Output is one human-readable
 *	block per query plus a trailing CSV line per (query, mode) for
 *	post-processing.
 *
 *	Build (see run_vexec.sh):
 *	  gcc ... vexec_bench.c $EX/{vexec,sql_parse_drv,sql_ast,sql_parse_gen}.c \
 *	      $EX/sqlite3.o $BUILD/libxtc.a -lpthread -ldl -lm -luring ...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "vexec.h"
#include "sqlite3.h"

static uint64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Count rows a VDBE query returns (the correctness/baseline oracle). */
static long
vdbe_count(sqlite3 *db, const char *sql)
{
	sqlite3_stmt *st = NULL;
	long n = 0;
	if (sqlite3_prepare_v2(db, sql, -1, &st, 0) != SQLITE_OK) return -1;
	while (sqlite3_step(st) == SQLITE_ROW) n++;
	sqlite3_finalize(st);
	return n;
}

/* Count rows the single-threaded vexec returns (or -1 if it falls back). */
static long
vexec_count(sqlite3 *db, const char *sql)
{
	vx_stmt_t *vs = NULL; char *e = NULL;
	long n = 0;
	int rc = vx_try_prepare(db, sql, &vs, &e);
	if (rc != 1) { sqlite3_free(e); return -1; }
	while (vx_step(vs) == SQLITE_ROW) n++;
	vx_finalize(vs);
	return n;
}

int
main(int argc, char **argv)
{
	const char *path = "/tmp/sqlxtc_vexec_bench.db";
	long rows = (argc > 1) ? atol(argv[1]) : 2000000;
	sqlite3 *db = NULL; char *err = NULL;
	int reps = 3;
	long r;

	static const char *queries[] = {
		"SELECT count(*) FROM t WHERE a > 500000",
		"SELECT count(*), sum(a), min(a), max(a) FROM t",
		"SELECT g, count(*), sum(a) FROM t GROUP BY g",
		"SELECT t.k, u.c FROM t JOIN u ON t.g = u.j"
	};
	int nq = (int)(sizeof queries / sizeof queries[0]);
	int workers[] = { 1, 2, 4, 8 };
	int nw = (int)(sizeof workers / sizeof workers[0]);
	int qi, wi;

	unlink(path);
	if (sqlite3_open(path, &db) != SQLITE_OK) { fprintf(stderr, "open\n"); return 1; }
	sqlite3_exec(db, "PRAGMA journal_mode=WAL", 0, 0, 0);
	sqlite3_exec(db, "PRAGMA synchronous=NORMAL", 0, 0, 0);
	if (sqlite3_exec(db,
	        "CREATE TABLE t(k INTEGER PRIMARY KEY, a INT, g INT, b TEXT);"
	        "CREATE TABLE u(j INTEGER PRIMARY KEY, c INT)",
	        0, 0, &err) != SQLITE_OK) {
		fprintf(stderr, "create: %s\n", err ? err : "?"); return 1;
	}
	fprintf(stderr, "seeding %ld rows...\n", rows);
	sqlite3_exec(db, "BEGIN", 0, 0, 0);
	{
		sqlite3_stmt *ins = NULL;
		sqlite3_prepare_v2(db, "INSERT INTO t(k,a,g,b) VALUES(?,?,?,?)", -1, &ins, 0);
		for (r = 1; r <= rows; r++) {
			char bb[24]; snprintf(bb, sizeof bb, "row%ld", r % 1000);
			sqlite3_reset(ins);
			sqlite3_bind_int64(ins, 1, r);
			sqlite3_bind_int64(ins, 2, (long)((r * 2654435761u) % 1000000));
			sqlite3_bind_int64(ins, 3, r % 64);          /* 64 groups */
			sqlite3_bind_text(ins, 4, bb, -1, SQLITE_TRANSIENT);
			sqlite3_step(ins);
		}
		sqlite3_finalize(ins);
		/* u: one row per group value so the join is well defined. */
		sqlite3_prepare_v2(db, "INSERT INTO u(j,c) VALUES(?,?)", -1, &ins, 0);
		for (r = 0; r < 64; r++) {
			sqlite3_reset(ins);
			sqlite3_bind_int64(ins, 1, r);
			sqlite3_bind_int64(ins, 2, r * 10);
			sqlite3_step(ins);
		}
		sqlite3_finalize(ins);
	}
	sqlite3_exec(db, "COMMIT", 0, 0, 0);
	fprintf(stderr, "seeded.\n\n");

	printf("rows=%ld  reps=%d\n", rows, reps);
	printf("# CSV: query,mode,workers,ms,rows,speedup_vs_vdbe\n");

	for (qi = 0; qi < nq; qi++) {
		const char *sql = queries[qi];
		long vdbe_rows, vx_rows;
		double vdbe_ms = 1e30, vx_ms = 1e30;
		int rep;

		printf("\n=== %s ===\n", sql);

		/* VDBE baseline (best of reps). */
		vdbe_rows = vdbe_count(db, sql);
		for (rep = 0; rep < reps; rep++) {
			uint64_t t0 = now_ns();
			(void)vdbe_count(db, sql);
			double ms = (double)(now_ns() - t0) / 1e6;
			if (ms < vdbe_ms) vdbe_ms = ms;
		}
		printf("  vdbe        : %8.2f ms  (%ld rows)\n", vdbe_ms, vdbe_rows);
		printf("%s,vdbe,1,%.3f,%ld,1.00\n", sql, vdbe_ms, vdbe_rows);

		/* vexec single-threaded. */
		vx_rows = vexec_count(db, sql);
		if (vx_rows < 0) {
			printf("  vexec       : (falls back to VDBE)\n");
			continue;
		}
		if (vx_rows != vdbe_rows)
			printf("  *** ROW COUNT MISMATCH: vexec=%ld vdbe=%ld ***\n", vx_rows, vdbe_rows);
		for (rep = 0; rep < reps; rep++) {
			uint64_t t0 = now_ns();
			(void)vexec_count(db, sql);
			double ms = (double)(now_ns() - t0) / 1e6;
			if (ms < vx_ms) vx_ms = ms;
		}
		printf("  vexec 1t    : %8.2f ms  (%5.2fx)\n", vx_ms, vdbe_ms / vx_ms);
		printf("%s,vexec-serial,1,%.3f,%ld,%.2f\n", sql, vx_ms, vx_rows, vdbe_ms / vx_ms);

		/* vexec parallel at N workers. */
		for (wi = 0; wi < nw; wi++) {
			int W = workers[wi];
			double pms = 1e30;
			long prows = -1;
			for (rep = 0; rep < reps; rep++) {
				vx_result_t *res = NULL; char *pe = NULL;
				uint64_t t0;
				int rc;
				t0 = now_ns();
				rc = vx_run_parallel(path, sql, W, &res, &pe);
				{
					double ms = (double)(now_ns() - t0) / 1e6;
					if (rc == 1) {
						if (ms < pms) pms = ms;
						prows = vx_result_nrow(res);
						vx_result_free(res);
					}
					sqlite3_free(pe);
					if (rc != 1) { pms = -1; break; }
				}
			}
			if (pms < 0) {
				printf("  vexec %dw    : (parallel falls back)\n", W);
				break;   /* this query is not parallelizable; stop scaling it */
			}
			if (prows != vdbe_rows)
				printf("  *** PARALLEL ROW COUNT MISMATCH: %ld vs %ld ***\n", prows, vdbe_rows);
			printf("  vexec %dw    : %8.2f ms  (%5.2fx)\n", W, pms, vdbe_ms / pms);
			printf("%s,vexec-parallel,%d,%.3f,%ld,%.2f\n", sql, W, pms, prows, vdbe_ms / pms);
		}
	}

	sqlite3_close(db);
	unlink(path);
	{ char w[256]; snprintf(w, sizeof w, "%s-wal", path); unlink(w);
	  snprintf(w, sizeof w, "%s-shm", path); unlink(w); }
	return 0;
}
