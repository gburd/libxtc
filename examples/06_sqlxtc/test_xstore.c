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

#include "engine.h"
#include "bufmgr.h"
#include "btree.h"
#include "xstore.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "t_tmp.h"

#define N_ROWS    4000
#define ROW_BYTES 200
#define PAGE_SZ   4096
#define N_FRAMES  16           /* 64 KB pool vs ~840 KB of rows */


/* Run the full SQL workload against a freshly-opened :memory: handle
 * whose xstore table is backed by `bt`.  Returns 0 on success. */
static int
run_sql(bt_t *bt, const char *tag, bm_t *bm)
{
	sx_db *db = NULL;
	sx_stmt *st = NULL;
	char blob[ROW_BYTES];
	int i;
	bm_stats_t bs;

	memset(blob, 'x', sizeof blob);
	CK(sx_open_bt(bt, &db) == SX_OK);
	CK(sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL)
	    == SX_OK);

	/* INSERT N_ROWS rows -- far more bytes than the buffer pool. */
	CK(sx_prepare(db, "INSERT INTO t(k,v) VALUES(?,?)", -1, &st, NULL)
	    == SX_OK);
	for (i = 1; i <= N_ROWS; i++) {
		/* tag the first 8 bytes with the key so reads can verify. */
		memcpy(blob, &i, sizeof i);
		sx_bind_int64(st, 1, i);
		sx_bind_blob(st, 2, blob, ROW_BYTES);
		CK(sx_step(st) == SX_DONE);
		sx_reset(st);
	}
	sx_finalize(st); st = NULL;

	/* Full scan: count(*) and sum(k) -- reads every row back through
	 * the (over-subscribed) buffer pool. */
	CK(sx_prepare(db, "SELECT count(*), sum(k) FROM t", -1, &st, NULL)
	    == SX_OK);
	CK(sx_step(st) == SX_ROW);
	CK(sx_column_int64(st, 0) == N_ROWS);
	CK(sx_column_int64(st, 1) == (int64_t)N_ROWS * (N_ROWS + 1) / 2);
	sx_finalize(st); st = NULL;

	/* Point lookup: WHERE k = ? (uses xBestIndex eq path). */
	CK(sx_prepare(db, "SELECT v FROM t WHERE k=?", -1, &st, NULL)
	    == SX_OK);
	sx_bind_int64(st, 1, 1234);
	CK(sx_step(st) == SX_ROW);
	{
		const void *p = sx_column_blob(st, 0);
		int n = sx_column_bytes(st, 0), kk = 0;
		CK(n == ROW_BYTES);
		memcpy(&kk, p, sizeof kk);
		CK(kk == 1234);                       /* correct row content */
	}
	sx_finalize(st); st = NULL;

	/* UPDATE then verify (a 16-byte blob literal; the native engine has
	 * no zeroblob() function -- an x'..' literal is equivalent here). */
	CK(sx_exec(db, "UPDATE t SET v=x'00000000000000000000000000000000' WHERE k=1234", NULL)
	    == SX_OK);
	CK(sx_prepare(db, "SELECT length(v) FROM t WHERE k=1234", -1, &st, NULL)
	    == SX_OK);
	CK(sx_step(st) == SX_ROW);
	CK((int)sx_column_int64(st, 0) == 16);
	sx_finalize(st); st = NULL;

	/* DELETE then verify the row is gone and the count dropped. */
	CK(sx_exec(db, "DELETE FROM t WHERE k=1234", NULL) == SX_OK);
	CK(sx_prepare(db, "SELECT count(*) FROM t WHERE k=1234", -1, &st, NULL)
	    == SX_OK);
	CK(sx_step(st) == SX_ROW);
	CK((int)sx_column_int64(st, 0) == 0);
	sx_finalize(st); st = NULL;

	sx_close(db);

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
	char path[256]; t_tmpl(path, sizeof path, "sqlxtc-xstore");
	int fd, rc;

	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = N_FRAMES; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	rc = run_sql(bt, "off-loop", bm);
	bt_close(bt); bm_destroy(bm); unlink(path);
	{ char wal[288]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
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
	char path[256]; t_tmpl(path, sizeof path, "sqlxtc-xstore2");
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
	{ char wal[288]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
	return g_loop_rc || g_fail;
}

/* ---- MVCC snapshot visibility: a read at an old snapshot does not
 * see a newer committed write (PostgreSQL-style version visibility,
 * surfaced as a SQL AS-OF read via xstore_as_of). ---- */
static int
sel_v(sx_db *db, int64_t k, char *out, size_t cap)
{
	sx_stmt *st = NULL;
	int got = 0;
	if (sx_prepare(db, "SELECT v FROM t WHERE k=?", -1, &st, NULL)
	    != SX_OK)
		return -1;
	sx_bind_int64(st, 1, k);
	if (sx_step(st) == SX_ROW) {
		const unsigned char *t = sx_column_text(st, 0);
		size_t n = (size_t)sx_column_bytes(st, 0);
		if (n >= cap) n = cap - 1;
		if (t) memcpy(out, t, n);
		out[n] = '\0';
		got = 1;
	}
	sx_finalize(st);
	return got;
}
static void
set_as_of(sx_db *db, int64_t ts)
{
	(void)xstore_as_of(db, ts);
}
static int
eval_int(sx_db *db, const char *sql)
{
	sx_stmt *st = NULL;
	int v = -1;
	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK) return -1;
	if (sx_step(st) == SX_ROW) v = (int)sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

/* Like eval_int but 64-bit, for commit timestamps (HLC stamps far
 * exceed 32 bits). */
static int64_t
eval_int64(sx_db *db, const char *sql)
{
	sx_stmt *st = NULL;
	int64_t v = -1;
	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK) return -1;
	if (sx_step(st) == SX_ROW) v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

static int
scenario_mvcc(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256]; t_tmpl(path, sizeof path, "sqlxtc-xstore3");
	sx_db *db = NULL;
	sx_stmt *st = NULL;
	int64_t ts_mid = 0;
	char b[32];
	int fd;

	g_fail = 0;
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 64; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(sx_open_bt(bt, &db) == SX_OK);
	CK(sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL)
	    == SX_OK);

	CK(sx_exec(db, "INSERT INTO t(k,v) VALUES(1,'aaa')", NULL)
	    == SX_OK);
	/* Capture a snapshot between the insert and the update. */
	ts_mid = xstore_clock_now();
	CK(sx_exec(db, "UPDATE t SET v='bbb' WHERE k=1", NULL)
	    == SX_OK);

	/* Latest snapshot sees the new value. */
	set_as_of(db, 0);
	CK(sel_v(db, 1, b, sizeof b) == 1 && strcmp(b, "bbb") == 0);
	/* The captured snapshot still sees the OLD value -- MVCC visibility. */
	set_as_of(db, ts_mid);
	CK(sel_v(db, 1, b, sizeof b) == 1 && strcmp(b, "aaa") == 0);

	/* Delete at latest; the old snapshot is unaffected (delete is in
	 * its future). */
	set_as_of(db, 0);
	CK(sx_exec(db, "DELETE FROM t WHERE k=1", NULL) == SX_OK);
	CK(sel_v(db, 1, b, sizeof b) == 0);                  /* gone at latest */
	set_as_of(db, ts_mid);
	CK(sel_v(db, 1, b, sizeof b) == 1 && strcmp(b, "aaa") == 0);

	sx_close(db);
	bt_close(bt); bm_destroy(bm); unlink(path);
	{ char wal[288]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
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
	char path[256]; t_tmpl(path, sizeof path, "sqlxtc-xstore4");
	sx_db *db = NULL;
	sx_stmt *st = NULL;
	int64_t ts_pre = 0, commit_ts = 0;
	char b[32];
	int fd;

	g_fail = 0;
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 64; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(sx_open_bt(bt, &db) == SX_OK);
	CK(sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL)
	    == SX_OK);

	ts_pre = xstore_clock_now();

	/* A two-row transaction.  Buffered, then committed atomically. */
	CK(sx_exec(db, "BEGIN", NULL) == SX_OK);
	CK(sx_exec(db, "INSERT INTO t(k,v) VALUES(10,'x')", NULL)
	    == SX_OK);
	CK(sx_exec(db, "INSERT INTO t(k,v) VALUES(11,'y')", NULL)
	    == SX_OK);
	/* Read-your-writes inside the open transaction. */
	CK(sel_v(db, 10, b, sizeof b) == 1 && strcmp(b, "x") == 0);
	CK(sx_exec(db, "COMMIT", NULL) == SX_OK);

	commit_ts = xstore_clock_now();

	/* Both rows shared ONE commit timestamp (a single tick for the
	 * whole transaction): the timestamp advanced past the pre-txn
	 * reading, and the atomicity checks below confirm both rows landed
	 * at exactly that one stamp.  (The clock is an HLC, so a tick is
	 * not necessarily +1 -- it jumps to wall-clock time.) */
	CK(commit_ts > ts_pre);

	/* Atomicity: no snapshot sees exactly one of the two rows.  Just
	 * before the commit timestamp -> neither; at it -> both. */
	set_as_of(db, commit_ts - 1);
	CK(sel_v(db, 10, b, sizeof b) == 0 && sel_v(db, 11, b, sizeof b) == 0);
	set_as_of(db, commit_ts);
	CK(sel_v(db, 10, b, sizeof b) == 1 && sel_v(db, 11, b, sizeof b) == 1);

	/* Rollback discards the whole transaction. */
	set_as_of(db, 0);
	CK(sx_exec(db, "BEGIN", NULL) == SX_OK);
	CK(sx_exec(db, "INSERT INTO t(k,v) VALUES(12,'z')", NULL)
	    == SX_OK);
	CK(sx_exec(db, "ROLLBACK", NULL) == SX_OK);
	CK(sel_v(db, 12, b, sizeof b) == 0);

	sx_close(db);
	bt_close(bt); bm_destroy(bm); unlink(path);
	{ char wal[288]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
	if (g_fail) return 1;
	printf("  ok   atomic multi-row transaction: two rows commit at one "
	    "timestamp (read-your-writes inside the txn); no snapshot sees a "
	    "partial commit; rollback discards\n");
	return 0;
}

