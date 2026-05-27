/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m2/test_io_lifecycle.c — verifies M2_CLAIMS.md I1, I2, I3.
 */

#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_io.h"

static MunitResult
test_init_fini(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	(void)p; (void)d;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_not_null(io);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);

	munit_assert_int(xtc_io_init(NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_io_fini(NULL), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

static MunitResult
test_init_fini_repeat(const MunitParameter p[], void *d)
{
	int i;
	(void)p; (void)d;
	for (i = 0; i < 100; i++) {
		xtc_io_t *io = NULL;
		munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
		munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	}
	return MUNIT_OK;
}

static MunitResult
test_backend_name(const MunitParameter p[], void *d)
{
	const char *name = xtc_io_backend_name();
	(void)p; (void)d;
	munit_assert_not_null(name);
	munit_assert_true(
	    strcmp(name, "poll")    == 0 ||
	    strcmp(name, "epoll")   == 0 ||
	    strcmp(name, "uring")   == 0 ||
	    strcmp(name, "kqueue")  == 0 ||
	    strcmp(name, "iocp")    == 0 ||
	    strcmp(name, "solaris") == 0 ||
	    strcmp(name, "aix")     == 0);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/I1_init_fini",          test_init_fini,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/I2_init_fini_repeat",   test_init_fini_repeat, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/I3_backend_name",       test_backend_name,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m2/io_lifecycle", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
