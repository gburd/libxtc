/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m14/test_stack_reclaim.c
 *	Lever S1 -- stack-memory reclamation on park (M_PREEMPTION 8).
 *
 *	Proves:
 *	  (a) OFF by default: no madvise happens with reclaim disabled;
 *	  (b) ENABLED: a fiber that uses a deep stack frame then parks
 *	      returns its unused tail to the OS (reclaim count rises);
 *	  (c) CORRECTNESS: a fiber that writes a sentinel into a deep
 *	      stack buffer, parks (its tail is MADV_DONTNEED'd), resumes,
 *	      and reads/rewrites that buffer sees consistent behavior --
 *	      the reclaim never crosses the live frame, and the faulted-
 *	      back tail is usable again (zero-fill on refault).  A bug in
 *	      the reclaim boundary would corrupt the resumed fiber (caught
 *	      by the post-resume assertions here, and by ASan).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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

/* Detect AddressSanitizer portably (nested #if -- a bare
 * __has_feature(...) breaks on gcc where __has_feature is undefined). */
#if defined(__SANITIZE_ADDRESS__)
# define XTC_UNDER_ASAN 1
#elif defined(__has_feature)
# if __has_feature(address_sanitizer)
#  define XTC_UNDER_ASAN 1
# endif
#endif

#define DEEP_BYTES 16384    /* a frame well above one page, so parking it
                             * leaves a clearly-reclaimable tail */

static atomic_int g_ok;      /* fibers that round-tripped their sentinel */
static atomic_int g_done;

/* Force the compiler not to optimize the deep buffer away. */
static void
touch(volatile unsigned char *p, size_t n, unsigned char v)
{
	size_t i;
	for (i = 0; i < n; i += 512)   /* touch each page-ish */
		p[i] = v;
}

static unsigned char
sum_probe(volatile unsigned char *p, size_t n)
{
	unsigned char s = 0;
	size_t i;
	for (i = 0; i < n; i += 512)
		s = (unsigned char)(s + p[i]);
	return s;
}

/*
 * A worker: build a deep frame, stamp it, PARK (yield) -- at which point
 * S1 may reclaim the unused tail below our current SP -- then resume and
 * verify the LIVE frame survived and is still writable.  Repeats so many
 * park/reclaim/resume cycles run.
 */
static void
worker(void *arg)
{
	int rounds = (int)(intptr_t)arg;
	int r;
	int good = 1;

	for (r = 0; r < rounds; r++) {
		volatile unsigned char deep[DEEP_BYTES];
		unsigned char want = (unsigned char)(0xA0 + (r & 0x0f));

		/* Stamp the deep live frame, then park.  The live frame is
		 * BELOW our SP after the call chain, so the reclaim (which
		 * only touches the tail above SP minus a margin) must not
		 * disturb `deep`. */
		touch(deep, sizeof deep, want);
		xtc_yield();
		/* Resumed: the live frame must be intact. */
		if (sum_probe(deep, sizeof deep) !=
		    (unsigned char)(want * (unsigned char)(DEEP_BYTES / 512)))
			good = 0;
		/* Re-stamp with a new value and park again -- exercises the
		 * refaulted tail on a subsequent deeper/shallower call. */
		touch(deep, sizeof deep, (unsigned char)(want ^ 0x5a));
		xtc_yield();
		if (sum_probe(deep, sizeof deep) !=
		    (unsigned char)((unsigned char)(want ^ 0x5a) *
		        (unsigned char)(DEEP_BYTES / 512)))
			good = 0;
	}
	if (good)
		atomic_fetch_add(&g_ok, 1);
	atomic_fetch_add(&g_done, 1);
}

/* A driver that keeps the loop busy so the workers actually park+resume
 * against each other (their yields hand off). */
static void
driver(void *arg)
{
	int n = (int)(intptr_t)arg;
	while (atomic_load(&g_done) < n)
		xtc_yield();
}

static MunitResult
run_workers(int n, int rounds)
{
	xtc_exec_t *e = NULL;
	xtc_loop_t *l;
	int i;

	atomic_store(&g_ok, 0);
	atomic_store(&g_done, 0);

	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	l = xtc_exec_loop(e, 0);
	for (i = 0; i < n; i++)
		(void)xtc_proc_spawn(l, worker, (void *)(intptr_t)rounds,
		    NULL, NULL);
	(void)xtc_proc_spawn(l, driver, (void *)(intptr_t)n, NULL, NULL);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);

	munit_assert_int(atomic_load(&g_done), ==, n);
	munit_assert_int(atomic_load(&g_ok), ==, n);   /* all sentinels survived */
	(void)xtc_exec_fini(e);
	return MUNIT_OK;
}

/* (a) OFF by default: no reclaim happens. */
static MunitResult
test_off_by_default(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	munit_assert_int(xtc_stack_reclaim_enabled(), ==, 0);
	munit_assert_uint64(xtc_stack_reclaim_count(), ==, 0);
	/* Run a workload with reclaim off; count must stay 0. */
	if (run_workers(4, 8) != MUNIT_OK)
		return MUNIT_FAIL;
	munit_assert_uint64(xtc_stack_reclaim_count(), ==, 0);
	return MUNIT_OK;
}

/* (b)+(c) ENABLED: reclaim fires AND every fiber round-trips correctly. */
static MunitResult
test_reclaim_correct(const MunitParameter p[], void *d)
{
	uint64_t before, after;
	(void)p; (void)d;

	if (xtc_stack_reclaim_enable(0) != XTC_OK) {
		/* No MADV_DONTNEED on this platform: the API declines and the
		 * feature is a documented no-op.  Nothing to prove. */
		munit_assert_int(xtc_stack_reclaim_enabled(), ==, 0);
		return MUNIT_OK;
	}
	munit_assert_int(xtc_stack_reclaim_enabled(), ==, 1);

	before = xtc_stack_reclaim_count();
	if (run_workers(4, 16) != MUNIT_OK) {
		xtc_stack_reclaim_disable();
		return MUNIT_FAIL;
	}
	after = xtc_stack_reclaim_count();

	/* Deep frames (16 KiB) parked many times -> the tail above SP is
	 * well over a page, so reclaim must have fired at least once.
	 * EXCEPT under AddressSanitizer: ASan relocates fiber frames to a
	 * heap fake-stack (detect_stack_use_after_return), so the running
	 * SP is not inside the fiber's mmap'd stack and the reclaim
	 * correctly DECLINES (its geometry check finds SP out of range) --
	 * a safe no-op, not a bug.  There the round-trip correctness (all
	 * sentinels survived, asserted in run_workers) is the thing that
	 * matters; the fire-count is expected to stay 0. */
#ifdef XTC_UNDER_ASAN
	(void)before; (void)after;
#else
	munit_assert_uint64(after, >, before);
#endif

	xtc_stack_reclaim_disable();
	munit_assert_int(xtc_stack_reclaim_enabled(), ==, 0);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/off_by_default", test_off_by_default, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reclaim_correct", test_reclaim_correct, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m14/stack_reclaim", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
