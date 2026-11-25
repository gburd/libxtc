/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m13/test_rcu.c -- verifies M13a epoch reclamation.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* pthread_setaffinity_np, cpu_set_t */
#endif
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_int.h"
#include "xtc_rcu.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_loop.h"
#include "xtc_async.h"
#include "xtc_tail.h"
#include "xtc_inject.h"
#include "coro_int.h" /* __xtc_current_task (test-internal) */

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

/*
 * A thread that registered before an xtc_rcu_fini + xtc_rcu_init (taken
 * while it was between read-sides) must still hold off reclamation in a
 * read-side after them.  fini freed the thread's slot and cleared only
 * the calling thread's pointer, so the reader kept writing a freed slot
 * that synchronize no longer scanned, and the object was freed under it.
 */
static _Atomic int g_fi_inside;
static _Atomic int g_fi_freed_inside;

static void
fi_free(void *p)
{
	if (atomic_load_explicit(&g_fi_inside, memory_order_seq_cst))
		atomic_store_explicit(&g_fi_freed_inside, 1,
		    memory_order_seq_cst);
	free(p);
}

static void *
fi_reader(void *arg)
{
	struct timespec ts = { 0, 100 * 1000 };
	(void)arg;
	xtc_rcu_read_lock(); /* register this thread's slot */
	xtc_rcu_read_unlock();
	atomic_store_explicit(&g_phase, 1, memory_order_seq_cst);
	while (atomic_load_explicit(&g_phase, memory_order_seq_cst) < 2)
		nanosleep(&ts, NULL);
	xtc_rcu_read_lock(); /* after fini + init */
	atomic_store_explicit(&g_fi_inside, 1, memory_order_seq_cst);
	atomic_store_explicit(&g_phase, 3, memory_order_seq_cst);
	while (atomic_load_explicit(&g_phase, memory_order_seq_cst) < 4)
		nanosleep(&ts, NULL);
	atomic_store_explicit(&g_fi_inside, 0, memory_order_seq_cst);
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

	atomic_store_explicit(&g_phase, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_fi_inside, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_fi_freed_inside, 0, memory_order_seq_cst);
	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);
	munit_assert_int(pthread_create(&rd, NULL, fi_reader, NULL), ==, 0);
	while (atomic_load_explicit(&g_phase, memory_order_seq_cst) < 1)
		nanosleep(&ts, NULL);
	xtc_rcu_fini();
	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);
	atomic_store_explicit(&g_phase, 2, memory_order_seq_cst);
	while (atomic_load_explicit(&g_phase, memory_order_seq_cst) < 3)
		nanosleep(&ts, NULL);
	xtc_rcu_retire(malloc(64), fi_free);
	munit_assert_int(pthread_create(&wr, NULL, fi_writer, NULL), ==, 0);
	nanosleep(&hold, NULL);
	atomic_store_explicit(&g_phase, 4, memory_order_seq_cst);
	pthread_join(wr, NULL);
	pthread_join(rd, NULL);
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();
	munit_assert_int(atomic_load_explicit(&g_fi_freed_inside,
	                     memory_order_seq_cst),
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
	_Atomic uint64_t sentinel;
	long gen;
};
static _Atomic(struct mig_node *) g_mig_ptr;
static _Atomic int g_mig_bad;
static _Atomic int g_mig_alloc;
static _Atomic int g_mig_freed;
static _Atomic int g_mig_readers_left;

static void
mig_free(void *p)
{
	struct mig_node *n = p;
	atomic_store_explicit(&n->sentinel, 0, memory_order_release);
	atomic_fetch_add_explicit(&g_mig_freed, 1, memory_order_relaxed);
	free(n);
}

