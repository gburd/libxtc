/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * test/m3/test_io_integration.c — verifies M3_CLAIMS.md Io1–Io3.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>

#include "io_pipe_compat.h"
#if !defined(_WIN32)
# include <unistd.h>
#endif

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_io.h"

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

/* [Io1] park on a readable fd; producer writes; we wake. */
struct read_ctx {
	int rfd, wfd;
	int phase;
	int run_count;
	xtc_loop_t *loop;
};

static int parker_read(xtc_task_t *self, void *u) {
	struct read_ctx *c = u;
	c->run_count++;
	if (c->phase == 0) {
		c->phase = 1;
		munit_assert_int(xtc_task_park_on_fd(self, c->rfd,
		    XTC_IO_READABLE), ==, XTC_OK);
		return XTC_TASK_PENDING;
	}
	/* Drain so the next loop step doesn't observe readiness again. */
	{ char b; test_read(c->rfd, &b, 1); }
	return XTC_TASK_DONE;
}

static int writer_then_done(xtc_task_t *self, void *u) {
	struct read_ctx *c = u;
	(void)self;
	munit_assert_int(test_write(c->wfd, "x", 1), ==, 1);
	return XTC_TASK_DONE;
}

static MunitResult
test_park_on_fd_read(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct read_ctx c;
	int p_[2];
	(void)p; (void)d;
	munit_assert_int(make_pipe_pair(p_), ==, 0);
	c.rfd = p_[0]; c.wfd = p_[1]; c.phase = 0; c.run_count = 0;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	c.loop = loop;
	munit_assert_int(xtc_task_spawn(loop, parker_read,      &c, NULL), ==, XTC_OK);
	munit_assert_int(xtc_task_spawn(loop, writer_then_done, &c, NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(c.run_count, ==, 2);

	test_close(c.rfd); test_close(c.wfd);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Io2] park on a writable fd. */
struct w_ctx { int wfd; int phase; int runs; };
static int parker_write(xtc_task_t *self, void *u) {
	struct w_ctx *c = u;
	c->runs++;
	if (c->phase == 0) {
		c->phase = 1;
		munit_assert_int(xtc_task_park_on_fd(self, c->wfd,
		    XTC_IO_WRITABLE), ==, XTC_OK);
		return XTC_TASK_PENDING;
	}
	return XTC_TASK_DONE;
}

static MunitResult
test_park_on_fd_write(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct w_ctx c;
	int p_[2];
	(void)p; (void)d;
	munit_assert_int(make_pipe_pair(p_), ==, 0);
	c.wfd = p_[1]; c.phase = 0; c.runs = 0;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_task_spawn(loop, parker_write, &c, NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(c.runs, ==, 2);
	test_close_pair(p_);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Io3] one-park-at-a-time: a second park is rejected. */
struct two_ctx { int fd1, fd2; int phase; int rc_second; };
static int parker_two(xtc_task_t *self, void *u) {
	struct two_ctx *c = u;
	c->phase++;
	if (c->phase == 1) {
		(void)xtc_task_park_on_fd(self, c->fd1, XTC_IO_READABLE);
		c->rc_second = xtc_task_park_on_fd(self, c->fd2,
		    XTC_IO_READABLE);
		return XTC_TASK_DONE;   /* unwind so we don't actually wait */
	}
	return XTC_TASK_DONE;
}

static MunitResult
test_park_one_at_a_time(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct two_ctx c;
	int a[2], b[2];
	(void)p; (void)d;
	munit_assert_int(make_pipe_pair(a), ==, 0);
	munit_assert_int(make_pipe_pair(b), ==, 0);
	c.fd1 = a[0]; c.fd2 = b[0]; c.phase = 0; c.rc_second = 0;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_task_spawn(loop, parker_two, &c, NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(c.rc_second, ==, XTC_E_INVAL);
	test_close_pair(a); test_close_pair(b);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/Io1_park_read",         test_park_on_fd_read,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Io2_park_write",        test_park_on_fd_write,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Io3_park_one_at_a_time",test_park_one_at_a_time, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m3/io_integration", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
