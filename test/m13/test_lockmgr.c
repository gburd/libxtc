/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m13/test_lockmgr.c — verifies M13c lock manager.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_lockmgr.h"
#include "xtc_int.h"

/* Conflict matrix sanity check via the public API. */
static MunitResult
test_modes(const MunitParameter p[], void *d)
{
	xtc_lockmgr_t *m;
	xtc_lockmgr_opts_t o = XTC_LOCKMGR_OPTS_DEFAULT;
	xtc_locker_t l1, l2;
	const char *obj = "obj";
	(void)p; (void)d;
	o.detect_mode = XTC_LOCK_DETECT_NONE;

	munit_assert_int(xtc_lockmgr_create(&o, &m), ==, XTC_OK);
	munit_assert_int(xtc_lockmgr_id(m, &l1), ==, XTC_OK);
	munit_assert_int(xtc_lockmgr_id(m, &l2), ==, XTC_OK);

	/* l1 takes S, l2 also takes S — should both succeed. */
	munit_assert_int(xtc_lock_get(m, l1, obj, 3, XTC_LOCK_S, 0), ==, XTC_OK);
	munit_assert_int(xtc_lock_get(m, l2, obj, 3, XTC_LOCK_S, 0), ==, XTC_OK);

	/* l1 wants X — should fail NOWAIT (l2 holds S). */
	munit_assert_int(xtc_lock_get(m, l1, obj, 3, XTC_LOCK_X, 0), ==, XTC_E_AGAIN);

	munit_assert_int(xtc_lock_release_all(m, l1), >, 0);
	munit_assert_int(xtc_lock_release_all(m, l2), >, 0);

	/* Now l1 can take X. */
	munit_assert_int(xtc_lock_get(m, l1, obj, 3, XTC_LOCK_X, 0), ==, XTC_OK);
	/* l2 wants any S/IS/IX/X — all should fail NOWAIT. */
	munit_assert_int(xtc_lock_get(m, l2, obj, 3, XTC_LOCK_S, 0), ==, XTC_E_AGAIN);
	munit_assert_int(xtc_lock_get(m, l2, obj, 3, XTC_LOCK_IS, 0), ==, XTC_E_AGAIN);
	munit_assert_int(xtc_lock_get(m, l2, obj, 3, XTC_LOCK_X, 0), ==, XTC_E_AGAIN);

	(void)xtc_lockmgr_id_free(m, l1);
	(void)xtc_lockmgr_id_free(m, l2);
	xtc_lockmgr_destroy(m);
	return MUNIT_OK;
}

/* Two threads contending: one acquires X, the other waits, the
 * first releases, the second proceeds. */
static xtc_lockmgr_t *g_mgr;
static _Atomic int    g_thread_got_lock;

static void *
contender(void *arg)
{
	xtc_locker_t l = *(xtc_locker_t *)arg;
	int rc = xtc_lock_get(g_mgr, l, "k", 1, XTC_LOCK_X, 5LL * 1000 * 1000 * 1000);
	if (rc == XTC_OK) atomic_store(&g_thread_got_lock, 1);
	return NULL;
}

static MunitResult
test_wait_grant(const MunitParameter p[], void *d)
{
	xtc_lockmgr_opts_t o = XTC_LOCKMGR_OPTS_DEFAULT;
	xtc_locker_t l1, l2;
	pthread_t th;
	struct timespec ts = { 0, 50 * 1000 * 1000 };  /* 50ms */
	(void)p; (void)d;
	o.detect_mode = XTC_LOCK_DETECT_NONE;

	munit_assert_int(xtc_lockmgr_create(&o, &g_mgr), ==, XTC_OK);
	munit_assert_int(xtc_lockmgr_id(g_mgr, &l1), ==, XTC_OK);
	munit_assert_int(xtc_lockmgr_id(g_mgr, &l2), ==, XTC_OK);

	munit_assert_int(xtc_lock_get(g_mgr, l1, "k", 1, XTC_LOCK_X, 0), ==, XTC_OK);
	atomic_store(&g_thread_got_lock, 0);

	pthread_create(&th, NULL, contender, &l2);
	(void)nanosleep(&ts, NULL);
	munit_assert_int(atomic_load(&g_thread_got_lock), ==, 0);

	(void)xtc_lock_release_all(g_mgr, l1);
	pthread_join(th, NULL);
	munit_assert_int(atomic_load(&g_thread_got_lock), ==, 1);

	(void)xtc_lockmgr_id_free(g_mgr, l1);
	(void)xtc_lockmgr_id_free(g_mgr, l2);
	xtc_lockmgr_destroy(g_mgr);
	return MUNIT_OK;
}

