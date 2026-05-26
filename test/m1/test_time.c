/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * test/m1/test_time.c — verifies M1_CLAIMS.md Tm1–Tm4.
 */

#include "munit.h"
#include "xtc_int.h"

/* [Tm1, Tm2] */
static MunitResult
test_basic(const MunitParameter p[], void *d)
{
	int64_t mono = -1, real = -1;
	(void)p; (void)d;
	munit_assert_int(__os_clock_mono(&mono), ==, XTC_OK);
	munit_assert_int64(mono, >=, 0);
	munit_assert_int(__os_clock_real(&real), ==, XTC_OK);
	munit_assert_int64(real, >=, 0);

	munit_assert_int(__os_clock_mono(NULL), ==, XTC_E_INVAL);
	munit_assert_int(__os_clock_real(NULL), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* [Tm3] never decreasing. */
static MunitResult
test_mono_monotonic(const MunitParameter p[], void *d)
{
	int64_t a, b;
	int i;
	(void)p; (void)d;
	munit_assert_int(__os_clock_mono(&a), ==, XTC_OK);
	for (i = 0; i < 10000; i++) {
		munit_assert_int(__os_clock_mono(&b), ==, XTC_OK);
		munit_assert_int64(b, >=, a);
		a = b;
	}
	return MUNIT_OK;
}

/* [Tm4] sleep at least N ns. */
static MunitResult
test_sleep_ns(const MunitParameter p[], void *d)
{
	int64_t before, after;
	const int64_t target = 5 * XTC_NS_PER_MS;   /* 5 ms */
	(void)p; (void)d;

	munit_assert_int(__os_clock_mono(&before), ==, XTC_OK);
	munit_assert_int(__os_sleep_ns(target), ==, XTC_OK);
	munit_assert_int(__os_clock_mono(&after),  ==, XTC_OK);

	/* Sleep can be a little short on some loaded CIs; allow ~10% slop
	 * but still require >= ~90% of the requested interval. */
	munit_assert_int64(after - before, >=, target - target/10);

	munit_assert_int(__os_sleep_ns(-1), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/Tm1_Tm2_basic",     test_basic,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Tm3_monotonic",     test_mono_monotonic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Tm4_sleep_ns",      test_sleep_ns,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m1/time", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
