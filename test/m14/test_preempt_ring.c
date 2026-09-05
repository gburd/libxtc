/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m14/test_preempt_ring.c
 *	L2 gate: io_uring ring-pointer preemption.  On the io_uring
 *	backend, xtc_exec_set_preempt arms a rearmed TIMEOUT SQE on a
 *	dedicated ring instead of the SIGVTALRM/kqueue CPU-time timer;
 *	xtc_yield_check's "due" test is then two relaxed loads of that
 *	ring's head/tail -- NO signal delivered.
 *
 *	This is the ring-flag counterpart of test_preempt_p1.c (the
 *	signal path): same shape (a long compute fiber time-sliced so
 *	peers make progress), but it ALSO proves the mechanism is the
 *	ring and not the signal -- the compute fiber samples
 *	xtc_preempt_ticks() (the signal handler's own counter, which the
 *	ring path never touches) and asserts it stayed 0 while the loop's
 *	yield-due count climbed.
 *
 *	INSPIRED BY Glommio's need_preempt (reactor.rs / sys/uring.rs
 *	preempt_pointers), adapted to libxtc's single-ring-per-loop
 *	backend via a dedicated preempt ring.  Skips on any non-uring
 *	backend (there is no ring to read; the signal path -- covered by
 *	test_preempt_p1 -- is the trigger there).
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_async.h"
#include "xtc_preempt.h"
#include "xtc_io.h"

static atomic_int  g_peer_progress;
static atomic_int  g_compute_done;
static atomic_long g_yields;
static atomic_ullong g_signal_ticks;   /* xtc_preempt_ticks() on the worker */
static volatile uint64_t g_sink;

/* A long compute fiber: burns CPU, calling xtc_yield_if_due() each
 * chunk.  With NO manual budget set, only a preempt tick (the ring, on
 * this backend) makes yield_if_due return true.  It samples the SIGNAL
 * handler's tick counter (xtc_preempt_ticks) so the test can prove the
 * ring -- not a signal -- did the slicing. */
static void
compute(void *arg)
{
	int chunks = (int)(intptr_t)arg;
	int i;
	for (i = 0; i < chunks; i++) {
		uint64_t acc = 0, k;
		for (k = 0; k < 3ULL * 1000 * 1000; k++)
			acc += (k ^ (k << 1)) + (acc >> 3);
		g_sink += acc;
		if (xtc_yield_if_due())
			atomic_fetch_add_explicit(&g_yields, 1,
			    memory_order_relaxed);
	}
	/* Record the signal handler's tick count observed ON the worker
	 * thread (thread-local): the ring path must not have delivered any
	 * SIGVTALRM, so this must be 0. */
	atomic_store_explicit(&g_signal_ticks, xtc_preempt_ticks(),
	    memory_order_relaxed);
	atomic_fetch_add_explicit(&g_compute_done, 1, memory_order_relaxed);
}

static void
peer(void *arg)
{
	(void)arg;
	while (!atomic_load_explicit(&g_compute_done, memory_order_relaxed)) {
		atomic_fetch_add_explicit(&g_peer_progress, 1,
		    memory_order_relaxed);
		xtc_yield();
	}
}

static MunitResult
test_ring_timeslice(const MunitParameter p[], void *d)
{
	xtc_exec_t *e = NULL;
	xtc_loop_t *l;
	int i, rc;
	(void)p; (void)d;

	/* Ring preempt is io_uring-only; the signal path (test_preempt_p1)
	 * covers every other backend.  Skip cleanly elsewhere. */
	if (strcmp(xtc_io_backend_name(), "uring") != 0)
		return MUNIT_OK;

	atomic_store(&g_peer_progress, 0);
	atomic_store(&g_compute_done, 0);
	atomic_store(&g_yields, 0);
	atomic_store(&g_signal_ticks, 0);

	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);   /* single loop */
	l = xtc_exec_loop(e, 0);

	/* Enable preemption at 1 ms per slice.  On uring this arms the
	 * ring TIMEOUT SQE (NOT the SIGVTALRM timer). */
	rc = xtc_exec_set_preempt(e, 1 * 1000 * 1000LL);
	munit_assert_int(rc, ==, XTC_OK);

	(void)xtc_proc_spawn(l, compute, (void *)(intptr_t)40, NULL, NULL);
	for (i = 0; i < 4; i++)
		(void)xtc_proc_spawn(l, peer, NULL, NULL, NULL);

	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);

	/* The ring sliced the compute fiber: it yielded, and the peers
	 * made progress meanwhile. */
	munit_assert_int(atomic_load(&g_compute_done), ==, 1);
	munit_assert_long(atomic_load(&g_yields), >, 0);
	munit_assert_int(atomic_load(&g_peer_progress), >, 0);
	/* And it did so with NO signal delivered: the SIGVTALRM handler's
	 * tick counter on the worker thread never advanced. */
	munit_assert_ullong(atomic_load(&g_signal_ticks), ==, 0);
	/* The loop's yield-due telemetry climbed (the ring source fed it). */
	munit_assert_uint64(xtc_yield_due_count(l), >, 0);

	(void)xtc_exec_fini(e);
	return MUNIT_OK;
}