/* Deadlock: l1 holds A, l2 holds B; l1 waits for B, l2 waits for A.
 * Detector should kill one of them. */
static xtc_lockmgr_t *g_dl_mgr;
static _Atomic int    g_t1_rc;
static _Atomic int    g_t2_rc;

static void *
dl_t1(void *arg)
{
	xtc_locker_t l = *(xtc_locker_t *)arg;
	(void)xtc_lock_get(g_dl_mgr, l, "A", 1, XTC_LOCK_X, 0);
	{ struct timespec ts = { 0, 100 * 1000 * 1000 }; nanosleep(&ts, NULL); }
	atomic_store(&g_t1_rc, xtc_lock_get(g_dl_mgr, l, "B", 1, XTC_LOCK_X,
	    5LL * 1000 * 1000 * 1000));
	return NULL;
}
static void *
dl_t2(void *arg)
{
	xtc_locker_t l = *(xtc_locker_t *)arg;
	(void)xtc_lock_get(g_dl_mgr, l, "B", 1, XTC_LOCK_X, 0);
	{ struct timespec ts = { 0, 100 * 1000 * 1000 }; nanosleep(&ts, NULL); }
	atomic_store(&g_t2_rc, xtc_lock_get(g_dl_mgr, l, "A", 1, XTC_LOCK_X,
	    5LL * 1000 * 1000 * 1000));
	return NULL;
}

static MunitResult
test_deadlock(const MunitParameter p[], void *d)
{
	xtc_lockmgr_opts_t o = XTC_LOCKMGR_OPTS_DEFAULT;
	xtc_locker_t l1, l2;
	pthread_t th1, th2;
	int t1, t2;
	(void)p; (void)d;

	o.detect_mode = XTC_LOCK_DETECT_PERIODIC;
	o.detect_interval_ns = 50LL * 1000 * 1000;     /* 50ms */
	munit_assert_int(xtc_lockmgr_create(&o, &g_dl_mgr), ==, XTC_OK);
	munit_assert_int(xtc_lockmgr_id(g_dl_mgr, &l1), ==, XTC_OK);
	munit_assert_int(xtc_lockmgr_id(g_dl_mgr, &l2), ==, XTC_OK);

	atomic_store(&g_t1_rc, XTC_OK);
	atomic_store(&g_t2_rc, XTC_OK);

	pthread_create(&th1, NULL, dl_t1, &l1);
	pthread_create(&th2, NULL, dl_t2, &l2);
	pthread_join(th1, NULL);
	pthread_join(th2, NULL);

	t1 = atomic_load(&g_t1_rc);
	t2 = atomic_load(&g_t2_rc);

	/* Exactly one of them should be killed by the detector. */
	munit_assert_true(t1 == XTC_E_DEADLK || t2 == XTC_E_DEADLK);
	/* The other got the lock or never tried. */
	munit_assert_true(t1 != XTC_E_AGAIN && t2 != XTC_E_AGAIN);

	(void)xtc_lockmgr_id_free(g_dl_mgr, l1);
	(void)xtc_lockmgr_id_free(g_dl_mgr, l2);
	xtc_lockmgr_destroy(g_dl_mgr);
	return MUNIT_OK;
}

