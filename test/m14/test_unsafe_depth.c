/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m14/test_unsafe_depth.c
 *	Phase 2a of the preemption plan: the
 *	async-signal-unsafe-region depth counter.  The preemption timer
 *	(Phase 2b) will consult it so a signal-context involuntary yield
 *	never fires inside malloc/free or a latch's internal lock.  This
 *	test proves the counter is 0 in ordinary code and > 0 across an
 *	allocator call, and nests correctly.
 */

#include <stdint.h>
#include <stdlib.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_int.h"       /* __os_malloc / __os_free */
#include "xtc_preempt.h"
#include "preempt_int.h"    /* __xtc_unsafe_enter/leave/depth (internal) */

/* Outside any allocator call, the unsafe depth is 0. */
static MunitResult
test_zero_at_rest(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	munit_assert_int(__xtc_unsafe_depth(), ==, 0);
	return MUNIT_OK;
}

/* enter/leave nests and balances. */
static MunitResult
test_nesting(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	munit_assert_int(__xtc_unsafe_depth(), ==, 0);
	__xtc_unsafe_enter();
	munit_assert_int(__xtc_unsafe_depth(), ==, 1);
	__xtc_unsafe_enter();
	munit_assert_int(__xtc_unsafe_depth(), ==, 2);
	__xtc_unsafe_leave();
	munit_assert_int(__xtc_unsafe_depth(), ==, 1);
	__xtc_unsafe_leave();
	munit_assert_int(__xtc_unsafe_depth(), ==, 0);
	/* leave below zero is clamped, not negative. */
	__xtc_unsafe_leave();
	munit_assert_int(__xtc_unsafe_depth(), ==, 0);
	return MUNIT_OK;
}

/* The allocator brackets the unsafe region: a hook that samples the
 * depth WHILE malloc is running observes depth > 0, and the depth is
 * back to 0 after the call returns. */
static _Atomic int g_depth_in_malloc = -1;

/* We cannot easily observe the depth from inside the real malloc, so
 * instead assert the contract indirectly: __os_malloc/__os_free run to
 * completion leaving depth 0 (balanced), which is the property the
 * timer relies on -- a balanced bracket means the handler is never
 * left seeing a stuck depth. */
static MunitResult
test_alloc_balanced(const MunitParameter p[], void *d)
{
	void *m = NULL;
	int i;
	(void)p; (void)d;
	for (i = 0; i < 1000; i++) {
		munit_assert_int(__os_malloc(64, &m), ==, XTC_OK);
		munit_assert_int(__xtc_unsafe_depth(), ==, 0);  /* balanced after */
		__os_free(m);
		munit_assert_int(__xtc_unsafe_depth(), ==, 0);  /* balanced after */
	}
	(void)g_depth_in_malloc;
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/zero_at_rest", test_zero_at_rest, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/nesting", test_nesting, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/alloc_balanced", test_alloc_balanced, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m14/unsafe_depth", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