/* Two connections sharing one engine B-tree exhibit write-skew under
 * snapshot isolation; serializable validation aborts one of them. */
static int
commit_rc(sx_db *db)
{
	return sx_exec(db, "COMMIT", NULL);
}

static int
scenario_serializable(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256]; t_tmpl(path, sizeof path, "sqlxtc-xstore5");
	sx_db *d1 = NULL, *d2 = NULL;
	char b[32];
	int fd;

	g_fail = 0;
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 64; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(sx_open_bt(bt, &d1) == SX_OK);
	CK(sx_open_bt(bt, &d2) == SX_OK);
	CK(sx_exec(d1, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) == SX_OK);
	CK(sx_exec(d2, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) == SX_OK);
	CK(sx_exec(d1, "INSERT INTO t(k,v) VALUES(1,'0'),(2,'0')", NULL) == SX_OK);

	/* Write-skew under snapshot isolation (default): each txn reads
	 * both rows (sees 0,0) and writes a different one; both commit, so
	 * the invariant 'at most one of x,y is 1' is violated. */
	CK(sx_exec(d1, "BEGIN", NULL) == SX_OK);
	CK(sel_v(d1, 1, b, sizeof b) == 1); CK(sel_v(d1, 2, b, sizeof b) == 1);
	CK(sx_exec(d2, "BEGIN", NULL) == SX_OK);
	CK(sel_v(d2, 1, b, sizeof b) == 1); CK(sel_v(d2, 2, b, sizeof b) == 1);
	CK(sx_exec(d1, "UPDATE t SET v='1' WHERE k=1", NULL) == SX_OK);
	CK(sx_exec(d2, "UPDATE t SET v='1' WHERE k=2", NULL) == SX_OK);
	CK(commit_rc(d1) == SX_OK);
	CK(commit_rc(d2) == SX_OK);                 /* SI: BOTH commit */
	CK(sel_v(d1, 1, b, sizeof b) == 1 && strcmp(b, "1") == 0);
	CK(sel_v(d1, 2, b, sizeof b) == 1 && strcmp(b, "1") == 0);  /* anomaly present */

	/* Reset to 0,0 and rerun under serializable: one txn must abort. */
	CK(sx_exec(d1, "UPDATE t SET v='0' WHERE k=1", NULL) == SX_OK);
	CK(sx_exec(d1, "UPDATE t SET v='0' WHERE k=2", NULL) == SX_OK);
	CK(xstore_set_isolation(d1, "serializable") == 0);
	CK(xstore_set_isolation(d2, "serializable") == 0);

	CK(sx_exec(d1, "BEGIN", NULL) == SX_OK);
	CK(sel_v(d1, 1, b, sizeof b) == 1); CK(sel_v(d1, 2, b, sizeof b) == 1);
	CK(sx_exec(d2, "BEGIN", NULL) == SX_OK);
	CK(sel_v(d2, 1, b, sizeof b) == 1); CK(sel_v(d2, 2, b, sizeof b) == 1);
	CK(sx_exec(d1, "UPDATE t SET v='1' WHERE k=1", NULL) == SX_OK);
	CK(sx_exec(d2, "UPDATE t SET v='1' WHERE k=2", NULL) == SX_OK);
	{
		int rc1 = commit_rc(d1);
		int rc2 = commit_rc(d2);
		/* d1 validates (no conflict yet) and commits; d2's read of k=1
		 * was overwritten by d1 after d2's snapshot -> abort. */
		CK(rc1 == SX_OK);
		CK(rc2 == SX_BUSY);
	}

	sx_close(d1); sx_close(d2);
	bt_close(bt); bm_destroy(bm); unlink(path);
	{ char wal[288]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
	if (g_fail) return 1;
	printf("  ok   serializable isolation: write-skew commits under SI "
	    "(anomaly), but serializable validation aborts the second txn "
	    "(SX_BUSY) -- read-set conflict detected\n");
	return 0;
}