/* ---- cross-loop: a MIGRATED fiber's yield-check must not touch another
 *      loop's preempt ring ------------------------------------------------
 *
 * xtc_yield_check consults the io_uring preempt ring, and that call is NOT
 * a pure read: on a due tick it does io_uring_cq_advance (moving the CQ
 * head) and re-arms a TIMEOUT SQE.  It used to key on t->loop -- the
 * fiber's HOME loop -- so a work-stolen fiber calling xtc_yield_if_due()
 * mutated a PEER loop's preempt ring (its single-producer SQ and CQ head)
 * from the wrong thread, concurrently with that loop's own worker doing
 * the same.  Fixed by keying on the RUNNING loop (__xtc_current_loop),
 * whose ring the calling thread owns by construction.
 *
 * test_ring_timeslice above cannot reach that: it uses xtc_exec_init(&e, 1)
 * -- a SINGLE loop -- so the home and running loop are always the same.
 * This case is the multi-loop counterpart: MIGRATABLE compute fibers all
 * spawned on loop 0 of an 8-loop executor with eager rebalance, so peers
 * steal them and they then run with t->loop != __xtc_current_loop while
 * hammering xtc_yield_if_due().
 *
 * HONESTY ABOUT WHAT THIS ASSERTS: the deterministic part is only that the
 * run completes, every fiber finishes, and the ring still slices
 * (yields > 0) while fibers migrate -- no hang, no lost work.  A cross-loop
 * ring mutation is a DATA RACE, so the assertion that actually catches it
 * is ThreadSanitizer (and ASan for resulting memory damage).  The value of
 * this case is that it makes the path REACHABLE for the sanitizer jobs,
 * which no prior test did: under plain make check it is a smoke test,
 * under the sanitizers it is the real guard.
 */
#define XL_FIBERS 16
static _Atomic int  g_xl_done;
static _Atomic long g_xl_yields;

static void
xl_compute(void *arg)
{
	int chunks = (int)(intptr_t)arg;
	int i;
	for (i = 0; i < chunks; i++) {
		uint64_t acc = 0, k;
		/* 3M iterations is ~3 ms on this class of machine, so a 1 ms
		 * slice reliably expires INSIDE the chunk -- which is what
		 * makes the following yield-check actually consult the preempt
		 * ring.  (1M measured ~1.1 ms: too close to the slice, and the
		 * check was never reached, making the test vacuous.) */
		for (k = 0; k < 3ULL * 1000 * 1000; k++)
			acc += (k ^ (k << 1)) + (acc >> 3);
		g_sink += acc;
		/* THE path under test: reached while this fiber may be running
		 * stolen on a loop other than the one it was spawned on. */
		if (xtc_yield_if_due())
			atomic_fetch_add_explicit(&g_xl_yields, 1,
			    memory_order_relaxed);
	}
	atomic_fetch_add_explicit(&g_xl_done, 1, memory_order_relaxed);
}

static MunitResult
test_ring_crossloop(const MunitParameter p[], void *d)
{
	xtc_exec_t *e = NULL;
	xtc_proc_opts_t opts;
	int i, rc;
	(void)p; (void)d;

	if (strcmp(xtc_io_backend_name(), "uring") != 0)
		return MUNIT_OK;   /* ring preempt is io_uring-only */

	atomic_store(&g_xl_done, 0);
	atomic_store(&g_xl_yields, 0);

	munit_assert_int(xtc_exec_init(&e, 8), ==, XTC_OK);
	/* Eager rebalance so idle peers actively steal, maximizing the number
	 * of fibers running with t->loop != __xtc_current_loop. */
	xtc_exec_set_eager_rebalance(e, 1);
	rc = xtc_exec_set_preempt(e, 1 * 1000 * 1000LL);   /* 1 ms slices */
	munit_assert_int(rc, ==, XTC_OK);

	/* All on loop 0 and MIGRATABLE: the other seven loops must steal
	 * them, which is what puts a fiber on a non-home loop. */
	for (i = 0; i < XL_FIBERS; i++) {
		memset(&opts, 0, sizeof opts);
		opts.name = "xl";
		opts.migratable = 1;
		munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 0), xl_compute,
		    (void *)(intptr_t)8, &opts, NULL), ==, XTC_OK);
	}

	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);

	/* No hang, no lost work: every fiber ran to completion... */
	munit_assert_int(atomic_load(&g_xl_done), ==, XL_FIBERS);
	/* ...and the ring still sliced them while they migrated. */
	munit_assert_long(atomic_load(&g_xl_yields), >, 0);

	(void)xtc_exec_fini(e);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/timeslice", test_ring_timeslice, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/crossloop", test_ring_crossloop, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m14/preempt_ring", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
