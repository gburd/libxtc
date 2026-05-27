/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/m2/test_io_fault_inject.c
 *	Fault-injection coverage for io_common.c error paths.
 *	Uses xtc_inject_attach + xtc_inject_check to trip the
 *	conditional bypasses added in io_common.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_io.h"
#include "xtc_inject.h"
#include "xtc_int.h"

static int g_callback_hits;

static void
inj_cb(const char *name, void *user)
{
	(void)name; (void)user;
	g_callback_hits++;
}

/* ----- calloc fail path ------------------------------------ */

static MunitResult
test_calloc_fail(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	int rc;
	(void)p; (void)d;
	g_callback_hits = 0;
	munit_assert_int(xtc_inject_attach("io.init.calloc_fail", inj_cb, NULL),
	    ==, XTC_OK);
	rc = xtc_io_init(&io);
	(void)xtc_inject_detach("io.init.calloc_fail");
	munit_assert_int(rc, ==, XTC_E_NOMEM);
	munit_assert_null(io);
	munit_assert_int(g_callback_hits, ==, 1);
	return MUNIT_OK;
}

/* ----- pipe fail path ------------------------------------ */

#if !defined(_WIN32)
static MunitResult
test_pipe_fail(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	int rc;
	(void)p; (void)d;
	g_callback_hits = 0;
	munit_assert_int(xtc_inject_attach("io.init.pipe_fail", inj_cb, NULL),
	    ==, XTC_OK);
	rc = xtc_io_init(&io);
	(void)xtc_inject_detach("io.init.pipe_fail");
	munit_assert_int(rc, ==, XTC_E_INTERNAL);
	munit_assert_null(io);
	munit_assert_int(g_callback_hits, ==, 1);
	return MUNIT_OK;
}

static MunitResult
test_fcntl_fail(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	int rc;
	(void)p; (void)d;
	g_callback_hits = 0;
	munit_assert_int(xtc_inject_attach("io.init.fcntl_fail", inj_cb, NULL),
	    ==, XTC_OK);
	rc = xtc_io_init(&io);
	(void)xtc_inject_detach("io.init.fcntl_fail");
	munit_assert_int(rc, ==, XTC_E_INTERNAL);
	munit_assert_null(io);
	munit_assert_int(g_callback_hits, ==, 1);
	return MUNIT_OK;
}
#endif

/* ----- backend init fail path ----------------------------- */

static MunitResult
test_backend_fail(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	int rc;
	(void)p; (void)d;
	g_callback_hits = 0;
	munit_assert_int(xtc_inject_attach("io.init.backend_fail", inj_cb, NULL),
	    ==, XTC_OK);
	rc = xtc_io_init(&io);
	(void)xtc_inject_detach("io.init.backend_fail");
	munit_assert_int(rc, ==, XTC_E_INTERNAL);
	munit_assert_null(io);
	munit_assert_int(g_callback_hits, ==, 1);
	return MUNIT_OK;
}

/* ----- xtc_io_init / xtc_io_fini with NULL --------------- */

static MunitResult
test_null_args(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	munit_assert_int(xtc_io_init(NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_io_fini(NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_io_wakeup(NULL), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* ----- successful path still works after attach is gone ----- */

static MunitResult
test_normal_after_inject(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	(void)p; (void)d;
	/* After the previous tests detached, init should work. */
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_not_null(io);
	munit_assert_int(xtc_io_wakeup(io), ==, XTC_OK);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/calloc_fail",         test_calloc_fail,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#if !defined(_WIN32)
	{ "/pipe_fail",           test_pipe_fail,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fcntl_fail",          test_fcntl_fail,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#endif
	{ "/backend_fail",        test_backend_fail,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/null_args",           test_null_args,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/normal_after_inject", test_normal_after_inject, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m2/io_fault_inject", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