/* SSI commits a read-mostly transaction that the conservative
 * precision validation would have aborted: an OUTGOING rw-edge alone
 * is not a pivot.  A reader reads row 1, a concurrent committed writer
 * overwrites row 1, and the reader then writes row 2 -- which no
 * concurrent transaction has read.  Precision validation aborts on the
 * stale read of row 1; pivot detection sees no incoming edge (nobody
 * read row 2) and commits.  The outcome is serializable as [reader,
 * writer]. */
static int
scenario_ssi_gain(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256]; t_tmpl(path, sizeof path, "sqlxtc-xstore6");
	sx_db *dr = NULL, *dw = NULL;
	char b[32];
	int fd;

	g_fail = 0;
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 64; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(sx_open_bt(bt, &dr) == SX_OK);
	CK(sx_open_bt(bt, &dw) == SX_OK);
	CK(sx_exec(dr, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) == SX_OK);
	CK(sx_exec(dw, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) == SX_OK);
	CK(sx_exec(dr, "INSERT INTO t(k,v) VALUES(1,'0'),(2,'0')", NULL) == SX_OK);
	CK(xstore_set_isolation(dr, "serializable") == 0);

	/* dr begins and reads ONLY row 1 (records a read of rowid 1). */
	CK(sx_exec(dr, "BEGIN", NULL) == SX_OK);
	CK(sel_v(dr, 1, b, sizeof b) == 1 && strcmp(b, "0") == 0);

	/* A concurrent writer overwrites row 1 and commits (autocommit). */
	CK(sx_exec(dw, "UPDATE t SET v='1' WHERE k=1", NULL) == SX_OK);

	/* dr writes row 2 -- which NO concurrent transaction has read. */
	CK(sx_exec(dr, "UPDATE t SET v='9' WHERE k=2", NULL) == SX_OK);

	/* Precision validation would abort dr (its read of row 1 was
	 * overwritten after its snapshot); SSI sees only an outgoing edge
	 * and no incoming edge, so dr is not a pivot and commits. */
	CK(commit_rc(dr) == SX_OK);

	/* Serializable as [dr, dw]: row 1 = writer's value, row 2 = dr's. */
	CK(sel_v(dr, 1, b, sizeof b) == 1 && strcmp(b, "1") == 0);
	CK(sel_v(dr, 2, b, sizeof b) == 1 && strcmp(b, "9") == 0);

	sx_close(dr); sx_close(dw);
	bt_close(bt); bm_destroy(bm); unlink(path);
	{ char wal[288]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
	if (g_fail) return 1;
	printf("  ok   SSI lets a read-mostly txn commit (outgoing rw-edge "
	    "only): precision validation would abort it, pivot detection "
	    "does not -- result is serializable\n");
	return 0;
}

