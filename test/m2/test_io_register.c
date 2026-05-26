/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * test/m2/test_io_register.c — verifies M2_CLAIMS.md R1–R5.
 */

#define _POSIX_C_SOURCE 200809L

#include "io_pipe_compat.h"
#if !defined(_WIN32)
# include <unistd.h>
#endif

#include "munit.h"
#include "xtc.h"
#include "xtc_io.h"

#if defined(_WIN32)
# define test_pipe_close(fd)            xtc_test_close_pipe((fd), -1)
# define test_pipe_close_pair(p)        xtc_test_close_pipe((p)[0], (p)[1])
#else
# define test_pipe_close(fd)            close(fd)
# define test_pipe_close_pair(p)        do { close((p)[0]); close((p)[1]); } while (0)
#endif

static int
make_pipe(int *r, int *w)
{
	return xtc_test_make_pipe(r, w);
}

/* [R1] */
static MunitResult
test_reg_basic(const MunitParameter p[], void *d)
{
	xtc_io_t *io;
	int r, w, marker = 0;
	(void)p; (void)d;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(make_pipe(&r, &w), ==, 0);
	munit_assert_int(xtc_io_reg_fd(io, r, XTC_IO_READABLE, &marker),
	    ==, XTC_OK);
	munit_assert_int(xtc_io_del_fd(io, r), ==, XTC_OK);
	test_pipe_close(r); test_pipe_close(w);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/* [R2] */
static MunitResult
test_reg_bad_args(const MunitParameter p[], void *d)
{
	xtc_io_t *io;
	(void)p; (void)d;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(xtc_io_reg_fd(NULL, 0, XTC_IO_READABLE, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_io_reg_fd(io, -1, XTC_IO_READABLE, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_io_reg_fd(io,  0, 0,                NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/* [R3] */
static MunitResult
test_reg_duplicate(const MunitParameter p[], void *d)
{
	xtc_io_t *io;
	int r, w;
	(void)p; (void)d;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(make_pipe(&r, &w), ==, 0);
	munit_assert_int(xtc_io_reg_fd(io, r, XTC_IO_READABLE, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_io_reg_fd(io, r, XTC_IO_READABLE, NULL),
	    ==, XTC_E_INVAL);
	test_pipe_close(r); test_pipe_close(w);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/* [R4] */
static MunitResult
test_mod_fd(const MunitParameter p[], void *d)
{
	xtc_io_t *io;
	int r, w, a = 1, b = 2;
	(void)p; (void)d;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(make_pipe(&r, &w), ==, 0);
	munit_assert_int(xtc_io_reg_fd(io, r, XTC_IO_READABLE, &a),
	    ==, XTC_OK);
	munit_assert_int(xtc_io_mod_fd(io, r, XTC_IO_READABLE, &b),
	    ==, XTC_OK);
	munit_assert_int(xtc_io_mod_fd(io, 9999, XTC_IO_READABLE, NULL),
	    ==, XTC_E_INVAL);     /* not registered */
	test_pipe_close(r); test_pipe_close(w);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/* [R5] */
static MunitResult
test_del_fd(const MunitParameter p[], void *d)
{
	xtc_io_t *io;
	int r, w;
	(void)p; (void)d;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(make_pipe(&r, &w), ==, 0);
	munit_assert_int(xtc_io_reg_fd(io, r, XTC_IO_READABLE, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_io_del_fd(io, r), ==, XTC_OK);
	munit_assert_int(xtc_io_del_fd(io, r), ==, XTC_E_INVAL);
	munit_assert_int(xtc_io_mod_fd(io, r, XTC_IO_READABLE, NULL),
	    ==, XTC_E_INVAL);
	test_pipe_close(r); test_pipe_close(w);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/R1_basic",     test_reg_basic,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/R2_bad_args",  test_reg_bad_args, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/R3_duplicate", test_reg_duplicate,NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/R4_mod_fd",    test_mod_fd,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/R5_del_fd",    test_del_fd,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m2/io_register", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
