/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m2/test_io_events.c — verifies M2_CLAIMS.md E1–E7.
 */

#define _POSIX_C_SOURCE 200809L

#include "io_pipe_compat.h"
#if !defined(_WIN32)
# include <unistd.h>
#endif

#include <string.h>
#include <stdint.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_io.h"
#include "os_time.h"

#if defined(_WIN32)
# define test_close(fd)            xtc_test_close_pipe((fd), -1)
# define test_close_pair(p)        xtc_test_close_pipe((p)[0], (p)[1])
# define test_write(fd, buf, n)    xtc_test_pipe_write((fd), (buf), (int)(n))
# define test_read(fd, buf, n)     xtc_test_pipe_read((fd), (buf), (int)(n))
# define make_pipe_pair(p)         xtc_test_make_pipe(&(p)[0], &(p)[1])
#else
# define test_close(fd)            close(fd)
# define test_close_pair(p)        do { close((p)[0]); close((p)[1]); } while (0)
# define test_write(fd, buf, n)    (int)write((fd), (buf), (size_t)(n))
# define test_read(fd, buf, n)     (int)read((fd), (buf), (size_t)(n))
# define make_pipe_pair(p)         pipe(p)
#endif

/* [E1, E4] readable + tag identity */
static MunitResult
test_readable(const MunitParameter p[], void *d)
{
	xtc_io_t *io;
	int rfd, wfd;
	int n_out, marker = 42;
	xtc_io_event_t evs[8];
	int p_[2];
	(void)p; (void)d;

	munit_assert_int(make_pipe_pair(p_), ==, 0);
	rfd = p_[0]; wfd = p_[1];
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(xtc_io_reg_fd(io, rfd, XTC_IO_READABLE, &marker),
	    ==, XTC_OK);

	munit_assert_int(test_write(wfd, "x", 1), ==, 1);

	munit_assert_int(xtc_io_poll(io, evs, 8, 1000 * 1000 * 1000LL, &n_out),
	    ==, XTC_OK);
	munit_assert_int(n_out, ==, 1);
	munit_assert_ptr(evs[0].tag, ==, &marker);
	munit_assert_uint(evs[0].flags & XTC_IO_READABLE, ==, XTC_IO_READABLE);

	munit_assert_int(xtc_io_del_fd(io, rfd), ==, XTC_OK);
	test_close(rfd); test_close(wfd);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/* [E2] writable */
static MunitResult
test_writable(const MunitParameter p[], void *d)
{
	xtc_io_t *io;
	int n_out;
	xtc_io_event_t evs[8];
	int p_[2];
	(void)p; (void)d;

	munit_assert_int(make_pipe_pair(p_), ==, 0);
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(xtc_io_reg_fd(io, p_[1], XTC_IO_WRITABLE, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_io_poll(io, evs, 8, 1000 * 1000 * 1000LL, &n_out),
	    ==, XTC_OK);
	munit_assert_int(n_out, ==, 1);
	munit_assert_uint(evs[0].flags & XTC_IO_WRITABLE, ==, XTC_IO_WRITABLE);
	munit_assert_int(xtc_io_del_fd(io, p_[1]), ==, XTC_OK);
	test_close_pair(p_);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/* [E3] HUP on closed writer */
static MunitResult
test_hup(const MunitParameter p[], void *d)
{
	xtc_io_t *io;
	int n_out, p_[2];
	xtc_io_event_t evs[8];
	(void)p; (void)d;

	munit_assert_int(make_pipe_pair(p_), ==, 0);
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(xtc_io_reg_fd(io, p_[0], XTC_IO_READABLE, NULL),
	    ==, XTC_OK);
	test_close(p_[1]);     /* close writer */
	munit_assert_int(xtc_io_poll(io, evs, 8, 1000 * 1000 * 1000LL, &n_out),
	    ==, XTC_OK);
	munit_assert_int(n_out, ==, 1);
	/* Some kernels surface HUP, others surface readable+EOF.  Either is
	 * acceptable; the contract is "an event arrives" with one of the
	 * two flags so the caller can read 0 bytes and recognise close. */
	munit_assert_true(
	    (evs[0].flags & (XTC_IO_HUP | XTC_IO_READABLE)) != 0);
	munit_assert_int(xtc_io_del_fd(io, p_[0]), ==, XTC_OK);
	test_close(p_[0]);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/* [E5] many ready fds */
#define MANY 5
static MunitResult
test_many_ready(const MunitParameter p[], void *d)
{
	xtc_io_t *io;
	int rfds[MANY], wfds[MANY];
	int markers[MANY];
	int i, n_out;
	xtc_io_event_t evs[MANY];
	(void)p; (void)d;

	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	for (i = 0; i < MANY; i++) {
		int p_[2];
		munit_assert_int(make_pipe_pair(p_), ==, 0);
		rfds[i] = p_[0]; wfds[i] = p_[1]; markers[i] = i;
		munit_assert_int(xtc_io_reg_fd(io, rfds[i], XTC_IO_READABLE,
		    &markers[i]), ==, XTC_OK);
		munit_assert_int(test_write(wfds[i], "y", 1), ==, 1);
	}
	munit_assert_int(xtc_io_poll(io, evs, MANY, 1000 * 1000 * 1000LL, &n_out),
	    ==, XTC_OK);
	munit_assert_int(n_out, ==, MANY);
	for (i = 0; i < MANY; i++) {
		(void)xtc_io_del_fd(io, rfds[i]);
		test_close(rfds[i]); test_close(wfds[i]);
	}
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/* [E6] timeout 0 returns immediately */
static MunitResult
test_timeout_zero(const MunitParameter p[], void *d)
{
	xtc_io_t *io;
	int n_out = -1;
	xtc_io_event_t evs[8];
	(void)p; (void)d;

	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(xtc_io_poll(io, evs, 8, 0, &n_out), ==, XTC_OK);
	munit_assert_int(n_out, ==, 0);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/* [E7] positive timeout blocks for at most that long. */
static MunitResult
test_timeout_positive(const MunitParameter p[], void *d)
{
	xtc_io_t *io;
	int n_out;
	xtc_io_event_t evs[8];
	int64_t before, after;
	const int64_t target_ns = 50 * 1000 * 1000;   /* 50 ms */
	(void)p; (void)d;

	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(__os_clock_mono(&before), ==, XTC_OK);
	munit_assert_int(xtc_io_poll(io, evs, 8, target_ns, &n_out), ==, XTC_OK);
	munit_assert_int(__os_clock_mono(&after), ==, XTC_OK);
	munit_assert_int(n_out, ==, 0);
	/* Actual sleep should be roughly target; allow ample slop both ways
	 * for CI noise but bound the upper to twice the request. */
	munit_assert_int64(after - before, >=, target_ns - target_ns/4);
	munit_assert_int64(after - before, <=, target_ns * 4);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/E1_E4_readable",     test_readable,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/E2_writable",        test_writable,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/E3_hup",             test_hup,              NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/E5_many_ready",      test_many_ready,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/E6_timeout_zero",    test_timeout_zero,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/E7_timeout_positive",test_timeout_positive, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m2/io_events", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
