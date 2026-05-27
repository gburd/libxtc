/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/m13/test_lwlock.c — verifies M13b lightweight lock.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_int.h"
#include "xtc_lwlock.h"

/* ----- basic acquire/release exclusive ------------------- */

static MunitResult
test_basic_exclusive(const MunitParameter p[], void *d)
{
	xtc_lwlock_t lk;
	(void)p; (void)d;
	munit_assert_int(xtc_lwlock_init(&lk, 1), ==, XTC_OK);
	munit_assert_int(xtc_lwlock_held_by_me(&lk), ==, 0);
	munit_assert_int(xtc_lwlock_acquire(&lk, XTC_LW_EXCLUSIVE), ==, XTC_OK);
	munit_assert_int(xtc_lwlock_held_by_me(&lk), ==, 1);
	munit_assert_int(xtc_lwlock_held_by_me_in_mode(&lk, XTC_LW_EXCLUSIVE),
	    ==, 1);
	munit_assert_int(xtc_lwlock_held_by_me_in_mode(&lk, XTC_LW_SHARED),
	    ==, 0);
	xtc_lwlock_release(&lk);
	munit_assert_int(xtc_lwlock_held_by_me(&lk), ==, 0);
	xtc_lwlock_destroy(&lk);
	return MUNIT_OK;
}

/* ----- basic shared ------------------------------------- */

static MunitResult
test_basic_shared(const MunitParameter p[], void *d)
{
	xtc_lwlock_t lk;
	(void)p; (void)d;
	munit_assert_int(xtc_lwlock_init(&lk, 2), ==, XTC_OK);
	munit_assert_int(xtc_lwlock_acquire(&lk, XTC_LW_SHARED), ==, XTC_OK);
	munit_assert_int(xtc_lwlock_held_by_me_in_mode(&lk, XTC_LW_SHARED),
	    ==, 1);
	/* A second shared acquire should succeed without blocking. */
	munit_assert_int(xtc_lwlock_acquire(&lk, XTC_LW_SHARED), ==, XTC_OK);
	xtc_lwlock_release(&lk);
	xtc_lwlock_release(&lk);
	xtc_lwlock_destroy(&lk);
	return MUNIT_OK;
}

/* ----- conditional acquire -------------------------------- */

static MunitResult
test_conditional(const MunitParameter p[], void *d)
{
	xtc_lwlock_t lk;
	(void)p; (void)d;
	munit_assert_int(xtc_lwlock_init(&lk, 3), ==, XTC_OK);
	/* Free → conditional EXCLUSIVE succeeds. */
	munit_assert_int(xtc_lwlock_acquire_cond(&lk, XTC_LW_EXCLUSIVE),
	    ==, XTC_OK);
	/* Held EXCLUSIVE → another EXCLUSIVE conditional fails. */
	munit_assert_int(xtc_lwlock_acquire_cond(&lk, XTC_LW_EXCLUSIVE),
	    ==, XTC_E_AGAIN);
	/* Held EXCLUSIVE → SHARED conditional fails. */
	munit_assert_int(xtc_lwlock_acquire_cond(&lk, XTC_LW_SHARED),
	    ==, XTC_E_AGAIN);
	xtc_lwlock_release(&lk);
	/* Now SHARED can succeed conditionally. */
	munit_assert_int(xtc_lwlock_acquire_cond(&lk, XTC_LW_SHARED),
	    ==, XTC_OK);
	/* Another shared also succeeds. */
	munit_assert_int(xtc_lwlock_acquire_cond(&lk, XTC_LW_SHARED),
	    ==, XTC_OK);
	/* But EXCLUSIVE conditional fails while shared held. */
	munit_assert_int(xtc_lwlock_acquire_cond(&lk, XTC_LW_EXCLUSIVE),
	    ==, XTC_E_AGAIN);
	xtc_lwlock_release(&lk);
	xtc_lwlock_release(&lk);
	xtc_lwlock_destroy(&lk);
	return MUNIT_OK;
}

/* ----- writer blocks and is woken ---------------------- */

static xtc_lwlock_t g_lk;
static _Atomic int  g_writer_acquired;
static _Atomic int  g_writer_started;

static void *
writer_thread(void *arg)
{
	(void)arg;
	atomic_store(&g_writer_started, 1);
	(void)xtc_lwlock_acquire(&g_lk, XTC_LW_EXCLUSIVE);
	atomic_store(&g_writer_acquired, 1);
	xtc_lwlock_release(&g_lk);
	return NULL;
}

