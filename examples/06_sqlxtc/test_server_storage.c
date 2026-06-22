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
#include <sys/stat.h>

#include "engine.h"
#include "db.h"
#include "quack.h"
#include "xstore.h"   /* xstore_bt_of / xstore_table_id -- the native catalog */
#include "btree.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "t_tmp.h"

#define NROW    500
#define NWORK   8
#define PERWORK 200


static char g_path[256];     /* storage file for the durability cycles */
static char g_path2[256];    /* storage file for the conn-per-proc cycle */
static int  g_phase1_rows;   /* rows written in cycle 1 */
static int  g_cycle2_seen;   /* rows seen after restart in cycle 2 */
static int  g_cpp_total;     /* total rows seen after the conn-per-proc run */
static char g_path3[300];
static int  g_transparent_ok;   /* plain CREATE TABLE routed to xstore */
static int  g_transparent_rows; /* rows read back from the routed table */

/* Transparent routing: a plain CREATE TABLE, run through the Quack db
 * layer with storage active, must become a CREATE VIRTUAL TABLE on
 * xstore (visible in sqlite_master) and round-trip data. */
static void
transparent_proc(void *arg)
{
	xtc_loop_t *loop = arg;
	sx_db *h = NULL;
	quack_buf_t buf;
	int64_t nrows = 0;
	char *err = NULL;
	sx_stmt *st = NULL;
	bt_t *bt;
	uint32_t tid = 0;

	CK(sx_storage_open(g_path3, 64) == SX_OK);
	CK(sx_storage_run(loop) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);   /* xstore auto-registered */
	CK(quack_buf_init(&buf, 256) == 0);

	/* Plain CREATE TABLE -- no USING xstore -- creates an xstore CATALOG
	 * table natively (no SQLite vtab, no sqlite_master).  An explicit
	 * INTEGER PRIMARY KEY is the xstore rowid (the from-scratch engine
	 * has no implicit-rowid tables). */
	quack_buf_reset(&buf);
	CK(db_exec(h, "CREATE TABLE foo(id INTEGER PRIMARY KEY, name TEXT, age INT)",
	    -1, &buf, &nrows, &err) == 0);

	/* It exists in the NATIVE catalog (the source of truth), not as a
	 * SQLite vtab. */
	bt = xstore_bt_of(h);
	g_transparent_ok = (bt != NULL && xstore_table_id(bt, "foo", &tid) && tid != 0);

	/* Data round-trips through the native table. */
	quack_buf_reset(&buf);
	(void)db_exec(h, "INSERT INTO foo(name,age) VALUES('alice',30),('bob',25)",
	    -1, &buf, &nrows, &err);
	if (sx_prepare(h, "SELECT count(*) FROM foo", -1, &st, NULL) == SX_OK) {
		if (sx_step(st) == SX_ROW)
			g_transparent_rows = (int)sx_column_int64(st, 0);
		sx_finalize(st); st = NULL;
	}
	free(err);
	quack_buf_free(&buf);
	sx_close(h);
	(void)sx_storage_checkpoint();
	sx_storage_close();
}
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
/* ---- cycle 4: CRASH recovery.  Write rows under a tiny pool (so
 * eviction tears the on-disk tree mid-SMO), then ABANDON the engine
 * without a clean shutdown -- no checkpoint, no marker, dirty pages
 * lost, the log intact.  Reopen: no marker, so the torn base is
 * discarded and the tree is rebuilt from the full log.  Every row must
 * reappear. ---- */
static char g_path4[300];
static int  g_crash_wrote;
static int  g_crash_seen;

