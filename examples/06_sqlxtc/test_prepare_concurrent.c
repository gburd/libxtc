/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_prepare_concurrent.c
 *	SELECT prepares of different connections run concurrently.
 *
 *	Every vx_prepare_select ran under one global recursive amutex,
 *	because a prepare's correlated-subquery state lived in four
 *	file-scope variables.  A plan that cannot be kept (a scalar
 *	subquery's value is spliced into it) is prepared at every
 *	execution, and its subquery RUNS inside the prepare, so the
 *	ledger blend's analytic query held the lock 0.5-2 ms per run: at
 *	64 clients 299 of 320 sampled client stacks waited on it and the
 *	whole mix fell from 135k to 34k txn/s.
 *
 *	(a) A parks inside its SELECT prepare (VX_PREPARE_TEST_HOOK, this
 *	    file #includes vexec.c); B, another connection on another
 *	    thread, must prepare and run its SELECT while A is parked.
 *	    Before the fix B blocked on the lock until A gave up (10 s).
 *	(b) NT threads run correlated scalar / IN / EXISTS (qualified and
 *	    unqualified outer names) and uncorrelated-subquery SELECTs
 *	    concurrently, each result checked: the correlation state is
 *	    per prepare now, so no prepare sees another's (the race the
 *	    lock was added for: a NULL inner-column entry, SIGSEGV).
 */

#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#include "xtc.h"

static _Atomic int g_armed, g_parked, g_b_done;
static _Atomic int g_b_during;      /* B finished while A was parked */
static pthread_t   g_a_thr;         /* only A's prepare parks */

/* Park A's first prepare after it armed until B is done, at most 10 s
 * (B's whole run is ~1 ms; 10 s leaves room for valgrind / qemu). */
static void
park_hook(void)
{
	int64_t t0;
	if (!atomic_load(&g_armed) || !pthread_equal(pthread_self(), g_a_thr) ||
	    !atomic_exchange(&g_armed, 0))
		return;
	atomic_store(&g_parked, 1);
	t0 = xtc_clock_mono();
	while (!atomic_load(&g_b_done) && xtc_clock_mono() - t0 < 10000000000LL)
		(void)xtc_sleep_ns(1000000);
	if (atomic_load(&g_b_done))
		atomic_store(&g_b_during, 1);
	atomic_store(&g_parked, 0);
}
#define VX_PREPARE_TEST_HOOK() park_hook()
#include "vexec.c"

#include "engine.h"
#include "t_tmp.h"

#define NT    8
#define ITERS 300

static char g_path[256];

static int64_t
one(sx_db *h, const char *sql, int *ok)
{
	sx_stmt *st = NULL;
	int64_t v = -1;
	*ok = 0;
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK) return -1;
	if (sx_step(st) == SX_ROW) { v = sx_column_int64(st, 0); *ok = 1; }
	sx_finalize(st);
	return v;
}

static void *
thr_a(void *arg)
{
	sx_db *h = NULL;
	int ok;
	(void)arg;
	if (sx_open(g_path, &h) != SX_OK) { g_fail = 1; return NULL; }
	g_a_thr = pthread_self();
	atomic_store(&g_armed, 1);
	CK(one(h, "SELECT count(*) FROM t WHERE k > 2", &ok) == 3 && ok);
	sx_close(h);
	return NULL;
}

static void *
thr_b(void *arg)
{
	sx_db *h = NULL;
	int ok;
	(void)arg;
	if (sx_open(g_path, &h) != SX_OK) { g_fail = 1; return NULL; }
	while (!atomic_load(&g_parked))
		(void)xtc_sleep_ns(100000);
	CK(one(h, "SELECT sum(v) FROM t WHERE k <= 2", &ok) == 30 && ok);
	atomic_store(&g_b_done, 1);
	sx_close(h);
	return NULL;
}

/* Correlated and uncorrelated subqueries; each with its known answer
 * over t(k, v) = (1,10) (2,20) (3,30) (4,40) (5,50) and u(k, w) =
 * (1,1) (3,3) (3,4) (5,5). */
