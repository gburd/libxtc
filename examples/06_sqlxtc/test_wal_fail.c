/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_wal_fail.c
 *	WAL write-failure handling (fail-stop).  The log file is capped with
 *	RLIMIT_FSIZE (SIGXFSZ ignored), so once it reaches the cap every
 *	pwrite fails with EFBIG -- the same path an ENOSPC or EIO takes.
 *
 *	1. WAL level: committer procs on a loop drive wal_commit until the
 *	   cap is hit.  Every commit acked XTC_OK must replay from the file;
 *	   at least one commit must fail; once one fails, every later one
 *	   fails (fail-stop); durable_lsn never covers a failed record; and
 *	   wal_commit_sync is refused too.
 *	2. Engine level: SQL autocommit INSERTs and BEGIN..COMMIT txns over
 *	   the native engine until the log fails.  A statement that returned
 *	   SX_OK must be present after recovery from the log; a failed one
 *	   must have reported an error; and after the failure the engine is
 *	   read-only (writes fail, reads still work).
 *
 *	Before the fix the writer ignored the write/fsync result, advanced
 *	durable_lsn and acked every committer: this test then sees zero
 *	failed commits and acked records missing from the replay.
 */

#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "btree.h"
#include "bufmgr.h"
#include "engine.h"
#include "wal.h"
#include "xstore.h"
#include "xtc.h"
#include "xtc_async.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "t_tmp.h"

#define CAP_BYTES   (64 * 1024)
#define N_CMT       8
#define PER_CMT     400
#define PAGE_SZ     4096

static wal_t       *g_wal;
static _Atomic int  g_left;
static _Atomic long g_ok, g_err, g_err_then_ok;
static _Atomic uint64_t g_max_ok_lsn;
static uint8_t      g_acked[N_CMT * PER_CMT];   /* 1 = acked XTC_OK */

static void
cap_fsize(rlim_t cap)
{
	struct rlimit rl;
	rl.rlim_cur = cap;
	rl.rlim_max = RLIM_INFINITY;
	if (getrlimit(RLIMIT_FSIZE, &rl) == 0)
		rl.rlim_cur = cap;
	(void)setrlimit(RLIMIT_FSIZE, &rl);
}

static void
committer(void *arg)
{
	long id = (long)arg;
	int i, seen_err = 0;
	char rec[200];

	for (i = 0; i < PER_CMT; i++) {
		uint64_t lsn = 0;
		int k = (int)id * PER_CMT + i, rc;
		memset(rec, 'x', sizeof rec);
		memcpy(rec, &k, sizeof k);
		rc = wal_commit(g_wal, rec, sizeof rec, &lsn);
		if (rc == XTC_OK) {
			uint64_t m = atomic_load(&g_max_ok_lsn);
			g_acked[k] = 1;
			atomic_fetch_add(&g_ok, 1);
			if (seen_err)
				atomic_fetch_add(&g_err_then_ok, 1);
			while (lsn > m && !atomic_compare_exchange_weak(&g_max_ok_lsn, &m, lsn))
				;
		} else {
			CK(rc == XTC_E_IO);
			seen_err = 1;
			atomic_fetch_add(&g_err, 1);
		}
	}
	if (atomic_fetch_sub(&g_left, 1) == 1)
		(void)wal_writer_stop(g_wal);
}

struct replay { uint8_t seen[N_CMT * PER_CMT]; int n; };
static int
replay_cb(uint64_t lsn, const void *rec, uint32_t len, void *user)
{
	struct replay *r = user;
	int k;
	(void)lsn;
	if (len >= sizeof k) {
		memcpy(&k, rec, sizeof k);
		if (k >= 0 && k < N_CMT * PER_CMT) r->seen[k] = 1;
	}
	r->n++;
	return 0;
}

