/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_snap_registry.c
 *	The snapshot registry (the version-GC horizon) is lock-free on the
 *	transaction path, and still never lets GC reclaim a version a live
 *	snapshot can see.
 *
 *	(1) Contention: 8 threads run read-only BEGIN / point SELECT / COMMIT
 *	    transactions.  Each BEGIN pins and each COMMIT releases a
 *	    snapshot.  The registry used one global mutex for both (and the
 *	    horizon scan held it across all slots): at 8 threads 70% of the
 *	    time was futex wait/wake.  Asserted: 8 threads deliver at least
 *	    2x the 1-thread rate (they delivered 0.45x); with fewer than 8
 *	    CPUs, at least 1.5x (a 4-vCPU CI runner measures ~2.0x).
 *	(2) Safety under the race: writer threads update rows (each update
 *	    adds a version, autovacuum prunes behind the horizon) while
 *	    REPEATABLE READ readers begin, read every row twice with a
 *	    sleep between, and require the same values; a GC thread runs
 *	    xstore_gc_run in a loop.  A version reclaimed under a live
 *	    snapshot shows up as a changed or missing row.
 *	(3) Autocommit statements: 4 threads run autocommit
 *	    "UPDATE t SET v = v + 1 WHERE k = 1" (autovacuum on) and 2 run
 *	    autocommit SELECTs of every row, with the GC thread running.  An
 *	    autocommit statement's snapshot was never pinned, so a prune
 *	    behind `now` could reclaim the version it was about to read: the
 *	    UPDATE then found no row and succeeded changing nothing (a lost
 *	    update: 1-4 of 1600 in test_intents_thread), and a SELECT missed
 *	    the row.  Asserted: every successful UPDATE changed one row, v
 *	    equals their count, and every SELECT saw all NROW rows.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "engine.h"
#include "xstore.h"
#include "xtc.h"
#include "t_tmp.h"

#define NROW 64

static int g_fail;
static char g_path[256];
static _Atomic int g_stop;
static _Atomic long g_reads, g_bad, g_upd;

#ifdef CK
#undef CK
#endif
#define CK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	g_fail = 1; } } while (0)

static double
now_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void *
reader_tx(void *arg)
{
	sx_db *h = NULL;
	sx_stmt *q = NULL;
	long i = (long)arg;
	if (sx_open(g_path, &h) != SX_OK) { g_fail = 1; return NULL; }
	if (sx_prepare(h, "SELECT v FROM t WHERE k = ?", -1, &q, NULL) != SX_OK) { g_fail = 1; return NULL; }
	while (!atomic_load(&g_stop)) {
		if (sx_exec(h, "BEGIN", NULL) != SX_OK) { g_fail = 1; break; }
		sx_bind_int64(q, 1, (i++ % NROW) + 1);
		if (sx_step(q) != SX_ROW) g_bad++;
		sx_reset(q);
		if (sx_exec(h, "COMMIT", NULL) != SX_OK) { g_fail = 1; break; }
		atomic_fetch_add(&g_reads, 1);
	}
	sx_finalize(q);
	sx_close(h);
	return NULL;
}

static double
rate(int nthr)
{
	pthread_t th[16];
	double t0;
	long r0;
	int i;
	atomic_store(&g_stop, 0);
	r0 = atomic_load(&g_reads);
	t0 = now_s();
	for (i = 0; i < nthr; i++) pthread_create(&th[i], NULL, reader_tx, (void *)(long)i);
	usleep(1500 * 1000);
	atomic_store(&g_stop, 1);
	for (i = 0; i < nthr; i++) pthread_join(th[i], NULL);
	return (double)(atomic_load(&g_reads) - r0) / (now_s() - t0);
}