static const struct { const char *sql; int64_t want; } g_q[] = {
	{ "SELECT count(*) FROM t WHERE EXISTS(SELECT 1 FROM u WHERE u.k = t.k)", 3 },
	{ "SELECT count(*) FROM t WHERE EXISTS(SELECT 1 FROM u WHERE w = v / 10)", 4 },
	{ "SELECT count(*) FROM t WHERE k IN (SELECT k FROM u WHERE u.w >= t.k)", 3 },
	{ "SELECT sum((SELECT count(*) FROM u WHERE u.k = t.k)) FROM t", 4 },
	{ "SELECT count(*) FROM t WHERE v > (SELECT max(w) FROM u) * 8", 1 },
	{ "SELECT sum(v) FROM t WHERE k IN (SELECT k FROM u)", 90 },
};
#define NQ ((int)(sizeof g_q / sizeof g_q[0]))
static _Atomic long g_wrong, g_runs;

static void *
thr_mix(void *arg)
{
	sx_db *h = NULL;
	long me = (long)arg;
	int i, ok;
	if (sx_open(g_path, &h) != SX_OK) { g_fail = 1; return NULL; }
	for (i = 0; i < ITERS; i++) {
		int q = (int)((me + i) % NQ);
		int64_t v = one(h, g_q[q].sql, &ok);
		if (!ok || v != g_q[q].want) {
			if (atomic_fetch_add(&g_wrong, 1) < 3)
				fprintf(stderr, "  wrong: %s -> %lld (ok=%d), want %lld\n",
				    g_q[q].sql, (long long)v, ok, (long long)g_q[q].want);
		}
		atomic_fetch_add(&g_runs, 1);
	}
	sx_close(h);
	return NULL;
}

int
main(void)
{
	pthread_t a, b, m[NT];
	sx_db *h = NULL;
	char wal[300], dwb[300];
	int fd, i, ok;
	long j;

	t_tmpl(g_path, sizeof g_path, "sqlxtc-pconc");
	if ((fd = mkstemp(g_path)) < 0) return 1;
	close(fd);
	unlink(g_path);
	snprintf(wal, sizeof wal, "%s-wal", g_path);
	snprintf(dwb, sizeof dwb, "%s.dwb", g_path);
	CK(sx_init() == SX_OK);
	CK(sx_storage_open(g_path, 512) == SX_OK);
	CK(sx_open(g_path, &h) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, v INTEGER)", NULL) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE u(k INTEGER, w INTEGER)", NULL) == SX_OK);
	CK(sx_exec(h, "INSERT INTO t VALUES(1,10),(2,20),(3,30),(4,40),(5,50)", NULL) == SX_OK);
	CK(sx_exec(h, "INSERT INTO u VALUES(1,1),(3,3),(3,4),(5,5)", NULL) == SX_OK);
	for (i = 0; i < NQ; i++)          /* the answers, single-threaded */
		CK(one(h, g_q[i].sql, &ok) == g_q[i].want && ok);

	/* (a) */
	(void)pthread_create(&a, NULL, thr_a, NULL);
	(void)pthread_create(&b, NULL, thr_b, NULL);
	(void)pthread_join(a, NULL);
	(void)pthread_join(b, NULL);
	printf("  (a) B's SELECT ran while A was parked in its prepare: %s\n",
	    atomic_load(&g_b_during) ? "yes" : "NO (serialized)");
	CK(atomic_load(&g_b_during) == 1);

	/* (b) */
	for (j = 0; j < NT; j++)
		(void)pthread_create(&m[j], NULL, thr_mix, (void *)j);
	for (j = 0; j < NT; j++)
		(void)pthread_join(m[j], NULL);
	printf("  (b) %ld concurrent subquery SELECTs, %ld wrong\n",
	    atomic_load(&g_runs), atomic_load(&g_wrong));
	CK(atomic_load(&g_runs) == (long)NT * ITERS);
	CK(atomic_load(&g_wrong) == 0);

	sx_close(h);
	CK(sx_storage_close() == SX_OK);
	unlink(g_path); unlink(wal); unlink(dwb);
	if (g_fail) { fprintf(stderr, "FAIL: test_prepare_concurrent\n"); return 1; }
	printf("  ok   SELECT prepares run concurrently\n");
	return 0;
}
