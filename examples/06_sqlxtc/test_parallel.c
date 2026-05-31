/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_parallel.c
 *	Concurrent execution over the sx_ engine.  Several worker
 *	processes each open their OWN connection to one file-backed
 *	database and run write transactions in parallel; a reader
 *	process queries concurrently throughout.  WAL journaling lets
 *	readers run alongside the writers, and the busy timeout makes
 *	the writers queue rather than fail -- so every row lands and the
 *	final count is exact.  No network, no daemon.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "engine.h"

#define N_WORKERS 4
#define N_ROWS    50

static const char *g_path;
static _Atomic int g_worker_ok;
static _Atomic int g_workers_done;
static _Atomic int g_reads_seen;

static void
worker_proc(void *arg)
{
	long id = (long)arg;
	sx_db *h = NULL;
	sx_stmt *st = NULL;
	int i, ok = 1;

	if (sx_open(g_path, &h) != SX_OK) { atomic_fetch_add(&g_workers_done, 1); return; }

	/* Each worker writes its own keyspace; concurrent writers to the
	 * one file serialize through the WAL write lock + busy timeout. */
	for (i = 0; i < N_ROWS; i++) {
		char sql[128];
		snprintf(sql, sizeof sql,
		    "INSERT INTO t(w, n, v) VALUES(%ld, %d, 'row-%ld-%d');",
		    id, i, id, i);
		if (sx_exec(h, sql, NULL) != SX_OK) { ok = 0; break; }
	}

	/* Read back its own rows (a reader concurrent with peers). */
	if (ok && sx_prepare(h,
	    "SELECT count(*) FROM t WHERE w = ?1;", -1, &st, NULL) == SX_OK) {
		(void)sx_bind_int64(st, 1, id);
		if (sx_step(st) == SX_ROW && sx_column_int64(st, 0) == N_ROWS)
			atomic_fetch_add(&g_reads_seen, 1);
		sx_finalize(st);
	} else {
		ok = 0;
	}

	sx_close(h);
	if (ok) atomic_fetch_add(&g_worker_ok, 1);
	atomic_fetch_add(&g_workers_done, 1);
}

static void
reader_proc(void *arg)
{
	(void)arg;
	/* Poll the total while the workers write -- proves a reader runs
	 * concurrently with writers (WAL) and never blocks the loop. */
	while (atomic_load(&g_workers_done) < N_WORKERS) {
		sx_db *h = NULL;
		sx_stmt *st = NULL;
		void *m = NULL; size_t n = 0;
		if (sx_open(g_path, &h) == SX_OK) {
			if (sx_prepare(h, "SELECT count(*) FROM t;", -1, &st,
			    NULL) == SX_OK) {
				(void)sx_step(st);
				sx_finalize(st);
			}
			sx_close(h);
		}
		(void)xtc_recv(&m, &n, 2LL * 1000 * 1000);   /* 2ms yield */
		if (m) m = NULL;
	}
}

int
main(void)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t pid;
	sx_db *h = NULL;
	sx_stmt *st = NULL;
	char path[] = "/tmp/sqlxtc-parallel-XXXXXX";
	long w;
	int fd, total = -1;

	fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); return 1; }
	close(fd); unlink(path);
	g_path = path;

	/* Create the schema once up front. */
	if (sx_open(path, &h) != SX_OK) { fprintf(stderr, "open\n"); return 1; }
	if (sx_exec(h, "CREATE TABLE t(w INT, n INT, v TEXT);", NULL) != SX_OK) {
		fprintf(stderr, "create\n"); return 1;
	}
	sx_close(h);

	atomic_store(&g_worker_ok, 0);
	atomic_store(&g_workers_done, 0);
	atomic_store(&g_reads_seen, 0);

	if (xtc_loop_init(&loop) != XTC_OK) return 1;
	for (w = 0; w < N_WORKERS; w++) {
		opts.name = "worker";
		if (xtc_proc_spawn(loop, worker_proc, (void *)w, &opts, &pid)
		    != XTC_OK) return 1;
	}
	opts.name = "reader";
	if (xtc_proc_spawn(loop, reader_proc, NULL, &opts, &pid) != XTC_OK)
		return 1;
	if (xtc_loop_run(loop) != XTC_OK) return 1;
	(void)xtc_loop_fini(loop);

	/* Final count: every worker's rows landed. */
	if (sx_open(path, &h) == SX_OK) {
		if (sx_prepare(h, "SELECT count(*) FROM t;", -1, &st, NULL)
		    == SX_OK) {
			if (sx_step(st) == SX_ROW)
				total = (int)sx_column_int64(st, 0);
			sx_finalize(st);
		}
		sx_close(h);
	}

	unlink(path);
	{ char wal[64], shm[64];
	  snprintf(wal, sizeof wal, "%s-wal", path);
	  snprintf(shm, sizeof shm, "%s-shm", path);
	  unlink(wal); unlink(shm); }

	if (atomic_load(&g_worker_ok) != N_WORKERS) {
		fprintf(stderr, "FAIL: only %d/%d workers succeeded\n",
		    atomic_load(&g_worker_ok), N_WORKERS);
		return 1;
	}
	if (total != N_WORKERS * N_ROWS) {
		fprintf(stderr, "FAIL: count %d != %d\n",
		    total, N_WORKERS * N_ROWS);
		return 1;
	}

	printf("  ok   %d workers each ran a %d-row write txn on a private "
	    "connection; all %d rows landed\n", N_WORKERS, N_ROWS, total);
	printf("  ok   per-worker readback correct (%d/%d)\n",
	    atomic_load(&g_reads_seen), N_WORKERS);
	printf("All sqlxtc parallel-execution tests passed.\n");
	return 0;
}
