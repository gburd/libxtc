/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m13/test_rcu.c -- verifies M13a epoch reclamation.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_int.h"
#include "xtc_rcu.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_async.h"

/* Counter incremented by the free function. */
static int g_freed_count;
static void
my_free(void *p)
{
	xtc_atomic_fetch_add_i32(&g_freed_count, 1, xtc_mo_relaxed);
	free(p);
}

static MunitResult
test_basic(const MunitParameter p[], void *d)
{
	void *a, *b;
	(void)p; (void)d;
	xtc_atomic_store_i32(&g_freed_count, 0, xtc_mo_seq_cst);
	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);

	xtc_rcu_read_lock();
	xtc_rcu_read_unlock();

	a = malloc(64);
	b = malloc(64);
	munit_assert_not_null(a);
	munit_assert_not_null(b);

	xtc_rcu_retire(a, my_free);
	xtc_rcu_retire(b, my_free);
	munit_assert_int(xtc_atomic_load_i32(&g_freed_count, xtc_mo_seq_cst),
	    ==, 0);

	/* Two grace periods to push past the bucket lag. */
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();

	munit_assert_int(xtc_atomic_load_i32(&g_freed_count, xtc_mo_seq_cst),
	    ==, 2);

	xtc_rcu_fini();
	return MUNIT_OK;
}

/* Reader holding a read-side blocks reclamation until they leave. */
static int g_phase;
static int g_reader_started;

static void *
slow_reader(void *arg)
{
	(void)arg;
	xtc_rcu_read_lock();
	xtc_atomic_store_i32(&g_reader_started, 1, xtc_mo_seq_cst);
	while (xtc_atomic_load_i32(&g_phase, xtc_mo_seq_cst) < 1) {
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

	xtc_atomic_store_i32(&g_freed_count, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_phase, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_reader_started, 0, xtc_mo_seq_cst);
	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);

	pthread_create(&th, NULL, slow_reader, NULL);
	while (!xtc_atomic_load_i32(&g_reader_started, xtc_mo_seq_cst)) {
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
		xtc_atomic_store_i32(&g_phase, 1, xtc_mo_seq_cst);
		pthread_join(sync_th, NULL);
		pthread_join(th, NULL);
	}

	/* A couple more synchronizes to flush. */
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();
	munit_assert_int(xtc_atomic_load_i32(&g_freed_count, xtc_mo_seq_cst),
	    ==, 1);
	xtc_rcu_fini();
	return MUNIT_OK;
}

/*
 * A reader that enters before the first synchronize must still hold off
 * reclamation.  The global epoch used to start at 0 (and xtc_rcu_fini
 * reset it to 0), but 0 is also the "not in a read side" mark, so such a
 * reader published "inactive" and synchronize freed its object under it.
 */
static int g_e0_inside;
static int g_e0_freed_inside;
static int g_e0_syncs;

static void
e0_free(void *p)
{
	if (xtc_atomic_load_i32(&g_e0_inside, xtc_mo_seq_cst))
		xtc_atomic_store_i32(&g_e0_freed_inside, 1, xtc_mo_seq_cst);
	xtc_atomic_fetch_add_i32(&g_freed_count, 1, xtc_mo_relaxed);
	free(p);
}

