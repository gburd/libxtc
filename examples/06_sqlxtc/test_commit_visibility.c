/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_commit_visibility.c
 *	Commit visibility is atomic.  A writer proc commits multi-row
 *	transactions (each row of txn i carries value i) through the
 *	group-commit WAL, so every COMMIT parks on the fsync after it has a
 *	timestamp and before its versions are applied.  Reader procs on the
 *	same loops run, concurrently:
 *	  - snapshot transactions that read the table twice: both reads
 *	    must agree (no row appears inside one snapshot);
 *	  - autocommit scans: every row must carry the same value (a txn is
 *	    seen whole or not at all).
 *	Before the fix the commit timestamp was published (hlc_tick moved
 *	the read clock) before the log write and apply, so a snapshot
 *	taken in that window later saw rows "appear", and a scan saw a
 *	partially applied transaction.
 *	Every scan must also return each key exactly once, in order: a
 *	resumed B-tree cursor whose parked leaf split re-yielded rows (an
 *	older version of a row already returned), which the 8-row read
 *	used to see as one "stale" row, e.g. [45,45,45,45,0,45,45,45], on
 *	4 loops only.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "btree.h"
#include "bufmgr.h"
#include "engine.h"
#include "wal.h"
#include "xstore.h"
#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "t_tmp.h"

#define ROWS    8
#define TXNS    150
#define READERS 6

static bt_t        *g_bt;
static wal_t       *g_wal;
static _Atomic int  g_done, g_readers_left;
static _Atomic long g_torn, g_nonrepeat, g_reads, g_badkeys;

static _Atomic int  g_printed;

static void
show(const char *what, const int64_t *v, int n, const int64_t *w, int m)
{
	int k;
	if (atomic_fetch_add(&g_printed, 1) >= 3) return;
	fprintf(stderr, "  %s: n=%d [", what, n);
	for (k = 0; k < n; k++) fprintf(stderr, "%s%lld", k ? "," : "", (long long)v[k]);
	fprintf(stderr, "]");
	if (w != NULL) {
		fprintf(stderr, " vs n=%d [", m);
		for (k = 0; k < m; k++) fprintf(stderr, "%s%lld", k ? "," : "", (long long)w[k]);
		fprintf(stderr, "]");
	}
	fprintf(stderr, "\n");
}

/* Read every row with one statement; returns the row count (up to
 * SCANCAP), fills v[].  Returns -2 (and counts a bad scan) if a key is
 * missing, duplicated or out of order -- the keys must be exactly
 * 0..ROWS-1: a scan that yields a row twice is a storage bug, whatever
 * the values say -- or -1 if the statement fails. */
#define SCANCAP (ROWS * 2)
static int
scan(sx_db *db, int64_t *v)
{
	sx_stmt *st = NULL;
	int64_t kk[SCANCAP];
	int n = 0, k;
	if (sx_prepare(db, "SELECT k, v FROM t ORDER BY k", -1, &st, NULL) != SX_OK) {
		atomic_fetch_add(&g_badkeys, 1);
		return -1;
	}
	while (sx_step(st) == SX_ROW && n < SCANCAP) {
		kk[n] = sx_column_int64(st, 0);
		v[n++] = sx_column_int64(st, 1);
	}
	sx_finalize(st);
	for (k = 0; k < n; k++)
		if (kk[k] != k) break;
	if (k < n || n != ROWS) {
		atomic_fetch_add(&g_badkeys, 1);
		if (atomic_fetch_add(&g_printed, 1) < 3) {
			fprintf(stderr, "  bad key set: n=%d k=[", n);
			for (k = 0; k < n; k++)
				fprintf(stderr, "%s%lld", k ? "," : "", (long long)kk[k]);
			fprintf(stderr, "] v=[");
			for (k = 0; k < n; k++)
				fprintf(stderr, "%s%lld", k ? "," : "", (long long)v[k]);
			fprintf(stderr, "]\n");
		}
		return -2;
	}
	return n;
}

static void
writer(void *arg)
{
	sx_db *db = NULL;
	char sql[96];
	int i, k;
	(void)arg;
	if (sx_open_bt(g_bt, &db) != SX_OK) { atomic_store(&g_done, 1); return; }
	for (i = 1; i <= TXNS; i++) {
		CK(sx_exec(db, "BEGIN", NULL) == SX_OK);
		for (k = 0; k < ROWS; k++) {
			snprintf(sql, sizeof sql,
			    "UPDATE t SET v=%d WHERE k=%d", i, k);
			CK(sx_exec(db, sql, NULL) == SX_OK);
		}
		CK(sx_exec(db, "COMMIT", NULL) == SX_OK);
	}
	atomic_store(&g_done, 1);
	sx_close(db);
}

