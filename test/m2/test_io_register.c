/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m2/test_io_register.c -- verifies M2_CLAIMS.md R1-R5.
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

/*
 * [R4b] reg -> mod -> del -> POLL.
 *
 * The poll is the whole point.  R4 above modifies and then tears the io
 * down without ever draining, so it never observes what a modified
 * registration does to the reap path -- which is why a real lifetime
 * defect shipped: the io_uring backend used to cancel and re-arm the
 * poll IN PLACE under the same heap node, leaving two submitted poll
 * generations sharing one object.  The following xtc_io_del_fd marked
 * that one object dead, the first terminal CQE freed it, and the second
 * generation's CQE then read and freed it again (valgrind: invalid read
 * + invalid free in xtc_io_poll; glibc aborts the process outright with
 * "double free detected in tcache", so this test does not merely fail on
 * the buggy code, it crashes).
 *
 * Part A must NOT poll between the mod and the del: a poll there drains
 * both generations, and then the delete has nothing in flight to race.
 * That is exactly the hole the shipped R4 fell through, so the ordering
 * here is load-bearing, not incidental.  Part B pins the behavior the
 * fix must not trade away (readiness after a mod reports the NEW tag).
 */
static MunitResult
test_mod_then_del_then_poll(const MunitParameter p[], void *d)
{
	xtc_io_t *io;
	xtc_io_event_t evs[8];
	char buf[8];
	int r, w, n_out, i, j, a = 1, b = 2, c = 3, e = 4, saw_new = 0;
	(void)p; (void)d;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(make_pipe(&r, &w), ==, 0);

	/* --- Part A: the use-after-free / double-free shape. --- */
	munit_assert_int(xtc_io_reg_fd(io, r, XTC_IO_READABLE, &a),
	    ==, XTC_OK);
	munit_assert_int(xtc_io_mod_fd(io, r, XTC_IO_READABLE, &b),
	    ==, XTC_OK);
	/* Make the fd ready so the modified registration has a real
	 * readiness completion in flight, on top of the cancel, when it is
	 * deleted.  No poll before the del -- see the comment above. */
	munit_assert_int(xtc_test_pipe_write(w, "x", 1), ==, 1);
	munit_assert_int(xtc_io_del_fd(io, r), ==, XTC_OK);
	/* Drain past every generation the mod may have left submitted. */
	for (i = 0; i < 5; i++) {
		munit_assert_int(xtc_io_poll(io, evs, 8, 50 * 1000 * 1000LL,
		    &n_out), ==, XTC_OK);
		for (j = 0; j < n_out; j++) {
			/* Deleted: no dispatch for this fd, old tag or new. */
			munit_assert_ptr(evs[j].tag, !=, &a);
			munit_assert_ptr(evs[j].tag, !=, &b);
		}
	}

	/* --- Part B: a mod reports the new tag, and the fd is reusable. ---
	 * The delete above really retired the modified registration, so this
	 * must not report a duplicate (a leaked live node would). */
	munit_assert_int(xtc_test_pipe_read(r, buf, 1), ==, 1);
	munit_assert_int(xtc_io_reg_fd(io, r, XTC_IO_READABLE, &c),
	    ==, XTC_OK);
	munit_assert_int(xtc_io_mod_fd(io, r, XTC_IO_READABLE, &e),
	    ==, XTC_OK);
	munit_assert_int(xtc_test_pipe_write(w, "y", 1), ==, 1);
	for (i = 0; i < 4 && !saw_new; i++) {
		munit_assert_int(xtc_io_poll(io, evs, 8, 200 * 1000 * 1000LL,
		    &n_out), ==, XTC_OK);
		for (j = 0; j < n_out; j++) {
			/* The tag the mod replaced must never be dispatched. */
			munit_assert_ptr(evs[j].tag, !=, &c);
			if (evs[j].tag == &e) saw_new = 1;
		}
	}
	munit_assert_int(saw_new, ==, 1);
	munit_assert_int(xtc_io_del_fd(io, r), ==, XTC_OK);
	munit_assert_int(xtc_io_poll(io, evs, 8, 50 * 1000 * 1000LL, &n_out),
	    ==, XTC_OK);
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
	{ "/R4b_mod_del_poll", test_mod_then_del_then_poll,
	                                      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/R5_del_fd",    test_del_fd,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m2/io_register", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