static void *
e0_reader(void *arg)
{
	(void)arg;
	xtc_rcu_read_lock();
	xtc_atomic_store_i32(&g_e0_inside, 1, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_reader_started, 1, xtc_mo_seq_cst);
	while (xtc_atomic_load_i32(&g_phase, xtc_mo_seq_cst) < 1) {
		struct timespec ts = { 0, 1 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}
	xtc_atomic_store_i32(&g_e0_inside, 0, xtc_mo_seq_cst);
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
		xtc_atomic_fetch_add_i32(&g_e0_syncs, 1, xtc_mo_seq_cst);
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
	xtc_atomic_store_i32(&g_freed_count, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_phase, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_reader_started, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_e0_inside, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_e0_freed_inside, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_e0_syncs, 0, xtc_mo_seq_cst);

	munit_assert_int(pthread_create(&rd, NULL, e0_reader, NULL), ==, 0);
	while (!xtc_atomic_load_i32(&g_reader_started, xtc_mo_seq_cst)) {
		struct timespec w = { 0, 100 * 1000 };
		nanosleep(&w, NULL);
	}
	xtc_rcu_retire(malloc(64), e0_free);
	munit_assert_int(pthread_create(&wr, NULL, e0_writer, NULL), ==, 0);
	nanosleep(&ts, NULL);
	/* The reader is still inside: nothing may have been freed, and the
	 * writer's first synchronize must still be waiting for it. */
	munit_assert_int(xtc_atomic_load_i32(&g_e0_freed_inside,
	                     xtc_mo_seq_cst),
	    ==, 0);
	munit_assert_int(xtc_atomic_load_i32(&g_freed_count, xtc_mo_seq_cst),
	    ==, 0);
	munit_assert_int(xtc_atomic_load_i32(&g_e0_syncs, xtc_mo_seq_cst), ==,
	    0);
	xtc_atomic_store_i32(&g_phase, 1, xtc_mo_seq_cst);
	pthread_join(rd, NULL);
	pthread_join(wr, NULL);
	munit_assert_int(xtc_atomic_load_i32(&g_e0_freed_inside,
	                     xtc_mo_seq_cst),
	    ==, 0);
	munit_assert_int(xtc_atomic_load_i32(&g_freed_count, xtc_mo_seq_cst),
	    ==, 1);
	munit_assert_uint64(xtc_rcu_current_epoch(), !=, 0);
	xtc_rcu_fini();
	munit_assert_uint64(xtc_rcu_current_epoch(), !=, 0);
	return MUNIT_OK;
}

/*
 * Migratable reader fibers on a 4-loop executor hold read-sides across
 * yields (so a read-side can end on another loop's thread) while a
 * writer fiber swaps, retires and synchronizes.  A reader must never see
 * a freed node, and every node must be reclaimed.  The reader slot of a
 * fiber is per fiber, not per thread: a per-thread slot would be
 * incremented on one thread and decremented on another.
 */
#define MIG_LOOPS 4
#define MIG_READERS 8
#define MIG_ITERS 2000
#define MIG_SWAPS 400
#define MIG_LIVE 0x11FE11FE11FE11FEULL

struct mig_node {
	uint64_t sentinel;
	long gen;
};
static struct mig_node *g_mig_ptr;
static int g_mig_bad;
static int g_mig_alloc;
static int g_mig_freed;
static int g_mig_readers_left;

static void
mig_free(void *p)
{
	struct mig_node *n = p;
	xtc_atomic_store_u64(&n->sentinel, 0, xtc_mo_release);
	xtc_atomic_fetch_add_i32(&g_mig_freed, 1, xtc_mo_relaxed);
	free(n);
}

static struct mig_node *
mig_new(long gen)
{
	struct mig_node *n = malloc(sizeof *n);
	munit_assert_not_null(n);
	xtc_atomic_store_u64(&n->sentinel, MIG_LIVE, xtc_mo_relaxed);
	n->gen = gen;
	xtc_atomic_fetch_add_i32(&g_mig_alloc, 1, xtc_mo_relaxed);
	return n;
}

static void
mig_reader(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < MIG_ITERS; i++) {
		struct mig_node *n;
		xtc_rcu_read_lock();
		n = xtc_atomic_load_ptr(&g_mig_ptr, xtc_mo_acquire);
		if ((i & 3) == 0)
			xtc_yield(); /* the read-side may resume elsewhere */
		if (xtc_atomic_load_u64(&n->sentinel, xtc_mo_acquire) !=
		    MIG_LIVE)
			xtc_atomic_fetch_add_i32(&g_mig_bad, 1, xtc_mo_relaxed);
		xtc_rcu_read_unlock();
	}
	xtc_atomic_fetch_sub_i32(&g_mig_readers_left, 1, xtc_mo_release);
}

static void
mig_writer(void *arg)
{
	int i;
	(void)arg;
	for (i = 1; i <= MIG_SWAPS; i++) {
		struct mig_node *old = xtc_atomic_exchange_ptr(&g_mig_ptr,
		    mig_new(i + 1), xtc_mo_acq_rel);
		xtc_rcu_retire(old, mig_free);
		xtc_rcu_synchronize();
	}
	while (xtc_atomic_load_i32(&g_mig_readers_left, xtc_mo_acquire) > 0)
		xtc_yield();
}

static MunitResult
test_fiber_readers_migrate(const MunitParameter p[], void *d)
{
	xtc_exec_t *ex = NULL;
	xtc_proc_opts_t o = XTC_PROC_OPTS_INIT;
	struct mig_node *old;
	int i;
	(void)p;
	(void)d;

	xtc_atomic_store_i32(&g_mig_bad, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_mig_alloc, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_mig_freed, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_mig_readers_left, MIG_READERS, xtc_mo_seq_cst);
	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);
	xtc_atomic_store_ptr(&g_mig_ptr, mig_new(1), xtc_mo_release);
	munit_assert_int(xtc_exec_init(&ex, MIG_LOOPS), ==, XTC_OK);
	o.migratable = 1;
	for (i = 0; i < MIG_READERS; i++) {
		xtc_loop_t *l = xtc_exec_loop(ex, (unsigned)(i % MIG_LOOPS));
		munit_assert_int(xtc_proc_spawn(l, mig_reader, NULL, &o, NULL),
		    ==, XTC_OK);
	}
	munit_assert_int(xtc_proc_spawn(xtc_exec_loop(ex, 0), mig_writer, NULL,
	                     NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_exec_run(ex), ==, XTC_OK);
	munit_assert_int(xtc_exec_fini(ex), ==, XTC_OK);

	old = xtc_atomic_exchange_ptr(&g_mig_ptr, NULL, xtc_mo_acq_rel);
	xtc_rcu_retire(old, mig_free);
	for (i = 0; i < 4; i++)
		xtc_rcu_synchronize();
	munit_assert_int(xtc_atomic_load_i32(&g_mig_bad, xtc_mo_seq_cst), ==,
	    0);
	munit_assert_int(xtc_atomic_load_i32(&g_mig_freed, xtc_mo_seq_cst), ==,
	    xtc_atomic_load_i32(&g_mig_alloc, xtc_mo_seq_cst));
	xtc_rcu_fini();
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/basic",          test_basic,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/blocks_reclaim", test_blocks_reclaim,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reader_before_first_sync", test_reader_before_first_sync, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fiber_readers_migrate", test_fiber_readers_migrate, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m13/rcu", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
