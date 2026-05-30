/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_vfs_loop.c
 *	On-loop test of the xtc-native VFS with blocking I/O offload.
 *
 *	A worker process opens a file-backed database through the "xtc"
 *	VFS and runs a write-heavy workload; every read/write/fsync is
 *	offloaded to the blocking pool, so the worker PARKS for the
 *	duration of each syscall instead of stalling the loop.  A
 *	heartbeat process running on the same loop ticks throughout --
 *	proving the reactor thread stays live while the database does
 *	disk I/O.  No network, no daemon.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "sqlite/sqlite3.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_vfs.h"

static _Atomic int g_worker_done;
static _Atomic int g_heartbeats;
static int         g_rows = -1;
static int         g_worker_ok = 0;

static int
count_cb(void *u, int argc, char **argv, char **col)
{
	(void)u; (void)col;
	if (argc > 0 && argv[0]) g_rows = atoi(argv[0]);
	return 0;
}

static void
worker_proc(void *arg)
{
	const char *path = arg;
	sqlite3 *db = NULL;
	char *err = NULL;
	int i, rc;

	rc = sqlite3_open_v2(path, &db,
	    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "xtc");
	if (rc != SQLITE_OK) goto done;

	/* One transaction, many rows: forces xWrite + xSync at commit. */
	(void)sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
	if (sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);",
	    NULL, NULL, &err) != SQLITE_OK) goto done;
	if (sqlite3_exec(db, "BEGIN;", NULL, NULL, &err) != SQLITE_OK) goto done;
	for (i = 0; i < 300; i++) {
		char sql[96];
		snprintf(sql, sizeof sql,
		    "INSERT INTO t(v) VALUES('row-%d-padding-padding');", i);
		if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK)
			goto done;
	}
	if (sqlite3_exec(db, "COMMIT;", NULL, NULL, &err) != SQLITE_OK) goto done;
	if (sqlite3_exec(db, "SELECT count(*) FROM t;", count_cb, NULL, &err)
	    != SQLITE_OK) goto done;
	g_worker_ok = 1;

done:
	if (err) sqlite3_free(err);
	if (db) sqlite3_close(db);
	atomic_store(&g_worker_done, 1);
}

static void
heartbeat_proc(void *arg)
{
	int spins = 0;
	(void)arg;
	/* Tick until the worker finishes (cap so we never hang). */
	while (!atomic_load(&g_worker_done) && spins < 100000) {
		void *m = NULL; size_t n = 0;
		(void)xtc_recv(&m, &n, 1LL * 1000 * 1000);   /* 1ms yield */
		if (m) m = NULL;
		atomic_fetch_add(&g_heartbeats, 1);
		spins++;
	}
}

int
main(void)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t w, h;
	char path[] = "/tmp/sqlxtc-vfs-loop-XXXXXX";
	int fd;

	if (xtc_vfs_register(0) != SQLITE_OK) {
		fprintf(stderr, "FAIL: xtc_vfs_register\n"); return 1;
	}
	fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); return 1; }
	close(fd); unlink(path);

	if (xtc_loop_init(&loop) != XTC_OK) { fprintf(stderr, "loop_init\n"); return 1; }
	opts.name = "worker";
	if (xtc_proc_spawn(loop, worker_proc, path, &opts, &w) != XTC_OK) return 1;
	opts.name = "heartbeat";
	if (xtc_proc_spawn(loop, heartbeat_proc, NULL, &opts, &h) != XTC_OK) return 1;

	if (xtc_loop_run(loop) != XTC_OK) { fprintf(stderr, "loop_run\n"); return 1; }
	(void)xtc_loop_fini(loop);

	unlink(path);
	{ char wal[64], shm[64];
	  snprintf(wal, sizeof wal, "%s-wal", path);
	  snprintf(shm, sizeof shm, "%s-shm", path);
	  unlink(wal); unlink(shm); }

	if (!g_worker_ok || g_rows != 300) {
		fprintf(stderr, "FAIL: workload (ok=%d rows=%d)\n",
		    g_worker_ok, g_rows);
		return 1;
	}
	if (atomic_load(&g_heartbeats) < 3) {
		fprintf(stderr, "FAIL: loop stalled during DB I/O "
		    "(only %d heartbeats)\n", atomic_load(&g_heartbeats));
		return 1;
	}

	printf("  ok   file-backed workload via xtc VFS (%d rows)\n", g_rows);
	printf("  ok   loop stayed live during DB I/O (%d heartbeats while "
	    "worker parked on offloaded reads/writes/fsync)\n",
	    atomic_load(&g_heartbeats));
	printf("All xtc VFS on-loop tests passed.\n");
	return 0;
}