static struct mig_node *
mig_new(long gen)
{
	struct mig_node *n = malloc(sizeof *n);
	munit_assert_not_null(n);
	atomic_store_explicit(&n->sentinel, MIG_LIVE, memory_order_relaxed);
	n->gen = gen;
	atomic_fetch_add_explicit(&g_mig_alloc, 1, memory_order_relaxed);
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
		n = atomic_load_explicit(&g_mig_ptr, memory_order_acquire);
		if ((i & 3) == 0)
			xtc_yield(); /* the read-side may resume elsewhere */
		if (atomic_load_explicit(&n->sentinel, memory_order_acquire) !=
		    MIG_LIVE)
			atomic_fetch_add_explicit(&g_mig_bad, 1,
			    memory_order_relaxed);
		xtc_rcu_read_unlock();
	}
	atomic_fetch_sub_explicit(&g_mig_readers_left, 1, memory_order_release);
}

static void
mig_writer(void *arg)
{
	int i;
	(void)arg;
	for (i = 1; i <= MIG_SWAPS; i++) {
		struct mig_node *old = atomic_exchange_explicit(&g_mig_ptr,
		    mig_new(i + 1), memory_order_acq_rel);
		xtc_rcu_retire(old, mig_free);
		xtc_rcu_synchronize();
	}
	while (
	    atomic_load_explicit(&g_mig_readers_left, memory_order_acquire) > 0)
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

	atomic_store_explicit(&g_mig_bad, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_mig_alloc, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_mig_freed, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_mig_readers_left, MIG_READERS,
	    memory_order_seq_cst);
	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);
	atomic_store_explicit(&g_mig_ptr, mig_new(1), memory_order_release);
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

	old = atomic_exchange_explicit(&g_mig_ptr, NULL, memory_order_acq_rel);
	xtc_rcu_retire(old, mig_free);
	for (i = 0; i < 4; i++)
		xtc_rcu_synchronize();
	munit_assert_int(atomic_load_explicit(&g_mig_bad, memory_order_seq_cst),
	    ==, 0);
	munit_assert_int(atomic_load_explicit(&g_mig_freed,
	                     memory_order_seq_cst),
	    ==, atomic_load_explicit(&g_mig_alloc, memory_order_seq_cst));
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
static atomic_int g_oom_inside;
static atomic_int g_oom_freed_inside;
static atomic_int g_oom_freed;
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
	if (atomic_load_explicit(&g_oom_inside, memory_order_seq_cst))
		atomic_store_explicit(&g_oom_freed_inside, 1,
		    memory_order_seq_cst);
	atomic_fetch_add_explicit(&g_oom_freed, 1, memory_order_seq_cst);
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

	atomic_store_explicit(&g_oom_inside, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_oom_freed_inside, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_oom_freed, 0, memory_order_seq_cst);
	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);
	(void)xtc_tail_reset();
	(void)xtc_tail_enable(XTC_TAIL_SCHED);
	leaked0 = __xtc_rcu_leaked();
	munit_assert_int(__os_alloc_get_hook(&saved), ==, XTC_OK);

	xtc_rcu_read_lock();
	atomic_store_explicit(&g_oom_inside, 1, memory_order_seq_cst);
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
	munit_assert_int(atomic_load_explicit(&g_oom_freed,
	                     memory_order_seq_cst),
	    ==, 0);
	atomic_store_explicit(&g_oom_inside, 0, memory_order_seq_cst);
	xtc_rcu_read_unlock();

	for (i = 0; i < 3; i++)
		xtc_rcu_synchronize();
	leaked = __xtc_rcu_leaked() - leaked0;
	munit_assert_int(atomic_load_explicit(&g_oom_freed_inside,
	                     memory_order_seq_cst),
	    ==, 0);
	munit_assert_uint64(leaked, >, 0);
	munit_assert_uint64((uint64_t)atomic_load_explicit(&g_oom_freed,
	                        memory_order_seq_cst) +
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

	atomic_store_explicit(&g_oom_inside, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_oom_freed_inside, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_oom_freed, 0, memory_order_seq_cst);
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
	atomic_store_explicit(&g_oom_inside, 1, memory_order_seq_cst);
	xtc_rcu_read_lock(); /* nested, still without a slot */
	xtc_rcu_read_unlock();

	xtc_rcu_retire(&g_oom_obj[0], oom_free);
	munit_assert_int(pthread_create(&sy, NULL, oom_syncer, NULL), ==, 0);
	nanosleep(&hold, NULL);
	munit_assert_int(atomic_load_explicit(&g_oom_freed,
	                     memory_order_seq_cst),
	    ==, 0);
	atomic_store_explicit(&g_oom_inside, 0, memory_order_seq_cst);
	xtc_rcu_read_unlock();
	pthread_join(sy, NULL);
	munit_assert_int(atomic_load_explicit(&g_oom_freed_inside,
	                     memory_order_seq_cst),
	    ==, 0);
	munit_assert_int(atomic_load_explicit(&g_oom_freed,
	                     memory_order_seq_cst),
	    ==, 1);

	/* With memory back, the next read-side gets a slot. */
	xtc_rcu_read_lock();
	xtc_rcu_retire(&g_oom_obj[1], oom_free);
	munit_assert_int(pthread_create(&sy, NULL, oom_syncer, NULL), ==, 0);
	nanosleep(&hold, NULL);
	munit_assert_int(atomic_load_explicit(&g_oom_freed,
	                     memory_order_seq_cst),
	    ==, 1);
	xtc_rcu_read_unlock();
	pthread_join(sy, NULL);
	munit_assert_int(atomic_load_explicit(&g_oom_freed,
	                     memory_order_seq_cst),
	    ==, 2);
	xtc_rcu_fini();
	return MUNIT_OK;
}