static void
reader(void *arg)
{
	sx_db *db = NULL;
	int64_t a[SCANCAP], b[SCANCAP];
	int na, nb, k;
	(void)arg;
	if (sx_open_bt(g_bt, &db) != SX_OK) goto out;
	while (!atomic_load(&g_done)) {
		/* autocommit scan: all rows from one txn */
		na = scan(db, a);
		for (k = 1; k < na; k++)
			if (a[k] != a[0]) break;
		if (na == ROWS && k < na) {
			atomic_fetch_add(&g_torn, 1);
			show("torn autocommit scan", a, na, NULL, 0);
		}
		/* snapshot txn: two reads agree */
		(void)sx_exec(db, "BEGIN", NULL);
		na = scan(db, a);
		(void)xtc_proc_sleep(300LL * 1000);   /* let commits land */
		nb = scan(db, b);
		(void)sx_exec(db, "COMMIT", NULL);
		if (na == ROWS && nb == ROWS &&
		    memcmp(a, b, (size_t)na * sizeof a[0]) != 0) {
			atomic_fetch_add(&g_nonrepeat, 1);
			show("non-repeatable snapshot", a, na, b, nb);
		}
		atomic_fetch_add(&g_reads, 1);
	}
	sx_close(db);
out:
	if (atomic_fetch_sub(&g_readers_left, 1) == 1)
		(void)wal_writer_stop(g_wal);
}

int
main(void)
{
	char logp[256], pg[256];
	wal_opts_t wo = { 0 };
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	sx_db *ddl = NULL;
	xtc_exec_t *ex = NULL;
	xtc_proc_opts_t po = { .name = "cv" };
	xtc_pid_t pid;
	int fd, i, nloops;

	for (nloops = 1; nloops <= 4; nloops += 3) {
		t_tmpl(logp, sizeof logp, "sqlxtc-cv-log");
		t_tmpl(pg, sizeof pg, "sqlxtc-cv-pg");
		if ((fd = mkstemp(logp)) < 0) return 1;
		close(fd);
		if ((fd = mkstemp(pg)) < 0) return 1;
		close(fd);
		wo.path = logp;
		if (wal_open(&wo, &g_wal) != XTC_OK) return 1;
		bo.path = pg; bo.page_size = 4096; bo.n_frames = 512;
		if (bm_create(&bo, &bm) != XTC_OK || bt_open(bm, &g_bt) != XTC_OK)
			return 1;
		xstore_set_wal((struct wal *)g_wal);
		if (sx_open_bt(g_bt, &ddl) != SX_OK) return 1;
		CK(sx_exec(ddl, "CREATE TABLE t(k INTEGER PRIMARY KEY, v INTEGER)", NULL) == SX_OK);
		for (i = 0; i < ROWS; i++) {
			char q[64];
			snprintf(q, sizeof q, "INSERT INTO t(k,v) VALUES(%d,0)", i);
			CK(sx_exec(ddl, q, NULL) == SX_OK);
		}
		atomic_store(&g_done, 0);
		atomic_store(&g_readers_left, READERS);
		atomic_store(&g_torn, 0);
		atomic_store(&g_nonrepeat, 0);
		atomic_store(&g_reads, 0);
		atomic_store(&g_badkeys, 0);
		if (xtc_exec_init(&ex, nloops) != XTC_OK) return 1;
		if (wal_writer_spawn(g_wal, xtc_exec_loop(ex, 0), NULL) != XTC_OK) return 1;
		for (i = 0; i < READERS; i++)
			if (xtc_proc_spawn(xtc_exec_loop(ex, i % nloops), reader, NULL,
			    &po, &pid) != XTC_OK)
				return 1;
		if (xtc_proc_spawn(xtc_exec_loop(ex, 0), writer, NULL, &po, &pid) != XTC_OK)
			return 1;
		if (xtc_exec_run(ex) != XTC_OK) return 1;
		(void)xtc_exec_fini(ex);
		printf("  [%d loop%s] reads=%ld torn-scans=%ld non-repeatable-snapshots=%ld"
		    " bad-key-sets=%ld\n",
		    nloops, nloops > 1 ? "s" : "", atomic_load(&g_reads),
		    atomic_load(&g_torn), atomic_load(&g_nonrepeat),
		    atomic_load(&g_badkeys));
		CK(atomic_load(&g_badkeys) == 0);
		CK(atomic_load(&g_reads) > 0);
		CK(atomic_load(&g_torn) == 0);
		CK(atomic_load(&g_nonrepeat) == 0);
		sx_close(ddl);
		xstore_set_wal(NULL);
		bt_close(g_bt);
		bm_destroy(bm);
		wal_close(g_wal);
		unlink(logp); unlink(pg);
	}
	if (g_fail) { fprintf(stderr, "FAIL: test_commit_visibility\n"); return 1; }
	printf("  ok   commit visibility: snapshots never see a partial or "
	    "in-flight transaction\n");
	return 0;
}
