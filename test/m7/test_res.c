/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m7/test_res.c -- xtc_res resource accountant error/edge branches.
 *
 *	The accountant is exercised indirectly by chan/slab/proc, but its
 *	NULL-arg guards, bad-kind rejections, cap boundaries, underflow
 *	clamp, and the alert re-arm cycle had gaps.  This closes them
 *	directly: cheap, deterministic, no loop needed.
 */

#include <stdint.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_res.h"

/* An out-of-range kind used to hit the bad-kind guards. */
#define BAD_KIND ((xtc_res_kind_t)999)

static MunitResult
test_init(const MunitParameter p[], void *d)
{
	xtc_res_t r;
	xtc_res_caps_t caps = XTC_RES_CAPS_DEFAULT;
	(void)p; (void)d;

	/* NULL accountant rejected. */
	munit_assert_int(xtc_res_init(NULL, &caps), ==, XTC_E_INVAL);

	/* caps == NULL uses the container-aware defaults. */
	munit_assert_int(xtc_res_init(&r, NULL), ==, XTC_OK);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_TASKS), ==, 0);

	/* Explicit caps struct is taken verbatim. */
	caps.tasks = 10;
	munit_assert_int(xtc_res_init(&r, &caps), ==, XTC_OK);
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_TASKS, 10), ==, XTC_OK);
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_TASKS, 1), ==,
	    XTC_E_RESOURCE);
	return MUNIT_OK;
}

static MunitResult
test_acquire_release(const MunitParameter p[], void *d)
{
	xtc_res_t r;
	xtc_res_caps_t caps = XTC_RES_CAPS_DEFAULT;
	(void)p; (void)d;
	caps.channels = 4;
	munit_assert_int(xtc_res_init(&r, &caps), ==, XTC_OK);

	/* acquire error branches: NULL, negative n, bad kind. */
	munit_assert_int(xtc_res_acquire(NULL, XTC_RES_CHANNELS, 1), ==,
	    XTC_E_INVAL);
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_CHANNELS, -1), ==,
	    XTC_E_INVAL);
	munit_assert_int(xtc_res_acquire(&r, BAD_KIND, 1), ==, XTC_E_INVAL);

	/* n == 0 is allowed and charges nothing. */
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_CHANNELS, 0), ==, XTC_OK);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_CHANNELS), ==, 0);

	/* Charge to the cap, then reject beyond, bumping the reject count. */
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_CHANNELS, 4), ==, XTC_OK);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_CHANNELS), ==, 4);
	munit_assert_int64(xtc_res_high(&r, XTC_RES_CHANNELS), ==, 4);
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_CHANNELS, 1), ==,
	    XTC_E_RESOURCE);
	munit_assert_int64(xtc_res_rejects(&r, XTC_RES_CHANNELS), ==, 1);

	/* release error branches: NULL, n <= 0, bad kind (all no-op). */
	xtc_res_release(NULL, XTC_RES_CHANNELS, 1);
	xtc_res_release(&r, XTC_RES_CHANNELS, 0);
	xtc_res_release(&r, XTC_RES_CHANNELS, -5);
	xtc_res_release(&r, BAD_KIND, 1);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_CHANNELS), ==, 4);

	/* Normal release. */
	xtc_res_release(&r, XTC_RES_CHANNELS, 2);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_CHANNELS), ==, 2);
	/* High-water mark stays at the peak. */
	munit_assert_int64(xtc_res_high(&r, XTC_RES_CHANNELS), ==, 4);

	/* Over-release underflows -> clamped to zero, never negative. */
	xtc_res_release(&r, XTC_RES_CHANNELS, 100);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_CHANNELS), ==, 0);
	return MUNIT_OK;
}

static MunitResult
test_query_guards(const MunitParameter p[], void *d)
{
	xtc_res_t r;
	xtc_res_caps_t caps = XTC_RES_CAPS_DEFAULT;
	(void)p; (void)d;
	munit_assert_int(xtc_res_init(&r, &caps), ==, XTC_OK);

	/* All three queries return 0 on NULL / bad kind. */
	munit_assert_int64(xtc_res_used(NULL, XTC_RES_TASKS), ==, 0);
	munit_assert_int64(xtc_res_used(&r, BAD_KIND), ==, 0);
	munit_assert_int64(xtc_res_high(NULL, XTC_RES_TASKS), ==, 0);
	munit_assert_int64(xtc_res_high(&r, BAD_KIND), ==, 0);
	munit_assert_int64(xtc_res_rejects(NULL, XTC_RES_TASKS), ==, 0);
	munit_assert_int64(xtc_res_rejects(&r, BAD_KIND), ==, 0);
	return MUNIT_OK;
}

