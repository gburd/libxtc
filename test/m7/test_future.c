/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m7/test_future.c -- xtc_future / xtc_promise (PLAN.md 2.4.1).
 *
 *	Covers: set + await from a fiber; wait from a plain thread; a
 *	dropped promise completing the future with XTC_E_ABORTED;
 *	non-blocking ready; timeout; and every combinator (map, then,
 *	when_all, when_any, with_timeout).  Fiber-context cases run
 *	inside a proc on a loop; the thread-context case runs on main.
 */

#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_future.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

/* ---- thread-context: set on a helper thread, wait on main ---- */
struct setter_arg { xtc_promise_t *prom; intptr_t value; };

static void *
setter_thread(void *a)
{
	struct setter_arg *s = a;
	struct timespec ts = { 0, 20 * 1000 * 1000 };  /* 20ms */
	(void)nanosleep(&ts, NULL);
	(void)xtc_promise_set(s->prom, s->value, XTC_OK);
	return NULL;
}

static MunitResult
test_wait_from_thread(const MunitParameter p[], void *d)
{
	xtc_promise_t *prom = NULL;
	xtc_future_t *fut = NULL;
	struct setter_arg sa;
	pthread_t th;
	intptr_t out = 0;
	(void)p; (void)d;

	munit_assert_int(xtc_future_new_pair(&prom, &fut), ==, XTC_OK);
	sa.prom = prom;
	sa.value = 4242;
	munit_assert_int(pthread_create(&th, NULL, setter_thread, &sa), ==, 0);

	/* Blocks on the condvar until the setter thread fires. */
	munit_assert_int(xtc_future_wait(fut, &out, -1), ==, XTC_OK);
	munit_assert_llong((long long)out, ==, 4242);
	(void)pthread_join(th, NULL);
	return MUNIT_OK;
}

/* ---- thread-context: poll-timeout on an unset future, then ready ---- */
static MunitResult
test_timeout_and_ready(const MunitParameter p[], void *d)
{
	xtc_promise_t *prom = NULL;
	xtc_future_t *fut = NULL;
	intptr_t out = 0;
	int ready = -1;
	(void)p; (void)d;

	munit_assert_int(xtc_future_new_pair(&prom, &fut), ==, XTC_OK);
	munit_assert_int(xtc_future_ready(fut, &ready), ==, XTC_OK);
	munit_assert_int(ready, ==, 0);
	/* 0-timeout poll on an unset future: AGAIN, not consumed. */
	munit_assert_int(xtc_future_wait(fut, &out, 0), ==, XTC_E_AGAIN);
	/* short bounded wait, still unset: AGAIN. */
	munit_assert_int(xtc_future_wait(fut, &out, 5 * 1000 * 1000),
	    ==, XTC_E_AGAIN);
	/* now set it and observe ready + value. */
	munit_assert_int(xtc_promise_set(prom, 7, XTC_OK), ==, XTC_OK);
	munit_assert_int(xtc_future_ready(fut, &ready), ==, XTC_OK);
	munit_assert_int(ready, ==, 1);
	munit_assert_int(xtc_future_wait(fut, &out, -1), ==, XTC_OK);
	munit_assert_llong((long long)out, ==, 7);
	return MUNIT_OK;
}

/* ---- dropped promise -> future completes ABORTED ---- */
static MunitResult
test_dropped_promise(const MunitParameter p[], void *d)
{
	xtc_promise_t *prom = NULL;
	xtc_future_t *fut = NULL;
	intptr_t out = 12345;
	(void)p; (void)d;

	munit_assert_int(xtc_future_new_pair(&prom, &fut), ==, XTC_OK);
	xtc_promise_drop(prom);
	munit_assert_int(xtc_future_wait(fut, &out, -1), ==, XTC_E_ABORTED);
	return MUNIT_OK;
}

/* ---- double-set is rejected ---- */
static MunitResult
test_double_set(const MunitParameter p[], void *d)
{
	xtc_promise_t *prom = NULL;
	xtc_future_t *fut = NULL;
	intptr_t out = 0;
	(void)p; (void)d;

	munit_assert_int(xtc_future_new_pair(&prom, &fut), ==, XTC_OK);
	munit_assert_int(xtc_promise_set(prom, 1, XTC_OK), ==, XTC_OK);
	/* prom is consumed by set; a second set on the (freed) handle is
	 * not valid to call -- instead verify the future carries the first
	 * value only. */
	munit_assert_int(xtc_future_wait(fut, &out, -1), ==, XTC_OK);
	munit_assert_llong((long long)out, ==, 1);
	return MUNIT_OK;
}