/*
 * Concurrent synchronize (davros report 22).  The epoch advanced with a
 * load then a store, so two callers at once could both store E+1, and
 * a caller preempted between the two moved the epoch backwards.
 */
#define CS_THREADS 8
#define CS_CALLS 20000

static void *
cs_syncer(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < CS_CALLS; i++)
		xtc_rcu_synchronize();
	return NULL;
}

static MunitResult
test_concurrent_synchronize(const MunitParameter p[], void *d)
{
	pthread_t th[CS_THREADS];
	uint64_t e0, e1;
	int i;
	(void)p;
	(void)d;

	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);
	e0 = xtc_rcu_current_epoch();
	for (i = 0; i < CS_THREADS; i++)
		munit_assert_int(pthread_create(&th[i], NULL, cs_syncer, NULL),
		    ==, 0);
	for (i = 0; i < CS_THREADS; i++)
		pthread_join(th[i], NULL);
	e1 = xtc_rcu_current_epoch();
	munit_assert_uint64(e1 - e0, ==, (uint64_t)CS_THREADS * CS_CALLS);
	xtc_rcu_fini();
	return MUNIT_OK;
}

/*
 * Readers and writers on OS threads at once.  Writers swap a published
 * node, retire the old one and synchronize; the free callback poisons
 * the node before it frees it.  A reader rechecks the node it holds
 * across its read-side, so a node freed under it reads as poisoned
 * (and is a heap-use-after-free under ASan).
 */
#define ST_READERS 6
#define ST_WRITERS 4
#define ST_SWAPS 3000
#define ST_LIVE 0x11FE1111FE111111ULL
#define ST_DEAD 0xDEADBEEFDEADBEEFULL

struct st_node {
	_Atomic uint64_t sentinel;
};

static _Atomic(struct st_node *) g_st_ptr;
static atomic_int g_st_bad;
static atomic_int g_st_writers_left;
static atomic_int g_st_alloc;
static atomic_int g_st_freed;

static void
st_free(void *p)
{
	struct st_node *n = p;
	atomic_store_explicit(&n->sentinel, ST_DEAD, memory_order_release);
	atomic_fetch_add_explicit(&g_st_freed, 1, memory_order_relaxed);
	free(n);
}

