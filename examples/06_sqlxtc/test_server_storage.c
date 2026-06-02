/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_server_storage.c
 *	The server's storage wiring, exercised in-process (no network
 *	daemon): the libxtc-native engine opened/run through the sx_ API
 *	with its background procs live, SQL executed through the engine
 *	handle, durability across a clean restart, and the
 *	connection-per-proc model -- many independent engine handles,
 *	each its own VDBE, executing concurrently over one shared MVCC
 *	store.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

#define NROW    500
#define NWORK   8
#define PERWORK 200

static int g_fail;
#define CK(c) do { if (!(c)) { fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#c); g_fail=1; } } while (0)

static char g_path[256];     /* storage file for the durability cycles */
static char g_path2[256];    /* storage file for the conn-per-proc cycle */
static int  g_phase1_rows;   /* rows written in cycle 1 */
static int  g_cycle2_seen;   /* rows seen after restart in cycle 2 */
static int  g_cpp_total;     /* total rows seen after the conn-per-proc run */
static _Atomic int g_workers_done;

/* Run "SELECT count(*) FROM t" on a fresh engine handle; -1 on error. */
static int
count_rows(void)
{
	sx_db *h = NULL;
	sx_stmt *st = NULL;
	int n = -1;
	if (sx_open(":memory:", &h) != SX_OK) return -1;
	(void)sx_exec(h, "CREATE VIRTUAL TABLE t USING xstore;", NULL);
	if (sx_prepare(h, "SELECT count(*) FROM t", -1, &st, NULL) == SX_OK) {
		if (sx_step(st) == SX_ROW) n = (int)sx_column_int64(st, 0);
		sx_finalize(st);
	}
	sx_close(h);
	return n;
}

/* ---- cycle 1: open the engine, run its background procs, write rows
 * through an engine handle, checkpoint durable. ---- */
static void
writer_proc(void *arg)
{
	xtc_loop_t *loop = arg;
	sx_db *h = NULL;
	int i;

	CK(sx_storage_open(g_path, 64) == SX_OK);
	CK(sx_storage_run(loop) == SX_OK);

	CK(sx_open(":memory:", &h) == SX_OK);
	CK(sx_exec(h, "CREATE VIRTUAL TABLE t USING xstore;", NULL) == SX_OK);
	for (i = 1; i <= NROW; i++) {
		char sql[80];
		snprintf(sql, sizeof sql, "INSERT INTO t(k,v) VALUES(%d,'row%d');", i, i);
		CK(sx_exec(h, sql, NULL) == SX_OK);
	}
	sx_close(h);
	g_phase1_rows = count_rows();
	CK(sx_storage_checkpoint() == SX_OK);   /* flush durable + truncate log */
	sx_storage_close();                      /* stops the background procs */
}

/* ---- cycle 2: reopen the SAME file and confirm the rows survived a
 * clean restart (superblock reopen + recovery through the engine). ---- */
static void
verify_proc(void *arg)
{
	xtc_loop_t *loop = arg;
	CK(sx_storage_open(g_path, 64) == SX_OK);   /* reopen + recover */
	CK(sx_storage_run(loop) == SX_OK);
	g_cycle2_seen = count_rows();
	sx_storage_close();
}

/* ---- cycle 3: connection-per-proc.  NWORK procs each open their own
 * engine handle and write a disjoint key range concurrently into the
 * one shared store; then we confirm every row landed. ---- */
struct warg { int base; };
static void
worker_proc(void *arg)
{
	struct warg *w = arg;
	sx_db *h = NULL;
	int i, ok = 1;

	if (sx_open(":memory:", &h) != SX_OK) ok = 0;
	if (ok && sx_exec(h, "CREATE VIRTUAL TABLE t USING xstore;", NULL) != SX_OK)
		ok = 0;
	for (i = 0; ok && i < PERWORK; i++) {
		char sql[80];
		snprintf(sql, sizeof sql,
		    "INSERT INTO t(k,v) VALUES(%d,'w');", w->base + i);
		if (sx_exec(h, sql, NULL) != SX_OK) ok = 0;
	}
	if (h) sx_close(h);
	CK(ok);
	atomic_fetch_add(&g_workers_done, 1);
}

static void
cpp_driver(void *arg)
{
	xtc_loop_t *loop = arg;
	static struct warg wa[NWORK];
	xtc_pid_t pid;
	int i, spins = 0;

	CK(sx_storage_open(g_path2, 64) == SX_OK);
	CK(sx_storage_run(loop) == SX_OK);
	atomic_store(&g_workers_done, 0);
	for (i = 0; i < NWORK; i++) {
		xtc_proc_opts_t po = { .name = "w" };
		wa[i].base = 1 + i * 100000;     /* disjoint key ranges */
		CK(xtc_proc_spawn(loop, worker_proc, &wa[i], &po, &pid) == XTC_OK);
	}
	/* Wait for all workers (cooperative: park, do not spin). */
	while (atomic_load(&g_workers_done) < NWORK && spins++ < 100000)
		xtc_proc_sleep(1LL * 1000 * 1000);   /* 1ms */
	CK(atomic_load(&g_workers_done) == NWORK);
	g_cpp_total = count_rows();
	sx_storage_close();
}

static int
run_cycle(void (*fn)(void *))
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t po = { .name = "drv" };
	xtc_pid_t pid;
	if (xtc_loop_init(&loop) != XTC_OK) return 1;
	if (xtc_proc_spawn(loop, fn, loop, &po, &pid) != XTC_OK) return 1;
	if (xtc_loop_run(loop) != XTC_OK) return 1;
	return xtc_loop_fini(loop) == XTC_OK ? 0 : 1;
}

int
main(void)
{
	int fd;

	g_fail = 0;
	strcpy(g_path, "/tmp/sqlxtc-srvstore-XXXXXX");
	fd = mkstemp(g_path); if (fd < 0) return 1; close(fd); unlink(g_path);
	strcpy(g_path2, "/tmp/sqlxtc-srvstore2-XXXXXX");
	fd = mkstemp(g_path2); if (fd < 0) return 1; close(fd); unlink(g_path2);

	CK(sx_init() == SX_OK);

	if (run_cycle(writer_proc) != 0) return 1;
	CK(g_phase1_rows == NROW);

	if (run_cycle(verify_proc) != 0) return 1;
	CK(g_cycle2_seen == NROW);          /* survived the restart */

	if (run_cycle(cpp_driver) != 0) return 1;
	CK(g_cpp_total == NWORK * PERWORK); /* all concurrent writers landed */

	(void)sx_shutdown();
	{
		char wal[300];
		snprintf(wal, sizeof wal, "%s-wal", g_path); unlink(wal); unlink(g_path);
		snprintf(wal, sizeof wal, "%s-wal", g_path2); unlink(wal); unlink(g_path2);
	}
	if (g_fail) return 1;
	printf("  ok   engine wired: %d rows written + checkpointed, %d survived "
	    "a clean restart\n", NROW, g_cycle2_seen);
	printf("  ok   connection-per-proc: %d procs x %d rows each = %d rows "
	    "concurrent over one shared store\n", NWORK, PERWORK, g_cpp_total);
	printf("All sqlxtc server-storage tests passed.\n");
	return 0;
}