/* ---- fiber-context: await inside a proc ---- */
static _Atomic int      g_fiber_ok;
static xtc_promise_t   *g_prom;
static xtc_future_t    *g_fut;

/* Producer proc: yields a few times, then sets the promise. */
static void
producer_proc(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < 10; i++)
		xtc_yield();
	(void)xtc_promise_set(g_prom, 0xABCD, XTC_OK);
}

/* Consumer proc: awaits the future, checks the value. */
static void
consumer_proc(void *arg)
{
	intptr_t out = 0;
	int rc;
	(void)arg;
	rc = xtc_future_await(g_fut, &out);
	if (rc == XTC_OK && out == 0xABCD)
		atomic_store(&g_fiber_ok, 1);
}

static MunitResult
test_await_in_fiber(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t a, b;
	(void)p; (void)d;

	atomic_store(&g_fiber_ok, 0);
	munit_assert_int(xtc_future_new_pair(&g_prom, &g_fut), ==, XTC_OK);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "consumer";
	munit_assert_int(xtc_proc_spawn(loop, consumer_proc, NULL, &opts, &a),
	    ==, XTC_OK);
	opts.name = "producer";
	munit_assert_int(xtc_proc_spawn(loop, producer_proc, NULL, &opts, &b),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);

	munit_assert_int(atomic_load(&g_fiber_ok), ==, 1);
	return MUNIT_OK;
}

/* ---- combinator: map ---- */
static intptr_t
double_it(intptr_t v, int status, void *user)
{
	(void)status; (void)user;
	return v * 2;
}

static MunitResult
test_map(const MunitParameter p[], void *d)
{
	xtc_promise_t *prom = NULL;
	xtc_future_t *fut = NULL, *mapped = NULL;
	intptr_t out = 0;
	(void)p; (void)d;

	munit_assert_int(xtc_future_new_pair(&prom, &fut), ==, XTC_OK);
	munit_assert_int(xtc_future_map(fut, double_it, NULL, &mapped),
	    ==, XTC_OK);
	munit_assert_int(xtc_promise_set(prom, 21, XTC_OK), ==, XTC_OK);
	munit_assert_int(xtc_future_wait(mapped, &out, -1), ==, XTC_OK);
	munit_assert_llong((long long)out, ==, 42);
	return MUNIT_OK;
}

/* map registered AFTER the source already completed (immediate fire). */
static MunitResult
test_map_already_ready(const MunitParameter p[], void *d)
{
	xtc_promise_t *prom = NULL;
	xtc_future_t *fut = NULL, *mapped = NULL;
	intptr_t out = 0;
	(void)p; (void)d;

	munit_assert_int(xtc_future_new_pair(&prom, &fut), ==, XTC_OK);
	munit_assert_int(xtc_promise_set(prom, 50, XTC_OK), ==, XTC_OK);
	munit_assert_int(xtc_future_map(fut, double_it, NULL, &mapped),
	    ==, XTC_OK);
	munit_assert_int(xtc_future_wait(mapped, &out, -1), ==, XTC_OK);
	munit_assert_llong((long long)out, ==, 100);
	return MUNIT_OK;
}

/* ---- combinator: then (chain to a second future) ---- */
static int
chain_next(intptr_t v, int status, void *user, xtc_future_t **out_next)
{
	xtc_promise_t *p2 = NULL;
	xtc_future_t *f2 = NULL;
	(void)status; (void)user;
	if (xtc_future_new_pair(&p2, &f2) != XTC_OK)
		return XTC_E_NOMEM;
	/* resolve the inner future with v + 100 immediately. */
	(void)xtc_promise_set(p2, v + 100, XTC_OK);
	*out_next = f2;
	return XTC_OK;
}

static MunitResult
test_then(const MunitParameter p[], void *d)
{
	xtc_promise_t *prom = NULL;
	xtc_future_t *fut = NULL, *chained = NULL;
	intptr_t out = 0;
	(void)p; (void)d;

	munit_assert_int(xtc_future_new_pair(&prom, &fut), ==, XTC_OK);
	munit_assert_int(xtc_future_then(fut, chain_next, NULL, &chained),
	    ==, XTC_OK);
	munit_assert_int(xtc_promise_set(prom, 5, XTC_OK), ==, XTC_OK);
	munit_assert_int(xtc_future_wait(chained, &out, -1), ==, XTC_OK);
	munit_assert_llong((long long)out, ==, 105);
	return MUNIT_OK;
}