/* M13c-parity: 9-mode matrix.  Verify each new mode at least once. */
static MunitResult
test_9_modes(const MunitParameter p[], void *d)
{
	xtc_lockmgr_t *m;
	xtc_lockmgr_opts_t o = XTC_LOCKMGR_OPTS_DEFAULT;
	xtc_locker_t l1, l2;
	const char *obj = "row";
	(void)p; (void)d;
	o.detect_mode = XTC_LOCK_DETECT_NONE;

	munit_assert_int(xtc_lockmgr_create(&o, &m), ==, XTC_OK);
	munit_assert_int(xtc_lockmgr_id(m, &l1), ==, XTC_OK);
	munit_assert_int(xtc_lockmgr_id(m, &l2), ==, XTC_OK);

	/* IS + IS compatible. */
	munit_assert_int(xtc_lock_get(m, l1, obj, 3, XTC_LOCK_IS, 0), ==, XTC_OK);
	munit_assert_int(xtc_lock_get(m, l2, obj, 3, XTC_LOCK_IS, 0), ==, XTC_OK);
	(void)xtc_lock_release_all(m, l1);
	(void)xtc_lock_release_all(m, l2);

	/* IWR conflicts with S. */
	munit_assert_int(xtc_lock_get(m, l1, obj, 3, XTC_LOCK_S,   0), ==, XTC_OK);
	munit_assert_int(xtc_lock_get(m, l2, obj, 3, XTC_LOCK_IWR, 0), ==, XTC_E_AGAIN);
	(void)xtc_lock_release_all(m, l1);

	/* RU compatible with S. */
	munit_assert_int(xtc_lock_get(m, l1, obj, 3, XTC_LOCK_S,  0), ==, XTC_OK);
	munit_assert_int(xtc_lock_get(m, l2, obj, 3, XTC_LOCK_RU, 0), ==, XTC_OK);
	(void)xtc_lock_release_all(m, l1);
	(void)xtc_lock_release_all(m, l2);

	/* WAIT mode is not get'able. */
	munit_assert_int(xtc_lock_get(m, l1, obj, 3, XTC_LOCK_WAIT, 0), ==, XTC_E_INVAL);

	(void)xtc_lockmgr_id_free(m, l1);
	(void)xtc_lockmgr_id_free(m, l2);
	xtc_lockmgr_destroy(m);
	return MUNIT_OK;
}

/* Custom 3-mode matrix. */
static MunitResult
test_custom_matrix(const MunitParameter p[], void *d)
{
	xtc_lockmgr_t *m;
	xtc_lockmgr_opts_t o = XTC_LOCKMGR_OPTS_DEFAULT;
	xtc_locker_t l1, l2;
	static const uint8_t custom[3 * 3] = {
		0, 0, 0,
		0, 0, 1,
		0, 1, 1
	};
	(void)p; (void)d;
	o.detect_mode = XTC_LOCK_DETECT_NONE;
	o.conflicts = custom;
	o.n_modes   = 3;

	munit_assert_int(xtc_lockmgr_create(&o, &m), ==, XTC_OK);
	munit_assert_int(xtc_lockmgr_id(m, &l1), ==, XTC_OK);
	munit_assert_int(xtc_lockmgr_id(m, &l2), ==, XTC_OK);

	munit_assert_int(xtc_lock_get(m, l1, "k", 1, 1, 0), ==, XTC_OK);
	munit_assert_int(xtc_lock_get(m, l2, "k", 1, 1, 0), ==, XTC_OK);
	munit_assert_int(xtc_lock_get(m, l1, "k", 1, 2, 0), ==, XTC_E_AGAIN);

	(void)xtc_lockmgr_id_free(m, l1);
	(void)xtc_lockmgr_id_free(m, l2);
	xtc_lockmgr_destroy(m);
	return MUNIT_OK;
}

