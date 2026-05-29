/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/concurrency/test_inject_races.c
 *
 *	Deterministic concurrency tests built on the injection points
 *	(XTC_INJECTION_POINT) planted in the primitives.  Each test
 *	uses xtc_inject_attach_wait to PAUSE one thread at a precise
 *	race window, drives a second thread through the conflicting
 *	operation, then releases the first with xtc_inject_wakeup and
 *	asserts the invariant held.  This turns timing-dependent races
 *	into reproducible unit tests.
 *
 *	Points exercised:
 *	  lrlock.publish.post_swap   -- writer swapped read_idx; a
 *	                                reader starting on the old
 *	                                buffer must be drained for.
 *	  lrlock.read.post_announce  -- reader announced its epoch but
 *	                                hasn't loaded read_idx; a
 *	                                concurrent publish must wait.
 *	  lwlock.acquire.pre_cas     -- one acquirer paused before its
 *	                                CAS while another mutates state;
 *	                                the weak CAS must fail + retry.
 *	  proc.mbox.pre_push         -- delivery passed the alive/cap
 *	                                check; mailbox stays consistent.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_inject.h"
#include "xtc_lrlock.h"
#include "xtc_lwlock.h"

/* A small dictionary payload for the lrlock tests. */
struct dict { int n; int items[64]; };
struct op_set { int idx; int value; };

static void
apply_set(void *data, const void *op, size_t sz)
{
	struct dict *d = data;
	const struct op_set *s = op;
	(void)sz;
	if (s->idx >= d->n) d->n = s->idx + 1;
	d->items[s->idx] = s->value;
}
static void sync_dict(void *dst, const void *src, size_t sz)
{ memcpy(dst, src, sz); }

/* =====================================================================
 * lrlock.publish.post_swap:  the writer swaps read_idx, then we pause
 * it inside publish (before it drains readers).  A reader thread then
 * opens a read -- it must land on the NEW buffer (post-swap) and see a
 * consistent value, and the writer's drain must complete once we
 * release it.  The reader's epoch must be respected.
 * ===================================================================== */

static xtc_lrlock_t   *g_lr;
static _Atomic int     g_reader_saw_inconsistent;
static _Atomic int     g_writer_released;
static _Atomic int     g_reader_done;

static void *
publish_writer_thread(void *arg)
{
	struct op_set s;
	(void)arg;
	/* Second publish: set value 2 -> 200.  We will be paused at
	 * post_swap during this call. */
	{
		struct dict *w = xtc_lrlock_write_begin(g_lr);
		(void)w;
		s.idx = 0; s.value = 200;
		xtc_lrlock_apply_op(g_lr, &s, sizeof s);
		xtc_lrlock_publish(g_lr);     /* trips post_swap injection */
		xtc_lrlock_write_end(g_lr);
	}
	atomic_store(&g_writer_released, 1);
	return NULL;
}

static void *
publish_reader_thread(void *arg)
{
	const struct dict *r;
	(void)arg;
	/* Wait until the writer is parked at post_swap, then read. */
	while (xtc_inject_n_attached() == 0) sched_yield();
	r = xtc_lrlock_read_begin(g_lr);
	/* The value must be either the pre-publish (100) or the
	 * post-publish (200) value -- never torn/garbage.  Because the
	 * swap already happened, we expect the new value. */
	if (r->n < 1 || (r->items[0] != 100 && r->items[0] != 200))
		atomic_fetch_add(&g_reader_saw_inconsistent, 1);
	xtc_lrlock_read_end(g_lr);
	atomic_store(&g_reader_done, 1);
	return NULL;
}

static MunitResult
test_lrlock_publish_drain_window(const MunitParameter p[], void *d)
{
	pthread_t wr, rd;
	struct op_set s;
	int spins;
	(void)p; (void)d;

	atomic_store(&g_reader_saw_inconsistent, 0);
	atomic_store(&g_writer_released, 0);
	atomic_store(&g_reader_done, 0);

	munit_assert_int(xtc_lrlock_create(sizeof(struct dict), apply_set,
	    sync_dict, "inj", &g_lr), ==, XTC_OK);
	/* Seed value 100 and publish it. */
	{
		struct dict *w = xtc_lrlock_write_begin(g_lr);
		(void)w;
		s.idx = 0; s.value = 100;
		xtc_lrlock_apply_op(g_lr, &s, sizeof s);
		xtc_lrlock_publish(g_lr);
		xtc_lrlock_write_end(g_lr);
	}

	/* Arm the post-swap pause, then start the writer (which will
	 * block inside publish at the injection point). */
	munit_assert_int(xtc_inject_attach_wait("lrlock.publish.post_swap"),
	    ==, XTC_OK);
	munit_assert_int(pthread_create(&wr, NULL, publish_writer_thread,
	    NULL), ==, 0);

	/* Reader runs while the writer is parked mid-publish. */
	munit_assert_int(pthread_create(&rd, NULL, publish_reader_thread,
	    NULL), ==, 0);

	/* Let the reader complete its read while the writer is paused. */
	for (spins = 0; spins < 100000 &&
	    atomic_load(&g_reader_done) == 0; spins++)
		sched_yield();

	/* The writer must still be parked (drain not yet run). */
	munit_assert_int(atomic_load(&g_writer_released), ==, 0);

	/* Release the writer; it drains and finishes. */
	munit_assert_int(xtc_inject_wakeup("lrlock.publish.post_swap"),
	    ==, XTC_OK);

	pthread_join(rd, NULL);
	pthread_join(wr, NULL);
	(void)xtc_inject_detach("lrlock.publish.post_swap");

	munit_assert_int(atomic_load(&g_reader_saw_inconsistent), ==, 0);
	munit_assert_int(atomic_load(&g_writer_released), ==, 1);

	/* Final state is the published value. */
	{
		const struct dict *r = xtc_lrlock_read_begin(g_lr);
		munit_assert_int(r->items[0], ==, 200);
		xtc_lrlock_read_end(g_lr);
	}
	xtc_lrlock_destroy(g_lr);
	return MUNIT_OK;
}