static struct st_node *
st_new(void)
{
	struct st_node *n = malloc(sizeof *n);
	munit_assert_not_null(n);
	atomic_store_explicit(&n->sentinel, ST_LIVE, memory_order_relaxed);
	atomic_fetch_add_explicit(&g_st_alloc, 1, memory_order_relaxed);
	return n;
}

static void *
st_reader(void *arg)
{
	struct st_node *n;
	int k;
	(void)arg;
	while (atomic_load_explicit(&g_st_writers_left, memory_order_acquire) > 0) {
		xtc_rcu_read_lock();
		n = atomic_load_explicit(&g_st_ptr, memory_order_acquire);
		for (k = 0; k < 64; k++) {
			if (atomic_load_explicit(&n->sentinel, memory_order_acquire) !=
			    ST_LIVE)
				atomic_fetch_add_explicit(&g_st_bad, 1,
				    memory_order_relaxed);
			if ((k & 15) == 0)
				sched_yield();
		}
		xtc_rcu_read_unlock();
	}
	return NULL;
}

static void *
st_writer(void *arg)
{
	struct st_node *old;
	int i;
	(void)arg;
	for (i = 0; i < ST_SWAPS; i++) {
		old = atomic_exchange_explicit(&g_st_ptr, st_new(),
		    memory_order_acq_rel);
		xtc_rcu_retire(old, st_free);
		xtc_rcu_synchronize();
	}
	atomic_fetch_sub_explicit(&g_st_writers_left, 1, memory_order_release);
	return NULL;
}

static MunitResult
test_concurrent_stress(const MunitParameter p[], void *d)
{
	pthread_t rd[ST_READERS], wr[ST_WRITERS];
	int i;
	(void)p;
	(void)d;

	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);
	atomic_store_explicit(&g_st_bad, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_st_alloc, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_st_freed, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_st_writers_left, ST_WRITERS, memory_order_seq_cst);
	atomic_store_explicit(&g_st_ptr, st_new(), memory_order_seq_cst);
	for (i = 0; i < ST_READERS; i++)
		munit_assert_int(pthread_create(&rd[i], NULL, st_reader, NULL),
		    ==, 0);
	for (i = 0; i < ST_WRITERS; i++)
		munit_assert_int(pthread_create(&wr[i], NULL, st_writer, NULL),
		    ==, 0);
	for (i = 0; i < ST_WRITERS; i++)
		pthread_join(wr[i], NULL);
	for (i = 0; i < ST_READERS; i++)
		pthread_join(rd[i], NULL);
	munit_assert_int(atomic_load_explicit(&g_st_bad, memory_order_seq_cst), ==, 0);
	xtc_rcu_retire(atomic_exchange_explicit(&g_st_ptr, NULL, memory_order_acq_rel),
	    st_free);
	for (i = 0; i < 3; i++)
		xtc_rcu_synchronize();
	munit_assert_int(atomic_load_explicit(&g_st_freed, memory_order_seq_cst), ==,
	    atomic_load_explicit(&g_st_alloc, memory_order_seq_cst));
	xtc_rcu_fini();
	return MUNIT_OK;
}

/*
 * The store-buffer case.  Round k: the writer stores g_po_flag = k, then
 * synchronizes, then stores g_po_done = k.  The reader enters a read
 * side and loads g_po_flag.  If it saw an older value it is a reader the
 * writer's grace period must wait for, so it must never see
 * g_po_done == k before it leaves.  Without a full fence between a
 * reader's epoch publish and its loads (and between the writer's advance
 * and its scan), the publish can sit in the reader's store buffer while
 * its load runs, the scan misses it, and synchronize returns under it.
 * With the fence after the publish removed, 10 of 20 runs failed on a
 * 48-core x86-64 host (1 to 13 violations each; the raw litmus failed
 * 1 in 20 to 9 in 10 runs, by core pair and load, and never on two
 * hyperthreads of one core, which is why the test pins the threads to
 * two cores).  So it catches a regression only some of the time; the
 * argument above xtc_rcu_synchronize is the guarantee.
 */