static void *
writer(void *arg)
{
	sx_db *h = NULL;
	char sql[96];
	long n = 0, id = (long)arg;
	if (sx_open(g_path, &h) != SX_OK) { g_fail = 1; return NULL; }
	(void)xstore_autovacuum_set((struct xsql *)h, 1);
	while (!atomic_load(&g_stop)) {
		snprintf(sql, sizeof sql, "UPDATE t SET v = v + 1 WHERE k = %ld", 1 + (n++ * 7 + id) % NROW);
		if (sx_exec(h, sql, NULL) == SX_OK) atomic_fetch_add(&g_upd, 1);
	}
	sx_close(h);
	return NULL;
}

static _Atomic long g_ac_ok, g_ac_zero, g_ac_short;

static void *
ac_writer(void *arg)
{
	sx_db *h = NULL;
	sx_stmt *u = NULL;
	(void)arg;
	if (sx_open(g_path, &h) != SX_OK) { g_fail = 1; return NULL; }
	(void)xstore_autovacuum_set((struct xsql *)h, 1);
	if (sx_prepare(h, "UPDATE t SET v = v + 1 WHERE k = 1", -1, &u, NULL) != SX_OK) { g_fail = 1; return NULL; }
	while (!atomic_load(&g_stop)) {
		int rc = sx_step(u);
		if (rc == SX_DONE && sx_stmt_changes(u) == 1) atomic_fetch_add(&g_ac_ok, 1);
		else if (rc == SX_DONE) atomic_fetch_add(&g_ac_zero, 1);
		sx_reset(u);                   /* else: a bounded-restart conflict */
	}
	sx_finalize(u);
	sx_close(h);
	return NULL;
}

static void *
ac_reader(void *arg)
{
	sx_db *h = NULL;
	sx_stmt *q = NULL;
	(void)arg;
	if (sx_open(g_path, &h) != SX_OK) { g_fail = 1; return NULL; }
	if (sx_prepare(h, "SELECT k FROM t", -1, &q, NULL) != SX_OK) { g_fail = 1; return NULL; }
	while (!atomic_load(&g_stop)) {
		int n = 0;
		while (sx_step(q) == SX_ROW) n++;
		sx_reset(q);
		if (n != NROW) atomic_fetch_add(&g_ac_short, 1);
	}
	sx_finalize(q);
	sx_close(h);
	return NULL;
}

static int64_t
one(sx_db *h, const char *sql)
{
	sx_stmt *st = NULL;
	int64_t v = -1;
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK) return -2;
	if (sx_step(st) == SX_ROW) v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

static void *
gcer(void *arg)
{
	sx_db *h = NULL;
	(void)arg;
	if (sx_open(g_path, &h) != SX_OK) { g_fail = 1; return NULL; }
	while (!atomic_load(&g_stop))
		(void)xstore_gc_run((struct xsql *)h);
	sx_close(h);
	return NULL;
}

static void *
rr_reader(void *arg)
{
	sx_db *h = NULL;
	sx_stmt *q = NULL;
	int64_t a[NROW];
	(void)arg;
	if (sx_open(g_path, &h) != SX_OK) { g_fail = 1; return NULL; }
	if (sx_prepare(h, "SELECT k, v FROM t ORDER BY k", -1, &q, NULL) != SX_OK) { g_fail = 1; return NULL; }
	while (!atomic_load(&g_stop)) {
		int n = 0, m = 0;
		if (sx_exec(h, "BEGIN ISOLATION LEVEL REPEATABLE READ", NULL) != SX_OK) { g_fail = 1; break; }
		while (sx_step(q) == SX_ROW && n < NROW) a[n++] = sx_column_int64(q, 1);
		sx_reset(q);
		usleep(200);
		while (sx_step(q) == SX_ROW) {
			if (m >= NROW || sx_column_int64(q, 1) != a[m]) g_bad++;
			m++;
		}
		sx_reset(q);
		if (n != NROW || m != NROW) g_bad++;
		(void)sx_exec(h, "COMMIT", NULL);
	}
	sx_finalize(q);
	sx_close(h);
	return NULL;
}

