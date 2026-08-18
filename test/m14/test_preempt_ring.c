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

static MunitTest tests[] = {
	{ "/timeslice", test_ring_timeslice, NULL, NULL,
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