static int
test_wal_level(void)
{
	char path[256];
	wal_opts_t wo = { 0 };
	wal_stats_t st;
	xtc_loop_t *loop = NULL;
	struct replay *rp;
	uint64_t lsn = 0;
	long c;
	int fd, k, lost = 0;

	t_tmpl(path, sizeof path, "sqlxtc-walfail");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	wo.path = path; wo.max_batch = 32;
	if (wal_open(&wo, &g_wal) != XTC_OK) return 1;
	atomic_store(&g_left, N_CMT);

	cap_fsize(CAP_BYTES);
	if (xtc_loop_init(&loop) != XTC_OK) return 1;
	if (wal_writer_spawn(g_wal, loop, NULL) != XTC_OK) return 1;
	for (c = 0; c < N_CMT; c++) {
		xtc_proc_opts_t po = { .name = "cmt" };
		xtc_pid_t pid;
		if (xtc_proc_spawn(loop, committer, (void *)c, &po, &pid) != XTC_OK)
			return 1;
	}
	if (xtc_loop_run(loop) != XTC_OK) return 1;
	(void)xtc_loop_fini(loop);

	/* Off-loop path is refused too once the log has failed. */
	wal_get_stats(g_wal, &st);
#ifdef WAL_HAVE_FAILSTOP
	CK(wal_failed(g_wal) == 1);
	CK(wal_commit_sync(g_wal, "y", 1, &lsn) == XTC_E_IO);
#else
	(void)lsn;
#endif
	cap_fsize(RLIM_INFINITY);
	CK(st.durable_lsn == atomic_load(&g_max_ok_lsn));
	wal_close(g_wal);

	rp = calloc(1, sizeof *rp);
	if (rp == NULL) return 1;
	CK(wal_scan(path, replay_cb, rp) == XTC_OK);
	for (k = 0; k < N_CMT * PER_CMT; k++)
		if (g_acked[k] && !rp->seen[k]) lost++;
	printf("  wal: acked=%ld failed=%ld acked-after-fail=%ld replayed=%d "
	    "acked-but-lost=%d durable_lsn=%llu\n", atomic_load(&g_ok),
	    atomic_load(&g_err), atomic_load(&g_err_then_ok), rp->n, lost,
	    (unsigned long long)st.durable_lsn);
	CK(atomic_load(&g_ok) > 0);
	CK(atomic_load(&g_err) > 0);
	CK(atomic_load(&g_err_then_ok) == 0);
	CK(lost == 0);
	free(rp);
	unlink(path);
	return 0;
}

/* ---- engine level ---- */

#define E_ROWS 3000
static sx_db       *g_db;
static uint8_t      g_e_ok[E_ROWS + 1];
static _Atomic long g_e_ok_n, g_e_err_n, g_e_ok_after;
static _Atomic int  g_e_read_ok;

static int
exec_ok(const char *sql)
{
	return sx_exec(g_db, sql, NULL) == SX_OK;
}

static void
engine_worker(void *arg)
{
	char sql[512], pad[201];
	int i, failed = 0;
	sx_stmt *st = NULL;
	(void)arg;
	memset(pad, 'p', 200); pad[200] = '\0';
	for (i = 1; i <= E_ROWS; i++) {
		int ok;
		if (i % 3 == 0) {
			/* explicit txn: one row; COMMIT's result decides */
			if (!exec_ok("BEGIN")) { ok = 0; goto rec; }
			snprintf(sql, sizeof sql,
			    "INSERT INTO t(k,v) VALUES(%d,'%s')", i, pad);
			if (!exec_ok(sql)) { (void)exec_ok("ROLLBACK"); ok = 0; goto rec; }
			ok = exec_ok("COMMIT");
		} else {
			snprintf(sql, sizeof sql,
			    "INSERT INTO t(k,v) VALUES(%d,'%s')", i, pad);
			ok = exec_ok(sql);
		}
rec:
		if (ok) {
			g_e_ok[i] = 1;
			atomic_fetch_add(&g_e_ok_n, 1);
			if (failed) atomic_fetch_add(&g_e_ok_after, 1);
		} else {
			failed = 1;
			atomic_fetch_add(&g_e_err_n, 1);
#ifdef WAL_HAVE_FAILSTOP
			CK(xstore_wal_failed() == 1);
			CK(strstr(sx_errmsg(g_db), "I/O error") != NULL);
#endif
		}
	}
	/* read-only after fail-stop: a read still answers */
	if (sx_prepare(g_db, "SELECT count(*) FROM t", -1, &st, NULL) == SX_OK) {
		if (sx_step(st) == SX_ROW && sx_column_int64(st, 0) >= 0)
			atomic_store(&g_e_read_ok, 1);
		sx_finalize(st);
	}
	(void)wal_writer_stop(g_wal);
}