static void
crash_writer_proc(void *arg)
{
	xtc_loop_t *loop = arg;
	sx_db *h = NULL;
	int i;
	CK(sx_storage_open(g_path4, 16) == SX_OK);   /* tiny pool: eviction tears the base */
	CK(sx_storage_run(loop) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	CK(sx_exec(h, "CREATE VIRTUAL TABLE t USING xstore;", NULL) == SX_OK);
	for (i = 1; i <= NROW; i++) {
		char sql[80];
		snprintf(sql, sizeof sql, "INSERT INTO t(k,v) VALUES(%d,'crash%d');", i, i);
		CK(sx_exec(h, sql, NULL) == SX_OK);   /* durable via group commit */
	}
	sx_close(h);
	g_crash_wrote = count_rows();
	sx_storage_abandon();                        /* CRASH: no checkpoint, no marker */
}

static void
crash_verify_proc(void *arg)
{
	xtc_loop_t *loop = arg;
	CK(sx_storage_open(g_path4, 16) == SX_OK);   /* no marker -> rebuild from full log */
	CK(sx_storage_run(loop) == SX_OK);
	g_crash_seen = count_rows();
	sx_storage_close();
}

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
	t_tmpl(g_path, sizeof g_path, "sqlxtc-srvstore");
	fd = mkstemp(g_path); if (fd < 0) return 1; close(fd); unlink(g_path);
	t_tmpl(g_path2, sizeof g_path2, "sqlxtc-srvstore2");
	fd = mkstemp(g_path2); if (fd < 0) return 1; close(fd); unlink(g_path2);

	CK(sx_init() == SX_OK);

	if (run_cycle(writer_proc) != 0) return 1;
	CK(g_phase1_rows == NROW);

	if (run_cycle(verify_proc) != 0) return 1;
	CK(g_cycle2_seen == NROW);          /* survived the restart */

	if (run_cycle(cpp_driver) != 0) return 1;
	CK(g_cpp_total == NWORK * PERWORK); /* all concurrent writers landed */

	t_tmpl(g_path3, sizeof g_path3, "sqlxtc-srvstore3");
	fd = mkstemp(g_path3); if (fd < 0) return 1; close(fd); unlink(g_path3);
	if (run_cycle(transparent_proc) != 0) return 1;
	CK(g_transparent_ok == 1);          /* plain CREATE TABLE -> xstore */
	CK(g_transparent_rows == 2);        /* and data round-trips */

	t_tmpl(g_path4, sizeof g_path4, "sqlxtc-srvstore4");
	fd = mkstemp(g_path4); if (fd < 0) return 1; close(fd); unlink(g_path4);
	if (run_cycle(crash_writer_proc) != 0) return 1;
	CK(g_crash_wrote == NROW);          /* all rows committed before the crash */
	if (run_cycle(crash_verify_proc) != 0) return 1;
	CK(g_crash_seen == NROW);           /* all rebuilt from the log after the crash */

	(void)sx_shutdown();
	{
		char wal[320];
		snprintf(wal, sizeof wal, "%s-wal", g_path); unlink(wal); unlink(g_path);
		snprintf(wal, sizeof wal, "%s-wal", g_path2); unlink(wal); unlink(g_path2);
		snprintf(wal, sizeof wal, "%s-wal", g_path3); unlink(wal); unlink(g_path3);
		snprintf(wal, sizeof wal, "%s-wal", g_path4); unlink(wal); unlink(g_path4);
		snprintf(wal, sizeof wal, "%s.clean", g_path); unlink(wal);
		snprintf(wal, sizeof wal, "%s.clean", g_path2); unlink(wal);
		snprintf(wal, sizeof wal, "%s.clean", g_path3); unlink(wal);
		snprintf(wal, sizeof wal, "%s.clean", g_path4); unlink(wal);
		snprintf(wal, sizeof wal, "%s.dwb", g_path); unlink(wal);
		snprintf(wal, sizeof wal, "%s.dwb", g_path2); unlink(wal);
		snprintf(wal, sizeof wal, "%s.dwb", g_path3); unlink(wal);
		snprintf(wal, sizeof wal, "%s.dwb", g_path4); unlink(wal);
	}
	if (g_fail) return 1;
	printf("  ok   engine wired: %d rows written + checkpointed, %d survived "
	    "a clean restart\n", NROW, g_cycle2_seen);
	printf("  ok   connection-per-proc: %d procs x %d rows each = %d rows "
	    "concurrent over one shared store\n", NWORK, PERWORK, g_cpp_total);
	printf("  ok   native CREATE TABLE -> xstore catalog: plain DDL creates"
	    " a native catalog table (no vtab, no sqlite_master), %d rows"
	    " round-tripped\n", g_transparent_rows);
	printf("  ok   crash recovery: %d rows written under a tiny pool then"
	    " abandoned (no clean shutdown); all %d rebuilt from the log onto a"
	    " fresh tree after the torn base was discarded\n",
	    g_crash_wrote, g_crash_seen);
	printf("All sqlxtc server-storage tests passed.\n");
	return 0;
}
