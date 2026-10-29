/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/sim/test_sim_sqlxtc_gcidx.c
 *	DST: the version GC (xstore_gc) never touches secondary-index
 *	entries, whatever their length.
 *
 *	xs_gc walks the tree's version keys (20 bytes: table, rowid,
 *	inverted commit ts).  An index entry key is [0xFE][index id:4]
 *	[elements][rowid:8], and with 7 bytes of elements -- one 4-character
 *	TEXT value, or one integer of 2^39..2^47 -- it is 20 bytes too.  Read
 *	as a version, its "commit ts" is 2^63 - 1 - rowid: below the GC
 *	horizon for a rowid near INT64_MAX, so two rows with the same value
 *	looked like two versions of one row and the GC deleted one entry:
 *	the index lost a row.  test_sim_sqlxtc_gc cannot make that shape
 *	(small sequential rowids, 15-byte keys on its plain index), so no
 *	number of its seeds finds it.
 *
 *	Per seed, inside xtc_sim_exec_run: a table with an index on a
 *	4-character TEXT column and one on a 2^40-scale integer, NROW rows
 *	at seed-chosen rowids in the top 2^56 of the id space with values
 *	from a small set (so most values repeat), random UPDATEs and
 *	DELETEs, xstore_gc_run between rounds; after each GC every value's
 *	count through each index must equal its count by a table scan, and
 *	xstore_index_check must report 0 missing entries.  The same seed
 *	run twice must give the same result text (and the run must return
 *	XTC_OK: no determinism guard tripped).
 *	The GC runs in seed-chosen batches of 2-17 keys, so a batch often
 *	ends right after a deleted row's tombstone with its older versions
 *	in the next one (a GC that forgot the row there resurrected it, its
 *	index entries gone: the clean-close GC lost index rows that way).
 *	usage: test_sim_sqlxtc_gcidx [base [count]]   (default 1 8)
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

#include "bufmgr.h"
#include "btree.h"
#include "engine.h"
#include "xstore.h"

#define N_LOOPS 2
#define NROW    120
#define NVAL    6                /* distinct values per indexed column */
#define ROUNDS  4
#define OUTCAP  4096

static bt_t  *g_bt;
static char   g_out[OUTCAP];
static int    g_ok;
static char   g_why[256];
static int64_t g_rid[NROW];

static int64_t
q1(sx_db *h, const char *sql)
{
	sx_stmt *st = NULL;
	int64_t v = -1;
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK)
		return -1;
	if (sx_step(st) == SX_ROW)
		v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

/* Every value's count through the index equals its count by a scan
 * (k + 0 defeats the index), and no entry is missing. */
static int
check(sx_db *h, int round)
{
	char sql[160];
	int v;
	int64_t a, b;
	uint64_t miss = 0, stale = 0;
	size_t l;
	for (v = 0; v < NVAL; v++) {
		snprintf(sql, sizeof sql,
		    "SELECT count(*) FROM t WHERE s = 'v%03d'", v);
		a = q1(h, sql);
		snprintf(sql, sizeof sql,
		    "SELECT count(*) FROM t WHERE s || '' = 'v%03d'", v);
		b = q1(h, sql);
		if (a != b) {
			snprintf(g_why, sizeof g_why, "round %d: s='v%03d' "
			    "index %lld, scan %lld", round, v, (long long)a,
			    (long long)b);
			return -1;
		}
		snprintf(sql, sizeof sql,
		    "SELECT count(*) FROM t WHERE n = %lld", (1LL << 40) + v);
		a = q1(h, sql);
		snprintf(sql, sizeof sql,
		    "SELECT count(*) FROM t WHERE n + 0 = %lld", (1LL << 40) + v);
		b = q1(h, sql);
		if (a != b) {
			snprintf(g_why, sizeof g_why, "round %d: n=2^40+%d "
			    "index %lld, scan %lld", round, v, (long long)a,
			    (long long)b);
			return -1;
		}
		l = strlen(g_out);
		snprintf(g_out + l, OUTCAP - l, "%lld,", (long long)a);
	}
	(void)xstore_index_check(g_bt, &miss, &stale);
	if (miss != 0) {
		snprintf(g_why, sizeof g_why, "round %d: xstore_index_check: "
		    "%llu entries missing", round, (unsigned long long)miss);
		return -1;
	}
	return 0;
}