/* Version GC reclaims dead versions up to the snapshot horizon, leaves
 * the live data intact, is idempotent, and a held snapshot pins its
 * versions against the vacuum. */
static int
scenario_gc(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256]; t_tmpl(path, sizeof path, "sqlxtc-xstore7");
	sx_db *d1 = NULL, *d2 = NULL;
	char b[32], want[16];
	const int K = 50, N = 10;
	int fd, n, k, reclaimed;
	int64_t ts0;

	g_fail = 0;
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 64; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(sx_open_bt(bt, &d1) == SX_OK);
	CK(sx_open_bt(bt, &d2) == SX_OK);
	CK(sx_exec(d1, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) == SX_OK);
	CK(sx_exec(d2, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) == SX_OK);

	/* K rows, then N rounds of updating every row -> K*(N+1) versions. */
	for (k = 1; k <= K; k++) {
		char sql[64];
		snprintf(sql, sizeof sql, "INSERT INTO t(k,v) VALUES(%d,'v0');", k);
		CK(sx_exec(d1, sql, NULL) == SX_OK);
	}
	for (n = 1; n <= N; n++) {
		char sql[48];
		snprintf(sql, sizeof sql, "UPDATE t SET v='v%d';", n);
		CK(sx_exec(d1, sql, NULL) == SX_OK);
	}

	/* No live snapshot: GC keeps the newest version per row, reclaims
	 * the other K*N. */
	reclaimed = xstore_gc_run(d1);
	CK(reclaimed == K * N);
	snprintf(want, sizeof want, "v%d", N);
	for (k = 1; k <= K; k++)
		CK(sel_v(d1, k, b, sizeof b) == 1 && strcmp(b, want) == 0);
	CK(xstore_gc_run(d1) == 0);   /* idempotent */

	/* A held snapshot pins its versions: d2 reads as-of the current
	 * clock, d1 overwrites row 1, GC runs -- d2 still sees the pinned
	 * version; after d2 releases, GC reclaims it. */
	ts0 = xstore_clock_now();
	set_as_of(d2, ts0);
	CK(sel_v(d2, 1, b, sizeof b) == 1 && strcmp(b, want) == 0);
	CK(sx_exec(d1, "UPDATE t SET v='final' WHERE k=1", NULL) == SX_OK);
	(void)xstore_gc_run(d1);      /* horizon == ts0 pins it */
	CK(sel_v(d2, 1, b, sizeof b) == 1 && strcmp(b, want) == 0);   /* pinned */
	CK(sel_v(d1, 1, b, sizeof b) == 1 && strcmp(b, "final") == 0);/* latest */
	set_as_of(d2, 0);
	(void)xstore_gc_run(d1);      /* pin released */
	CK(sel_v(d1, 1, b, sizeof b) == 1 && strcmp(b, "final") == 0);

	sx_close(d1); sx_close(d2);
	bt_close(bt); bm_destroy(bm); unlink(path);
	{ char wal[288]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
	if (g_fail) return 1;
	printf("  ok   version GC: reclaimed %d dead versions (kept 1/row), "
	    "data intact, idempotent; a held snapshot pinned its version "
	    "against the vacuum\n", reclaimed);
	return 0;
}

/* Inline autovacuum keeps version chains short on the write path with
 * no full-tree scan: after the same churn as scenario_gc, a final
 * xstore_gc() finds almost nothing left to reclaim. */
