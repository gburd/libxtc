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

	/* tick_pending() is test-and-clear (atomic_exchange to 0).  Capture
	 * ONE call's result: a non-zero return means a tick was pending and
	 * this call consumed it.  Do NOT re-call while the timer is still
	 * armed to "prove" it cleared -- under slow emulation (riscv64 under
	 * QEMU) the live timer can post a fresh tick between the two calls,
	 * so a second call is inherently racy and proves nothing.  The clear
	 * is an internal exchange guarantee, not a re-testable observable
	 * against a running timer. */
	(void)xtc_preempt_tick_pending();

	munit_assert_int(xtc_preempt_disarm(), ==, XTC_OK);
	/* After disarm the timer is deleted and pending forced to 0, so
	 * tick_pending() is now deterministically clear. */
	munit_assert_int(xtc_preempt_tick_pending(), ==, 0);

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

/*
 * macOS-only: exercise the kqueue EVFILT_TIMER tick source directly
 * (src/ptc/preempt.c's XTC_HAVE_KQUEUE_TIMER branch).  On every other
 * platform this is a clean no-op MUNIT_SKIP, matching this codebase's
 * existing convention for a platform-gated test (see e.g.
 * test/m18/test_tls_client.c's skip_test when XTC_TLS_ENABLED is
 * absent, or test/m4/test_fctx.c's HAVE_FCTX gate).
 *
 * This test calls ONLY the public xtc_preempt_* API -- it does not
 * reach into preempt.c's static kq_timer_helper/g_kqt internals -- so
 * it validates the seam's OBSERVABLE behavior (arm succeeds, ticks
 * accrue, disarm stops them), exactly like test_arm_ticks above does
 * for the POSIX-timer platforms.  It is written and compiles cleanly
 * under a macOS cross-compile (verified on this Linux host via
 * "zig cc -target arm64-apple-macos-none" / "x86_64-macos-none",
 * syntax/type-check only -- see the implementer's report); it has NOT
 * been RUN on real macOS by this change.  It will run for real the
 * next time the macos-latest CI job (.github/workflows/ci.yml)
 * executes "make tests-c" after this change is pushed.
 */
#if defined(__APPLE__)
static MunitResult
test_macos_kqueue_timer(const MunitParameter p[], void *d)
{
	uint64_t t0, t1, t2;
	int rc;
	(void)p; (void)d;

	/* On macOS xtc_preempt_supported() probes a real kqueue(2), which
	 * should always succeed on a sane host; if it somehow does not,
	 * treat that as the documented "platform lacks the tick source"
	 * case rather than a hard failure -- the same shape test_arm_ticks
	 * uses above for the POSIX-timer platforms. */
	if (!xtc_preempt_supported()) {
		munit_assert_int(xtc_preempt_arm(1 * 1000 * 1000LL), ==,
		    XTC_E_NOSYS);
		return MUNIT_OK;
	}

	rc = xtc_preempt_arm(1 * 1000 * 1000LL);   /* 1 ms interval */
	munit_assert_int(rc, ==, XTC_OK);

	t0 = xtc_preempt_ticks();
	/* The kqueue tick source is WALL-CLOCK (EVFILT_TIMER has no
	 * CPU-time filter), unlike the POSIX per-thread CPU-time timer --
	 * so ticks accrue on elapsed time whether or not this thread is
	 * busy.  A bounded sleep-poll loop (not a CPU burn) is therefore
	 * the correct way to observe it, and cannot hang: xtc_sleep_ns is
	 * the public thread-sleep wrapper (see xtc.h), never a raw
	 * nanosleep. */
	{
		int tries = 0;
		while (xtc_preempt_ticks() == t0 && tries < 200) {
			(void)xtc_sleep_ns(5 * 1000 * 1000LL);   /* 5 ms */
			tries++;
		}
	}
	t1 = xtc_preempt_ticks();
	munit_assert_uint64(t1, >, t0);        /* the kqueue timer fired */

	if (xtc_preempt_tick_pending())
		munit_assert_int(xtc_preempt_tick_pending(), ==, 0);

	munit_assert_int(xtc_preempt_disarm(), ==, XTC_OK);
	t1 = xtc_preempt_ticks();
	(void)xtc_sleep_ns(50 * 1000 * 1000LL);   /* 50 ms: well past disarm */
	t2 = xtc_preempt_ticks();
	munit_assert_uint64(t2, ==, t1);       /* disarm stopped the helper */

	return MUNIT_OK;
}
#else
static MunitResult
test_macos_kqueue_timer(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	/* Not Apple: the kqueue EVFILT_TIMER tick source does not exist on
	 * this platform (preempt.c's XTC_HAVE_KQUEUE_TIMER is Apple-only).
	 * Clean skip, same convention as every other platform-gated test in
	 * this suite. */
	return MUNIT_SKIP;
}
#endif

static MunitTest tests[] = {
	{ "/off_by_default", test_off_by_default, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/arm_ticks", test_arm_ticks, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/rearm", test_rearm, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/macos_kqueue_timer", test_macos_kqueue_timer, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
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
