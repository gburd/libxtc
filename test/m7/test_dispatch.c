/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m7/test_dispatch.c -- xtc_dispatch (roadmap B1).
 *
 *	The callback -> fiber bridge.  Covers: submitting an effect from
 *	a FOREIGN OS thread (the executor runs on its own worker threads;
 *	the test's main thread is foreign to them) and awaiting the
 *	result; the future carrying fn's return value; cancellation
 *	resolving the future ABORTED without a hang; releasing a handle
 *	without cancelling; and argument validation.  These prove the
 *	"exactly once" resolution the dispatcher promises: every
 *	dispatched effect's future resolves once, with a value or
 *	ABORTED, never lost and never doubled.
 */

#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_dispatch.h"
#include "xtc_future.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"

/*
 * An executor running on its own worker threads in SERVICE MODE (never
 * idle-auto-stops), so the test's main thread is a genuinely FOREIGN
 * thread relative to the runtime -- exactly the dispatcher's intended
 * caller.  The executor is driven on a helper thread; main dispatches
 * into it and awaits.
 */
struct fixture {
	xtc_exec_t   *exec;
	pthread_t     th;
	_Atomic int   running;
};

static void *
exec_runner(void *a)
{
	struct fixture *f = a;
	atomic_store(&f->running, 1);
	(void)xtc_exec_run(f->exec);   /* service mode: until xtc_exec_stop */
	return NULL;
}

static void
fixture_start(struct fixture *f)
{
	munit_assert_int(xtc_exec_init(&f->exec, 2), ==, XTC_OK);
	xtc_exec_set_service_mode(f->exec, 1);
	atomic_store(&f->running, 0);
	munit_assert_int(pthread_create(&f->th, NULL, exec_runner, f), ==, 0);
	while (atomic_load(&f->running) == 0)
		usleep(200);
	/* Give the worker threads a beat to spin up their pollers. */
	usleep(5 * 1000);
}

static xtc_loop_t *
fixture_loop(struct fixture *f)
{
	return xtc_exec_loop(f->exec, 0);
}

static void
fixture_stop(struct fixture *f)
{
	munit_assert_int(xtc_exec_stop(f->exec), ==, XTC_OK);
	(void)pthread_join(f->th, NULL);
	munit_assert_int(xtc_exec_fini(f->exec), ==, XTC_OK);
}

/* ---- effect fns ---- */

static _Atomic int g_ran;   /* counts how many times an effect body ran */

static int
effect_return_7(void *arg)
{
	(void)arg;
	atomic_fetch_add(&g_ran, 1);
	return 7;
}

static int
effect_return_arg(void *arg)
{
	atomic_fetch_add(&g_ran, 1);
	return (int)(intptr_t)arg;
}

/* A long-running effect that cooperatively checks for cancellation so a
 * xtc_dispatch_cancel actually gets observed and the fiber unwinds. */
static int
effect_wait_for_cancel(void *arg)
{
	int i;
	(void)arg;
	atomic_fetch_add(&g_ran, 1);
	for (i = 0; i < 100000; i++) {
		if (xtc_cancel_requested())
			return 0;         /* observed; fiber will unwind/exit */
		xtc_proc_sleep(1 * 1000 * 1000);   /* 1ms cancellation point */
	}
	return 99;   /* not cancelled in time (should not happen in the test) */
}

/* ---- foreign-thread dispatch, await from main ---- */
static MunitResult
test_foreign_submit(const MunitParameter p[], void *d)
{
	struct fixture f;
	xtc_future_t *fut = NULL;
	intptr_t out = 0;
	(void)p; (void)d;

	atomic_store(&g_ran, 0);
	fixture_start(&f);

	/* main is foreign to the executor's worker threads: this exercises
	 * the cross-thread spawn (MPSC inbox + I/O wakeup) path. */
	munit_assert_int(
	    xtc_dispatch(fixture_loop(&f), effect_return_7, NULL, &fut, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_future_wait(fut, &out, -1), ==, XTC_OK);
	munit_assert_llong((long long)out, ==, 7);
	munit_assert_int(atomic_load(&g_ran), ==, 1);   /* exactly once */

	fixture_stop(&f);
	return MUNIT_OK;
}

/* fn's return value is carried through as the future value. */
static MunitResult
test_value_carried(const MunitParameter p[], void *d)
{
	struct fixture f;
	xtc_future_t *fut = NULL;
	intptr_t out = 0;
	(void)p; (void)d;

	atomic_store(&g_ran, 0);
	fixture_start(&f);
	munit_assert_int(
	    xtc_dispatch(fixture_loop(&f), effect_return_arg,
	        (void *)(intptr_t)321, &fut, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_future_wait(fut, &out, -1), ==, XTC_OK);
	munit_assert_llong((long long)out, ==, 321);
	fixture_stop(&f);
	return MUNIT_OK;
}

/* cancel -> the future resolves XTC_E_ABORTED, never hangs. */
static MunitResult
test_cancel(const MunitParameter p[], void *d)
{
	struct fixture f;
	xtc_future_t *fut = NULL;
	xtc_dispatch_handle_t *h = NULL;
	intptr_t out = 12345;
	int rc;
	(void)p; (void)d;

	atomic_store(&g_ran, 0);
	fixture_start(&f);
	munit_assert_int(
	    xtc_dispatch(fixture_loop(&f), effect_wait_for_cancel, NULL,
	        &fut, &h),
	    ==, XTC_OK);
	munit_assert_not_null(h);

	/* Give the effect a moment to start, then cancel it. */
	usleep(20 * 1000);
	munit_assert_int(xtc_dispatch_cancel(h), ==, XTC_OK);

	/* The future MUST resolve (bounded), either ABORTED (finalizer
	 * dropped the promise on unwind) or OK with 0 (the effect saw the
	 * cancel and returned 0 before unwinding).  It must never hang and
	 * never carry the not-cancelled sentinel 99. */
	rc = xtc_future_wait(fut, &out, 2 * 1000 * 1000 * 1000LL);
	munit_assert_int(rc != XTC_E_AGAIN, ==, 1);   /* did not time out */
	if (rc == XTC_OK)
		munit_assert_llong((long long)out, !=, 99);
	else
		munit_assert_int(rc, ==, XTC_E_ABORTED);

	fixture_stop(&f);
	return MUNIT_OK;
}

/* handle_free without cancel: the effect still runs and resolves. */
static MunitResult
test_handle_free(const MunitParameter p[], void *d)
{
	struct fixture f;
	xtc_future_t *fut = NULL;
	xtc_dispatch_handle_t *h = NULL;
	intptr_t out = 0;
	(void)p; (void)d;

	atomic_store(&g_ran, 0);
	fixture_start(&f);
	munit_assert_int(
	    xtc_dispatch(fixture_loop(&f), effect_return_7, NULL, &fut, &h),
	    ==, XTC_OK);
	munit_assert_not_null(h);
	xtc_dispatch_handle_free(h);   /* release, do not cancel */
	munit_assert_int(xtc_future_wait(fut, &out, -1), ==, XTC_OK);
	munit_assert_llong((long long)out, ==, 7);
	fixture_stop(&f);
	return MUNIT_OK;
}

/* ---- argument validation ---- */
static MunitResult
test_inval(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_future_t *fut = NULL;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_dispatch(NULL, effect_return_7, NULL, &fut, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_dispatch(loop, NULL, NULL, &fut, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_dispatch(loop, effect_return_7, NULL, NULL, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_dispatch_cancel(NULL), ==, XTC_E_INVAL);
	xtc_dispatch_handle_free(NULL);   /* no-op, no crash */
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/foreign_submit", test_foreign_submit, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/value_carried",  test_value_carried,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/cancel",         test_cancel,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/handle_free",    test_handle_free,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/inval",          test_inval,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = {
	"/m7/dispatch", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
