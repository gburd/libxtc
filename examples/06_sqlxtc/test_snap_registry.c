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
 *	    2x the 1-thread rate (they delivered 0.45x).
 *	(2) Safety under the race: writer threads update rows (each update
 *	    adds a version, autovacuum prunes behind the horizon) while
 *	    REPEATABLE READ readers begin, read every row twice with a
 *	    sleep between, and require the same values; a GC thread runs
 *	    xstore_gc_run in a loop.  A version reclaimed under a live
 *	    snapshot shows up as a changed or missing row.
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
	CK(r8 >= 2.0 * r1);

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

	(void)sx_storage_close();
	(void)sx_shutdown();
	for (i = 0; i < 3; i++) { snprintf(side, sizeof side, "%s%s", g_path, sfx[i]); unlink(side); }
	if (g_fail) { fprintf(stderr, "FAIL: test_snap_registry\n"); return 1; }
	printf("  ok   snapshot registry scales and never under-protects a live snapshot\n");
	return 0;
}