static void
worker(void *arg)
{
	xtc_exec_t *e = arg;
	sx_db *h = NULL;
	char sql[200];
	int i, r, k, rc;
	if (sx_open_bt(g_bt, &h) != SX_OK) {
		snprintf(g_why, sizeof g_why, "open failed");
		goto out;
	}
	(void)xstore_autovacuum_set((struct xsql *)h, 0);
	if (sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, s TEXT, n INTEGER)",
	    NULL) != SX_OK ||
	    sx_exec(h, "CREATE INDEX t_s ON t(s)", NULL) != SX_OK ||
	    sx_exec(h, "CREATE INDEX t_n ON t(n)", NULL) != SX_OK) {
		snprintf(g_why, sizeof g_why, "create failed");
		goto out;
	}
	for (i = 0; i < NROW; i++) {
		/* the top 2^56 of the id space: the fake ts is below 2^56;
		 * a repeated id (p ~ 1e-13) just draws again */
		do {
			g_rid[i] = INT64_MAX - (int64_t)__xtc_sim_rng_range(
			    XTC_SIM_RNG_APP, (uint64_t)1 << 56);
			k = (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, NVAL);
			snprintf(sql, sizeof sql, "INSERT INTO t VALUES(%lld, "
			    "'v%03d', %lld)", (long long)g_rid[i], k,
			    (1LL << 40) + k);
			rc = sx_exec(h, sql, NULL);
		} while (rc == SX_CONSTRAINT);
		if (rc != SX_OK) {
			snprintf(g_why, sizeof g_why, "insert failed");
			goto out;
		}
	}
	for (r = 0; r < ROUNDS; r++) {
		for (i = 0; i < NROW / 4; i++) {
			int64_t id = g_rid[__xtc_sim_rng_range(XTC_SIM_RNG_APP, NROW)];
			k = (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, NVAL);
			if (__xtc_sim_rng_range(XTC_SIM_RNG_APP, 5) == 0)
				snprintf(sql, sizeof sql, "DELETE FROM t WHERE k = %lld",
				    (long long)id);
			else
				snprintf(sql, sizeof sql, "UPDATE t SET s = 'v%03d', "
				    "n = %lld WHERE k = %lld", k, (1LL << 40) + k,
				    (long long)id);
			if (sx_exec(h, sql, NULL) != SX_OK) {
				snprintf(g_why, sizeof g_why, "write failed");
				goto out;
			}
			(void)xtc_proc_sleep((int64_t)(1 +
			    __xtc_sim_rng_range(XTC_SIM_RNG_APP, 500)) * 1000);
		}
		(void)xtc_proc_sleep(2LL * 1000 * 1000);   /* the horizon moves */
		xstore_gc_set_batch(2 + (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 16));
		rc = xstore_gc_run((struct xsql *)h);
		xstore_gc_set_batch(0);
		if (rc < 0) {
			snprintf(g_why, sizeof g_why, "xstore_gc_run failed");
			goto out;
		}
		if (check(h, r) != 0)
			goto out;
	}
	g_ok = 1;
out:
	if (h != NULL) {
		(void)sx_exec(h, "DROP TABLE IF EXISTS t", NULL);
		sx_close(h);
	}
	(void)xtc_exec_stop(e);
}

static int
run_one(uint64_t seed, char *out)
{
	xtc_exec_t *e = NULL;
	int rc;
	g_out[0] = '\0';
	g_why[0] = '\0';
	g_ok = 0;
	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK)
		return -1;
	(void)xtc_proc_spawn(xtc_exec_loop(e, 1), worker, e, NULL, NULL);
	rc = xtc_sim_exec_run(e, seed, 20000000);
	(void)xtc_exec_fini(e);
	snprintf(out, OUTCAP, "%s", g_out);
	return (rc == XTC_OK && g_ok) ? 0 : -1;
}

/* A fresh, empty tree in the same page file, and the commit clock back
 * at its start (xstore_clock_reset: no store open, nothing in flight). */
static int
fresh(bm_t **bm, bm_opts_t *bo)
{
	if (g_bt != NULL)
		bt_close(g_bt);
	if (*bm != NULL)
		bm_destroy(*bm);
	g_bt = NULL;
	*bm = NULL;
	xstore_clock_reset();
	bo->reopen = 0;
	if (truncate(bo->path, 0) != 0 || bm_create(bo, bm) != XTC_OK ||
	    bt_open(*bm, &g_bt) != XTC_OK)
		return -1;
	return 0;
}

int
main(int argc, char **argv)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	char btf[] = "/tmp/xtc-gcidx-bt-XXXXXX";
	static char a[OUTCAP], b[OUTCAP];
	uint64_t base = argc > 1 ? strtoull(argv[1], NULL, 0) : 1;
	uint64_t count = argc > 2 ? strtoull(argv[2], NULL, 0) : 8, s;
	int fd, fails = 0;

	/* SQL on a fiber needs sqlxtc's 512 KiB stack (main.c): the 64 KiB
	 * default overflows in bt_insert_leaf on FreeBSD/clang. */
	if (xtc_set_stack_size(512u * 1024u) != XTC_OK)
		return 1;
	fd = mkstemp(btf);
	if (fd < 0)
		return 1;
	close(fd);
	bo.path = btf;
	bo.page_size = 4096;
	bo.n_frames = 512;
	for (s = base; s < base + count; s++) {
		uint64_t seed = 0x9E3779B97F4A7C15ull * (s + 1);
		int ra, rb;
		/* Each run on a fresh tree from the same commit clock: stamps
		 * are the run's virtual milliseconds (xstore.c hlc_phys_ms),
		 * and the clock never runs backwards, so a second run in this
		 * process would otherwise continue from the first one's last
		 * stamp (and its versions would sit under the older ones). */
		ra = fresh(&bm, &bo) != 0 ? -1 : run_one(seed, a);
		rb = ra != 0 || fresh(&bm, &bo) != 0 ? -1 : run_one(seed, b);
		if (ra != 0 || rb != 0) {
			printf("  FAIL: seed=%llu (0x%016llx): %s\n",
			    (unsigned long long)s, (unsigned long long)seed,
			    g_why[0] ? g_why : "run error or guard trip");
			printf("  corpus row: test_sim_sqlxtc_gcidx %llu 1 "
			    "<description, fixing commit>\n", (unsigned long long)s);
			fails++;
			continue;
		}
		if (strcmp(a, b) != 0) {
			printf("  FAIL: seed=%llu: two runs differ\n",
			    (unsigned long long)s);
			fails++;
		}
	}
	if (g_bt != NULL)
		bt_close(g_bt);
	if (bm != NULL)
		bm_destroy(bm);
	unlink(btf);
	if (fails) {
		printf("FAIL: %d of %llu seeds\n", fails, (unsigned long long)count);
		return 1;
	}
	printf("OK: xstore_gc keeps every index entry -- %llu seeds, %d rows at "
	    "rowids near INT64_MAX with repeated 7-byte index values, replay "
	    "clean\n", (unsigned long long)count, NROW);
	return 0;
}
