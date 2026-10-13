/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m13/test_rcu.c -- verifies M13a epoch reclamation.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_int.h"
#include "xtc_rcu.h"

/* Counter incremented by the free function. */
static _Atomic int g_freed_count;
static void
my_free(void *p)
{
	atomic_fetch_add_explicit(&g_freed_count, 1, memory_order_relaxed);
	free(p);
}

static MunitResult
test_basic(const MunitParameter p[], void *d)
{
	void *a, *b;
	(void)p; (void)d;
	atomic_store(&g_freed_count, 0);
	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);

	xtc_rcu_read_lock();
	xtc_rcu_read_unlock();

	a = malloc(64);
	b = malloc(64);
	munit_assert_not_null(a);
	munit_assert_not_null(b);

	xtc_rcu_retire(a, my_free);
	xtc_rcu_retire(b, my_free);
	munit_assert_int(atomic_load(&g_freed_count), ==, 0);

	/* Two grace periods to push past the bucket lag. */
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();

	munit_assert_int(atomic_load(&g_freed_count), ==, 2);

	xtc_rcu_fini();
	return MUNIT_OK;
}

/* Reader holding a read-side blocks reclamation until they leave. */
static _Atomic int  g_phase;
static _Atomic int  g_reader_started;

static void *
slow_reader(void *arg)
{
	(void)arg;
	xtc_rcu_read_lock();
	atomic_store(&g_reader_started, 1);
	while (atomic_load(&g_phase) < 1) {
		struct timespec ts = { 0, 1 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}
	xtc_rcu_read_unlock();
	return NULL;
}

static MunitResult
test_blocks_reclaim(const MunitParameter p[], void *d)
{
	pthread_t th;
	void *p1;
	(void)p; (void)d;

	atomic_store(&g_freed_count, 0);
	atomic_store(&g_phase, 0);
	atomic_store(&g_reader_started, 0);
	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);

	pthread_create(&th, NULL, slow_reader, NULL);
	while (!atomic_load(&g_reader_started)) {
		struct timespec ts = { 0, 100 * 1000 };
		nanosleep(&ts, NULL);
	}

	p1 = malloc(64);
	xtc_rcu_retire(p1, my_free);

	/* Synchronise will start a new epoch but block waiting for
	 * the reader to leave the old one.  Run synchronize on a
	 * helper thread to avoid deadlocking the test. */
	{
		pthread_t sync_th;
		struct timespec ts = { 0, 50 * 1000 * 1000 };  /* 50ms */
		pthread_create(&sync_th, NULL,
		    (void *(*)(void *))(uintptr_t)xtc_rcu_synchronize, NULL);

		nanosleep(&ts, NULL);
		/* Reader still alive; nothing reclaimed yet (the bucket
		 * is two epochs old once we eventually advance). */
		atomic_store(&g_phase, 1);
		pthread_join(sync_th, NULL);
		pthread_join(th, NULL);
	}

	/* A couple more synchronizes to flush. */
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();
	munit_assert_int(atomic_load(&g_freed_count), ==, 1);
	xtc_rcu_fini();
	return MUNIT_OK;
}

/*
 * A reader that enters before the first synchronize must still hold off
 * reclamation.  The global epoch used to start at 0 (and xtc_rcu_fini
 * reset it to 0), but 0 is also the "not in a read side" mark, so such a
 * reader published "inactive" and synchronize freed its object under it.
 */
static _Atomic int g_e0_inside;
static _Atomic int g_e0_freed_inside;
static _Atomic int g_e0_syncs;

static void
e0_free(void *p)
{
	if (atomic_load(&g_e0_inside))
		atomic_store(&g_e0_freed_inside, 1);
	atomic_fetch_add_explicit(&g_freed_count, 1, memory_order_relaxed);
	free(p);
}

static void *
e0_reader(void *arg)
{
	(void)arg;
	xtc_rcu_read_lock();
	atomic_store(&g_e0_inside, 1);
	atomic_store(&g_reader_started, 1);
	while (atomic_load(&g_phase) < 1) {
		struct timespec ts = { 0, 1 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}
	atomic_store(&g_e0_inside, 0);
	xtc_rcu_read_unlock();
	return NULL;
}

static void *
e0_writer(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < 3; i++) {
		xtc_rcu_synchronize();
		atomic_fetch_add(&g_e0_syncs, 1);
	}
	return NULL;
}

static MunitResult
test_reader_before_first_sync(const MunitParameter p[], void *d)
{
	pthread_t rd, wr;
	struct timespec ts = { 0, 50 * 1000 * 1000 };   /* 50 ms */
	(void)p; (void)d;

	xtc_rcu_fini();                 /* a fresh epoch, as at process start */
	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);
	atomic_store(&g_freed_count, 0);
	atomic_store(&g_phase, 0);
	atomic_store(&g_reader_started, 0);
	atomic_store(&g_e0_inside, 0);
	atomic_store(&g_e0_freed_inside, 0);
	atomic_store(&g_e0_syncs, 0);

	munit_assert_int(pthread_create(&rd, NULL, e0_reader, NULL), ==, 0);
	while (!atomic_load(&g_reader_started)) {
		struct timespec w = { 0, 100 * 1000 };
		nanosleep(&w, NULL);
	}
	xtc_rcu_retire(malloc(64), e0_free);
	munit_assert_int(pthread_create(&wr, NULL, e0_writer, NULL), ==, 0);
	nanosleep(&ts, NULL);
	/* The reader is still inside: nothing may have been freed, and the
	 * writer's first synchronize must still be waiting for it. */
	munit_assert_int(atomic_load(&g_e0_freed_inside), ==, 0);
	munit_assert_int(atomic_load(&g_freed_count), ==, 0);
	munit_assert_int(atomic_load(&g_e0_syncs), ==, 0);
	atomic_store(&g_phase, 1);
	pthread_join(rd, NULL);
	pthread_join(wr, NULL);
	munit_assert_int(atomic_load(&g_e0_freed_inside), ==, 0);
	munit_assert_int(atomic_load(&g_freed_count), ==, 1);
	munit_assert_uint64(xtc_rcu_current_epoch(), !=, 0);
	xtc_rcu_fini();
	munit_assert_uint64(xtc_rcu_current_epoch(), !=, 0);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/basic",          test_basic,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/blocks_reclaim", test_blocks_reclaim,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reader_before_first_sync", test_reader_before_first_sync, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m13/rcu", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
