/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_ckpt_bound.c
 *	The log stays bounded while the server runs, and crash recovery
 *	after periodic checkpoints keeps every committed write.
 *
 *	The engine is opened exactly as the server does (sx_storage_open +
 *	sx_storage_run on a loop) with a 256 KiB checkpoint floor.  Eight
 *	committer procs churn 64 rows (UPDATE, autocommit and BEGIN/COMMIT)
 *	for ~8 MB of log writes, so the live data stays tiny while the
 *	history is large.  The log file is sampled while they run.  Then
 *	the engine is abandoned (a crash: no flush, no clean marker),
 *	reopened, and every row must hold its last committed value.
 *
 *	Before the fix nothing checkpointed while running: the log grew
 *	with the whole history (~8 MB here, unbounded in general).
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "engine.h"
#include "wal.h"
#include "xtc.h"
#include "xtc_async.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "t_tmp.h"

#define NROW   64
#define NCMT   8
#define ROUNDS 600
#define PAD    200

static char         g_wal[300];
static _Atomic int  g_left;
static _Atomic long g_max_wal, g_commits;
static _Atomic long g_last[NROW];   /* last committed value per row */

/* The log's length (the file runs up to 8 MiB ahead of it: the log
 * preallocates, see wal_prealloc). */
static long
wal_bytes(void)
{
	struct wal *w = sx_storage_wal();
	return w != NULL ? (long)wal_size(w) : -1;
}

static void
committer(void *arg)
{
	long id = (long)arg;
	sx_db *h = NULL;
	char sql[512], pad[PAD + 1];
	int r;
	memset(pad, 'p', PAD); pad[PAD] = '\0';
	if (sx_open(":memory:", &h) != SX_OK) { g_fail = 1; goto out; }
	for (r = 0; r < ROUNDS; r++) {
		/* each committer owns rows id, id+NCMT, ...: no write conflicts */
		int row = (int)id + NCMT * (r % (NROW / NCMT));
		long val = (long)r * 100 + id;
		long sz;
		snprintf(sql, sizeof sql,
		    "UPDATE t SET v=%ld, pad='%s' WHERE k=%d", val, pad, row);
		if (r % 3 == 0) {
			CK(sx_exec(h, "BEGIN", NULL) == SX_OK);
			CK(sx_exec(h, sql, NULL) == SX_OK);
			if (sx_exec(h, "COMMIT", NULL) == SX_OK)
				atomic_store(&g_last[row], val);
			else
				g_fail = 1;
		} else if (sx_exec(h, sql, NULL) == SX_OK)
			atomic_store(&g_last[row], val);
		else
			g_fail = 1;
		atomic_fetch_add(&g_commits, 1);
		sz = wal_bytes();
		if (sz > atomic_load(&g_max_wal)) atomic_store(&g_max_wal, sz);
	}
	sx_close(h);
out:
	if (atomic_fetch_sub(&g_left, 1) == 1)
		sx_storage_quiesce();          /* stop the background procs */
}

int
main(void)
{
	char path[256], sql[128], side[320];
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t po = { .size = sizeof(xtc_proc_opts_t), .name = "cmt" };
	xtc_pid_t pid;
	sx_db *h = NULL;
	sx_stmt *st = NULL;
	long c, bad = 0, end_wal;
	int fd, i;

	/* SQL runs on fibers: use the server's 512 KiB stack (main.c), not
	 * the 64 KiB library default, which sanitizer frames can exceed. */
	if (xtc_set_stack_size(512u * 1024u) != XTC_OK) return 1;
	t_tmpl(path, sizeof path, "sqlxtc-ckpt");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	snprintf(g_wal, sizeof g_wal, "%s-wal", path);
	CK(sx_init() == SX_OK);
#ifdef SX_HAVE_PERIODIC_CHECKPOINT
	sx_storage_set_checkpoint(256u << 10);
#endif
	CK(sx_storage_open(path, 256) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, v INTEGER, pad TEXT)", NULL) == SX_OK);
	for (i = 0; i < NROW; i++) {
		snprintf(sql, sizeof sql, "INSERT INTO t(k,v,pad) VALUES(%d,-1,'')", i);
		CK(sx_exec(h, sql, NULL) == SX_OK);
		atomic_store(&g_last[i], -1);
	}
	sx_close(h);

	atomic_store(&g_left, NCMT);
	if (xtc_loop_init(&loop) != XTC_OK) return 1;
	CK(sx_storage_run(loop) == SX_OK);
	for (c = 0; c < NCMT; c++)
		if (xtc_proc_spawn(loop, committer, (void *)c, &po, &pid) != XTC_OK)
			return 1;
	if (xtc_loop_run(loop) != XTC_OK) return 1;
	(void)xtc_loop_fini(loop);
	end_wal = wal_bytes();

	/* crash, then recover from the log alone */
	sx_storage_abandon();
	CK(sx_storage_open(path, 256) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	for (i = 0; i < NROW; i++) {
		long got = -2;
		snprintf(sql, sizeof sql, "SELECT v FROM t WHERE k=%d", i);
		if (sx_prepare(h, sql, -1, &st, NULL) == SX_OK) {
			if (sx_step(st) == SX_ROW) got = (long)sx_column_int64(st, 0);
			sx_finalize(st);
		}
		if (got != atomic_load(&g_last[i])) bad++;
	}
	sx_close(h);
	printf("  commits=%ld log-written~%ld KB max-wal=%ld KB end-wal=%ld KB "
	    "checkpoints=%llu rows-wrong-after-crash=%ld\n",
	    atomic_load(&g_commits), atomic_load(&g_commits) * (PAD + 80) / 1024,
	    atomic_load(&g_max_wal) / 1024, end_wal / 1024,
#ifdef SX_HAVE_PERIODIC_CHECKPOINT
	    (unsigned long long)sx_storage_checkpoints(),
#else
	    0ull,
#endif
	    bad);
	/* Bound: the trigger is max(256 KiB, 2 x post-checkpoint size); the
	 * live data is ~64 x 300 B, so the log must never pass ~1 MiB. */
	CK(atomic_load(&g_max_wal) < 1024L * 1024L);
	CK(bad == 0);
	(void)sx_storage_close();
	(void)sx_shutdown();
	unlink(path); unlink(g_wal);
	snprintf(side, sizeof side, "%s.dwb", path); unlink(side);
	if (g_fail) { fprintf(stderr, "FAIL: test_ckpt_bound\n"); return 1; }
	printf("  ok   periodic checkpoint: log bounded while running; crash"
	    " recovery keeps every committed write\n");
	return 0;
}