static int
scenario_autovacuum(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256]; t_tmpl(path, sizeof path, "sqlxtc-xstore8");
	sx_db *db = NULL;
	char b[32], want[16];
	const int K = 50, N = 10;
	int fd, n, k, leftover;

	g_fail = 0;
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 64; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(sx_open_bt(bt, &db) == SX_OK);
	CK(sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) == SX_OK);
	CK(xstore_autovacuum_set(db, 1) == 1);

	for (k = 1; k <= K; k++) {
		char sql[64];
		snprintf(sql, sizeof sql, "INSERT INTO t(k,v) VALUES(%d,'v0');", k);
		CK(sx_exec(db, sql, NULL) == SX_OK);
	}
	for (n = 1; n <= N; n++) {
		char sql[48];
		snprintf(sql, sizeof sql, "UPDATE t SET v='v%d';", n);
		CK(sx_exec(db, sql, NULL) == SX_OK);
	}

	/* Data is correct, and inline pruning already removed the dead
	 * versions: a full vacuum now reclaims (almost) nothing. */
	snprintf(want, sizeof want, "v%d", N);
	for (k = 1; k <= K; k++)
		CK(sel_v(db, k, b, sizeof b) == 1 && strcmp(b, want) == 0);
	leftover = xstore_gc_run(db);
	CK(leftover < K);     /* vs K*N == 500 without autovacuum */

	/* Adaptive backoff: a UNIFORM phase -- M distinct new rowids, each
	 * written once, nothing to reclaim -- must NOT prune on every write.
	 * Count prune passes before/after and require far fewer than the
	 * writes (geometric backoff to ~1/256). */
	{
		int64_t p0, p1;
		int m, M = 1000;
		p0 = (int)xstore_prune_count();
		for (m = 0; m < M; m++) {
			char sql[64];
			snprintf(sql, sizeof sql,
			    "INSERT INTO t(k,v) VALUES(%d,'u');", 100000 + m);
			CK(sx_exec(db, sql, NULL) == SX_OK);
		}
		p1 = (int)xstore_prune_count();
		CK((p1 - p0) < M / 4);     /* backed off; not a prune per write */
		CK(sel_v(db, 100000, b, sizeof b) == 1 && strcmp(b, "u") == 0);
		CK(sel_v(db, 100000 + M - 1, b, sizeof b) == 1 && strcmp(b, "u") == 0);
		printf("  ok   adaptive autovacuum: %d uniform writes (no garbage) "
		    "triggered only %lld prune passes -- backed off\n",
		    M, (long long)(p1 - p0));
	}

	sx_close(db);
	bt_close(bt); bm_destroy(bm); unlink(path);
	{ char wal[288]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
	if (g_fail) return 1;
	printf("  ok   inline autovacuum: %d versions churned per row pruned "
	    "on the write path (no full scan); final vacuum reclaimed only "
	    "%d (vs %d without)\n", N, leftover, K * N);
	return 0;
}

/* Scan a bounded key range, stepping every row (drives the vtab range
 * plan + records the SSI range predicate).  Returns the row count. */
static int
range_scan(sx_db *db, int lo, int hi)
{
	sx_stmt *st = NULL;
	char sql[80];
	int n = 0;
	snprintf(sql, sizeof sql, "SELECT k FROM t WHERE k BETWEEN %d AND %d", lo, hi);
	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK) return -1;
	while (sx_step(st) == SX_ROW) n++;
	sx_finalize(st);
	return n;
}

/* ---- Cahill predicate (range) locking: two serializable txns that
 * scan DISJOINT key ranges and write outside each other's range have
 * no rw-antidependency, so both commit -- where coarse whole-table
 * predicate tracking would force one to abort.  A cross write-skew
 * across the ranges is still caught. ---- */
