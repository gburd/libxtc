/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m14/test_preempt_p1.c
 *	Phase 1 of the preemption plan:
 *	cooperative-assisted preemption.  With the per-worker preemption
 *	timer enabled (xtc_exec_set_preempt) and NO manual yield budget, a
 *	long compute fiber that calls xtc_yield_if_due() periodically is
 *	time-sliced by the TIMER (not a manual budget), so co-located
 *	fibers make progress.  This is the safe, no-signal-unwind variant;
 *	true no-cooperation preemption is Phase 2.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_async.h"
#include "xtc_preempt.h"

static atomic_int  g_peer_progress;   /* co-located fibers' work */
static atomic_int  g_compute_done;
static atomic_long g_yields;          /* times the compute fiber yielded */
static volatile uint64_t g_sink;

/* A long compute fiber: burns CPU, calling xtc_yield_if_due() each
 * chunk.  With NO manual budget set, only a preemption-timer tick makes
 * yield_if_due return true -- so every yield here is timer-driven. */
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
	atomic_fetch_add_explicit(&g_compute_done, 1, memory_order_relaxed);
}

/* A peer fiber: does a little work + yields cooperatively, repeatedly,
 * until the compute fiber is done.  It makes progress only if the
 * compute fiber yields the loop to it. */
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
test_p1_timeslice(const MunitParameter p[], void *d)
{
	xtc_exec_t *e = NULL;
	xtc_loop_t *l;
	int i, rc;
	(void)p; (void)d;

	if (!xtc_preempt_supported()) {
		/* No per-thread CPU-time timers: Phase 1 no-ops; skip. */
		return MUNIT_OK;
	}

	atomic_store(&g_peer_progress, 0);
	atomic_store(&g_compute_done, 0);
	atomic_store(&g_yields, 0);

	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);   /* single loop */
	l = xtc_exec_loop(e, 0);

	/* Enable preemption at 1 ms of CPU time per slice.  NO manual
	 * yield budget -- the timer is the only thing that makes
	 * yield_if_due fire. */
	rc = xtc_exec_set_preempt(e, 1 * 1000 * 1000LL);
	munit_assert_int(rc, ==, XTC_OK);

	/* One long compute fiber + several peers, all on the one loop. */
	(void)xtc_proc_spawn(l, compute, (void *)(intptr_t)40, NULL, NULL);
	for (i = 0; i < 4; i++)
		(void)xtc_proc_spawn(l, peer, NULL, NULL, NULL);

	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);

	/* The compute fiber finished, and because the preemption timer
	 * sliced it, the peers made progress meanwhile (each peer
	 * iteration is one yield-round it only gets when compute yields).
	 * Without timer preemption + no manual budget, compute would run
	 * each 3M-iter chunk to completion between yields, but it DOES
	 * yield_if_due every chunk -- with the timer, those yields fire. */
	munit_assert_int(atomic_load(&g_compute_done), ==, 1);
	munit_assert_long(atomic_load(&g_yields), >, 0);        /* timer sliced it */
	munit_assert_int(atomic_load(&g_peer_progress), >, 0);  /* peers ran */

	(void)xtc_exec_fini(e);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/timeslice", test_p1_timeslice, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m14/preempt_p1", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