/* ---- combinator: when_all ---- */
static MunitResult
test_when_all(const MunitParameter p[], void *d)
{
	xtc_promise_t *p1 = NULL, *p2 = NULL, *p3 = NULL;
	xtc_future_t *f[3] = { NULL, NULL, NULL };
	xtc_future_t *all = NULL;
	intptr_t out = 0;
	(void)p; (void)d;

	munit_assert_int(xtc_future_new_pair(&p1, &f[0]), ==, XTC_OK);
	munit_assert_int(xtc_future_new_pair(&p2, &f[1]), ==, XTC_OK);
	munit_assert_int(xtc_future_new_pair(&p3, &f[2]), ==, XTC_OK);
	munit_assert_int(xtc_future_when_all(f, 3, &all), ==, XTC_OK);

	munit_assert_int(xtc_promise_set(p1, 1, XTC_OK), ==, XTC_OK);
	munit_assert_int(xtc_promise_set(p2, 2, XTC_OK), ==, XTC_OK);
	/* not complete until the third resolves. */
	{ int r = -1; munit_assert_int(xtc_future_ready(all, &r), ==, XTC_OK);
	  munit_assert_int(r, ==, 0); }
	munit_assert_int(xtc_promise_set(p3, 3, XTC_OK), ==, XTC_OK);

	munit_assert_int(xtc_future_wait(all, &out, -1), ==, XTC_OK);
	munit_assert_llong((long long)out, ==, 3);   /* count */
	return MUNIT_OK;
}

/* when_all with one failure -> first non-OK status propagates. */
static MunitResult
test_when_all_error(const MunitParameter p[], void *d)
{
	xtc_promise_t *p1 = NULL, *p2 = NULL;
	xtc_future_t *f[2] = { NULL, NULL };
	xtc_future_t *all = NULL;
	intptr_t out = 0;
	(void)p; (void)d;

	munit_assert_int(xtc_future_new_pair(&p1, &f[0]), ==, XTC_OK);
	munit_assert_int(xtc_future_new_pair(&p2, &f[1]), ==, XTC_OK);
	munit_assert_int(xtc_future_when_all(f, 2, &all), ==, XTC_OK);
	munit_assert_int(xtc_promise_set(p1, 0, XTC_E_IO), ==, XTC_OK);
	munit_assert_int(xtc_promise_set(p2, 0, XTC_OK), ==, XTC_OK);
	munit_assert_int(xtc_future_wait(all, &out, -1), ==, XTC_E_IO);
	return MUNIT_OK;
}

/* ---- combinator: when_any ---- */
static MunitResult
test_when_any(const MunitParameter p[], void *d)
{
	xtc_promise_t *p1 = NULL, *p2 = NULL;
	xtc_future_t *f[2] = { NULL, NULL };
	xtc_future_t *any = NULL;
	intptr_t out = 0;
	(void)p; (void)d;

	munit_assert_int(xtc_future_new_pair(&p1, &f[0]), ==, XTC_OK);
	munit_assert_int(xtc_future_new_pair(&p2, &f[1]), ==, XTC_OK);
	munit_assert_int(xtc_future_when_any(f, 2, &any), ==, XTC_OK);
	/* second one wins; first never set (its promise leaks a pending
	 * completion, dropped when the test ends -- acceptable here). */
	munit_assert_int(xtc_promise_set(p2, 999, XTC_OK), ==, XTC_OK);
	munit_assert_int(xtc_future_wait(any, &out, -1), ==, XTC_OK);
	munit_assert_llong((long long)out, ==, 999);
	/* drop the never-resolved one to release its cell cleanly. */
	xtc_promise_drop(p1);
	return MUNIT_OK;
}

/* ---- combinator: with_timeout (fiber, times out) ---- */
static _Atomic int g_to_status;

static void
timeout_consumer(void *arg)
{
	xtc_promise_t *prom = NULL;
	xtc_future_t *fut = NULL, *tf = NULL;
	intptr_t out = 0;
	int rc;
	(void)arg;
	if (xtc_future_new_pair(&prom, &fut) != XTC_OK) return;
	/* Source is never set within the window -> timeout fires. */
	if (xtc_future_with_timeout(fut, 20 * 1000 * 1000, &tf) != XTC_OK) {
		xtc_promise_drop(prom);
		return;
	}
	rc = xtc_future_await(tf, &out);
	atomic_store(&g_to_status, rc);
	xtc_promise_drop(prom);   /* release the never-set source */
}

