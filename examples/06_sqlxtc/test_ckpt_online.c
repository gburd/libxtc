/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_ckpt_online.c
 *	The periodic log checkpoint must not stop commits for the whole
 *	rewrite, and must keep every acknowledged commit.
 *
 *	The quiesced checkpoint (xstore_checkpoint_wal under
 *	xstore_quiesce_*) held every commit while it dumped the live tree:
 *	0.5-3.5 s per checkpoint at TPC-C W=1 and ~16 s at W=10, a third of
 *	a C=1 run with no commit at all.  The online checkpoint quiesces only
 *	to take a start point and to swap files.
 *
 *	A loaded table (NBIG rows, so a dump takes measurable time) plus a
 *	small hot table; committer procs update the hot rows while a
 *	checkpointer proc runs CHECKPOINTS checkpoints back to back.  Asserts:
 *	  (1) commits completed WHILE a checkpoint was between its start
 *	      point and its swap (the quiesced version: 0 -- every commit
 *	      waits at the gate for the whole dump);
 *	  (2) the online checkpoints copied live frames (commits landed in
 *	      the old log during the dump and were carried over);
 *	  (3) after a crash (no flush, no clean marker) recovery from the
 *	      compacted log alone restores every row's last acknowledged
 *	      value, and the big table intact;
 *	  (4) the log stayed bounded (it is compacted, not just copied);
 *	  (5) shutdown during an online checkpoint does not hang: a
 *	      checkpoint still dumping when the engine stops (its final
 *	      quiesce closes the gate for good) gives up and leaves the old
 *	      log, which still recovers every acknowledged commit.  (A W=10
 *	      TPC-C load hung here: the dump outlived sx_ckpt_stop's wait.)
 *	SQLXTC_CKPT_QUIESCED=1 runs the same workload with the quiesced
 *	checkpoint, for the fail-before.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "engine.h"
#include "wal.h"
#include "xstore.h"
#include "xtc.h"
#include "xtc_async.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "t_tmp.h"

#define NBIG        20000
#define NHOT        64
#define NCMT        4
#define CHECKPOINTS 4

static int          g_fail;
static int          g_quiesced;
static char         g_path[256], g_wal[300];
static _Atomic int  g_left, g_stop, g_in_ckpt;
static _Atomic long g_commits, g_during, g_last[NHOT];
static uint64_t     g_tail;
static long         g_max_wal;

#ifdef CK
#undef CK
#endif
#define CK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	g_fail = 1; } } while (0)

static long
wal_bytes(void)
{
	struct stat sb;
	return stat(g_wal, &sb) == 0 ? (long)sb.st_size : -1;
}

static void
committer(void *arg)
{
	long id = (long)arg, r;
	sx_db *h = NULL;
	char sql[160];
	if (sx_open(":memory:", &h) != SX_OK) { g_fail = 1; goto out; }
	for (r = 0; !atomic_load(&g_stop); r++) {
		int row = (int)id + NCMT * (int)(r % (NHOT / NCMT));
		long val = r * 100 + id;
		int during = atomic_load(&g_in_ckpt);
		snprintf(sql, sizeof sql, "UPDATE hot SET v=%ld WHERE k=%d", val, row);
		if (sx_exec(h, sql, NULL) != SX_OK) { g_fail = 1; break; }
		atomic_store(&g_last[row], val);
		atomic_fetch_add(&g_commits, 1);
		if (during && atomic_load(&g_in_ckpt))
			atomic_fetch_add(&g_during, 1);   /* began and ended inside one */
	}
	sx_close(h);
out:
	if (atomic_fetch_sub(&g_left, 1) == 1)
		sx_storage_quiesce();
}

static void
checkpointer(void *arg)
{
	int i;
	(void)arg;
	(void)xtc_proc_sleep(20LL * 1000 * 1000);     /* let commits start */
	for (i = 0; i < CHECKPOINTS; i++) {
		uint64_t tail = 0;
		long sz;
		atomic_store(&g_in_ckpt, 1);
		if (g_quiesced) {
			xstore_quiesce_begin();
			CK(xstore_checkpoint_wal(sx_storage_bt(), sx_storage_wal(),
			    sx_storage_wal_path()) == XTC_OK);
			xstore_quiesce_end();
		} else {
			CK(xstore_checkpoint_wal_online(sx_storage_bt(),
			    sx_storage_wal(), sx_storage_wal_path(), &tail) == XTC_OK);
		}
		atomic_store(&g_in_ckpt, 0);
		g_tail += tail;
		if ((sz = wal_bytes()) > g_max_wal) g_max_wal = sz;
		(void)xtc_proc_sleep(5LL * 1000 * 1000);
	}
	atomic_store(&g_stop, 1);
}

static _Atomic int g_ock_rc = 99;

static void
ock_proc(void *arg)
{
	(void)arg;
	atomic_store(&g_ock_rc, xstore_checkpoint_wal_online(sx_storage_bt(),
	    sx_storage_wal(), sx_storage_wal_path(), NULL));
}

/* A proc that stops the engine (as the server's drain does) while the
 * checkpoint proc is inside its dump. */
static void
stopper(void *arg)
{
	(void)arg;
	(void)xtc_proc_sleep(1LL * 1000 * 1000);   /* let the dump start */
	sx_storage_quiesce();
}

static int64_t one(sx_db *h, const char *sql);

static void
stop_during_checkpoint(void)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t po = { .name = "ock" };
	xtc_pid_t pid;
	sx_db *h = NULL;
	int64_t t0;

	CK(sx_storage_open(g_path, 2048) == SX_OK);
	if (xtc_loop_init(&loop) != XTC_OK) { g_fail = 1; return; }
	CK(sx_storage_run(loop) == SX_OK);
	(void)xtc_proc_spawn(loop, ock_proc, NULL, &po, &pid);
	po.name = "stop";
	(void)xtc_proc_spawn(loop, stopper, NULL, &po, &pid);
	t0 = xtc_clock_mono();
	CK(xtc_loop_run(loop) == XTC_OK);      /* hung forever before the fix */
	(void)xtc_loop_fini(loop);
	printf("  stop during an online checkpoint: loop drained in %lld ms, "
	    "checkpoint rc=%d\n", (long long)((xtc_clock_mono() - t0) / 1000000),
	    atomic_load(&g_ock_rc));
	CK(atomic_load(&g_ock_rc) != 99);      /* it returned */
	sx_storage_abandon();
	CK(sx_storage_open(g_path, 2048) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	CK(one(h, "SELECT count(*) FROM big") == NBIG);
	sx_close(h);
	(void)sx_storage_close();
}

static int64_t
one(sx_db *h, const char *sql)
{
	sx_stmt *st = NULL;
	int64_t v = -2;
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK) return -2;
	if (sx_step(st) == SX_ROW) v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

int
main(void)
{
	char sql[200], side[320];
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t po = { .name = "cmt" };
	xtc_pid_t pid;
	sx_db *h = NULL;
	long bad = 0;
	int fd, i;
	const char *q = getenv("SQLXTC_CKPT_QUIESCED");

	g_quiesced = q != NULL && q[0] == '1';
	if (xtc_set_stack_size(512u * 1024u) != XTC_OK) return 1;
	t_tmpl(g_path, sizeof g_path, "sqlxtc-ockpt");
	if ((fd = mkstemp(g_path)) < 0) return 1;
	close(fd);
	snprintf(g_wal, sizeof g_wal, "%s-wal", g_path);
	CK(sx_init() == SX_OK);
	sx_storage_set_checkpoint(1ull << 40);        /* no periodic ones */
	CK(sx_storage_open(g_path, 2048) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE big(k INTEGER PRIMARY KEY, pad TEXT)", NULL) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE hot(k INTEGER PRIMARY KEY, v INTEGER)", NULL) == SX_OK);
	CK(sx_exec(h, "BEGIN", NULL) == SX_OK);
	for (i = 0; i < NBIG; i++) {
		snprintf(sql, sizeof sql, "INSERT INTO big VALUES(%d, "
		    "'%0120d')", i, i);
		CK(sx_exec(h, sql, NULL) == SX_OK);
	}
	CK(sx_exec(h, "COMMIT", NULL) == SX_OK);
	for (i = 0; i < NHOT; i++) {
		snprintf(sql, sizeof sql, "INSERT INTO hot VALUES(%d, -1)", i);
		CK(sx_exec(h, sql, NULL) == SX_OK);
		atomic_store(&g_last[i], -1);
	}
	sx_close(h);

	atomic_store(&g_left, NCMT);
	if (xtc_loop_init(&loop) != XTC_OK) return 1;
	CK(sx_storage_run(loop) == SX_OK);
	for (i = 0; i < NCMT; i++)
		if (xtc_proc_spawn(loop, committer, (void *)(long)i, &po, &pid) != XTC_OK)
			return 1;
	po.name = "ckpt";
	if (xtc_proc_spawn(loop, checkpointer, NULL, &po, &pid) != XTC_OK)
		return 1;
	if (xtc_loop_run(loop) != XTC_OK) return 1;
	(void)xtc_loop_fini(loop);

	/* crash, then recover from the (compacted) log alone */
	sx_storage_abandon();
	CK(sx_storage_open(g_path, 2048) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	for (i = 0; i < NHOT; i++) {
		snprintf(sql, sizeof sql, "SELECT v FROM hot WHERE k=%d", i);
		if (one(h, sql) != atomic_load(&g_last[i])) bad++;
	}
	CK(one(h, "SELECT count(*) FROM big") == NBIG);
	CK(one(h, "SELECT k FROM big WHERE k = 12345") == 12345);
	sx_close(h);
	printf("  %s: commits=%ld during-checkpoint=%ld live-frames-copied=%llu "
	    "max-wal=%ld KB rows-wrong-after-crash=%ld\n",
	    g_quiesced ? "quiesced" : "online", atomic_load(&g_commits),
	    atomic_load(&g_during), (unsigned long long)g_tail,
	    g_max_wal / 1024, bad);
	CK(atomic_load(&g_during) > 0);
	CK(g_quiesced || g_tail > 0);
	CK(bad == 0);
	CK(g_max_wal < 16L * 1024 * 1024);          /* compacted (~4 MB live) */
	(void)sx_storage_close();

	/* (5) stop the engine while an online checkpoint is dumping */
	stop_during_checkpoint();

	(void)sx_shutdown();
	unlink(g_path); unlink(g_wal);
	snprintf(side, sizeof side, "%s.dwb", g_path); unlink(side);
	if (g_fail) { fprintf(stderr, "FAIL: test_ckpt_online\n"); return 1; }
	printf("  ok   online checkpoint: commits run during the rewrite; crash"
	    " recovery keeps every acknowledged commit\n");
	return 0;
}
