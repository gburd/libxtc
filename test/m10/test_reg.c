/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m10/test_reg.c — verifies M10.5 process registry.
 */

#include "munit.h"
#include "xtc.h"
#include "xtc_proc.h"
#include "xtc_reg.h"

static MunitResult
test_reg_basic(const MunitParameter p[], void *d)
{
	xtc_reg_t *r;
	xtc_pid_t pid_a = { 0, 1, 1 }, pid_b = { 0, 2, 1 };
	xtc_pid_t got;
	(void)p; (void)d;
	munit_assert_int(xtc_reg_create(&r), ==, XTC_OK);
	munit_assert_int(xtc_reg_count(r), ==, 0);

	munit_assert_int(xtc_reg_register(r, "alpha", pid_a), ==, XTC_OK);
	munit_assert_int(xtc_reg_register(r, "beta",  pid_b), ==, XTC_OK);
	munit_assert_int(xtc_reg_count(r), ==, 2);

	/* Duplicate name rejected. */
	munit_assert_int(xtc_reg_register(r, "alpha", pid_b), ==, XTC_E_INVAL);

	/* whereis */
	munit_assert_int(xtc_reg_whereis(r, "alpha", &got), ==, XTC_OK);
	munit_assert_true(xtc_pid_eq(got, pid_a));
	munit_assert_int(xtc_reg_whereis(r, "beta", &got), ==, XTC_OK);
	munit_assert_true(xtc_pid_eq(got, pid_b));
	munit_assert_int(xtc_reg_whereis(r, "missing", &got), ==, XTC_E_INVAL);

	/* unregister */
	munit_assert_int(xtc_reg_unregister(r, "alpha"), ==, XTC_OK);
	munit_assert_int(xtc_reg_whereis(r, "alpha", &got), ==, XTC_E_INVAL);
	munit_assert_int(xtc_reg_unregister(r, "alpha"), ==, XTC_E_INVAL);
	munit_assert_int(xtc_reg_count(r), ==, 1);

	/* Re-register under new pid OK. */
	munit_assert_int(xtc_reg_register(r, "alpha", pid_b), ==, XTC_OK);
	munit_assert_int(xtc_reg_whereis(r, "alpha", &got), ==, XTC_OK);
	munit_assert_true(xtc_pid_eq(got, pid_b));

	xtc_reg_destroy(r);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/reg_basic", test_reg_basic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m10.5/reg", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