static MunitResult
test_upgrade_downgrade(const MunitParameter p[], void *d)
{
	xtc_lockmgr_t *m;
	xtc_lockmgr_opts_t o = XTC_LOCKMGR_OPTS_DEFAULT;
	xtc_locker_t l1, l2;
	(void)p; (void)d;
	o.detect_mode = XTC_LOCK_DETECT_NONE;

	munit_assert_int(xtc_lockmgr_create(&o, &m), ==, XTC_OK);
	munit_assert_int(xtc_lockmgr_id(m, &l1), ==, XTC_OK);
	munit_assert_int(xtc_lockmgr_id(m, &l2), ==, XTC_OK);

	munit_assert_int(xtc_lock_get(m, l1, "k", 1, XTC_LOCK_S, 0), ==, XTC_OK);
	munit_assert_int(xtc_lock_upgrade(m, l1, "k", 1, XTC_LOCK_X), ==, XTC_OK);
	munit_assert_int(xtc_lock_get(m, l2, "k", 1, XTC_LOCK_S, 0), ==, XTC_E_AGAIN);
	munit_assert_int(xtc_lock_downgrade(m, l1, "k", 1, XTC_LOCK_S), ==, XTC_OK);
	munit_assert_int(xtc_lock_get(m, l2, "k", 1, XTC_LOCK_S, 0), ==, XTC_OK);

	(void)xtc_lockmgr_id_free(m, l1);
	(void)xtc_lockmgr_id_free(m, l2);
	xtc_lockmgr_destroy(m);
	return MUNIT_OK;
}

static MunitResult
test_lock_vec(const MunitParameter p[], void *d)
{
	xtc_lockmgr_t *m;
	xtc_lockmgr_opts_t o = XTC_LOCKMGR_OPTS_DEFAULT;
	xtc_locker_t l1;
	xtc_lock_req_t reqs[3];
	int executed = 0;
	(void)p; (void)d;
	o.detect_mode = XTC_LOCK_DETECT_NONE;

	munit_assert_int(xtc_lockmgr_create(&o, &m), ==, XTC_OK);
	munit_assert_int(xtc_lockmgr_id(m, &l1), ==, XTC_OK);

	memset(reqs, 0, sizeof reqs);
	reqs[0].op = XTC_LOCK_OP_GET; reqs[0].mode = XTC_LOCK_S;
	reqs[0].obj = "a"; reqs[0].obj_size = 1;
	reqs[1].op = XTC_LOCK_OP_GET; reqs[1].mode = XTC_LOCK_S;
	reqs[1].obj = "b"; reqs[1].obj_size = 1;
	reqs[2].op = XTC_LOCK_OP_GET; reqs[2].mode = XTC_LOCK_X;
	reqs[2].obj = "c"; reqs[2].obj_size = 1;
	munit_assert_int(xtc_lock_vec(m, l1, reqs, 3, &executed), ==, XTC_OK);
	munit_assert_int(executed, ==, 3);
	munit_assert_int(xtc_lockmgr_n_held(m), ==, 3);

	memset(reqs, 0, sizeof reqs);
	reqs[0].op = XTC_LOCK_OP_PUT_ALL;
	munit_assert_int(xtc_lock_vec(m, l1, reqs, 1, &executed), ==, XTC_OK);
	munit_assert_int(xtc_lockmgr_n_held(m), ==, 0);

	(void)xtc_lockmgr_id_free(m, l1);
	xtc_lockmgr_destroy(m);
	return MUNIT_OK;
}

static MunitResult
test_stats_failchk(const MunitParameter p[], void *d)
{
	xtc_lockmgr_t *m;
	xtc_lockmgr_opts_t o = XTC_LOCKMGR_OPTS_DEFAULT;
	xtc_lockmgr_stat_t s;
	xtc_locker_t l1, l2;
	(void)p; (void)d;
	o.detect_mode = XTC_LOCK_DETECT_NONE;

	munit_assert_int(xtc_lockmgr_create(&o, &m), ==, XTC_OK);
	munit_assert_int(xtc_lockmgr_id(m, &l1), ==, XTC_OK);
	munit_assert_int(xtc_lockmgr_id(m, &l2), ==, XTC_OK);

	munit_assert_int(xtc_lock_get(m, l1, "a", 1, XTC_LOCK_X, 0), ==, XTC_OK);
	munit_assert_int(xtc_lock_get(m, l1, "b", 1, XTC_LOCK_S, 0), ==, XTC_OK);

	munit_assert_int(xtc_lockmgr_stat(m, &s), ==, XTC_OK);
	munit_assert_int(s.n_held,    ==, 2);
	munit_assert_int(s.n_objects, ==, 2);
	munit_assert_int(s.n_lockers, ==, 2);
	munit_assert_uint64(s.n_acquires, ==, 2);

	munit_assert_int(xtc_lockmgr_failchk(m, l1), ==, XTC_OK);
	munit_assert_int(xtc_lockmgr_stat(m, &s), ==, XTC_OK);
	munit_assert_int(s.n_held, ==, 0);
	munit_assert_int(xtc_lock_get(m, l2, "a", 1, XTC_LOCK_X, 0), ==, XTC_OK);

	(void)xtc_lockmgr_id_free(m, l1);
	(void)xtc_lockmgr_id_free(m, l2);
	xtc_lockmgr_destroy(m);
	return MUNIT_OK;
}