static MunitResult
test_set_cap(const MunitParameter p[], void *d)
{
	xtc_res_t r;
	xtc_res_caps_t caps = XTC_RES_CAPS_DEFAULT;
	xtc_res_kind_t k;
	(void)p; (void)d;
	munit_assert_int(xtc_res_init(&r, &caps), ==, XTC_OK);

	/* NULL / bad kind are no-ops. */
	xtc_res_set_cap(NULL, XTC_RES_TASKS, 1);
	xtc_res_set_cap(&r, BAD_KIND, 1);

	/* Set a cap on every real kind, then verify it bounds acquire. */
	for (k = XTC_RES_TASKS; (int)k < XTC_RES__COUNT; k++) {
		xtc_res_set_cap(&r, k, 2);
		munit_assert_int(xtc_res_acquire(&r, k, 2), ==, XTC_OK);
		munit_assert_int(xtc_res_acquire(&r, k, 1), ==, XTC_E_RESOURCE);
		/* Raise the cap: the same acquire now succeeds. */
		xtc_res_set_cap(&r, k, 10);
		munit_assert_int(xtc_res_acquire(&r, k, 1), ==, XTC_OK);
	}
	return MUNIT_OK;
}

static int g_fires;
static void
res_alert(xtc_res_kind_t k, int64_t used, int64_t cap, void *user)
{
	(void)k; (void)used; (void)cap; (void)user;
	g_fires++;
}

static MunitResult
test_alert(const MunitParameter p[], void *d)
{
	xtc_res_t r;
	xtc_res_caps_t caps = XTC_RES_CAPS_DEFAULT;
	(void)p; (void)d;
	caps.tasks = 100;
	munit_assert_int(xtc_res_init(&r, &caps), ==, XTC_OK);

	/* set_alert guards: NULL, bad kind, out-of-range pct. */
	munit_assert_int(xtc_res_set_alert(NULL, XTC_RES_TASKS, 0.5), ==,
	    XTC_E_INVAL);
	munit_assert_int(xtc_res_set_alert(&r, BAD_KIND, 0.5), ==,
	    XTC_E_INVAL);
	munit_assert_int(xtc_res_set_alert(&r, XTC_RES_TASKS, -0.1), ==,
	    XTC_E_INVAL);
	munit_assert_int(xtc_res_set_alert(&r, XTC_RES_TASKS, 1.5), ==,
	    XTC_E_INVAL);
	munit_assert_int(xtc_res_set_alert_fn(NULL, res_alert, NULL), ==,
	    XTC_E_INVAL);

	/* Arm at 80%, install the callback. */
	munit_assert_int(xtc_res_set_alert(&r, XTC_RES_TASKS, 0.8), ==, XTC_OK);
	munit_assert_int(xtc_res_set_alert_fn(&r, res_alert, NULL), ==, XTC_OK);
	g_fires = 0;

	/* Below threshold: no fire.  Cross it: fires once, then latches. */
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_TASKS, 70), ==, XTC_OK);
	munit_assert_int(g_fires, ==, 0);
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_TASKS, 15), ==, XTC_OK);
	munit_assert_int(g_fires, ==, 1);
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_TASKS, 5), ==, XTC_OK);
	munit_assert_int(g_fires, ==, 1);   /* still latched */

	/* Drop below threshold (re-arm), then cross again -> second fire. */
	xtc_res_release(&r, XTC_RES_TASKS, 40);
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_TASKS, 40), ==, XTC_OK);
	munit_assert_int(g_fires, ==, 2);

	/* Disable the callback: no further fires even across the threshold. */
	munit_assert_int(xtc_res_set_alert_fn(&r, NULL, NULL), ==, XTC_OK);
	xtc_res_release(&r, XTC_RES_TASKS, 50);
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_TASKS, 50), ==, XTC_OK);
	munit_assert_int(g_fires, ==, 2);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/init",            test_init,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/acquire_release", test_acquire_release, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/query_guards",    test_query_guards,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/set_cap",         test_set_cap,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/alert",           test_alert,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m7/res", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
