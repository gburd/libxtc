/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_kill_stmt.c
 *	A connection proc killed in the MIDDLE of a statement must not leave
 *	a page latch (or the tree's SMO lock) behind.
 *
 *	A statement's B-tree work holds latches across yields: a scan holds
 *	its leaf while it fixes the next one, an insert that splits holds
 *	the SMO lock and latches up the path, and a fix that misses a full
 *	pool waits in the evictor with xtc_yield.  A kill delivered at such a
 *	yield unwound the fiber with the latch held, and the next writer of
 *	that page waited forever (test_wal_kill_engine hung on 5 of 60 seeds
 *	on a 64-vCPU host with a leaked shared latch on an internal node).
 *
 *	Here: a 48-frame pool under a ~2,000-page table, so every scan and
 *	insert evicts and yields; NC connection procs on NL loops each run a
 *	full-table SELECT then an INSERT (splits); a boss kills one every
 *	0.2-1 ms -- anywhere, not only in COMMIT -- and respawns it.  The
 *	watchdog fails the run when no statement completes for HANG_S s.
 *	At the end the table must still be fully readable and writable.
 *
 *	usage: test_kill_stmt [ms [seed]]
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "t_tmp.h"

#define NL      4
#define NC      16
#define NROW    20000
#define HANG_S  8

static char             g_path[256];
static xtc_exec_t      *g_exec;
static xtc_pid_t        g_pid[NC];
static int              g_live[NC];
static _Atomic int      g_stop, g_done, g_final_ok;
static _Atomic uint64_t g_stmts, g_kills;
static _Atomic int64_t  g_next = NROW + 1;
static uint64_t         g_seed;
static int64_t          g_ms;

static uint64_t
rng(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

static int64_t
one(sx_db *h, const char *sql)
{
	sx_stmt *st = NULL;
	int64_t v = -2;
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK)
		return -2;
	if (sx_step(st) == SX_ROW)
		v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

static void
close_db(void *arg)
{
	sx_close(arg);
}

static void
conn(void *arg)
{
	sx_db *h = NULL;
	char sql[160];
	(void)arg;
	if (sx_open(":memory:", &h) != SX_OK)
		return;
	(void)xtc_proc_at_exit(close_db, h);
	while (!atomic_load(&g_stop)) {
		int64_t k = atomic_fetch_add(&g_next, 1);
		if (one(h, "SELECT count(*) FROM t WHERE v >= 0") >= 0)
			atomic_fetch_add(&g_stmts, 1);
		snprintf(sql, sizeof sql, "INSERT INTO t VALUES(%lld, %lld, "
		    "'pad-pad-pad-pad-pad-pad-pad-pad-pad-pad-pad-pad-pad-pad')",
		    (long long)k, (long long)k);
		if (sx_exec(h, sql, NULL) == SX_OK)
			atomic_fetch_add(&g_stmts, 1);
	}
}

static void *
watchdog(void *arg)
{
	uint64_t last = 0;
	int64_t since = xtc_clock_mono();
	(void)arg;
	while (!atomic_load(&g_done)) {
		uint64_t c = atomic_load(&g_stmts);
		int64_t now = xtc_clock_mono();
		if (c != last) {
			last = c;
			since = now;
		} else if (now - since > (int64_t)HANG_S * 1000000000LL) {
			fprintf(stderr, "HANG (seed %llu): no statement for %d s; "
			    "statements %llu kills %llu\n", (unsigned long long)g_seed,
			    HANG_S, (unsigned long long)c,
			    (unsigned long long)atomic_load(&g_kills));
			fprintf(stderr, "FAIL: test_kill_stmt: hang\n");
			_exit(3);
		}
		(void)xtc_sleep_ns(50LL * 1000 * 1000);
	}
	return NULL;
}

static int
spawn_slot(int i)
{
	xtc_proc_opts_t o = { .size = sizeof(xtc_proc_opts_t), .name = "conn" };
	uint64_t ref;
	if (xtc_proc_spawn_monitor(xtc_exec_loop(g_exec, i % NL), conn, NULL,
	    &o, &g_pid[i], &ref) != XTC_OK)
		return -1;
	g_live[i] = 1;
	return 0;
}

static int
reap(int respawn)
{
	void *m;
	size_t n;
	int i, live = 0;
	while (xtc_recv(&m, &n, 0) == XTC_OK) {
		xtc_pid_t who;
		if (m != NULL && xtc_down_decode(m, n, &who, NULL) == XTC_OK)
			for (i = 0; i < NC; i++)
				if (g_live[i] && xtc_pid_eq(g_pid[i], who)) {
					g_live[i] = 0;
					if (respawn)
						(void)spawn_slot(i);
				}
		xtc_free(m);
	}
	for (i = 0; i < NC; i++)
		live += g_live[i];
	return live;
}

static void
boss(void *arg)
{
	uint64_t r = g_seed;
	int64_t end = xtc_clock_mono() + g_ms * 1000000LL;
	sx_db *h = NULL;
	int i;

	(void)arg;
	for (i = 0; i < NC; i++)
		(void)spawn_slot(i);
	while (xtc_clock_mono() < end) {
		int k = (int)(rng(&r) % NC);
		(void)xtc_proc_sleep((int64_t)(200 + rng(&r) % 800) * 1000);
		(void)reap(1);
		if (g_live[k] && xtc_exit_pid(g_pid[k], 7) == XTC_OK)
			atomic_fetch_add(&g_kills, 1);
	}
	atomic_store(&g_stop, 1);
	while (reap(0) > 0)
		(void)xtc_proc_sleep(1000LL * 1000);
	/* Every page must still be latchable: a full scan and an insert. */
	if (sx_open(":memory:", &h) == SX_OK) {
		int64_t n = one(h, "SELECT count(*) FROM t WHERE v >= 0");
		int ins = sx_exec(h, "INSERT INTO t VALUES(-1, -1, 'z')", NULL);
		atomic_store(&g_final_ok, n >= NROW && ins == SX_OK);
		sx_close(h);
	}
	atomic_fetch_add(&g_stmts, 1);
	sx_storage_quiesce();
}

int
main(int argc, char **argv)
{
	const char *sfx[] = { "", "-wal", ".dwb", "-wal.ocompact", "-wal.compact" };
	char side[320], sql[160];
	xtc_proc_opts_t bo = { .size = sizeof(xtc_proc_opts_t), .name = "boss" };
	xtc_pid_t bp;
	pthread_t wd;
	sx_db *h = NULL;
	int fd, i;

	g_ms = argc > 1 ? atol(argv[1]) : 1500;
	g_seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x5eed;
	if (g_seed == 0)
		g_seed = 1;
	if (xtc_set_stack_size(512u * 1024u) != XTC_OK)
		return 1;
	t_tmpl(g_path, sizeof g_path, "sqlxtc-killstmt");
	if ((fd = mkstemp(g_path)) < 0)
		return 1;
	close(fd);
	unlink(g_path);
	if (sx_init() != SX_OK)
		return 1;
	if (sx_storage_open(g_path, 48) != SX_OK || sx_open(":memory:", &h) != SX_OK)
		return 1;
	if (sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, v INTEGER, p TEXT)",
	    NULL) != SX_OK)
		return 1;
	if (sx_exec(h, "BEGIN", NULL) != SX_OK)
		return 1;
	for (i = 1; i <= NROW; i++) {
		snprintf(sql, sizeof sql, "INSERT INTO t VALUES(%d, %d, "
		    "'pad-pad-pad-pad-pad-pad-pad-pad-pad-pad-pad-pad-pad-pad')", i, i);
		if (sx_exec(h, sql, NULL) != SX_OK)
			return 1;
	}
	if (sx_exec(h, "COMMIT", NULL) != SX_OK)
		return 1;
	sx_close(h);
	if (xtc_exec_init(&g_exec, NL) != XTC_OK ||
	    sx_storage_run(xtc_exec_loop(g_exec, 0)) != SX_OK ||
	    xtc_proc_spawn(xtc_exec_loop(g_exec, 0), boss, NULL, &bo, &bp) != XTC_OK)
		return 1;
	(void)pthread_create(&wd, NULL, watchdog, NULL);
	(void)xtc_exec_run(g_exec);
	atomic_store(&g_done, 1);
	(void)pthread_join(wd, NULL);
	(void)xtc_exec_fini(g_exec);
	(void)sx_storage_close();
	(void)sx_shutdown();
	for (i = 0; i < (int)(sizeof sfx / sizeof sfx[0]); i++) {
		snprintf(side, sizeof side, "%s%s", g_path, sfx[i]);
		unlink(side);
	}
	printf("  kill_stmt: kills %llu statements %llu final %s\n",
	    (unsigned long long)atomic_load(&g_kills),
	    (unsigned long long)atomic_load(&g_stmts),
	    atomic_load(&g_final_ok) ? "ok" : "FAILED");
	if (!atomic_load(&g_final_ok) || atomic_load(&g_kills) == 0) {
		fprintf(stderr, "FAIL: test_kill_stmt\n");
		return 1;
	}
	printf("  ok   procs killed mid-statement leave no latch behind\n");
	return 0;
}
