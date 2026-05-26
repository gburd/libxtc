/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * examples/04_lockmgr_demo.c — heavyweight lock manager: two
 * "transactions" both want X locks on overlapping objects; the
 * deadlock detector resolves the conflict by aborting one.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "xtc.h"
#include "xtc_lockmgr.h"

static xtc_lockmgr_t *g_mgr;
static _Atomic int    g_t1_rc;
static _Atomic int    g_t2_rc;

static void *
txn_a(void *arg)
{
	xtc_locker_t l = *(xtc_locker_t *)arg;
	struct timespec sleep = { 0, 100 * 1000 * 1000 };
	int rc;

	printf("txn_a: get X on A\n");
	(void)xtc_lock_get(g_mgr, l, "A", 1, XTC_LOCK_X, 0);
	(void)nanosleep(&sleep, NULL);
	printf("txn_a: get X on B (likely blocks)\n");
	rc = xtc_lock_get(g_mgr, l, "B", 1, XTC_LOCK_X, 5LL * 1000 * 1000 * 1000);
	atomic_store(&g_t1_rc, rc);
	if (rc == XTC_E_DEADLK) printf("txn_a: aborted by detector!\n");
	else                    printf("txn_a: got both locks\n");
	return NULL;
}

static void *
txn_b(void *arg)
{
	xtc_locker_t l = *(xtc_locker_t *)arg;
	struct timespec sleep = { 0, 100 * 1000 * 1000 };
	int rc;

	printf("txn_b: get X on B\n");
	(void)xtc_lock_get(g_mgr, l, "B", 1, XTC_LOCK_X, 0);
	(void)nanosleep(&sleep, NULL);
	printf("txn_b: get X on A (likely blocks)\n");
	rc = xtc_lock_get(g_mgr, l, "A", 1, XTC_LOCK_X, 5LL * 1000 * 1000 * 1000);
	atomic_store(&g_t2_rc, rc);
	if (rc == XTC_E_DEADLK) printf("txn_b: aborted by detector!\n");
	else                    printf("txn_b: got both locks\n");
	return NULL;
}

int
main(void)
{
	xtc_lockmgr_opts_t opts = XTC_LOCKMGR_OPTS_DEFAULT;
	xtc_locker_t l1, l2;
	pthread_t t1, t2;

	opts.detect_mode  = XTC_LOCK_DETECT_PERIODIC;
	opts.detect_interval_ns = 50 * 1000 * 1000;       /* 50 ms */
	opts.victim       = XTC_LOCK_VICTIM_YOUNGEST;

	if (xtc_lockmgr_create(&opts, &g_mgr) != XTC_OK) return 1;
	(void)xtc_lockmgr_id(g_mgr, &l1);
	(void)xtc_lockmgr_id(g_mgr, &l2);

	pthread_create(&t1, NULL, txn_a, &l1);
	pthread_create(&t2, NULL, txn_b, &l2);
	pthread_join(t1, NULL);
	pthread_join(t2, NULL);

	{
		xtc_lockmgr_stat_t s;
		xtc_lockmgr_stat(g_mgr, &s);
		printf("\nstats: held=%d waiting=%d acquires=%llu releases=%llu deadlocks=%llu\n",
		    s.n_held, s.n_waiting,
		    (unsigned long long)s.n_acquires,
		    (unsigned long long)s.n_releases,
		    (unsigned long long)s.n_deadlocks_found);
	}

	(void)xtc_lockmgr_id_free(g_mgr, l1);
	(void)xtc_lockmgr_id_free(g_mgr, l2);
	xtc_lockmgr_destroy(g_mgr);
	return 0;
}