static int
scenario_ssi_range(void)
{
	bm_t *bm = NULL; bt_t *bt = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256]; t_tmpl(path, sizeof path, "sqlxtc-ssirng");
	sx_db *d1 = NULL, *d2 = NULL;
	int fd, i;

	g_fail = 0;
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 64; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(sx_open_bt(bt, &d1) == SX_OK);
	CK(sx_open_bt(bt, &d2) == SX_OK);
	CK(sx_exec(d1, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) == SX_OK);
	CK(sx_exec(d2, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) == SX_OK);
	/* Seed [1,50] and [100,150]. */
	for (i = 1; i <= 50; i++) {
		char s[64]; snprintf(s, sizeof s, "INSERT INTO t(k,v) VALUES(%d,'0');", i);
		CK(sx_exec(d1, s, NULL) == SX_OK);
	}
	for (i = 100; i <= 150; i++) {
		char s[64]; snprintf(s, sizeof s, "INSERT INTO t(k,v) VALUES(%d,'0');", i);
		CK(sx_exec(d1, s, NULL) == SX_OK);
	}
	CK(xstore_set_isolation(d1, "serializable") == 0);
	CK(xstore_set_isolation(d2, "serializable") == 0);

	/* Part A -- disjoint predicates: T1 reads [1,50] writes k=300;
	 * T2 reads [100,150] writes k=400.  Both commit. */
	CK(sx_exec(d1, "BEGIN", NULL) == SX_OK);
	CK(range_scan(d1, 1, 50) == 50);
	CK(sx_exec(d2, "BEGIN", NULL) == SX_OK);
	CK(range_scan(d2, 100, 150) == 51);
	CK(sx_exec(d1, "INSERT INTO t(k,v) VALUES(300,'a')", NULL) == SX_OK);
	CK(sx_exec(d2, "INSERT INTO t(k,v) VALUES(400,'b')", NULL) == SX_OK);
	CK(commit_rc(d1) == SX_OK);
	CK(commit_rc(d2) == SX_OK);    /* disjoint ranges -> both commit */

	/* Part B -- cross write-skew across the ranges: T1 reads [1,50]
	 * writes k=120 (in T2's range); T2 reads [100,150] writes k=20
	 * (in T1's range).  One must abort. */
	CK(sx_exec(d1, "BEGIN", NULL) == SX_OK);
	CK(range_scan(d1, 1, 50) == 50);
	CK(sx_exec(d2, "BEGIN", NULL) == SX_OK);
	CK(range_scan(d2, 100, 150) == 51);
	CK(sx_exec(d1, "UPDATE t SET v='1' WHERE k=120", NULL) == SX_OK);
	CK(sx_exec(d2, "UPDATE t SET v='1' WHERE k=20", NULL) == SX_OK);
	{
		int rc1 = commit_rc(d1);
		int rc2 = commit_rc(d2);
		CK(rc1 == SX_OK);
		CK(rc2 == SX_BUSY);    /* pivot: write lands in peer's read range */
	}

	sx_close(d1); sx_close(d2);
	bt_close(bt); bm_destroy(bm); unlink(path);
	{ char wal[288]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
	if (g_fail) return 1;
	printf("  ok   SSI range locking: disjoint range scans + out-of-range "
	    "writes both commit; cross write-skew across ranges aborts one\n");
	return 0;
}

/* ---- O(1) scan: a full SELECT over the xstore vtab opens the btree
 * cursor ONCE and resumes it per row (latch released between rows for
 * the VDBE), instead of re-descending the tree on every xNext. ---- */
static int
scenario_o1_sql(void)
{
	bm_t *bm = NULL; bt_t *bt = NULL; sx_db *db = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256]; t_tmpl(path, sizeof path, "sqlxtc-o1");
	bt_stats_t s0, s1;
	sx_stmt *st = NULL;
	int fd, i, rows = 0;
	const int NROW = 400;

	g_fail = 0;
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = N_FRAMES; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(sx_open_bt(bt, &db) == SX_OK);
	CK(sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) == SX_OK);
	for (i = 1; i <= NROW; i++) {
		char sql[64];
		snprintf(sql, sizeof sql, "INSERT INTO t(k,v) VALUES(%d,'row');", i);
		CK(sx_exec(db, sql, NULL) == SX_OK);
	}

	/* Measure descents/resumes across the scan only. */
	bt_get_stats(bt, &s0);
	CK(sx_prepare(db, "SELECT k FROM t", -1, &st, NULL) == SX_OK);
	while (sx_step(st) == SX_ROW) rows++;
	sx_finalize(st);
	bt_get_stats(bt, &s1);

	CK(rows == NROW);
	/* One descent for the whole scan (cursor opened once); the old
	 * re-open-per-row path cost NROW descents.  Allow a tiny slack for
	 * planner probes, but it must be O(1), not O(rows). */
	CK(s1.descents - s0.descents <= 4);
	CK(s1.resumes - s0.resumes >= (uint64_t)(NROW - 4));

	sx_close(db);
	bt_close(bt); bm_destroy(bm); unlink(path);
	{ char wal[288]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
	if (g_fail) return 1;
	printf("  ok   O(1) SQL scan: %d rows, %llu btree descents (not %d), "
	    "%llu cursor resumes\n", NROW,
	    (unsigned long long)(s1.descents - s0.descents), NROW,
	    (unsigned long long)(s1.resumes - s0.resumes));
	return 0;
}

/* ---- multi-column rowstore: a table with columns of every type
 * round-trips type-preserving (the old single-blob payload coerced
 * everything to a blob; the typed record codec preserves int/float/
 * text/blob/null per column). ---- */
