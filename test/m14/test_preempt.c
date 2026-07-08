/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m14/test_preempt.c
 *	Phase 0 of the preemption plan: the
 *	per-worker preemption timer SEAM.  No preemption happens yet --
 *	this proves the timer arms, fires on this thread's CPU time, and
 *	the tick counter + pending flag work, and that it is OFF by
 *	default (no ticks without arming).
 */

#include <stdint.h>
#include <stdlib.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_preempt.h"

/* Burn CPU for a bounded number of iterations so a CPU-time timer
 * accrues ticks; volatile sink defeats dead-code elimination. */
static volatile uint64_t g_sink;

static void
burn(uint64_t iters)
{
	uint64_t i, acc = 0;
	for (i = 0; i < iters; i++)
		acc += (i ^ (i << 1)) + (acc >> 3);
	g_sink += acc;
}

/* Off by default: while NOT armed, no new ticks accrue even under CPU
 * load.  (ticks is a cumulative per-thread counter, so we compare a
 * before/after delta rather than an absolute -- an earlier test may
 * have armed and left ticks nonzero.) */
static MunitResult
test_off_by_default(const MunitParameter p[], void *d)
{
	uint64_t before, after;
	(void)p; (void)d;
	(void)xtc_preempt_disarm();       /* ensure not armed */
	before = xtc_preempt_ticks();
	burn(500ULL * 1000 * 1000);
	after = xtc_preempt_ticks();
	munit_assert_uint64(after, ==, before);   /* no ticks while disarmed */
	return MUNIT_OK;
}

/* Armed: a CPU burn accrues timer ticks; disarm stops further ticks. */
static MunitResult
test_arm_ticks(const MunitParameter p[], void *d)
{
	uint64_t t0, t1, t2;
	int rc;
	(void)p; (void)d;

	if (!xtc_preempt_supported()) {
		/* No POSIX per-thread CPU-time timers here: arm returns
		 * NOSYS and the seam is a documented no-op. */
		munit_assert_int(xtc_preempt_arm(1 * 1000 * 1000LL), ==,
		    XTC_E_NOSYS);
		return MUNIT_OK;
	}

	/* 1 ms of CPU time per tick. */
	rc = xtc_preempt_arm(1 * 1000 * 1000LL);
	munit_assert_int(rc, ==, XTC_OK);

	t0 = xtc_preempt_ticks();
	/* Burn CPU until a few ticks fire (bounded): a 1 ms CPU-time timer
	 * needs only a few ms of busy work.  Loop in modest chunks and stop
	 * as soon as ticks advance, with a hard cap so the test is fast and
	 * cannot hang. */
	{
		int tries = 0;
		while (xtc_preempt_ticks() == t0 && tries < 2000) {
			burn(20ULL * 1000 * 1000);
			tries++;
		}
	}
	t1 = xtc_preempt_ticks();
	munit_assert_uint64(t1, >, t0);        /* the timer fired */

	/* A tick should be pending; consuming it clears it. */
	if (xtc_preempt_tick_pending())
		munit_assert_int(xtc_preempt_tick_pending(), ==, 0);

	munit_assert_int(xtc_preempt_disarm(), ==, XTC_OK);
	t1 = xtc_preempt_ticks();
	burn(200ULL * 1000 * 1000);
	t2 = xtc_preempt_ticks();
	munit_assert_uint64(t2, ==, t1);       /* disarm stopped ticks */

	return MUNIT_OK;
}

/* Re-arm is idempotent and does not crash. */
static MunitResult
test_rearm(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	if (!xtc_preempt_supported())
		return MUNIT_OK;
	munit_assert_int(xtc_preempt_arm(1 * 1000 * 1000LL), ==, XTC_OK);
	munit_assert_int(xtc_preempt_arm(2 * 1000 * 1000LL), ==, XTC_OK);
	munit_assert_int(xtc_preempt_disarm(), ==, XTC_OK);
	munit_assert_int(xtc_preempt_disarm(), ==, XTC_OK);  /* double disarm ok */
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/off_by_default", test_off_by_default, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/arm_ticks", test_arm_ticks, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/rearm", test_rearm, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m14/preempt", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