static int
test_engine_level(void)
{
	char logp[256], pa[256], pb[256];
	wal_opts_t wo = { 0 };
	bm_opts_t bo = BM_OPTS_DEFAULT, b2 = BM_OPTS_DEFAULT;
	bm_t *bm = NULL, *bm2 = NULL;
	bt_t *bt = NULL, *bt2 = NULL;
	sx_db *db2 = NULL;
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t po = { .name = "eng" };
	xtc_pid_t pid;
	int fd, i, lost = 0, present = 0;

	t_tmpl(logp, sizeof logp, "sqlxtc-walfail-log");
	t_tmpl(pa, sizeof pa, "sqlxtc-walfail-A");
	t_tmpl(pb, sizeof pb, "sqlxtc-walfail-B");
	if ((fd = mkstemp(logp)) < 0) return 1;
	close(fd);
	if ((fd = mkstemp(pa)) < 0) return 1;
	close(fd);
	if ((fd = mkstemp(pb)) < 0) return 1;
	close(fd);

	wo.path = logp;
	if (wal_open(&wo, &g_wal) != XTC_OK) return 1;
	/* Large pool: nothing is evicted, so the page file stays empty and
	 * the WAL is the only durable copy (and the only file that grows). */
	bo.path = pa; bo.page_size = PAGE_SZ; bo.n_frames = 2048;
	if (bm_create(&bo, &bm) != XTC_OK || bt_open(bm, &bt) != XTC_OK)
		return 1;
	xstore_set_wal((struct wal *)g_wal);
	if (sx_open_bt(bt, &g_db) != SX_OK) return 1;
	if (sx_exec(g_db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) != SX_OK)
		return 1;

	cap_fsize(CAP_BYTES * 4);
	if (xtc_loop_init(&loop) != XTC_OK) return 1;
	if (wal_writer_spawn(g_wal, loop, NULL) != XTC_OK) return 1;
	if (xtc_proc_spawn(loop, engine_worker, NULL, &po, &pid) != XTC_OK) return 1;
	if (xtc_loop_run(loop) != XTC_OK) return 1;
	(void)xtc_loop_fini(loop);
	cap_fsize(RLIM_INFINITY);

	/* crash: drop the pool unflushed, recover a fresh tree from the log */
	sx_close(g_db);
	xstore_set_wal(NULL);
	bt_close(bt);
	bm_destroy(bm);
	wal_close(g_wal);

	b2.path = pb; b2.page_size = PAGE_SZ; b2.n_frames = 2048;
	if (bm_create(&b2, &bm2) != XTC_OK || bt_open(bm2, &bt2) != XTC_OK)
		return 1;
	CK(xstore_recover(bt2, logp) == XTC_OK);
	if (sx_open_bt(bt2, &db2) != SX_OK) return 1;
	(void)sx_exec(db2, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL);
	for (i = 1; i <= E_ROWS; i++) {
		sx_stmt *st = NULL;
		int got = 0;
		if (sx_prepare(db2, "SELECT k FROM t WHERE k=?", -1, &st, NULL) != SX_OK)
			return 1;
		sx_bind_int64(st, 1, i);
		got = (sx_step(st) == SX_ROW);
		sx_finalize(st);
		if (got) present++;
		if (g_e_ok[i] && !got) lost++;
	}
	printf("  engine: ok=%ld failed=%ld ok-after-fail=%ld present-after-"
	    "recovery=%d acked-but-lost=%d read-after-fail=%d\n",
	    atomic_load(&g_e_ok_n), atomic_load(&g_e_err_n),
	    atomic_load(&g_e_ok_after), present, lost, atomic_load(&g_e_read_ok));
	CK(atomic_load(&g_e_ok_n) > 0);
	CK(atomic_load(&g_e_err_n) > 0);
	CK(atomic_load(&g_e_ok_after) == 0);
	CK(lost == 0);
	CK(atomic_load(&g_e_read_ok) == 1);
	sx_close(db2); bt_close(bt2); bm_destroy(bm2);
	unlink(logp); unlink(pa); unlink(pb);
	return 0;
}

int
main(void)
{
	signal(SIGXFSZ, SIG_IGN);
	/* SQL runs on fibers: use the server's 512 KiB stack (main.c), not
	 * the 64 KiB library default, which sanitizer frames can exceed. */
	if (xtc_set_stack_size(512u * 1024u) != XTC_OK) return 1;
	if (test_wal_level() != 0) { fprintf(stderr, "FAIL: wal setup\n"); return 1; }
	if (test_engine_level() != 0) { fprintf(stderr, "FAIL: engine setup\n"); return 1; }
	if (g_fail) { fprintf(stderr, "FAIL: test_wal_fail\n"); return 1; }
	printf("  ok   WAL fail-stop: failed log writes are refused, never acked;"
	    " every acked commit survives recovery\n");
	return 0;
}