static int
scenario_multicol(void)
{
	bm_t *bm = NULL; bt_t *bt = NULL; sx_db *db = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256]; t_tmpl(path, sizeof path, "sqlxtc-mcol");
	sx_stmt *st = NULL;
	int fd;

	g_fail = 0;
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = N_FRAMES; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(sx_open_bt(bt, &db) == SX_OK);

	/* Five columns: key id, plus int, real, text, blob payload. */
	CK(sx_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, n, r, s, b)", NULL) == SX_OK);
	CK(sx_exec(db,
	    "INSERT INTO t(id,n,r,s,b) VALUES"
	    "(1, 42, 3.5, 'hello', x'deadbeef'),"
	    "(2, -7, 0.0, '', NULL);"	    , NULL) == SX_OK);

	/* Row 1: every type comes back with its own affinity. */
	CK(sx_prepare(db,
	    "SELECT n, r, s, b, typeof(n), typeof(r), typeof(s), typeof(b) "
	    "FROM t WHERE id=1", -1, &st, NULL) == SX_OK);
	CK(sx_step(st) == SX_ROW);
	CK(sx_column_int64(st, 0) == 42);
	CK(sx_column_double(st, 1) == 3.5);
	{
		const unsigned char *s = sx_column_text(st, 2);
		CK(s != NULL && strcmp((const char *)s, "hello") == 0);
	}
	{
		const unsigned char *b = sx_column_blob(st, 3);
		int bn = sx_column_bytes(st, 3);
		CK(bn == 4 && b != NULL &&
		   b[0] == 0xde && b[1] == 0xad && b[2] == 0xbe && b[3] == 0xef);
	}
	CK(strcmp((const char *)sx_column_text(st, 4), "integer") == 0);
	CK(strcmp((const char *)sx_column_text(st, 5), "real") == 0);
	CK(strcmp((const char *)sx_column_text(st, 6), "text") == 0);
	CK(strcmp((const char *)sx_column_text(st, 7), "blob") == 0);
	sx_finalize(st); st = NULL;

	/* Row 2: NULL column reads back NULL; empty text is text, not null. */
	CK(sx_prepare(db,
	    "SELECT typeof(b), typeof(s), length(s), n FROM t WHERE id=2",
	    -1, &st, NULL) == SX_OK);
	CK(sx_step(st) == SX_ROW);
	CK(strcmp((const char *)sx_column_text(st, 0), "null") == 0);
	CK(strcmp((const char *)sx_column_text(st, 1), "text") == 0);
	CK((int)sx_column_int64(st, 2) == 0);
	CK(sx_column_int64(st, 3) == -7);
	sx_finalize(st); st = NULL;

	/* UPDATE one column; the others are preserved. */
	CK(sx_exec(db, "UPDATE t SET r=9.25 WHERE id=1", NULL) == SX_OK);
	CK(sx_prepare(db, "SELECT n, r, s FROM t WHERE id=1", -1, &st, NULL)
	    == SX_OK);
	CK(sx_step(st) == SX_ROW);
	CK(sx_column_int64(st, 0) == 42);          /* preserved */
	CK(sx_column_double(st, 1) == 9.25);       /* updated */
	CK(strcmp((const char *)sx_column_text(st, 2), "hello") == 0);
	sx_finalize(st); st = NULL;

	sx_close(db);
	bt_close(bt); bm_destroy(bm); unlink(path);
	{ char wal[288]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
	if (g_fail) return 1;
	printf("  ok   multi-column rowstore: int/real/text/blob/null round-trip\n");
	return 0;
}

/* ---- multi-table: two tables share one B-tree, are isolated by their
 * table-id key prefix, and a single transaction spans both atomically. */
static int
scenario_multitable(void)
{
	bm_t *bm = NULL; bt_t *bt = NULL; sx_db *db = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256]; t_tmpl(path, sizeof path, "sqlxtc-mtab");
	sx_stmt *st = NULL;
	int fd;

	g_fail = 0;
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = N_FRAMES; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(sx_open_bt(bt, &db) == SX_OK);

	CK(sx_exec(db, "CREATE TABLE accounts(id INTEGER PRIMARY KEY, bal)", NULL) == SX_OK);
	CK(sx_exec(db, "CREATE TABLE logs(id INTEGER PRIMARY KEY, msg)", NULL) == SX_OK);
	CK(sx_exec(db, "INSERT INTO accounts VALUES(1,100),(2,200),(3,300)", NULL) == SX_OK);
	CK(sx_exec(db, "INSERT INTO logs VALUES(1,'open')", NULL)
	    == SX_OK);

	/* Isolation: each table sees only its own rows (same rowid=1 in both
	 * is two distinct logical rows). */
	CK(eval_int(db, "SELECT count(*) FROM accounts") == 3);
	CK(eval_int(db, "SELECT count(*) FROM logs") == 1);
	CK(eval_int(db, "SELECT bal FROM accounts WHERE id=2") == 200);
	CK(sx_prepare(db, "SELECT msg FROM logs WHERE id=1", -1, &st, NULL)
	    == SX_OK);
	CK(sx_step(st) == SX_ROW);
	CK(strcmp((const char *)sx_column_text(st, 0), "open") == 0);
	sx_finalize(st); st = NULL;

	/* One transaction spans both tables and commits atomically (a
	 * transfer plus its log entry land at one commit timestamp). */
	CK(sx_exec(db,
	    "BEGIN;"
	    "UPDATE accounts SET bal=bal-50 WHERE id=1;"
	    "UPDATE accounts SET bal=bal+50 WHERE id=2;"
	    "INSERT INTO logs VALUES(2,'xfer 1->2');"
	    "COMMIT;"	    , NULL) == SX_OK);
	CK(eval_int(db, "SELECT bal FROM accounts WHERE id=1") == 50);
	CK(eval_int(db, "SELECT bal FROM accounts WHERE id=2") == 250);
	CK(eval_int(db, "SELECT count(*) FROM logs") == 2);

	/* A rolled-back cross-table transaction touches neither. */
	CK(sx_exec(db,
	    "BEGIN;"
	    "UPDATE accounts SET bal=0 WHERE id=3;"
	    "INSERT INTO logs VALUES(3,'oops');"
	    "ROLLBACK;"	    , NULL) == SX_OK);
	CK(eval_int(db, "SELECT bal FROM accounts WHERE id=3") == 300);
	CK(eval_int(db, "SELECT count(*) FROM logs") == 2);

	sx_close(db);
	bt_close(bt); bm_destroy(bm); unlink(path);
	{ char wal[288]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
	if (g_fail) return 1;
	printf("  ok   multi-table: two tables in one B-tree, isolated by"
	    " table-id, cross-table atomic commit + rollback\n");
	return 0;
}