int
main(void)
{
	char side[320];
	const char *sfx[] = { "", "-wal", ".dwb" };
	sx_db *h = NULL;
	pthread_t th[12];
	double r1, r8;
	int64_t v0, v1;
	int fd, i;

	t_tmpl(g_path, sizeof g_path, "sqlxtc-snapreg");
	if ((fd = mkstemp(g_path)) < 0) return 1;
	close(fd);
	unlink(g_path);
	CK(sx_init() == SX_OK);
	CK(sx_storage_open(g_path, 1024) == SX_OK);
	CK(sx_open(g_path, &h) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, v INTEGER)", NULL) == SX_OK);
	for (i = 1; i <= NROW; i++) {
		char sql[64];
		snprintf(sql, sizeof sql, "INSERT INTO t VALUES(%d, 0)", i);
		CK(sx_exec(h, sql, NULL) == SX_OK);
	}
	sx_close(h);

	/* (1) */
	r1 = rate(1);
	r8 = rate(8);
	printf("  read-only txns/s: 1 thread %.0f, 8 threads %.0f (%.2fx)\n", r1, r8, r8 / r1);
	/* 8 threads cannot beat ~ncpu x on fewer CPUs; the mutex version
	 * delivered 0.45x anywhere, so 1.5x still separates the two. */
	/* xtc_ncpus: the CPUs this process may use (affinity and cgroup
	 * quota), not the host's online count -- a 2-CPU container on a
	 * 32-CPU host measured ~1.7x against the 2.0x the online count
	 * demanded. */
	CK(r8 >= (xtc_ncpus() >= 8 ? 2.0 : 1.5) * r1);

	/* (2) */
	atomic_store(&g_stop, 0);
	for (i = 0; i < 4; i++) pthread_create(&th[i], NULL, writer, (void *)(long)i);
	pthread_create(&th[4], NULL, gcer, NULL);
	for (i = 5; i < 9; i++) pthread_create(&th[i], NULL, rr_reader, NULL);
	usleep(3000 * 1000);
	atomic_store(&g_stop, 1);
	for (i = 0; i < 9; i++) pthread_join(th[i], NULL);
	printf("  updates=%ld snapshot-violations=%ld\n", atomic_load(&g_upd), atomic_load(&g_bad));
	CK(atomic_load(&g_upd) > 1000);
	CK(atomic_load(&g_bad) == 0);

	/* (3) */
	CK(sx_open(g_path, &h) == SX_OK);
	v0 = one(h, "SELECT v FROM t WHERE k = 1");
	atomic_store(&g_stop, 0);
	for (i = 0; i < 4; i++) pthread_create(&th[i], NULL, ac_writer, NULL);
	pthread_create(&th[4], NULL, gcer, NULL);
	for (i = 5; i < 7; i++) pthread_create(&th[i], NULL, ac_reader, NULL);
	usleep(3000 * 1000);
	atomic_store(&g_stop, 1);
	for (i = 0; i < 7; i++) pthread_join(th[i], NULL);
	v1 = one(h, "SELECT v FROM t WHERE k = 1");
	printf("  autocommit: %ld updates, %ld changed nothing, v advanced %lld; "
	    "%ld short SELECTs\n", atomic_load(&g_ac_ok), atomic_load(&g_ac_zero),
	    (long long)(v1 - v0), atomic_load(&g_ac_short));
	CK(atomic_load(&g_ac_ok) > 1000);
	CK(atomic_load(&g_ac_zero) == 0);
	CK(v1 - v0 == atomic_load(&g_ac_ok));
	CK(atomic_load(&g_ac_short) == 0);
	sx_close(h);

	(void)sx_storage_close();
	(void)sx_shutdown();
	for (i = 0; i < 3; i++) { snprintf(side, sizeof side, "%s%s", g_path, sfx[i]); unlink(side); }
	if (g_fail) { fprintf(stderr, "FAIL: test_snap_registry\n"); return 1; }
	printf("  ok   snapshot registry scales and never under-protects a live snapshot\n");
	return 0;
}
