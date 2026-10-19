/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_row_intents.c
 *	Row write intents: a write-write conflict surfaces at the
 *	conflicting STATEMENT (first-updater-wins), never only at COMMIT,
 *	on fibers on 1 and 4 loops:
 *	  (a) wait then succeed (READ COMMITTED): A updates row 1 in a
 *	      transaction; B's UPDATE of row 1 waits for A, and once A
 *	      commits it re-runs on the new row and succeeds; final value
 *	      has both increments, B's COMMIT succeeds.
 *	  (b) wait then conflict (SNAPSHOT): same, but B's snapshot predates
 *	      A's commit, so B's UPDATE fails "could not serialize" -- at the
 *	      UPDATE -- and B's later statements fail "transaction is
 *	      aborted" until ROLLBACK.
 *	  (d) wait then succeed (SNAPSHOT, the UPDATE is B's first access):
 *	      B has observed nothing, so the UPDATE re-runs on a snapshot
 *	      taken after A's commit (as a later BEGIN would) and succeeds.
 *	  (c) deadlock: A holds r1 and wants r2, B holds r2 and wants r1:
 *	      exactly one of the two second UPDATEs fails, quickly (well
 *	      under the intent wait cap), the other then succeeds and
 *	      commits; each row ends with only the winner's increment.
 *	Before row intents B's UPDATE in (a)/(b) succeeded at once and the
 *	conflict appeared only at B's COMMIT; in (c) both UPDATEs succeeded
 *	and the loser was found only at COMMIT.
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

static bt_t        *g_bt;
static wal_t       *g_wal;
static sx_db       *g_c[2];
static _Atomic int  g_step, g_left;
static int          g_case;
/* Results of the current case, written by the workers. */
static int          g_upd_rc[2], g_commit_rc[2], g_after_rc[2];
static char         g_msg[2][160];
static int64_t      g_t0[2], g_t1[2];

static int
run(sx_db *db, const char *sql, char *msg, size_t cap)
{
	char *err = NULL;
	int rc = sx_exec(db, sql, &err);
	if (msg != NULL) snprintf(msg, cap, "%s", rc == SX_OK ? "" : sx_errmsg(db));
	free(err);
	return rc;
}

static int64_t
val(sx_db *db, int id)
{
	sx_stmt *st = NULL;
	char q[64];
	int64_t v = -1;
	snprintf(q, sizeof q, "SELECT v FROM t WHERE id=%d", id);
	if (sx_prepare(db, q, -1, &st, NULL) != SX_OK) return -2;
	if (sx_step(st) == SX_ROW) v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

/* Wait (napping) until the shared step counter reaches n. */
static void
await(int n)
{
	while (atomic_load(&g_step) < n)
		(void)xtc_proc_sleep(50LL * 1000);
}

static void
worker(void *arg)
{
	long me = (long)arg;
	sx_db *db = g_c[me];
	if (g_case != 2) {
		/* (a) READ COMMITTED / (b) SNAPSHOT / (d) SNAPSHOT, B's UPDATE
		 * first: A takes row 1 first. */
		const char *begin = g_case == 0
		    ? "BEGIN ISOLATION LEVEL READ COMMITTED" : "BEGIN";
		if (me == 0) {
			CK(run(db, begin, NULL, 0) == SX_OK);
			CK(run(db, "UPDATE t SET v=v+10 WHERE id=1", NULL, 0) == SX_OK);
			atomic_store(&g_step, 1);
			await(2);                         /* B started (and reads) */
			(void)xtc_proc_sleep(5LL * 1000 * 1000);   /* B is waiting */
			g_commit_rc[0] = run(db, "COMMIT", NULL, 0);
		} else {
			await(1);
			CK(run(db, begin, NULL, 0) == SX_OK);
			if (g_case != 3)
				CK(val(db, 2) == 0);      /* fixes B's snapshot */
			atomic_store(&g_step, 2);
			g_t0[1] = xtc_clock_mono();
			g_upd_rc[1] = run(db, "UPDATE t SET v=v+20 WHERE id=1",
			    g_msg[1], sizeof g_msg[1]);
			g_t1[1] = xtc_clock_mono();
			/* After a failed statement the block is aborted. */
			g_after_rc[1] = run(db, "SELECT v FROM t WHERE id=2", NULL, 0);
			g_commit_rc[1] = run(db, g_upd_rc[1] == SX_OK ? "COMMIT"
			    : "ROLLBACK", NULL, 0);
		}
	} else {
		/* (c) deadlock: A holds r1 wants r2; B holds r2 wants r1. */
		int mine = (int)me + 1, other = 2 - (int)me;
		char q[64];
		CK(run(db, "BEGIN", NULL, 0) == SX_OK);
		snprintf(q, sizeof q, "UPDATE t SET v=v+1 WHERE id=%d", mine);
		CK(run(db, q, NULL, 0) == SX_OK);
		atomic_fetch_add(&g_step, 1);
		await(2);                                 /* both hold one */
		snprintf(q, sizeof q, "UPDATE t SET v=v+1 WHERE id=%d", other);
		g_t0[me] = xtc_clock_mono();
		g_upd_rc[me] = run(db, q, g_msg[me], sizeof g_msg[me]);
		g_t1[me] = xtc_clock_mono();
		g_commit_rc[me] = run(db, g_upd_rc[me] == SX_OK ? "COMMIT"
		    : "ROLLBACK", NULL, 0);
	}
	if (atomic_fetch_sub(&g_left, 1) == 1)
		(void)wal_writer_stop(g_wal);
}

static int
one_case(int c, int nloops)
{
	char logp[256], pg[256];
	wal_opts_t wo = { 0 };
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	xtc_exec_t *ex = NULL;
	xtc_proc_opts_t po = { .name = "ilk" };
	xtc_pid_t pid;
	int fd;
	long i;
	uint64_t w0 = xstore_intent_waits(), d0 = xstore_intent_deadlocks();
	int64_t f1, f2;

	t_tmpl(logp, sizeof logp, "sqlxtc-ilk-log");
	t_tmpl(pg, sizeof pg, "sqlxtc-ilk-pg");
	if ((fd = mkstemp(logp)) < 0) return 1;
	close(fd);
	if ((fd = mkstemp(pg)) < 0) return 1;
	close(fd);
	wo.path = logp;
	if (wal_open(&wo, &g_wal) != XTC_OK) return 1;
	bo.path = pg; bo.page_size = 4096; bo.n_frames = 256;
	if (bm_create(&bo, &bm) != XTC_OK || bt_open(bm, &g_bt) != XTC_OK)
		return 1;
	xstore_set_wal((struct wal *)g_wal);
	for (i = 0; i < 2; i++)
		if (sx_open_bt(g_bt, &g_c[i]) != SX_OK) return 1;
	CK(run(g_c[0], "CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER)", NULL, 0) == SX_OK);
	CK(run(g_c[0], "INSERT INTO t(id,v) VALUES(1,0)", NULL, 0) == SX_OK);
	CK(run(g_c[0], "INSERT INTO t(id,v) VALUES(2,0)", NULL, 0) == SX_OK);
	g_case = c;
	atomic_store(&g_step, 0);
	atomic_store(&g_left, 2);
	memset(g_upd_rc, 0, sizeof g_upd_rc);
	memset(g_commit_rc, 0, sizeof g_commit_rc);
	memset(g_after_rc, 0, sizeof g_after_rc);
	memset(g_msg, 0, sizeof g_msg);
	if (xtc_exec_init(&ex, nloops) != XTC_OK) return 1;
	if (wal_writer_spawn(g_wal, xtc_exec_loop(ex, 0), NULL) != XTC_OK) return 1;
	for (i = 0; i < 2; i++)
		if (xtc_proc_spawn(xtc_exec_loop(ex, (int)(i % nloops)), worker,
		    (void *)i, &po, &pid) != XTC_OK)
			return 1;
	if (xtc_exec_run(ex) != XTC_OK) return 1;
	(void)xtc_exec_fini(ex);
	f1 = val(g_c[0], 1);
	f2 = val(g_c[0], 2);
	if (c == 0) {
		printf("  [%d loop%s] RC wait: B UPDATE rc=%d after %lld us, COMMIT "
		    "A=%d B=%d, row1=%lld, waits=%llu\n", nloops, nloops > 1 ? "s" : "",
		    g_upd_rc[1], (long long)(g_t1[1] - g_t0[1]) / 1000,
		    g_commit_rc[0], g_commit_rc[1], (long long)f1,
		    (unsigned long long)(xstore_intent_waits() - w0));
		CK(g_upd_rc[1] == SX_OK);            /* waited, then re-ran */
		CK(g_commit_rc[0] == SX_OK && g_commit_rc[1] == SX_OK);
		CK(f1 == 30);                        /* both increments */
		CK(xstore_intent_waits() > w0);      /* B really waited */
		CK(g_t1[1] - g_t0[1] >= 2LL * 1000 * 1000);
	} else if (c == 1) {
		printf("  [%d loop%s] SI wait: B UPDATE rc=%d (%s) after %lld us, next "
		    "stmt rc=%d, COMMIT A=%d, row1=%lld\n", nloops, nloops > 1 ? "s" : "",
		    g_upd_rc[1], g_msg[1], (long long)(g_t1[1] - g_t0[1]) / 1000,
		    g_after_rc[1], g_commit_rc[0], (long long)f1);
		CK(g_upd_rc[1] == SX_BUSY);          /* at the UPDATE ... */
		CK(strstr(g_msg[1], "could not serialize") != NULL);
		CK(g_after_rc[1] != SX_OK);          /* ... the block is aborted */
		CK(g_commit_rc[1] == SX_OK);         /* ROLLBACK ends it */
		CK(g_commit_rc[0] == SX_OK);
		CK(f1 == 10);                        /* A's only */
		CK(g_t1[1] - g_t0[1] >= 2LL * 1000 * 1000);   /* it waited for A */
	} else if (c == 3) {
		printf("  [%d loop%s] SI first-access wait: B UPDATE rc=%d after %lld us, "
		    "COMMIT A=%d B=%d, row1=%lld\n", nloops, nloops > 1 ? "s" : "",
		    g_upd_rc[1], (long long)(g_t1[1] - g_t0[1]) / 1000,
		    g_commit_rc[0], g_commit_rc[1], (long long)f1);
		CK(g_upd_rc[1] == SX_OK);            /* re-ran on a new snapshot */
		CK(g_commit_rc[0] == SX_OK && g_commit_rc[1] == SX_OK);
		CK(f1 == 30);
		CK(g_t1[1] - g_t0[1] >= 2LL * 1000 * 1000);
	} else {
		int lost = (g_upd_rc[0] != SX_OK) + (g_upd_rc[1] != SX_OK);
		int w = g_upd_rc[0] == SX_OK ? 0 : 1, l = 1 - w;
		int64_t lt = g_t1[l] - g_t0[l];
		printf("  [%d loop%s] deadlock: UPDATE rc A=%d B=%d (%s), loser "
		    "failed after %lld us, COMMIT winner=%d, rows=%lld,%lld, "
		    "deadlocks=%llu\n", nloops, nloops > 1 ? "s" : "",
		    g_upd_rc[0], g_upd_rc[1], g_msg[l], (long long)lt / 1000,
		    g_commit_rc[w], (long long)f1, (long long)f2,
		    (unsigned long long)(xstore_intent_deadlocks() - d0));
		CK(lost == 1);                       /* exactly one fails */
		CK(strstr(g_msg[l], "could not serialize") != NULL);
		CK(g_commit_rc[w] == SX_OK);
		CK(f1 == 1 && f2 == 1);              /* winner's +1s; loser's undone */
		CK(lt < 1000LL * 1000 * 1000);       /* quickly: not the 10 s cap */
		CK(xstore_intent_deadlocks() > d0);
	}
	for (i = 0; i < 2; i++) sx_close(g_c[i]);
	xstore_set_wal(NULL);
	bt_close(g_bt);
	bm_destroy(bm);
	wal_close(g_wal);
	unlink(logp); unlink(pg);
	return 0;
}

int
main(void)
{
	int nloops, c;
	if (xtc_set_stack_size(512u * 1024u) != XTC_OK) return 1;
	for (nloops = 1; nloops <= 4; nloops += 3)
		for (c = 0; c < 4; c++)
			if (one_case(c, nloops) != 0) return 1;
	if (g_fail) { fprintf(stderr, "FAIL: test_row_intents\n"); return 1; }
	printf("  ok   row write intents: conflicts surface at the statement "
	    "(wait, re-run or fail), deadlocks pick one victim\n");
	return 0;
}
