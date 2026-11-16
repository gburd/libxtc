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
#include "xtc_tail.h"
#include "xtc_inject.h"

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
 * A thread that registered before an xtc_rcu_fini + xtc_rcu_init (taken
 * while it was between read-sides) must still hold off reclamation in a
 * read-side after them.  fini freed the thread's slot and cleared only
 * the calling thread's pointer, so the reader kept writing a freed slot
 * that synchronize no longer scanned, and the object was freed under it.
 */
static int g_fi_inside;
static int g_fi_freed_inside;

static void
fi_free(void *p)
{
	if (xtc_atomic_load_i32(&g_fi_inside, xtc_mo_seq_cst))
		xtc_atomic_store_i32(&g_fi_freed_inside, 1, xtc_mo_seq_cst);
	free(p);
}

static void *
fi_reader(void *arg)
{
	struct timespec ts = { 0, 100 * 1000 };
	(void)arg;
	xtc_rcu_read_lock(); /* register this thread's slot */
	xtc_rcu_read_unlock();
	xtc_atomic_store_i32(&g_phase, 1, xtc_mo_seq_cst);
	while (xtc_atomic_load_i32(&g_phase, xtc_mo_seq_cst) < 2)
		nanosleep(&ts, NULL);
	xtc_rcu_read_lock(); /* after fini + init */
	xtc_atomic_store_i32(&g_fi_inside, 1, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_phase, 3, xtc_mo_seq_cst);
	while (xtc_atomic_load_i32(&g_phase, xtc_mo_seq_cst) < 4)
		nanosleep(&ts, NULL);
	xtc_atomic_store_i32(&g_fi_inside, 0, xtc_mo_seq_cst);
	xtc_rcu_read_unlock();
	return NULL;
}

static void *
fi_writer(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < 3; i++)
		xtc_rcu_synchronize();
	return NULL;
}

static MunitResult
test_thread_reader_after_fini(const MunitParameter p[], void *d)
{
	struct timespec ts = { 0, 100 * 1000 };
	struct timespec hold = { 0, 50 * 1000 * 1000 }; /* 50 ms */
	pthread_t rd, wr;
	(void)p;
	(void)d;

	xtc_atomic_store_i32(&g_phase, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_fi_inside, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_fi_freed_inside, 0, xtc_mo_seq_cst);
	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);
	munit_assert_int(pthread_create(&rd, NULL, fi_reader, NULL), ==, 0);
	while (xtc_atomic_load_i32(&g_phase, xtc_mo_seq_cst) < 1)
		nanosleep(&ts, NULL);
	xtc_rcu_fini();
	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);
	xtc_atomic_store_i32(&g_phase, 2, xtc_mo_seq_cst);
	while (xtc_atomic_load_i32(&g_phase, xtc_mo_seq_cst) < 3)
		nanosleep(&ts, NULL);
	xtc_rcu_retire(malloc(64), fi_free);
	munit_assert_int(pthread_create(&wr, NULL, fi_writer, NULL), ==, 0);
	nanosleep(&hold, NULL);
	xtc_atomic_store_i32(&g_phase, 4, xtc_mo_seq_cst);
	pthread_join(wr, NULL);
	pthread_join(rd, NULL);
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();
	munit_assert_int(xtc_atomic_load_i32(&g_fi_freed_inside,
	                     xtc_mo_seq_cst),
	    ==, 0);
	xtc_rcu_fini();
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

/*
 * Out of memory, xtc_rcu must never free under a reader.  A retirement
 * it cannot record in its slab goes on the heap; one it cannot record
 * at all is leaked and counted (it used to be freed at once, under the
 * readers).  A reader that cannot get a slot is still waited for (it
 * used to be invisible to synchronize).  The allocator hook fails
 * calloc (the slab cannot grow: a new chunk's header is a calloc) and
 * then malloc (no heap either), and the slabs are exhausted first.
 */
#define OOM_N 3000
static int g_oom_inside;
static int g_oom_freed_inside;
static int g_oom_freed;
static char g_oom_obj[2 * OOM_N];

static void *
oom_fail_calloc(size_t n, size_t sz)
{
	(void)n;
	(void)sz;
	return NULL;
}
static void *
oom_fail_malloc(size_t sz)
{
	(void)sz;
	return NULL;
}
static void *
oom_fail_aligned(size_t a, size_t sz)
{
	(void)a;
	(void)sz;
	return NULL;
}

static void
oom_free(void *p)
{
	(void)p;
	if (xtc_atomic_load_i32(&g_oom_inside, xtc_mo_seq_cst))
		xtc_atomic_store_i32(&g_oom_freed_inside, 1, xtc_mo_seq_cst);
	xtc_atomic_fetch_add_i32(&g_oom_freed, 1, xtc_mo_seq_cst);
}

static int
oom_count_drops(const xtc_tail_rec_t *rec, void *user)
{
	if (rec->kind == XTC_TAIL_LIFECYCLE_DROP)
		++*(int *)user;
	return 0;
}

extern uint64_t __xtc_rcu_leaked(void);

