/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_intents_thread.c
 *	Row write intents on PLAIN THREADS (an embedded engine: the sx_* API
 *	called from OS threads, not fibers) must wait like they do on fibers.
 *
 *	xs_intent_take returned a hard conflict at once when the caller was
 *	not a proc, so a READ COMMITTED UPDATE that met a row held by another
 *	transaction failed instead of waiting for it.  A client that retries
 *	(TPC-C's driver) then spun: at 32 threads, 2.8M Payment retries in a
 *	60 s run on the warehouse row, and throughput fell from 108k tpmC at
 *	8 threads to 46k.
 *
 *	A: BEGIN; UPDATE row 1 (+1); hold 50 ms; COMMIT.
 *	B: while A holds it, autocommit UPDATE row 1 (+10): must WAIT and
 *	   succeed, not fail; the row ends at 11 (both increments).
 *	Then a deadlock between two threads (A: r1 then r2; B: r2 then r1):
 *	exactly one of the second UPDATEs fails, well before the wait cap.
 *	Then N threads each run BEGIN; UPDATE hot row (first statement, the
 *	TPC-C Payment shape); COMMIT under SNAPSHOT: the UPDATE waits, sees
 *	for the holder, re-runs on a new snapshot and succeeds.  It checked
 *	"committed past my snapshot" BEFORE waiting, and a holder's version
 *	is in the tree before it is visible, so each re-run failed at once
 *	on the same holder and after 8 the statement failed: 552 of 1600.
 *	Then N threads hammer one row with autocommit increments: no lost
 *	update (the final value is exactly the number that succeeded) and
 *	failures are rare.  Not zero: a statement that keeps losing the row
 *	re-runs at most 8 times and then fails (engine.c, SXN_WRITE; the
 *	same bound on fibers) -- about 1 in 50k here.  Before the fix nearly
 *	every contended increment failed at once (1567 of 1600).
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "xstore.h"
#include "xtc.h"
#include "t_tmp.h"

#define NHAM 8
#define PERH 200

static int         g_fail;
static char        g_path[256];
static _Atomic int g_step;
static int         g_rc[2];
static int64_t     g_t0, g_t1;
static _Atomic long g_ham_ok, g_ham_bad;

#ifdef CK
#undef CK
#endif
#define CK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	g_fail = 1; } } while (0)

static int
ex(sx_db *h, const char *sql)
{
	char *err = NULL;
	int rc = sx_exec(h, sql, &err);
	free(err);
	return rc;
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

static void
spin_until(int s)
{
	while (atomic_load(&g_step) < s)
		(void)xtc_sleep_ns(100 * 1000);
}

/* ---- (a) wait then succeed ---- */
static void *
holder(void *a)
{
	sx_db *h = NULL;
	(void)a;
	if (sx_open(g_path, &h) != SX_OK) { g_fail = 1; return NULL; }
	CK(ex(h, "BEGIN") == SX_OK);
	CK(ex(h, "UPDATE t SET v = v + 1 WHERE k = 1") == SX_OK);
	atomic_store(&g_step, 1);
	(void)xtc_sleep_ns(50LL * 1000 * 1000);
	g_rc[0] = ex(h, "COMMIT");
	sx_close(h);
	return NULL;
}

static void *
waiter(void *a)
{
	sx_db *h = NULL;
	(void)a;
	if (sx_open(g_path, &h) != SX_OK) { g_fail = 1; return NULL; }
	spin_until(1);
	g_t0 = xtc_clock_mono();
	g_rc[1] = ex(h, "UPDATE t SET v = v + 10 WHERE k = 1");
	g_t1 = xtc_clock_mono();
	sx_close(h);
	return NULL;
}

/* ---- (c) deadlock ---- */
static void *
dl(void *a)
{
	long me = (long)a;
	sx_db *h = NULL;
	char q[96];
	if (sx_open(g_path, &h) != SX_OK) { g_fail = 1; return NULL; }
	CK(ex(h, "BEGIN") == SX_OK);
	snprintf(q, sizeof q, "UPDATE t SET v = v + 1 WHERE k = %ld", me ? 3L : 2L);
	CK(ex(h, q) == SX_OK);
	atomic_fetch_add(&g_step, 1);
	spin_until(12);                  /* both hold their first row */
	snprintf(q, sizeof q, "UPDATE t SET v = v + 1 WHERE k = %ld", me ? 2L : 3L);
	g_rc[me] = ex(h, q);
	if (g_rc[me] == SX_OK)
		(void)ex(h, "COMMIT");
	else
		(void)ex(h, "ROLLBACK");
	sx_close(h);
	return NULL;
}

/* ---- first-statement UPDATE in a SNAPSHOT transaction ---- */
static _Atomic long g_tx_ok, g_tx_bad;

static void *
txhammer(void *a)
{
	sx_db *h = NULL;
	int i;
	(void)a;
	if (sx_open(g_path, &h) != SX_OK) { g_fail = 1; return NULL; }
	for (i = 0; i < PERH; i++) {
		int rc;
		CK(ex(h, "BEGIN") == SX_OK);
		rc = ex(h, "UPDATE t SET v = v + 1 WHERE k = 5");
		if (rc == SX_OK && (rc = ex(h, "COMMIT")) == SX_OK) {
			atomic_fetch_add(&g_tx_ok, 1);
		} else {
			atomic_fetch_add(&g_tx_bad, 1);
			(void)ex(h, "ROLLBACK");
		}
	}
	sx_close(h);
	return NULL;
}

/* ---- hammer ---- */
static void *
hammer(void *a)
{
	sx_db *h = NULL;
	int i;
	(void)a;
	if (sx_open(g_path, &h) != SX_OK) { g_fail = 1; return NULL; }
	for (i = 0; i < PERH; i++) {
		char *err = NULL;
		int rc = sx_exec(h, "UPDATE t SET v = v + 1 WHERE k = 4", &err);
		if (rc == SX_OK) {
			atomic_fetch_add(&g_ham_ok, 1);
		} else {
			atomic_fetch_add(&g_ham_bad, 1);
			CK(rc == SX_BUSY);        /* the bounded-restart conflict only */
		}
		free(err);
	}
	sx_close(h);
	return NULL;
}

int
main(void)
{
	pthread_t a, b, hm[NHAM];
	sx_db *h = NULL;
	char wal[300], dwb[300];
	int fd, i;
	int64_t ms;

	t_tmpl(g_path, sizeof g_path, "sqlxtc-ithr");
	if ((fd = mkstemp(g_path)) < 0) return 1;
	close(fd);
	unlink(g_path);
	snprintf(wal, sizeof wal, "%s-wal", g_path);
	snprintf(dwb, sizeof dwb, "%s.dwb", g_path);
	CK(sx_init() == SX_OK);
	CK(sx_storage_open(g_path, 256) == SX_OK);
	CK(sx_open(g_path, &h) == SX_OK);
	CK(ex(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, v INTEGER)") == SX_OK);
	for (i = 1; i <= 5; i++) {
		char q[64];
		snprintf(q, sizeof q, "INSERT INTO t VALUES(%d, 0)", i);
		CK(ex(h, q) == SX_OK);
	}

	/* (a) */
	atomic_store(&g_step, 0);
	(void)pthread_create(&a, NULL, holder, NULL);
	(void)pthread_create(&b, NULL, waiter, NULL);
	(void)pthread_join(a, NULL);
	(void)pthread_join(b, NULL);
	ms = (g_t1 - g_t0) / 1000000;
	printf("  (a) holder COMMIT rc=%d, waiter UPDATE rc=%d after %lld ms, v=%lld\n",
	    g_rc[0], g_rc[1], (long long)ms,
	    (long long)one(h, "SELECT v FROM t WHERE k = 1"));
	CK(g_rc[0] == SX_OK);
	CK(g_rc[1] == SX_OK);                   /* waited, not failed */
	CK(ms >= 20);                           /* it really waited */
	CK(one(h, "SELECT v FROM t WHERE k = 1") == 11);

	/* (c) */
	atomic_store(&g_step, 10);
	(void)pthread_create(&a, NULL, dl, (void *)0L);
	(void)pthread_create(&b, NULL, dl, (void *)1L);
	g_t0 = xtc_clock_mono();
	(void)pthread_join(a, NULL);
	(void)pthread_join(b, NULL);
	ms = (xtc_clock_mono() - g_t0) / 1000000;
	printf("  (c) deadlock: rc=%d/%d in %lld ms\n", g_rc[0], g_rc[1], (long long)ms);
	CK((g_rc[0] == SX_OK) != (g_rc[1] == SX_OK));   /* exactly one wins */
	CK(ms < 2000);
	CK(one(h, "SELECT v FROM t WHERE k = 2") == 1);
	CK(one(h, "SELECT v FROM t WHERE k = 3") == 1);

	/* first-statement UPDATE in a transaction */
	for (i = 0; i < NHAM; i++)
		(void)pthread_create(&hm[i], NULL, txhammer, NULL);
	for (i = 0; i < NHAM; i++)
		(void)pthread_join(hm[i], NULL);
	printf("  txn first-statement: %ld ok, %ld failed, v=%lld (want = ok)\n",
	    atomic_load(&g_tx_ok), atomic_load(&g_tx_bad),
	    (long long)one(h, "SELECT v FROM t WHERE k = 5"));
	CK(atomic_load(&g_tx_bad) * 100 <= NHAM * PERH);    /* <= 1% */
	CK(one(h, "SELECT v FROM t WHERE k = 5") == atomic_load(&g_tx_ok));

	/* hammer */
	for (i = 0; i < NHAM; i++)
		(void)pthread_create(&hm[i], NULL, hammer, NULL);
	for (i = 0; i < NHAM; i++)
		(void)pthread_join(hm[i], NULL);
	printf("  hammer: %ld ok, %ld failed, v=%lld (want = ok)\n",
	    atomic_load(&g_ham_ok), atomic_load(&g_ham_bad),
	    (long long)one(h, "SELECT v FROM t WHERE k = 4"));
	CK(atomic_load(&g_ham_bad) * 100 <= NHAM * PERH);   /* <= 1% */
	CK(one(h, "SELECT v FROM t WHERE k = 4") == atomic_load(&g_ham_ok));

	sx_close(h);
	CK(sx_storage_close() == SX_OK);
	unlink(g_path); unlink(wal); unlink(dwb);
	if (g_fail) { fprintf(stderr, "FAIL: test_intents_thread\n"); return 1; }
	printf("  ok   row intents wait on plain threads as on fibers\n");
	return 0;
}