#define PO_ROUNDS 10000000L
static atomic_long g_po_flag, g_po_done, g_po_go, g_po_bad;

static void *
po_reader(void *arg)
{
	long k, f;
	int i;
	(void)arg;
	for (k = 1; k <= PO_ROUNDS; k++) {
		while (atomic_load_explicit(&g_po_go, memory_order_acquire) < k)
			;
		xtc_rcu_read_lock();
		f = atomic_load_explicit(&g_po_flag, memory_order_acquire);
		for (i = 0; f < k && i < 200; i++)
			if (atomic_load_explicit(&g_po_done, memory_order_acquire) >=
			    k) {
				atomic_fetch_add_explicit(&g_po_bad, 1,
				    memory_order_relaxed);
				break;
			}
		xtc_rcu_read_unlock();
	}
	return NULL;
}

/*
 * Put the writer (the calling thread) and the reader on two different
 * cores, never two hyperthreads of one core: siblings see each other's
 * buffered stores sooner and the window never opened between them.  Best
 * effort, Linux only: cpu 0 and the first cpu that is not its sibling.
 */
static void
po_pin_cores(pthread_t reader)
{
#if defined(__linux__)
	FILE *f = fopen("/sys/devices/system/cpu/cpu0/topology/"
	                "thread_siblings_list",
	    "r");
	int a = 0, b = 0, other;
	cpu_set_t set;

	if (f == NULL)
		return;
	if (fscanf(f, "%d%*[,-]%d", &a, &b) < 1)
		b = a;
	fclose(f);
	other = (a == 0 && b == 1) ? 2 : 1;
	if (other == b)
		other++;
	if (sysconf(_SC_NPROCESSORS_ONLN) <= other)
		return;
	CPU_ZERO(&set);
	CPU_SET(0, &set);
	(void)pthread_setaffinity_np(pthread_self(), sizeof set, &set);
	CPU_ZERO(&set);
	CPU_SET(other, &set);
	(void)pthread_setaffinity_np(reader, sizeof set, &set);
#else
	(void)reader;
#endif
}

static MunitResult
test_publish_order(const MunitParameter p[], void *d)
{
	pthread_t th;
	long k;
	(void)p;
	(void)d;

	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);
	atomic_store_explicit(&g_po_flag, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_po_done, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_po_go, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_po_bad, 0, memory_order_seq_cst);
	munit_assert_int(pthread_create(&th, NULL, po_reader, NULL), ==, 0);
	po_pin_cores(th);
	for (k = 1; k <= PO_ROUNDS; k++) {
		atomic_store_explicit(&g_po_go, k, memory_order_release);
		atomic_store_explicit(&g_po_flag, k, memory_order_seq_cst);
		xtc_rcu_synchronize();
		atomic_store_explicit(&g_po_done, k, memory_order_release);
		/* A no-op (this thread wrote g_po_flag), but it changes the
		 * timing: without it the planted reorder was caught in 0 of
		 * 20 runs, with it in 10 of 20. */
		while (atomic_load_explicit(&g_po_flag, memory_order_relaxed) != k)
			;
	}
	pthread_join(th, NULL);
	munit_assert_long(atomic_load_explicit(&g_po_bad, memory_order_seq_cst), ==,
	    0);
	xtc_rcu_fini();
	return MUNIT_OK;
}