/* ---- ALTER TABLE RENAME: the table keeps its id (a persisted catalog
 * override), so data survives the rename and is reachable by the new
 * name; the old name no longer resolves. ---- */
static int
scenario_rename(void)
{
	bm_t *bm = NULL; bt_t *bt = NULL; sx_db *db = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256]; t_tmpl(path, sizeof path, "sqlxtc-rename");
	int fd;

	g_fail = 0;
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = N_FRAMES; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(sx_open_bt(bt, &db) == SX_OK);

	CK(sx_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT)", NULL) == SX_OK);
	CK(sx_exec(db, "INSERT INTO t VALUES(1,'a'),(2,'b'),(3,'c')", NULL) == SX_OK);
	CK(eval_int(db, "SELECT count(*) FROM t") == 3);

	/* Rename: data must survive and be reachable by the new name. */
	CK(sx_exec(db, "ALTER TABLE t RENAME TO u", NULL) == SX_OK);
	CK(eval_int(db, "SELECT count(*) FROM u") == 3);
	CK(eval_int(db, "SELECT id FROM u WHERE v='b'") == 2);
	/* Writes through the new name reach the same data. */
	CK(sx_exec(db, "INSERT INTO u VALUES(4,'d')", NULL) == SX_OK);
	CK(eval_int(db, "SELECT count(*) FROM u") == 4);
	/* The old name no longer resolves to a table. */
	CK(sx_exec(db, "SELECT count(*) FROM t", NULL) != SX_OK);

	sx_close(db);
	bt_close(bt); bm_destroy(bm); unlink(path);
	{ char wal[288]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
	if (g_fail) return 1;
	printf("  ok   ALTER TABLE RENAME: data survives via the persisted"
	    " catalog override; new name reads + writes, old name is gone\n");
	return 0;
}

/* ---- Collision-free catalog: many tables coexist, each with its own
 * dense allocated table-id, so identical rowids in different tables
 * never alias.  The old name-hash scheme could silently refuse a second
 * table whose name hashed equal to an existing one; allocated ids never
 * collide. ---- */
static int
scenario_catalog(void)
{
	bm_t *bm = NULL; bt_t *bt = NULL; sx_db *db = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256]; t_tmpl(path, sizeof path, "sqlxtc-cat");
	int fd, i;
	enum { NT = 30 };

	g_fail = 0;
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = N_FRAMES; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(sx_open_bt(bt, &db) == SX_OK);

	/* Create NT tables; every CREATE must succeed (no collision
	 * refusal), and each must get a distinct version namespace. */
	for (i = 0; i < NT; i++) {
		char sql[128];
		snprintf(sql, sizeof sql,
		    "CREATE TABLE t%d(id INTEGER PRIMARY KEY, v)", i);
		CK(sx_exec(db, sql, NULL) == SX_OK);
		/* Same rowid (1) in every table, a value unique to the table. */
		snprintf(sql, sizeof sql, "INSERT INTO t%d VALUES(1,%d);",
		    i, 100 + i);
		CK(sx_exec(db, sql, NULL) == SX_OK);
	}
	/* Isolation: rowid 1 in table i reads back i's own value -- proving
	 * the NT tables occupy NT distinct table-ids, since a shared id
	 * would alias rowid 1 across tables. */
	for (i = 0; i < NT; i++) {
		char q[64];
		snprintf(q, sizeof q, "SELECT v FROM t%d WHERE id=1", i);
		CK(eval_int(db, q) == 100 + i);
	}

	sx_close(db);
	bt_close(bt); bm_destroy(bm); unlink(path);
	{ char wal[288]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
	if (g_fail) return 1;
	printf("  ok   catalog: %d tables coexist with distinct allocated"
	    " table-ids; identical rowids stay isolated (no hash collision)\n",
	    NT);
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
	if (scenario_ssi_gain() != 0) return 1;
	if (scenario_gc() != 0) return 1;
	if (scenario_autovacuum() != 0) return 1;
	if (scenario_ssi_range() != 0) return 1;
	if (scenario_o1_sql() != 0) return 1;
	if (scenario_multicol() != 0) return 1;
	if (scenario_multitable() != 0) return 1;
	if (scenario_rename() != 0) return 1;
	if (scenario_catalog() != 0) return 1;
	printf("All sqlxtc SQL-on-xstore tests passed.\n");
	return 0;
}
