/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_storage_sup.c
 *	The storage engine's background procs are supervised.  Under commit
 *	load (writer procs inserting over a pool far smaller than the
 *	table, so the provider and trickler have work), each is killed
 *	with xtc_exit_pid:
 *	  - the page provider and the trickler are pure background work:
 *	    each kill must be followed by a restart (a new live pid) within
 *	    BOUND_MS; past its restart budget the next kill escalates;
 *	  - the checkpoint proc and the WAL writer are not restarted: the
 *	    kill must reach the sx_storage_on_failure hook (the server stops
 *	    on it as on SIGTERM) within BOUND_MS;
 *	and after each session sx_storage_quiesce must take under 1 s, and
 *	a reopened store must hold every acknowledged commit.
 *	Before the supervision a killed provider or trickler stayed dead for
 *	the rest of the run and nothing noticed any of the four exits.
 *
 *	usage: test_storage_sup [seed]
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
#include "xtc_inspect.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "t_tmp.h"

#define NW       4
#define NROW     4000
#define BUDGET   10           /* restarts the engine allows per proc */

static char             g_path[256];
static xtc_exec_t      *g_exec;
static _Atomic int      g_stop, g_done, g_live;
static _Atomic int64_t  g_next = NROW + 1, g_acked;
static _Atomic int      g_hook_wal, g_hook_ckpt, g_hook_pp, g_hook_tr;
static int64_t          g_bound_ms = 500, g_q_ms[2] = { -1, -1 };
static int              g_restarts[2], g_session;
static uint64_t         g_seed;