static MunitResult
test_retire_oom(const MunitParameter p[], void *d)
{
	struct __os_alloc_hook saved, failing;
	uint64_t leaked0, leaked;
	int i, drops = 0;
	(void)p;
	(void)d;

	xtc_atomic_store_i32(&g_oom_inside, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_oom_freed_inside, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_oom_freed, 0, xtc_mo_seq_cst);
	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);
	(void)xtc_tail_reset();
	(void)xtc_tail_enable(XTC_TAIL_SCHED);
	leaked0 = __xtc_rcu_leaked();
	munit_assert_int(__os_alloc_get_hook(&saved), ==, XTC_OK);

	xtc_rcu_read_lock();
	xtc_atomic_store_i32(&g_oom_inside, 1, xtc_mo_seq_cst);
	failing = saved;
	failing.calloc = oom_fail_calloc;
	munit_assert_int(__os_alloc_set_hook(&failing), ==, XTC_OK);
	for (i = 0; i < OOM_N; i++) /* exhaust the slab: heap records */
		xtc_rcu_retire(&g_oom_obj[i], oom_free);
	failing.malloc = oom_fail_malloc;
	munit_assert_int(__os_alloc_set_hook(&failing), ==, XTC_OK);
	for (i = OOM_N; i < 2 * OOM_N; i++) /* no record at all: leaked */
		xtc_rcu_retire(&g_oom_obj[i], oom_free);
	munit_assert_int(__os_alloc_set_hook(&saved), ==, XTC_OK);
	munit_assert_int(xtc_atomic_load_i32(&g_oom_freed, xtc_mo_seq_cst), ==,
	    0);
	xtc_atomic_store_i32(&g_oom_inside, 0, xtc_mo_seq_cst);
	xtc_rcu_read_unlock();

	for (i = 0; i < 3; i++)
		xtc_rcu_synchronize();
	leaked = __xtc_rcu_leaked() - leaked0;
	munit_assert_int(xtc_atomic_load_i32(&g_oom_freed_inside,
	                     xtc_mo_seq_cst),
	    ==, 0);
	munit_assert_uint64(leaked, >, 0);
	munit_assert_uint64((uint64_t)xtc_atomic_load_i32(&g_oom_freed,
	                        xtc_mo_seq_cst) +
	        leaked,
	    ==, 2 * OOM_N);
	(void)xtc_tail_read(oom_count_drops, &drops);
	munit_assert_uint64((uint64_t)drops, ==, leaked);
	xtc_tail_disable();
	xtc_rcu_fini();
	return MUNIT_OK;
}

static void
oom_inject(const char *name, void *user)
{
	(void)name;
	(void)user;
}

static void *
oom_syncer(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < 3; i++)
		xtc_rcu_synchronize();
	return NULL;
}

static MunitResult
test_reader_oom(const MunitParameter p[], void *d)
{
	struct __os_alloc_hook saved, failing;
	struct timespec hold = { 0, 50 * 1000 * 1000 }; /* 50 ms */
	pthread_t sy;
	(void)p;
	(void)d;

	xtc_atomic_store_i32(&g_oom_inside, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_oom_freed_inside, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_oom_freed, 0, xtc_mo_seq_cst);
	munit_assert_int(__os_alloc_get_hook(&saved), ==, XTC_OK);
	failing = saved;
	failing.calloc = oom_fail_calloc;
	failing.malloc = oom_fail_malloc;
	failing.aligned = oom_fail_aligned;

	/* This thread's first read-side, with no memory for its slot: the
	 * allocator fails (the test runs in a fresh process, so the slot
	 * slab is not there yet either), and the injection point makes it
	 * fail without a fork too. */
	munit_assert_int(xtc_inject_attach("rcu.slot_oom", oom_inject, NULL),
	    ==, XTC_OK);
	munit_assert_int(__os_alloc_set_hook(&failing), ==, XTC_OK);
	xtc_rcu_read_lock();
	munit_assert_int(__os_alloc_set_hook(&saved), ==, XTC_OK);
	munit_assert_int(xtc_inject_detach("rcu.slot_oom"), ==, XTC_OK);
	xtc_atomic_store_i32(&g_oom_inside, 1, xtc_mo_seq_cst);
	xtc_rcu_read_lock(); /* nested, still without a slot */
	xtc_rcu_read_unlock();

	xtc_rcu_retire(&g_oom_obj[0], oom_free);
	munit_assert_int(pthread_create(&sy, NULL, oom_syncer, NULL), ==, 0);
	nanosleep(&hold, NULL);
	munit_assert_int(xtc_atomic_load_i32(&g_oom_freed, xtc_mo_seq_cst), ==,
	    0);
	xtc_atomic_store_i32(&g_oom_inside, 0, xtc_mo_seq_cst);
	xtc_rcu_read_unlock();
	pthread_join(sy, NULL);
	munit_assert_int(xtc_atomic_load_i32(&g_oom_freed_inside,
	                     xtc_mo_seq_cst),
	    ==, 0);
	munit_assert_int(xtc_atomic_load_i32(&g_oom_freed, xtc_mo_seq_cst), ==,
	    1);

	/* With memory back, the next read-side gets a slot. */
	xtc_rcu_read_lock();
	xtc_rcu_retire(&g_oom_obj[1], oom_free);
	munit_assert_int(pthread_create(&sy, NULL, oom_syncer, NULL), ==, 0);
	nanosleep(&hold, NULL);
	munit_assert_int(xtc_atomic_load_i32(&g_oom_freed, xtc_mo_seq_cst), ==,
	    1);
	xtc_rcu_read_unlock();
	pthread_join(sy, NULL);
	munit_assert_int(xtc_atomic_load_i32(&g_oom_freed, xtc_mo_seq_cst), ==,
	    2);
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
	{ "/thread_reader_after_fini", test_thread_reader_after_fini, NULL,
	    NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/retire_oom", test_retire_oom, NULL, NULL, MUNIT_TEST_OPTION_NONE,
	    NULL },
	{ "/reader_oom", test_reader_oom, NULL, NULL, MUNIT_TEST_OPTION_NONE,
	    NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m13/rcu", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