/*
 * A fiber that can never run again must not hold a grace period open.
 * Each route leaves a fiber inside a read side for good: it calls
 * xtc_exit_self there, it is killed parked there, its loop is finalized
 * with it parked there, or it exits there and a new fiber reuses its
 * task struct.  A kill or an exit unwinds past read_unlock, and
 * xtc_loop_fini freed a parked fiber without its slot, so every later
 * xtc_rcu_synchronize in the process waited forever.  The synchronize
 * runs on a helper thread with a 2 s deadline, so a hang is a failure
 * rather than a stuck test.  The fini route also checks the case that
 * is NOT released: a loop that has stopped but still exists could run
 * the parked fiber again, so a synchronize waits for it until the loop
 * is finalized.
 *
 * The recycle route checks the struct really is reused (a spawn from
 * the loop's own thread takes it off the owner-only free list), then
 * that the new fiber's read side is visible: a synchronize must wait
 * while it is inside.  A struct that kept the dead fiber's nest > 0
 * would hide the new reader from synchronize: its read_lock would not
 * be the outermost, so it would publish no epoch (a use-after-free).
 */
static xtc_pid_t g_dg_victim;
static atomic_int g_dg_in, g_dg_synced, g_dg_reading, g_dg_release;
static xtc_task_t *g_dg_dead_task, *g_dg_new_task;

static void
dg_exit_inside(void *arg)
{
	(void)arg;
	g_dg_dead_task = __xtc_current_task();
	xtc_rcu_read_lock();
	atomic_store(&g_dg_in, 1);
	(void)xtc_exit_self(0);
}

static void
dg_park_inside(void *arg)
{
	(void)arg;
	xtc_rcu_read_lock();
	atomic_store(&g_dg_in, 1);
	for (;;)
		(void)xtc_proc_sleep(1000LL * 1000 * 1000);
}

static void
dg_killer(void *arg)
{
	(void)arg;
	while (!atomic_load(&g_dg_in))
		xtc_yield();
	(void)xtc_exit_pid(g_dg_victim, 9);
}

static void
dg_stopper(void *arg)
{
	while (!atomic_load(&g_dg_in))
		xtc_yield();
	(void)xtc_loop_stop(arg);
}

/* Inside a read side until the test releases it. */
static void
dg_recycled_reader(void *arg)
{
	(void)arg;
	g_dg_new_task = __xtc_current_task();
	xtc_rcu_read_lock();
	atomic_store(&g_dg_reading, 1);
	while (!atomic_load(&g_dg_release))
		(void)xtc_proc_sleep(1000LL * 1000);
	xtc_rcu_read_unlock();
}

/* On the loop: let a fiber exit inside a read side, then spawn a reader
 * once its struct is back on the free list.  A failed spawn sets
 * g_dg_reading to -1. */
static void
dg_recycle_parent(void *arg)
{
	int i;

	if (xtc_proc_spawn(arg, dg_exit_inside, NULL, NULL, NULL) != XTC_OK) {
		atomic_store(&g_dg_reading, -1);
		return;
	}
	while (!atomic_load(&g_dg_in))
		xtc_yield();
	for (i = 0; i < 10; i++)
		xtc_yield();
	if (xtc_proc_spawn(arg, dg_recycled_reader, NULL, NULL, NULL) !=
	    XTC_OK)
		atomic_store(&g_dg_reading, -1);
}

static void *
dg_loop_runner(void *arg)
{
	(void)xtc_loop_run(arg);
	return NULL;
}

static void *
dg_syncer(void *arg)
{
	(void)arg;
	xtc_rcu_synchronize();
	atomic_store(&g_dg_synced, 1);
	return NULL;
}

/* Start a synchronize on a helper thread. */
static pthread_t
dg_sync_start(void)
{
	pthread_t th;

	atomic_store(&g_dg_synced, 0);
	munit_assert_int(pthread_create(&th, NULL, dg_syncer, NULL), ==, 0);
	return th;
}

/* 1 (and the helper joined) if its synchronize returns within ms. */
static int
dg_sync_wait(pthread_t th, int64_t ms)
{
	int64_t deadline = xtc_clock_mono() + ms * 1000 * 1000;

	while (!atomic_load(&g_dg_synced) && xtc_clock_mono() < deadline)
		sched_yield();
	if (!atomic_load(&g_dg_synced))
		return 0;
	pthread_join(th, NULL);
	return 1;
}