static uint64_t
rng(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

static int64_t
ms(void)
{
	return xtc_clock_mono() / 1000000;
}

static void
on_failure(const char *what, void *arg)
{
	(void)arg;
	if (strcmp(what, "wal") == 0)
		atomic_fetch_add(&g_hook_wal, 1);
	else if (strcmp(what, "checkpoint") == 0)
		atomic_fetch_add(&g_hook_ckpt, 1);
	else if (strcmp(what, "bm-provider") == 0)
		atomic_fetch_add(&g_hook_pp, 1);
	else if (strcmp(what, "bm-trickler") == 0)
		atomic_fetch_add(&g_hook_tr, 1);
}

static void
close_db(void *arg)
{
	sx_close(arg);
	atomic_fetch_sub(&g_live, 1);
}

static void
writer(void *arg)
{
	sx_db *h = NULL;
	char sql[160];
	(void)arg;
	if (sx_open(":memory:", &h) != SX_OK) {
		atomic_fetch_sub(&g_live, 1);
		return;
	}
	if (xtc_proc_at_exit(close_db, h) != XTC_OK) {
		g_fail = 1;
		close_db(h);
		return;
	}
	while (!atomic_load(&g_stop)) {
		int64_t k = atomic_fetch_add(&g_next, 1);
		snprintf(sql, sizeof sql, "INSERT INTO t VALUES(%lld, %lld, "
		    "'pad-pad-pad-pad-pad-pad-pad-pad-pad-pad-pad-pad-pad-pad')",
		    (long long)k, (long long)k);
		if (sx_exec(h, sql, NULL) == SX_OK)
			atomic_fetch_add(&g_acked, 1);
	}
}

/* Kill `which` and wait for a restart (a different live pid): 1 if one
 * came within the bound. */
static int
kill_restart(int which)
{
	xtc_pid_t old, now;
	int64_t t0;

	if (!sx_storage_proc_pid(which, &old))
		return 0;
	if (xtc_exit_pid(old, 9) != XTC_OK)
		return 0;
	t0 = ms();
	while (ms() - t0 < g_bound_ms) {
		if (sx_storage_proc_pid(which, &now) && !xtc_pid_eq(now, old))
			return 1;
		(void)xtc_proc_sleep(1000LL * 1000);
	}
	return 0;
}

/* Wait for *hook to reach `want` within the bound: 1 if it did. */
static int
await_hook(_Atomic int *hook, int want)
{
	int64_t t0 = ms();
	while (ms() - t0 < g_bound_ms) {
		if (atomic_load(hook) >= want)
			return 1;
		(void)xtc_proc_sleep(1000LL * 1000);
	}
	return atomic_load(hook) >= want;
}

static void
kill_one(int which)
{
	xtc_pid_t p;
	if (sx_storage_proc_pid(which, &p))
		(void)xtc_exit_pid(p, 9);
}

static void
boss(void *arg)
{
	xtc_proc_opts_t o = { .name = "writer" };
	uint64_t r = g_seed;
	xtc_pid_t wp;
	int64_t a;
	int i, s = g_session;
	(void)arg;

	for (i = 0; i < NW; i++) {
		atomic_fetch_add(&g_live, 1);
		if (xtc_proc_spawn(xtc_exec_loop(g_exec, 1), writer, NULL, &o,
		    &wp) != XTC_OK)
			atomic_fetch_sub(&g_live, 1);
	}
	(void)xtc_proc_sleep(50LL * 1000 * 1000);
	if (s == 0) {
		/* Restartable: provider and trickler, a few kills each, at
		 * random points of their passes. */
		for (i = 0; i < 4; i++) {
			(void)xtc_proc_sleep((int64_t)(1 + rng(&r) % 20) * 1000000);
			g_restarts[0] += kill_restart(SX_PROC_PROVIDER);
			(void)xtc_proc_sleep((int64_t)(1 + rng(&r) % 20) * 1000000);
			g_restarts[1] += kill_restart(SX_PROC_TRICKLER);
		}
		CK(g_restarts[0] == 4);
		CK(g_restarts[1] == 4);
		CK(atomic_load(&g_hook_pp) == 0 && atomic_load(&g_hook_tr) == 0);
		/* Not restartable: the checkpoint proc escalates. */
		kill_one(SX_PROC_CKPT);
		CK(await_hook(&g_hook_ckpt, 1));
	} else {
		/* The trickler past its budget escalates; then the WAL
		 * writer, which is never restarted. */
		for (i = 0; i <= BUDGET; i++) {
			kill_one(SX_PROC_TRICKLER);
			(void)xtc_proc_sleep(5LL * 1000 * 1000);
		}
		CK(await_hook(&g_hook_tr, 1));
		kill_one(SX_PROC_WAL);
		CK(await_hook(&g_hook_wal, 1));
	}
	(void)xtc_proc_sleep(30LL * 1000 * 1000);     /* commits continue */
	atomic_store(&g_stop, 1);
	while (atomic_load(&g_live) > 0)
		(void)xtc_proc_sleep(1000LL * 1000);
	a = ms();
	sx_storage_quiesce();
	g_q_ms[s] = ms() - a;
}

static void *
watchdog(void *arg)
{
	int64_t t0 = ms();
	(void)arg;
	while (!atomic_load(&g_done)) {
		if (ms() - t0 > 60000) {
			fprintf(stderr, "FAIL: test_storage_sup: hang (session %d)\n",
			    g_session);
			_exit(3);
		}
		(void)xtc_sleep_ns(50LL * 1000 * 1000);
	}
	return NULL;
}

static int64_t
count_rows(void)
{
	sx_db *h = NULL;
	sx_stmt *st = NULL;
	int64_t v = -1;
	if (sx_storage_open(g_path, 64) != SX_OK || sx_open(":memory:", &h) != SX_OK)
		return -1;
	if (sx_prepare(h, "SELECT count(*) FROM t", -1, &st, NULL) == SX_OK &&
	    sx_step(st) == SX_ROW)
		v = sx_column_int64(st, 0);
	sx_finalize(st);
	sx_close(h);
	(void)sx_storage_close();
	return v;
}

static int
session(int s)
{
	xtc_proc_opts_t bo = { .name = "boss" };
	xtc_pid_t bp;
	pthread_t wd;
	int64_t rows;

	g_session = s;
	atomic_store(&g_stop, 0);
	atomic_store(&g_done, 0);
	if (sx_storage_open(g_path, 64) != SX_OK)
		return -1;
	sx_storage_on_failure(on_failure, NULL);
	if (xtc_exec_init(&g_exec, 2) != XTC_OK ||
	    sx_storage_run(xtc_exec_loop(g_exec, 0)) != SX_OK ||
	    xtc_proc_spawn(xtc_exec_loop(g_exec, 0), boss, NULL, &bo, &bp) != XTC_OK)
		return -1;
	(void)pthread_create(&wd, NULL, watchdog, NULL);
	(void)xtc_exec_run(g_exec);
	atomic_store(&g_done, 1);
	(void)pthread_join(wd, NULL);
	(void)xtc_exec_fini(g_exec);
	CK(sx_storage_close() == SX_OK);
	rows = count_rows();
	printf("  session %d: restarts pp %d/4 tr %d/4  hooks ckpt %d wal %d "
	    "pp %d tr %d  quiesce %lld ms  acked %lld rows %lld\n", s,
	    g_restarts[0], g_restarts[1], atomic_load(&g_hook_ckpt),
	    atomic_load(&g_hook_wal), atomic_load(&g_hook_pp),
	    atomic_load(&g_hook_tr), (long long)g_q_ms[s],
	    (long long)atomic_load(&g_acked), (long long)rows);
	CK(g_q_ms[s] >= 0 && g_q_ms[s] < 1000);
	CK(rows == NROW + atomic_load(&g_acked));    /* no acked commit lost */
	return 0;
}

int
main(int argc, char **argv)
{
	const char *sfx[] = { "", "-wal", ".dwb", "-wal.ocompact", "-wal.compact" };
	char side[320], sql[160];
	sx_db *h = NULL;
	int fd, i;

	g_seed = argc > 1 ? strtoull(argv[1], NULL, 0) : 0x5eed;
	if (g_seed == 0)
		g_seed = 1;
	if (getenv("XTC_UNDER_VALGRIND") != NULL ||
	    getenv("XTC_TEST_EMULATED") != NULL)
		g_bound_ms *= 20;
	if (xtc_set_stack_size(512u * 1024u) != XTC_OK)
		return 1;
	t_tmpl(g_path, sizeof g_path, "sqlxtc-storsup");
	if ((fd = mkstemp(g_path)) < 0)
		return 1;
	close(fd);
	unlink(g_path);
	if (sx_init() != SX_OK || sx_storage_open(g_path, 64) != SX_OK ||
	    sx_open(":memory:", &h) != SX_OK)
		return 1;
	if (sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, v INTEGER, p TEXT)",
	    NULL) != SX_OK || sx_exec(h, "BEGIN", NULL) != SX_OK)
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
	if (sx_storage_close() != SX_OK)
		return 1;
	CK(session(0) == 0);
	CK(session(1) == 0);
	(void)sx_shutdown();
	for (i = 0; i < (int)(sizeof sfx / sizeof sfx[0]); i++) {
		snprintf(side, sizeof side, "%s%s", g_path, sfx[i]);
		unlink(side);
	}
	if (g_fail) {
		fprintf(stderr, "FAIL: test_storage_sup\n");
		return 1;
	}
	printf("  ok   storage procs: provider/trickler restarted, wal/checkpoint "
	    "escalate, quiesce stays fast\n");
	return 0;
}
