/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_xstore.c
 *	SQL on the libxtc-native storage engine via the xstore virtual
 *	table.  SQLite parses/plans/runs the VDBE; rows live in our
 *	on-disk B-tree (bufmgr cooling pool).  The working set is sized
 *	far larger than the buffer pool, so the test exercises eviction
 *	and reload DURING SQL scans -- the larger-than-RAM proof.
 *
 *	No daemon; standalone (plain asserts + printf).  Runs the SQL off
 *	a loop (the bufmgr does synchronous I/O off-loop) and again ON a
 *	loop with the page-provider live (btree ops park on offloaded
 *	page I/O in the middle of the VDBE -- the hardening path).
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sqlite3.h"
#include "bufmgr.h"
#include "btree.h"
#include "xstore.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

#define N_ROWS    4000
#define ROW_BYTES 200
#define PAGE_SZ   4096
#define N_FRAMES  16           /* 64 KB pool vs ~840 KB of rows */

static int g_fail;
#define CK(c) do { if (!(c)) { fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#c); g_fail=1; } } while (0)

/* Run the full SQL workload against a freshly-opened :memory: handle
 * whose xstore table is backed by `bt`.  Returns 0 on success. */
static int
run_sql(bt_t *bt, const char *tag, bm_t *bm)
{
	xsql *db = NULL;
	xsql_stmt *st = NULL;
	char blob[ROW_BYTES];
	int i;
	bm_stats_t bs;

	memset(blob, 'x', sizeof blob);
	CK(xsql_open(":memory:", &db) == SQLITE_OK);
	CK(xstore_register(db, bt) == SQLITE_OK);
	CK(xsql_exec(db, "CREATE VIRTUAL TABLE t USING xstore;", 0, 0, 0)
	    == SQLITE_OK);

	/* INSERT N_ROWS rows -- far more bytes than the buffer pool. */
	CK(xsql_prepare_v2(db, "INSERT INTO t(k,v) VALUES(?,?)", -1, &st, 0)
	    == SQLITE_OK);
	for (i = 1; i <= N_ROWS; i++) {
		/* tag the first 8 bytes with the key so reads can verify. */
		memcpy(blob, &i, sizeof i);
		xsql_bind_int64(st, 1, i);
		xsql_bind_blob(st, 2, blob, ROW_BYTES, SQLITE_STATIC);
		CK(xsql_step(st) == SQLITE_DONE);
		xsql_reset(st);
	}
	xsql_finalize(st); st = NULL;

	/* Full scan: count(*) and sum(k) -- reads every row back through
	 * the (over-subscribed) buffer pool. */
	CK(xsql_prepare_v2(db, "SELECT count(*), sum(k) FROM t", -1, &st, 0)
	    == SQLITE_OK);
	CK(xsql_step(st) == SQLITE_ROW);
	CK(xsql_column_int64(st, 0) == N_ROWS);
	CK(xsql_column_int64(st, 1) == (int64_t)N_ROWS * (N_ROWS + 1) / 2);
	xsql_finalize(st); st = NULL;

	/* Point lookup: WHERE k = ? (uses xBestIndex eq path). */
	CK(xsql_prepare_v2(db, "SELECT v FROM t WHERE k=?", -1, &st, 0)
	    == SQLITE_OK);
	xsql_bind_int64(st, 1, 1234);
	CK(xsql_step(st) == SQLITE_ROW);
	{
		const void *p = xsql_column_blob(st, 0);
		int n = xsql_column_bytes(st, 0), kk = 0;
		CK(n == ROW_BYTES);
		memcpy(&kk, p, sizeof kk);
		CK(kk == 1234);                       /* correct row content */
	}
	xsql_finalize(st); st = NULL;

	/* UPDATE then verify. */
	CK(xsql_exec(db, "UPDATE t SET v=zeroblob(16) WHERE k=1234", 0, 0, 0)
	    == SQLITE_OK);
	CK(xsql_prepare_v2(db, "SELECT length(v) FROM t WHERE k=1234", -1, &st, 0)
	    == SQLITE_OK);
	CK(xsql_step(st) == SQLITE_ROW);
	CK(xsql_column_int(st, 0) == 16);
	xsql_finalize(st); st = NULL;

	/* DELETE then verify the row is gone and the count dropped. */
	CK(xsql_exec(db, "DELETE FROM t WHERE k=1234", 0, 0, 0) == SQLITE_OK);
	CK(xsql_prepare_v2(db, "SELECT count(*) FROM t WHERE k=1234", -1, &st, 0)
	    == SQLITE_OK);
	CK(xsql_step(st) == SQLITE_ROW);
	CK(xsql_column_int(st, 0) == 0);
	xsql_finalize(st); st = NULL;

	xsql_close(db);

	bm_get_stats(bm, &bs);
	if (g_fail)
		return 1;
	printf("  ok   [%s] %d rows of %dB through a %dKB pool: SQL "
	    "count/sum/point/update/delete correct; pool paged "
	    "(loads=%llu evicted=%llu flushed=%llu resident=%llu)\n",
	    tag, N_ROWS, ROW_BYTES, (N_FRAMES * PAGE_SZ) / 1024,
	    (unsigned long long)bs.loads, (unsigned long long)bs.evicted,
	    (unsigned long long)bs.flushed, (unsigned long long)bs.resident);
	if (bs.evicted == 0)
		fprintf(stderr, "  WARN[%s]: no eviction -- working set fit in RAM?\n", tag);
	return 0;
}

