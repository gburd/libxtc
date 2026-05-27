/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m0/test_errors.c
 *	Verifies M0_CLAIMS.md [C6]: error contract.
 *	  - XTC_OK == 0.
 *	  - Every XTC_E_* < 0.
 *	  - xtc_strerror returns non-NULL stable text for known codes,
 *	    and "unknown" for codes outside the set (never NULL).
 */

#include "xtc.h"
#include "munit.h"
#include <string.h>
#include <limits.h>

static MunitResult
test_ok_is_zero(const MunitParameter params[], void *data)
{
	(void)params; (void)data;
	munit_assert_int((int)XTC_OK, ==, 0);
	return MUNIT_OK;
}

static MunitResult
test_errors_negative(const MunitParameter params[], void *data)
{
	(void)params; (void)data;
	munit_assert_int(XTC_E_INVAL,    <, 0);
	munit_assert_int(XTC_E_NOMEM,    <, 0);
	munit_assert_int(XTC_E_NOSYS,    <, 0);
	munit_assert_int(XTC_E_RANGE,    <, 0);
	munit_assert_int(XTC_E_AGAIN,    <, 0);
	munit_assert_int(XTC_E_INTERNAL, <, 0);
	munit_assert_int(XTC_E_RESOURCE, <, 0);
	return MUNIT_OK;
}

static MunitResult
test_strerror_known(const MunitParameter params[], void *data)
{
	(void)params; (void)data;
	munit_assert_string_equal(xtc_strerror(XTC_OK),         "ok");
	munit_assert_string_equal(xtc_strerror(XTC_E_INVAL),    "invalid argument");
	munit_assert_string_equal(xtc_strerror(XTC_E_NOMEM),    "out of memory");
	munit_assert_string_equal(xtc_strerror(XTC_E_NOSYS),    "not implemented on this platform");
	munit_assert_string_equal(xtc_strerror(XTC_E_RANGE),    "numeric out of range");
	munit_assert_string_equal(xtc_strerror(XTC_E_AGAIN),    "resource temporarily unavailable");
	munit_assert_string_equal(xtc_strerror(XTC_E_INTERNAL), "internal invariant violation");
	munit_assert_string_equal(xtc_strerror(XTC_E_RESOURCE), "resource cap reached");
	return MUNIT_OK;
}

static MunitResult
test_strerror_unknown_safe(const MunitParameter params[], void *data)
{
	(void)params; (void)data;
	munit_assert_string_equal(xtc_strerror(-9999),    "unknown");
	munit_assert_string_equal(xtc_strerror(INT_MAX),  "unknown");
	munit_assert_not_null(xtc_strerror(INT_MIN));
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/c6_ok_is_zero",          test_ok_is_zero,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/c6_errors_negative",     test_errors_negative,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/c6_strerror_known",      test_strerror_known,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/c6_strerror_unknown",    test_strerror_unknown_safe,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m0/errors", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