static MunitResult
test_writer_blocks_on_reader(const MunitParameter p[], void *d)
{
	pthread_t th;
	int spins;
	(void)p; (void)d;
	munit_assert_int(xtc_lwlock_init(&g_lk, 4), ==, XTC_OK);
	atomic_store(&g_writer_started, 0);
	atomic_store(&g_writer_acquired, 0);

	munit_assert_int(xtc_lwlock_acquire(&g_lk, XTC_LW_SHARED), ==, XTC_OK);
	pthread_create(&th, NULL, writer_thread, NULL);

	/* Wait for writer to start contesting; then verify it cannot
	 * progress while we hold shared. */
	for (spins = 0; spins < 1000 && !atomic_load(&g_writer_started); spins++) {
		struct timespec ts = { 0, 1 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}
	{ struct timespec ts = { 0, 50 * 1000 * 1000 }; nanosleep(&ts, NULL); }
	munit_assert_int(atomic_load(&g_writer_acquired), ==, 0);

	xtc_lwlock_release(&g_lk);
	pthread_join(th, NULL);
	munit_assert_int(atomic_load(&g_writer_acquired), ==, 1);
	xtc_lwlock_destroy(&g_lk);
	return MUNIT_OK;
}

/* ----- many shared readers concurrent --------------------- */

#define N_SHARED_THREADS 8
#define N_OPS_PER       1000

static xtc_lwlock_t g_lk2;
static _Atomic int  g_shared_max_concurrent;
static _Atomic int  g_shared_in_section;

static void *
shared_thread(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < N_OPS_PER; i++) {
		(void)xtc_lwlock_acquire(&g_lk2, XTC_LW_SHARED);
		{
			int n = atomic_fetch_add(&g_shared_in_section, 1) + 1;
			int cur_max;
			do {
				cur_max = atomic_load(&g_shared_max_concurrent);
				if (n <= cur_max) break;
			} while (!atomic_compare_exchange_weak(
			    &g_shared_max_concurrent, &cur_max, n));
		}
		atomic_fetch_sub(&g_shared_in_section, 1);
		xtc_lwlock_release(&g_lk2);
	}
	return NULL;
}

static MunitResult
test_concurrent_shared(const MunitParameter p[], void *d)
{
	pthread_t th[N_SHARED_THREADS];
	int i;
	(void)p; (void)d;
	munit_assert_int(xtc_lwlock_init(&g_lk2, 5), ==, XTC_OK);
	atomic_store(&g_shared_in_section, 0);
	atomic_store(&g_shared_max_concurrent, 0);
	for (i = 0; i < N_SHARED_THREADS; i++)
		pthread_create(&th[i], NULL, shared_thread, NULL);
	for (i = 0; i < N_SHARED_THREADS; i++) pthread_join(th[i], NULL);
	/* We should observe at least 2 concurrent shared holders during
	 * the run; failing that, our shared path is serialising
	 * unnecessarily.  On a fast CPU we typically see 4–8. */
	munit_assert_int(atomic_load(&g_shared_max_concurrent), >=, 2);
	xtc_lwlock_destroy(&g_lk2);
	return MUNIT_OK;
}

/* ----- writer + readers race condition ------------------ */

static xtc_lwlock_t g_lk3;
static int          g_protected_value;
static _Atomic int  g_failures;
static _Atomic int  g_stop;

static void *
race_writer(void *arg)
{
	(void)arg;
	while (!atomic_load(&g_stop)) {
		(void)xtc_lwlock_acquire(&g_lk3, XTC_LW_EXCLUSIVE);
		g_protected_value = 0xdead;
		g_protected_value = 0xbeef;
		xtc_lwlock_release(&g_lk3);
	}
	return NULL;
}

static void *
race_reader(void *arg)
{
	(void)arg;
	while (!atomic_load(&g_stop)) {
		int v;
		(void)xtc_lwlock_acquire(&g_lk3, XTC_LW_SHARED);
		v = g_protected_value;
		if (v != 0 && v != 0xbeef)
			atomic_fetch_add(&g_failures, 1);
		xtc_lwlock_release(&g_lk3);
	}
	return NULL;
}

static MunitResult
test_writer_reader_race(const MunitParameter p[], void *d)
{
	pthread_t writers[2], readers[4];
	struct timespec ts;
	int i;
	(void)p; (void)d;
	munit_assert_int(xtc_lwlock_init(&g_lk3, 6), ==, XTC_OK);
	g_protected_value = 0;
	atomic_store(&g_stop, 0);
	atomic_store(&g_failures, 0);
	for (i = 0; i < 2; i++) pthread_create(&writers[i], NULL, race_writer, NULL);
	for (i = 0; i < 4; i++) pthread_create(&readers[i], NULL, race_reader, NULL);
	ts.tv_sec = 0; ts.tv_nsec = 200 * 1000 * 1000;
	nanosleep(&ts, NULL);
	atomic_store(&g_stop, 1);
	for (i = 0; i < 2; i++) pthread_join(writers[i], NULL);
	for (i = 0; i < 4; i++) pthread_join(readers[i], NULL);
	munit_assert_int(atomic_load(&g_failures), ==, 0);
	xtc_lwlock_destroy(&g_lk3);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/basic_exclusive",        test_basic_exclusive,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/basic_shared",           test_basic_shared,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/conditional",            test_conditional,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/writer_blocks_on_reader",test_writer_blocks_on_reader,NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/concurrent_shared",      test_concurrent_shared,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/writer_reader_race",     test_writer_reader_race,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m13/lwlock", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