static xtc_lockmgr_t *g_onblk_mgr;
static _Atomic int    g_onblk_t1_rc;
static _Atomic int    g_onblk_t2_rc;

static void *onblk_t1(void *arg) {
	xtc_locker_t l = *(xtc_locker_t *)arg;
	(void)xtc_lock_get(g_onblk_mgr, l, "A", 1, XTC_LOCK_X, 0);
	{ struct timespec ts = { 0, 50 * 1000 * 1000 }; nanosleep(&ts, NULL); }
	atomic_store(&g_onblk_t1_rc,
	    xtc_lock_get(g_onblk_mgr, l, "B", 1, XTC_LOCK_X, -1));
	return NULL;
}
static void *onblk_t2(void *arg) {
	xtc_locker_t l = *(xtc_locker_t *)arg;
	(void)xtc_lock_get(g_onblk_mgr, l, "B", 1, XTC_LOCK_X, 0);
	{ struct timespec ts = { 0, 50 * 1000 * 1000 }; nanosleep(&ts, NULL); }
	atomic_store(&g_onblk_t2_rc,
	    xtc_lock_get(g_onblk_mgr, l, "A", 1, XTC_LOCK_X, -1));
	return NULL;
}

static MunitResult
test_detect_on_block(const MunitParameter p[], void *d)
{
	xtc_lockmgr_opts_t o = XTC_LOCKMGR_OPTS_DEFAULT;
	xtc_locker_t l1, l2;
	pthread_t th1, th2;
	int t1, t2;
	(void)p; (void)d;
	o.detect_mode = XTC_LOCK_DETECT_ON_BLOCK;
	o.victim      = XTC_LOCK_VICTIM_YOUNGEST;

	munit_assert_int(xtc_lockmgr_create(&o, &g_onblk_mgr), ==, XTC_OK);
	munit_assert_int(xtc_lockmgr_id(g_onblk_mgr, &l1), ==, XTC_OK);
	munit_assert_int(xtc_lockmgr_id(g_onblk_mgr, &l2), ==, XTC_OK);

	atomic_store(&g_onblk_t1_rc, XTC_OK);
	atomic_store(&g_onblk_t2_rc, XTC_OK);
	pthread_create(&th1, NULL, onblk_t1, &l1);
	pthread_create(&th2, NULL, onblk_t2, &l2);
	pthread_join(th1, NULL);
	pthread_join(th2, NULL);
	t1 = atomic_load(&g_onblk_t1_rc);
	t2 = atomic_load(&g_onblk_t2_rc);
	munit_assert_true(t1 == XTC_E_DEADLK || t2 == XTC_E_DEADLK);

	(void)xtc_lockmgr_id_free(g_onblk_mgr, l1);
	(void)xtc_lockmgr_id_free(g_onblk_mgr, l2);
	xtc_lockmgr_destroy(g_onblk_mgr);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/modes",              test_modes,              NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/wait_grant",         test_wait_grant,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/deadlock",           test_deadlock,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/9_modes",            test_9_modes,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/custom_matrix",      test_custom_matrix,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/upgrade_downgrade",  test_upgrade_downgrade,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/lock_vec",           test_lock_vec,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/stats_failchk",      test_stats_failchk,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/detect_on_block",    test_detect_on_block,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m13/lockmgr", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
