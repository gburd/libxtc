/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m2/test_io_wakeup.c -- verifies M2_CLAIMS.md W1-W4.
 */

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_io.h"
#include "os_thread.h"
#include "os_time.h"

/* [W1, W2] basic wakeup */
struct w1_ctx { xtc_io_t *io; int delay_ms; };

static void *
w1_waker(void *arg)
{
	struct w1_ctx *c = arg;
	(void)__os_sleep_ns((int64_t)c->delay_ms * 1000 * 1000);
	(void)xtc_io_wakeup(c->io);
	return NULL;
}

static MunitResult
test_wakeup_basic(const MunitParameter p[], void *d)
{
	xtc_io_t *io;
	__os_thread_t thr = {0};
	struct w1_ctx c;
	xtc_io_event_t evs[8];
	int n_out;
	int64_t before, after;
	(void)p; (void)d;

	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	c.io = io; c.delay_ms = 20;
	munit_assert_int(__os_thread_create(&thr, w1_waker, &c), ==, XTC_OK);

	munit_assert_int(__os_clock_mono(&before), ==, XTC_OK);
	munit_assert_int(xtc_io_poll(io, evs, 8, -1, &n_out), ==, XTC_OK);
	munit_assert_int(__os_clock_mono(&after), ==, XTC_OK);

	munit_assert_int(__os_thread_join(&thr, NULL), ==, XTC_OK);
	munit_assert_int(n_out, ==, 1);
	munit_assert_uint(evs[0].flags & XTC_IO_WAKEUP, ==, XTC_IO_WAKEUP);
	munit_assert_ptr(evs[0].tag, ==, NULL);
	/* Returned within reasonable time after the waker fired. */
	munit_assert_int64(after - before, >=, 15 * 1000 * 1000);
	munit_assert_int64(after - before, <=, 500 * 1000 * 1000);

	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/* [W3] coalesce */
static MunitResult
test_wakeup_coalesce(const MunitParameter p[], void *d)
{
	xtc_io_t *io;
	xtc_io_event_t evs[8];
	int n_out, i;
	(void)p; (void)d;

	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	for (i = 0; i < 100; i++)
		munit_assert_int(xtc_io_wakeup(io), ==, XTC_OK);
	munit_assert_int(xtc_io_poll(io, evs, 8, 0, &n_out), ==, XTC_OK);
	munit_assert_int(n_out, ==, 1);
	munit_assert_uint(evs[0].flags & XTC_IO_WAKEUP, ==, XTC_IO_WAKEUP);
	/* Drained: subsequent zero-timeout poll sees nothing. */
	munit_assert_int(xtc_io_poll(io, evs, 8, 0, &n_out), ==, XTC_OK);
	munit_assert_int(n_out, ==, 0);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/* [W4] concurrent wakeups */
#define W4_THREADS  8
#define W4_PER      1000
struct w4_ctx { xtc_io_t *io; };

static void *
w4_worker(void *arg)
{
	struct w4_ctx *c = arg;
	int i;
	for (i = 0; i < W4_PER; i++) (void)xtc_io_wakeup(c->io);
	return NULL;
}

static MunitResult
test_wakeup_concurrent(const MunitParameter p[], void *d)
{
	xtc_io_t *io;
	__os_thread_t thr[W4_THREADS] = {{0}};
	struct w4_ctx c;
	xtc_io_event_t evs[8];
	int n_out, i;
	(void)p; (void)d;

	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	c.io = io;
	for (i = 0; i < W4_THREADS; i++)
		munit_assert_int(__os_thread_create(&thr[i], w4_worker, &c),
		    ==, XTC_OK);
	for (i = 0; i < W4_THREADS; i++)
		munit_assert_int(__os_thread_join(&thr[i], NULL), ==, XTC_OK);

	/* At least one wakeup must be observable; coalescing means we may
	 * see exactly one even after thousands of writes. */
	munit_assert_int(xtc_io_poll(io, evs, 8, 100 * 1000 * 1000LL, &n_out),
	    ==, XTC_OK);
	munit_assert_int(n_out, >=, 1);
	munit_assert_uint(evs[0].flags & XTC_IO_WAKEUP, ==, XTC_IO_WAKEUP);

	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/W1_W2_basic",      test_wakeup_basic,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/W3_coalesce",      test_wakeup_coalesce,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/W4_concurrent",    test_wakeup_concurrent,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m2/io_wakeup", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
