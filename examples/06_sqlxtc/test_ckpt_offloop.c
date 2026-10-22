/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_ckpt_offloop.c
 *	The periodic log checkpoint must not freeze the loop it is started
 *	from.
 *
 *	The online checkpoint's dump ran on the blocking pool, but its
 *	quiesced finish -- copy the log tail, fsync the new log, rename,
 *	fsync the directory, reopen -- ran on the checkpoint fiber with plain
 *	syscalls, so the whole loop stopped for it: 134 ms on a 100k-account
 *	ledger store, 1.1 s (one fsync) on a 10-warehouse TPC-C store.  In
 *	sqlxtc-server that loop is loop 0: the listener, the page provider,
 *	the trickler and every Nth connection (all of them at -t 1).
 *
 *	The engine runs as the server runs it (sx_storage_run on one loop,
 *	a 256 KiB checkpoint floor).  A committer churns rows on the loop so
 *	periodic checkpoints happen, while a ticker on the SAME loop sleeps
 *	1 ms at a time and records the longest gap between its wake-ups.
 *	fsync is interposed to take SLOW_FSYNC_MS (a slow disk), which only
 *	the checkpoint's finish calls (commits use fdatasync).  Asserts:
 *	  (1) CKPTS checkpoints completed, each calling the slow fsync;
 *	  (2) the ticker never went SLOW_FSYNC_MS without running (on the
 *	      fiber, every slow fsync froze it for at least that long);
 *	  (3) crash recovery keeps every committed value.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE                  /* RTLD_NEXT */
#endif
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "engine.h"
#include "xtc.h"
#include "xtc_async.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "t_tmp.h"

#define NROW          32
#define ROUNDS        200000
#define CKPTS         3
#define PAD           200
#define SLOW_FSYNC_MS 150

static _Atomic int  g_done, g_fsyncs;
static _Atomic long g_last[NROW];
static int64_t      g_max_gap;
static uint64_t     g_ck0;

/* A slow disk for fsync only.  Interposed by symbol: the engine's
 * wal.c calls fsync for the new log and its directory. */
int
fsync(int fd)
{
	static int (*real)(int);
	struct timespec ts = { 0, SLOW_FSYNC_MS * 1000000L };
	if (real == NULL)
		*(void **)&real = dlsym(RTLD_NEXT, "fsync");
	atomic_fetch_add(&g_fsyncs, 1);
	(void)nanosleep(&ts, NULL);
	return real(fd);
}

static void
committer(void *arg)
{
	sx_db *h = NULL;
	char sql[512], pad[PAD + 1];
	int r;
	(void)arg;
	memset(pad, 'p', PAD); pad[PAD] = '\0';
	if (sx_open(":memory:", &h) != SX_OK) { g_fail = 1; goto out; }
	/* Until CKPTS checkpoints have completed (or ROUNDS, a bound). */
	for (r = 0; r < ROUNDS && sx_storage_checkpoints() - g_ck0 < CKPTS; r++) {
		int row = r % NROW;
		snprintf(sql, sizeof sql, "UPDATE t SET v=%d, pad='%s' WHERE k=%d",
		    r, pad, row);
		if (sx_exec(h, sql, NULL) != SX_OK) { g_fail = 1; break; }
		atomic_store(&g_last[row], r);
	}
	sx_close(h);
out:
	atomic_store(&g_done, 1);
	sx_storage_quiesce();
}

static void
ticker(void *arg)
{
	int64_t prev = xtc_clock_mono(), now;
	(void)arg;
	while (!atomic_load(&g_done)) {
		(void)xtc_proc_sleep(1LL * 1000 * 1000);
		now = xtc_clock_mono();
		if (now - prev > g_max_gap)
			g_max_gap = now - prev;
		prev = now;
	}
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
	char path[256], sql[128], side[320];
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t po = { .name = "cmt" };
	xtc_pid_t pid;
	sx_db *h = NULL;
	long bad = 0;
	int fd, i, nf;
	uint64_t ck;

	if (xtc_set_stack_size(512u * 1024u) != XTC_OK) return 1;
	t_tmpl(path, sizeof path, "sqlxtc-ckoff");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	CK(sx_init() == SX_OK);
	sx_storage_set_checkpoint(256u << 10);
	CK(sx_storage_open(path, 256) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, v INTEGER, pad TEXT)", NULL) == SX_OK);
	for (i = 0; i < NROW; i++) {
		snprintf(sql, sizeof sql, "INSERT INTO t(k,v,pad) VALUES(%d,-1,'')", i);
		CK(sx_exec(h, sql, NULL) == SX_OK);
		atomic_store(&g_last[i], -1);
	}
	sx_close(h);

	ck = g_ck0 = sx_storage_checkpoints();
	nf = atomic_load(&g_fsyncs);
	if (xtc_loop_init(&loop) != XTC_OK) return 1;
	CK(sx_storage_run(loop) == SX_OK);
	if (xtc_proc_spawn(loop, committer, NULL, &po, &pid) != XTC_OK) return 1;
	po.name = "tick";
	if (xtc_proc_spawn(loop, ticker, NULL, &po, &pid) != XTC_OK) return 1;
	if (xtc_loop_run(loop) != XTC_OK) return 1;
	(void)xtc_loop_fini(loop);
	ck = sx_storage_checkpoints() - ck;
	nf = atomic_load(&g_fsyncs) - nf;

	sx_storage_abandon();                         /* crash */
	CK(sx_storage_open(path, 256) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	for (i = 0; i < NROW; i++) {
		snprintf(sql, sizeof sql, "SELECT v FROM t WHERE k=%d", i);
		if (one(h, sql) != atomic_load(&g_last[i])) bad++;
	}
	sx_close(h);
	(void)sx_storage_close();
	(void)sx_shutdown();

	printf("  checkpoints=%llu slow fsyncs=%d (%d ms each); longest loop "
	    "gap %lld ms; rows wrong after crash=%ld\n",
	    (unsigned long long)ck, nf, SLOW_FSYNC_MS,
	    (long long)(g_max_gap / 1000000), bad);
	CK(ck >= CKPTS && nf >= CKPTS);               /* (1) */
	CK(g_max_gap < SLOW_FSYNC_MS * 1000000LL);    /* (2) */
	CK(bad == 0);                                 /* (3) */

	unlink(path);
	snprintf(side, sizeof side, "%s-wal", path); unlink(side);
	if (g_fail) { fprintf(stderr, "FAIL: test_ckpt_offloop\n"); return 1; }
	printf("  ok   periodic checkpoint runs off the loop: the loop kept "
	    "running through its fsyncs\n");
	return 0;
}