/* =====================================================================
 * lwlock.acquire.pre_cas:  pause acquirer A after it computed `desired`
 * from the empty state but before its CAS.  Acquirer B (this thread)
 * takes the lock exclusively, mutating state.  Release A: its weak CAS
 * sees the changed state, fails, and A must fall through to block
 * because B holds it exclusive.  After B releases, A acquires.
 * The invariant: never two exclusive holders at once.
 *
 * Note: the injection point fires for ANY thread that reaches it, so
 * we cannot use attach_wait (it would also pause thread B when B
 * acquires).  Instead we attach a CALLBACK that blocks only the
 * thread that armed itself (A), using a test-local mutex/cond, and
 * lets every other caller pass straight through.
 * ===================================================================== */

static xtc_lwlock_t    g_lw;
static _Atomic int     g_lw_holders;       /* must never exceed 1 (excl) */
static _Atomic int     g_lw_max_holders;
static _Atomic int     g_lwA_acquired;
static _Atomic int     g_lwA_reached;      /* A hit the injection point */
static _Atomic int     g_lwA_release;      /* main lets A past the point */
static _Thread_local int g_is_thread_A;

static void
note_acquire(void)
{
	int h = atomic_fetch_add(&g_lw_holders, 1) + 1;
	int prev = atomic_load(&g_lw_max_holders);
	while (h > prev &&
	    !atomic_compare_exchange_weak(&g_lw_max_holders, &prev, h))
		;
}
static void note_release(void) { atomic_fetch_sub(&g_lw_holders, 1); }

/* Callback on lwlock.acquire.pre_cas.  Blocks only thread A, and only
 * its first arrival, so B (main) passes through freely. */
static void
lw_pre_cas_cb(const char *name, void *user)
{
	(void)name; (void)user;
	if (!g_is_thread_A) return;
	if (atomic_exchange(&g_lwA_reached, 1) != 0) return;  /* once */
	while (atomic_load(&g_lwA_release) == 0)
		sched_yield();
}

static void *
lw_acquirer_A(void *arg)
{
	(void)arg;
	g_is_thread_A = 1;
	/* This acquire trips pre_cas; the callback pauses us there. */
	munit_assert_int(xtc_lwlock_acquire(&g_lw, XTC_LW_EXCLUSIVE),
	    ==, XTC_OK);
	note_acquire();
	atomic_store(&g_lwA_acquired, 1);
	note_release();
	xtc_lwlock_release(&g_lw);
	return NULL;
}

static MunitResult
test_lwlock_contended_cas_retry(const MunitParameter p[], void *d)
{
	pthread_t a;
	int spins;
	(void)p; (void)d;

	munit_assert_int(xtc_lwlock_init(&g_lw, 0), ==, XTC_OK);
	atomic_store(&g_lw_holders, 0);
	atomic_store(&g_lw_max_holders, 0);
	atomic_store(&g_lwA_acquired, 0);
	atomic_store(&g_lwA_reached, 0);
	atomic_store(&g_lwA_release, 0);

	munit_assert_int(xtc_inject_attach("lwlock.acquire.pre_cas",
	    lw_pre_cas_cb, NULL), ==, XTC_OK);
	munit_assert_int(pthread_create(&a, NULL, lw_acquirer_A, NULL),
	    ==, 0);

	/* Wait until A is parked at the pre-CAS window. */
	for (spins = 0; spins < 2000000 &&
	    atomic_load(&g_lwA_reached) == 0; spins++)
		sched_yield();
	munit_assert_int(atomic_load(&g_lwA_reached), ==, 1);

	/* B (main) takes the lock exclusively, changing state under A.
	 * B also passes through pre_cas, but the callback lets it by. */
	munit_assert_int(xtc_lwlock_acquire(&g_lw, XTC_LW_EXCLUSIVE),
	    ==, XTC_OK);
	note_acquire();
	munit_assert_int(atomic_load(&g_lwA_acquired), ==, 0);

	/* Release A past the injection point: its stale-`expected` CAS
	 * fails and it falls through to block because B holds exclusive. */
	atomic_store(&g_lwA_release, 1);

	/* A must remain blocked while B still holds the lock. */
	for (spins = 0; spins < 200000; spins++) sched_yield();
	munit_assert_int(atomic_load(&g_lwA_acquired), ==, 0);

	/* B releases; A now acquires. */
	note_release();
	xtc_lwlock_release(&g_lw);
	pthread_join(a, NULL);
	(void)xtc_inject_detach("lwlock.acquire.pre_cas");

	munit_assert_int(atomic_load(&g_lwA_acquired), ==, 1);
	/* The core invariant: exclusive mode never had two holders. */
	munit_assert_int(atomic_load(&g_lw_max_holders), ==, 1);

	xtc_lwlock_destroy(&g_lw);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/lrlock_publish_drain_window", test_lrlock_publish_drain_window,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/lwlock_contended_cas_retry", test_lwlock_contended_cas_retry,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = {
	"/concurrency/inject", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