static MunitResult
dg_route(const MunitParameter p[], void *d)
{
	const char *route = munit_parameters_get(p, "route");
	xtc_loop_t *l = NULL;
	pthread_t th, lt;
	int started = 0, hidden;
	(void)d;

	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);
	atomic_store(&g_dg_in, 0);
	atomic_store(&g_dg_reading, 0);
	atomic_store(&g_dg_release, 0);
	munit_assert_int(xtc_loop_init(&l), ==, XTC_OK);
	if (strcmp(route, "exit") == 0) {
		munit_assert_int(xtc_proc_spawn(l, dg_exit_inside, NULL, NULL,
		                     NULL),
		    ==, XTC_OK);
		munit_assert_int(xtc_loop_run(l), ==, XTC_OK);
	} else if (strcmp(route, "recycle") == 0) {
		munit_assert_int(xtc_proc_spawn(l, dg_recycle_parent, l, NULL,
		                     NULL),
		    ==, XTC_OK);
		munit_assert_int(pthread_create(&lt, NULL, dg_loop_runner, l),
		    ==, 0);
		while (!atomic_load(&g_dg_reading))
			sched_yield();
		munit_assert_int(atomic_load(&g_dg_reading), ==, 1);
		munit_assert_ptr_equal(g_dg_new_task, g_dg_dead_task);
		th = dg_sync_start();
		hidden = dg_sync_wait(th, 200);
		atomic_store(&g_dg_release, 1);
		munit_assert_int(pthread_join(lt, NULL), ==, 0);
		if (hidden)
			munit_error("route recycle: synchronize returned while "
			            "the reader on a recycled task was inside");
		started = 1;
	} else if (strcmp(route, "kill") == 0) {
		munit_assert_int(xtc_proc_spawn(l, dg_park_inside, NULL, NULL,
		                     &g_dg_victim),
		    ==, XTC_OK);
		munit_assert_int(xtc_proc_spawn(l, dg_killer, NULL, NULL, NULL),
		    ==, XTC_OK);
		munit_assert_int(xtc_loop_run(l), ==, XTC_OK);
	} else {
		munit_assert_int(xtc_proc_spawn(l, dg_park_inside, NULL, NULL,
		                     NULL),
		    ==, XTC_OK);
		munit_assert_int(xtc_proc_spawn(l, dg_stopper, l, NULL, NULL),
		    ==, XTC_OK);
		munit_assert_int(xtc_loop_run(l), ==, XTC_OK);
		/* Halted, not finalized: the loop could run the fiber again,
		 * so its read side stands.  The caller must end its readers
		 * before it synchronizes. */
		th = dg_sync_start();
		munit_assert_false(dg_sync_wait(th, 200));
		(void)xtc_loop_fini(l);
		l = NULL;
		started = 1;
	}
	munit_assert_true(atomic_load(&g_dg_in));
	if (!started)
		th = dg_sync_start();
	if (!dg_sync_wait(th, 2000)) {
		(void)pthread_detach(th);
		munit_errorf("route %s: a dead fiber's read side held off "
		             "xtc_rcu_synchronize for 2 s",
		    route);
	}
	if (l != NULL)
		(void)xtc_loop_fini(l);
	xtc_rcu_fini();
	return MUNIT_OK;
}

static char *dg_routes[] = { "exit", "kill", "fini", "recycle", NULL };
static MunitParameterEnum dg_params[] = { { "route", dg_routes },
	{ NULL, NULL } };

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
	{ "/concurrent_synchronize", test_concurrent_synchronize, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/concurrent_stress", test_concurrent_stress, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/publish_order", test_publish_order, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/dead_fiber_read_side", dg_route, NULL, NULL, MUNIT_TEST_OPTION_NONE,
	    dg_params },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m13/rcu", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