static MunitResult
test_with_timeout(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t c;
	(void)p; (void)d;

	atomic_store(&g_to_status, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "to";
	munit_assert_int(xtc_proc_spawn(loop, timeout_consumer, NULL, &opts, &c),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);

	munit_assert_int(atomic_load(&g_to_status), ==, XTC_E_AGAIN);
	return MUNIT_OK;
}

/* ---- argument validation ---- */
static MunitResult
test_inval(const MunitParameter p[], void *d)
{
	xtc_promise_t *prom = NULL;
	xtc_future_t *fut = NULL;
	int ready = 0;
	(void)p; (void)d;
	munit_assert_int(xtc_future_new_pair(NULL, &fut), ==, XTC_E_INVAL);
	munit_assert_int(xtc_future_new_pair(&prom, NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_promise_set(NULL, 0, XTC_OK), ==, XTC_E_INVAL);
	munit_assert_int(xtc_future_await(NULL, NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_future_ready(NULL, &ready), ==, XTC_E_INVAL);
	xtc_promise_drop(NULL);   /* no-op, no crash */
	return MUNIT_OK;
}

/* ---- kill a parked waiter, then fulfil the future -------------------
 *
 * A fiber parked in xtc_future_wait is killed (xtc_exit_pid) and the
 * promise is then set.  In 1.55.0 the resume hook inside xtc_yield
 * unwound the killed waiter with its stack waiter still linked on the
 * cell, so the set walked a dead frame (SIGSEGV / ASan
 * stack-use-after-return).  The victim must die (DOWN arrives) and the
 * set must be clean.  The timed variant is resumed by its own timer
 * first; the infinite one only by the set.
 */
static xtc_future_t  *g_kf_fut;
static xtc_promise_t *g_kf_prom;
static int64_t        g_kf_tmo;
static xtc_loop_t    *g_kf_loop;
static atomic_int     g_kf_returned;
static int            g_kf_down;

static void
kf_victim(void *arg)
{
	intptr_t v = 0;
	(void)arg;
	(void)xtc_future_wait(g_kf_fut, &v, g_kf_tmo);
	atomic_store(&g_kf_returned, 1);          /* must not be reached */
}

static void
kf_driver(void *arg)
{
	xtc_pid_t victim;
	uint64_t ref;
	void *m = NULL;
	size_t n = 0;
	int i;
	(void)arg;
	munit_assert_int(xtc_proc_spawn_monitor(g_kf_loop, kf_victim, NULL,
	    NULL, &victim, &ref), ==, XTC_OK);
	for (i = 0; i < 10; i++) xtc_yield();     /* victim parks */
	munit_assert_int(xtc_exit_pid(victim, 9), ==, XTC_OK);
	(void)xtc_proc_sleep(100LL * 1000 * 1000);  /* past a 50 ms timer */
	munit_assert_int(xtc_promise_set(g_kf_prom, 7, XTC_OK), ==, XTC_OK);
	g_kf_down = xtc_recv(&m, &n, 2000LL * 1000 * 1000) == XTC_OK;
	if (m != NULL) xtc_free(m);
}

static MunitResult
test_kill_then_fulfil(const MunitParameter params[], void *data)
{
	const char *wt = munit_parameters_get(params, "wait");
	xtc_loop_t *loop = NULL;
	intptr_t v = 0;
	(void)data;
	g_kf_tmo = (wt != NULL && strcmp(wt, "timed") == 0) ?
	    50LL * 1000 * 1000 : -1;
	atomic_store(&g_kf_returned, 0);
	g_kf_down = 0;
	munit_assert_int(xtc_future_new_pair(&g_kf_prom, &g_kf_fut), ==, XTC_OK);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	g_kf_loop = loop;
	munit_assert_int(xtc_proc_spawn(loop, kf_driver, NULL, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	munit_assert_int(atomic_load(&g_kf_returned), ==, 0);
	munit_assert_int(g_kf_down, ==, 1);
	/* The killed waiter left the future unconsumed: the value is still
	 * there for anyone holding the handle. */
	munit_assert_int(xtc_future_wait(g_kf_fut, &v, 0), ==, XTC_OK);
	munit_assert_int((int)v, ==, 7);
	return MUNIT_OK;
}

static char *kf_waits[] = { (char *)"inf", (char *)"timed", NULL };
static MunitParameterEnum kf_params[] = {
	{ (char *)"wait", kf_waits },
	{ NULL, NULL }
};

static MunitTest tests[] = {
	{ "/wait_from_thread",   test_wait_from_thread,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/timeout_and_ready",  test_timeout_and_ready,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/dropped_promise",    test_dropped_promise,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/double_set",         test_double_set,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/await_in_fiber",     test_await_in_fiber,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/map",                test_map,                NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/map_already_ready",  test_map_already_ready,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/then",               test_then,               NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/when_all",           test_when_all,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/when_all_error",     test_when_all_error,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/when_any",           test_when_any,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/with_timeout",       test_with_timeout,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/inval",              test_inval,              NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/kill_then_fulfil",   test_kill_then_fulfil,   NULL, NULL, MUNIT_TEST_OPTION_NONE, kf_params },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = {
	"/m7/future", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