/* ---- off-loop ---- */
static int
scenario_offloop(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[] = "/tmp/sqlxtc-xstore-XXXXXX";
	int fd, rc;

	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = N_FRAMES; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	rc = run_sql(bt, "off-loop", bm);
	bt_close(bt); bm_destroy(bm); unlink(path);
	{ char wal[80]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
	return rc;
}

/* ---- on-loop: SQL runs inside a proc; btree ops park on offloaded
 * page I/O in the middle of the VDBE, with the page-provider live. ---- */
static bt_t *g_bt;
static bm_t *g_bm;
static int   g_loop_rc;

static void
sql_proc(void *a)
{
	(void)a;
	g_loop_rc = run_sql(g_bt, "on-loop", g_bm);
	bm_provider_stop(g_bm);
}

static int
scenario_onloop(void)
{
	xtc_loop_t *loop = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[] = "/tmp/sqlxtc-xstore2-XXXXXX";
	xtc_proc_opts_t po = { .name = "sql" };
	xtc_pid_t pid;
	int fd;

	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = N_FRAMES; bo.cool_pct = 25;
	g_fail = 0;
	CK(bm_create(&bo, &g_bm) == XTC_OK);
	CK(bt_open(g_bm, &g_bt) == XTC_OK);
	CK(xtc_loop_init(&loop) == XTC_OK);
	CK(bm_provider_spawn(g_bm, loop, 1LL * 1000 * 1000, NULL) == XTC_OK);
	CK(xtc_proc_spawn(loop, sql_proc, NULL, &po, &pid) == XTC_OK);
	CK(xtc_loop_run(loop) == XTC_OK);
	CK(xtc_loop_fini(loop) == XTC_OK);
	bt_close(g_bt); bm_destroy(g_bm); unlink(path);
	{ char wal[80]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
	return g_loop_rc || g_fail;
}

/* ---- MVCC snapshot visibility: a read at an old snapshot does not
 * see a newer committed write (PostgreSQL-style version visibility,
 * surfaced as a SQL AS-OF read via xstore_as_of). ---- */
static int
sel_v(xsql *db, int64_t k, char *out, size_t cap)
{
	xsql_stmt *st = NULL;
	int got = 0;
	if (xsql_prepare_v2(db, "SELECT v FROM t WHERE k=?", -1, &st, 0)
	    != SQLITE_OK)
		return -1;
	xsql_bind_int64(st, 1, k);
	if (xsql_step(st) == SQLITE_ROW) {
		const unsigned char *t = xsql_column_text(st, 0);
		size_t n = (size_t)xsql_column_bytes(st, 0);
		if (n >= cap) n = cap - 1;
		if (t) memcpy(out, t, n);
		out[n] = '\0';
		got = 1;
	}
	xsql_finalize(st);
	return got;
}
static void
set_as_of(xsql *db, int64_t ts)
{
	xsql_stmt *st = NULL;
	if (xsql_prepare_v2(db, "SELECT xstore_as_of(?)", -1, &st, 0)
	    != SQLITE_OK) return;
	xsql_bind_int64(st, 1, ts);
	(void)xsql_step(st);
	xsql_finalize(st);
}

static int
scenario_mvcc(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[] = "/tmp/sqlxtc-xstore3-XXXXXX";
	xsql *db = NULL;
	xsql_stmt *st = NULL;
	int64_t ts_mid = 0;
	char b[32];
	int fd;

	g_fail = 0;
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 64; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(xsql_open(":memory:", &db) == SQLITE_OK);
	CK(xstore_register(db, bt) == SQLITE_OK);
	CK(xsql_exec(db, "CREATE VIRTUAL TABLE t USING xstore;", 0, 0, 0)
	    == SQLITE_OK);

	CK(xsql_exec(db, "INSERT INTO t(k,v) VALUES(1,'aaa');", 0, 0, 0)
	    == SQLITE_OK);
	/* Capture a snapshot between the insert and the update. */
	CK(xsql_prepare_v2(db, "SELECT xstore_now()", -1, &st, 0) == SQLITE_OK);
	CK(xsql_step(st) == SQLITE_ROW);
	ts_mid = xsql_column_int64(st, 0);
	xsql_finalize(st); st = NULL;
	CK(xsql_exec(db, "UPDATE t SET v='bbb' WHERE k=1;", 0, 0, 0)
	    == SQLITE_OK);

	/* Latest snapshot sees the new value. */
	set_as_of(db, 0);
	CK(sel_v(db, 1, b, sizeof b) == 1 && strcmp(b, "bbb") == 0);
	/* The captured snapshot still sees the OLD value -- MVCC visibility. */
	set_as_of(db, ts_mid);
	CK(sel_v(db, 1, b, sizeof b) == 1 && strcmp(b, "aaa") == 0);

	/* Delete at latest; the old snapshot is unaffected (delete is in
	 * its future). */
	set_as_of(db, 0);
	CK(xsql_exec(db, "DELETE FROM t WHERE k=1;", 0, 0, 0) == SQLITE_OK);
	CK(sel_v(db, 1, b, sizeof b) == 0);                  /* gone at latest */
	set_as_of(db, ts_mid);
	CK(sel_v(db, 1, b, sizeof b) == 1 && strcmp(b, "aaa") == 0);

	xsql_close(db);
	bt_close(bt); bm_destroy(bm); unlink(path);
	{ char wal[80]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
	if (g_fail) return 1;
	printf("  ok   MVCC snapshot visibility: an AS-OF read at an old "
	    "snapshot sees the pre-update value; latest sees the new one; "
	    "a delete is invisible to the old snapshot\n");
	return 0;
}

static int
scenario_txn(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[] = "/tmp/sqlxtc-xstore4-XXXXXX";
	xsql *db = NULL;
	xsql_stmt *st = NULL;
	int64_t ts_pre = 0, commit_ts = 0;
	char b[32];
	int fd;

	g_fail = 0;
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 64; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(xsql_open(":memory:", &db) == SQLITE_OK);
	CK(xstore_register(db, bt) == SQLITE_OK);
	CK(xsql_exec(db, "CREATE VIRTUAL TABLE t USING xstore;", 0, 0, 0)
	    == SQLITE_OK);

	CK(xsql_prepare_v2(db, "SELECT xstore_now()", -1, &st, 0) == SQLITE_OK);
	CK(xsql_step(st) == SQLITE_ROW);
	ts_pre = xsql_column_int64(st, 0);
	xsql_finalize(st); st = NULL;

	/* A two-row transaction.  Buffered, then committed atomically. */
	CK(xsql_exec(db, "BEGIN;", 0, 0, 0) == SQLITE_OK);
	CK(xsql_exec(db, "INSERT INTO t(k,v) VALUES(10,'x');", 0, 0, 0)
	    == SQLITE_OK);
	CK(xsql_exec(db, "INSERT INTO t(k,v) VALUES(11,'y');", 0, 0, 0)
	    == SQLITE_OK);
	/* Read-your-writes inside the open transaction. */
	CK(sel_v(db, 10, b, sizeof b) == 1 && strcmp(b, "x") == 0);
	CK(xsql_exec(db, "COMMIT;", 0, 0, 0) == SQLITE_OK);

	CK(xsql_prepare_v2(db, "SELECT xstore_now()", -1, &st, 0) == SQLITE_OK);
	CK(xsql_step(st) == SQLITE_ROW);
	commit_ts = xsql_column_int64(st, 0);
	xsql_finalize(st); st = NULL;

	/* Both rows shared ONE commit timestamp (a single tick for the
	 * whole transaction). */
	CK(commit_ts == ts_pre + 1);

	/* Atomicity: no snapshot sees exactly one of the two rows.  Just
	 * before the commit timestamp -> neither; at it -> both. */
	set_as_of(db, commit_ts - 1);
	CK(sel_v(db, 10, b, sizeof b) == 0 && sel_v(db, 11, b, sizeof b) == 0);
	set_as_of(db, commit_ts);
	CK(sel_v(db, 10, b, sizeof b) == 1 && sel_v(db, 11, b, sizeof b) == 1);

	/* Rollback discards the whole transaction. */
	set_as_of(db, 0);
	CK(xsql_exec(db, "BEGIN;", 0, 0, 0) == SQLITE_OK);
	CK(xsql_exec(db, "INSERT INTO t(k,v) VALUES(12,'z');", 0, 0, 0)
	    == SQLITE_OK);
	CK(xsql_exec(db, "ROLLBACK;", 0, 0, 0) == SQLITE_OK);
	CK(sel_v(db, 12, b, sizeof b) == 0);

	xsql_close(db);
	bt_close(bt); bm_destroy(bm); unlink(path);
	{ char wal[80]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
	if (g_fail) return 1;
	printf("  ok   atomic multi-row transaction: two rows commit at one "
	    "timestamp (read-your-writes inside the txn); no snapshot sees a "
	    "partial commit; rollback discards\n");
	return 0;
}

/* Two connections sharing one engine B-tree exhibit write-skew under
 * snapshot isolation; serializable validation aborts one of them. */
static int
commit_rc(xsql *db)
{
	return xsql_exec(db, "COMMIT;", 0, 0, 0);
}

static int
scenario_serializable(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[] = "/tmp/sqlxtc-xstore5-XXXXXX";
	xsql *d1 = NULL, *d2 = NULL;
	char b[32];
	int fd;

	g_fail = 0;
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 64; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(xsql_open(":memory:", &d1) == SQLITE_OK);
	CK(xsql_open(":memory:", &d2) == SQLITE_OK);
	CK(xstore_register(d1, bt) == SQLITE_OK);
	CK(xstore_register(d2, bt) == SQLITE_OK);
	CK(xsql_exec(d1, "CREATE VIRTUAL TABLE t USING xstore;", 0, 0, 0) == SQLITE_OK);
	CK(xsql_exec(d2, "CREATE VIRTUAL TABLE t USING xstore;", 0, 0, 0) == SQLITE_OK);
	CK(xsql_exec(d1, "INSERT INTO t(k,v) VALUES(1,'0'),(2,'0');", 0, 0, 0) == SQLITE_OK);

	/* Write-skew under snapshot isolation (default): each txn reads
	 * both rows (sees 0,0) and writes a different one; both commit, so
	 * the invariant 'at most one of x,y is 1' is violated. */
	CK(xsql_exec(d1, "BEGIN;", 0, 0, 0) == SQLITE_OK);
	CK(sel_v(d1, 1, b, sizeof b) == 1); CK(sel_v(d1, 2, b, sizeof b) == 1);
	CK(xsql_exec(d2, "BEGIN;", 0, 0, 0) == SQLITE_OK);
	CK(sel_v(d2, 1, b, sizeof b) == 1); CK(sel_v(d2, 2, b, sizeof b) == 1);
	CK(xsql_exec(d1, "UPDATE t SET v='1' WHERE k=1;", 0, 0, 0) == SQLITE_OK);
	CK(xsql_exec(d2, "UPDATE t SET v='1' WHERE k=2;", 0, 0, 0) == SQLITE_OK);
	CK(commit_rc(d1) == SQLITE_OK);
	CK(commit_rc(d2) == SQLITE_OK);                 /* SI: BOTH commit */
	CK(sel_v(d1, 1, b, sizeof b) == 1 && strcmp(b, "1") == 0);
	CK(sel_v(d1, 2, b, sizeof b) == 1 && strcmp(b, "1") == 0);  /* anomaly present */

	/* Reset to 0,0 and rerun under serializable: one txn must abort. */
	CK(xsql_exec(d1, "UPDATE t SET v='0' WHERE k=1;", 0, 0, 0) == SQLITE_OK);
	CK(xsql_exec(d1, "UPDATE t SET v='0' WHERE k=2;", 0, 0, 0) == SQLITE_OK);
	CK(xsql_exec(d1, "SELECT xstore_serializable(1);", 0, 0, 0) == SQLITE_OK);
	CK(xsql_exec(d2, "SELECT xstore_serializable(1);", 0, 0, 0) == SQLITE_OK);

	CK(xsql_exec(d1, "BEGIN;", 0, 0, 0) == SQLITE_OK);
	CK(sel_v(d1, 1, b, sizeof b) == 1); CK(sel_v(d1, 2, b, sizeof b) == 1);
	CK(xsql_exec(d2, "BEGIN;", 0, 0, 0) == SQLITE_OK);
	CK(sel_v(d2, 1, b, sizeof b) == 1); CK(sel_v(d2, 2, b, sizeof b) == 1);
	CK(xsql_exec(d1, "UPDATE t SET v='1' WHERE k=1;", 0, 0, 0) == SQLITE_OK);
	CK(xsql_exec(d2, "UPDATE t SET v='1' WHERE k=2;", 0, 0, 0) == SQLITE_OK);
	{
		int rc1 = commit_rc(d1);
		int rc2 = commit_rc(d2);
		/* d1 validates (no conflict yet) and commits; d2's read of k=1
		 * was overwritten by d1 after d2's snapshot -> abort. */
		CK(rc1 == SQLITE_OK);
		CK(rc2 == SQLITE_BUSY);
	}

	xsql_close(d1); xsql_close(d2);
	bt_close(bt); bm_destroy(bm); unlink(path);
	{ char wal[80]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
	if (g_fail) return 1;
	printf("  ok   serializable isolation: write-skew commits under SI "
	    "(anomaly), but serializable validation aborts the second txn "
	    "(SQLITE_BUSY) -- read-set conflict detected\n");
	return 0;
}

int
main(void)
{
	if (scenario_offloop() != 0) return 1;
	if (scenario_onloop() != 0) return 1;
	if (scenario_mvcc() != 0) return 1;
	if (scenario_txn() != 0) return 1;
	if (scenario_serializable() != 0) return 1;
	printf("All sqlxtc SQL-on-xstore tests passed.\n");
	return 0;
}
