/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m4/test_async.c -- verifies M4_CLAIMS.md C1-C9.
 */

#include <stdint.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_async.h"
#include "os_time.h"

/* [C1, C2] basic spawn-and-await. */
static intptr_t add_one(void *arg) {
	int v = *(int *)arg;
	return (intptr_t)(v + 1);
}

static MunitResult
test_basic(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_task_t *t;
	int v = 41;
	intptr_t r = 0;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_async(loop, add_one, &v, &t), ==, XTC_OK);
	munit_assert_int(xtc_await(t, &r), ==, XTC_OK);
	munit_assert_int((int)r, ==, 42);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [C4] yield round-trip. */
static int yield_count;
static intptr_t yielding_coro(void *arg) {
	int n = *(int *)arg;
	int i;
	for (i = 0; i < n; i++) {
		yield_count++;
		xtc_yield();
	}
	return (intptr_t)yield_count;
}

static MunitResult
test_yield(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_task_t *t;
	int n = 5;
	intptr_t r = 0;
	(void)p; (void)d;
	yield_count = 0;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_async(loop, yielding_coro, &n, &t), ==, XTC_OK);
	munit_assert_int(xtc_await(t, &r), ==, XTC_OK);
	munit_assert_int((int)r, ==, 5);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [C5] locals survive yield.  This is the headline contract that
 * distinguishes fibers from protothreads. */
static intptr_t locals_test(void *arg) {
	(void)arg;
	int a = 100, b = 200, c = 300;
	xtc_yield();
	munit_assert_int(a, ==, 100);
	xtc_yield();
	munit_assert_int(b, ==, 200);
	xtc_yield();
	munit_assert_int(c, ==, 300);
	return (intptr_t)(a + b + c);
}

static MunitResult
test_locals_survive(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_task_t *t;
	intptr_t r = 0;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_async(loop, locals_test, NULL, &t), ==, XTC_OK);
	munit_assert_int(xtc_await(t, &r), ==, XTC_OK);
	munit_assert_int((int)r, ==, 600);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [C9] many coroutines coexist. */
#define MANY 64
struct mctx { int seed; int sum; };
static intptr_t many_coro(void *arg) {
	struct mctx *c = arg;
	int i, s = 0;
	for (i = 0; i < 10; i++) { s += c->seed + i; xtc_yield(); }
	c->sum = s;
	return s;
}

static MunitResult
test_many_coros(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct mctx ctxs[MANY];
	xtc_task_t *tasks[MANY];
	int i, expected;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	for (i = 0; i < MANY; i++) {
		ctxs[i].seed = i;
		ctxs[i].sum = 0;
		munit_assert_int(xtc_async(loop, many_coro, &ctxs[i], &tasks[i]),
		    ==, XTC_OK);
	}
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	for (i = 0; i < MANY; i++) {
		int j;
		expected = 0;
		for (j = 0; j < 10; j++) expected += i + j;
		munit_assert_int(ctxs[i].sum, ==, expected);
	}
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [C7] coroutine that returns normally is reaped. */
static intptr_t trivial(void *arg) { (void)arg; return 77; }

static MunitResult
test_coro_done(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_task_t *t;
	intptr_t r = 0;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_async(loop, trivial, NULL, &t), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	/* After loop_run, the coro is done.  Awaiting yields its result. */
	munit_assert_int(xtc_await(t, &r), ==, XTC_OK);
	munit_assert_int((int)r, ==, 77);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [C3] nested await: a coroutine can spawn and await another. */
static int inner_arg;
static intptr_t inner(void *arg) {
	inner_arg = *(int *)arg;
	return (intptr_t)(inner_arg * 2);
}
struct outer_ctx { xtc_loop_t *loop; int n; };
static intptr_t outer(void *arg) {
	struct outer_ctx *o = arg;
	xtc_task_t *child = NULL;
	intptr_t r = 0;
	(void)xtc_async(o->loop, inner, &o->n, &child);
	(void)xtc_await(child, &r);
	return r + 1;
}

static MunitResult
test_nested(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_task_t *t;
	struct outer_ctx ctx;
	intptr_t r = 0;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	ctx.loop = loop; ctx.n = 21;
	munit_assert_int(xtc_async(loop, outer, &ctx, &t), ==, XTC_OK);
	munit_assert_int(xtc_await(t, &r), ==, XTC_OK);
	munit_assert_int((int)r, ==, 21 * 2 + 1);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [P1, P2, P3] protothread basics. */
static int pt_log[8];
static int pt_log_n;
static XTC_PT_THREAD(pt_simple(xtc_pt_t *self, int *runs)) {
	XTC_PT_BEGIN(self);
	pt_log[pt_log_n++] = 1;
	XTC_PT_YIELD(self);
	pt_log[pt_log_n++] = 2;
	XTC_PT_YIELD(self);
	pt_log[pt_log_n++] = 3;
	(*runs)++;
	XTC_PT_END(self);
}

static MunitResult
test_pt_basic(const MunitParameter p[], void *d)
{
	xtc_pt_t pt;
	int runs = 0;
	char rc;
	(void)p; (void)d;
	XTC_PT_INIT(&pt);
	pt_log_n = 0;
	rc = pt_simple(&pt, &runs);   munit_assert_int(rc, ==, XTC_PT_YIELDED);
	rc = pt_simple(&pt, &runs);   munit_assert_int(rc, ==, XTC_PT_YIELDED);
	rc = pt_simple(&pt, &runs);   munit_assert_int(rc, ==, XTC_PT_DONE);
	munit_assert_int(runs, ==, 1);
	munit_assert_int(pt_log_n, ==, 3);
	munit_assert_int(pt_log[0], ==, 1);
	munit_assert_int(pt_log[1], ==, 2);
	munit_assert_int(pt_log[2], ==, 3);
	return MUNIT_OK;
}

/* yield watchdog: a coro over its per-loop time budget is reported by
 * xtc_yield_check, and xtc_yield_if_due lets it cooperate so a peer
 * coro on the same loop makes progress. */
static int g_yc_over;          /* coro saw itself over budget */
static int g_yc_ticks;         /* peer ran while the hog computed */
static int g_yc_hog_done;

static void
busy_ns(int64_t ns)
{
	int64_t start = 0, now = 0;
	(void)__os_clock_mono(&start);
	do { (void)__os_clock_mono(&now); } while (now - start < ns);
}

static intptr_t
yc_hog(void *arg)
{
	int i;
	(void)arg;
	/* ~24ms of compute in ~2ms slices, cooperating each slice. */
	for (i = 0; i < 12; i++) {
		busy_ns(3LL * 1000 * 1000);    /* 3ms > 2ms budget */
		if (xtc_yield_check())
			g_yc_over = 1;
		(void)xtc_yield_if_due();
	}
	g_yc_hog_done = 1;
	return 0;
}

static intptr_t
yc_ticker(void *arg)
{
	(void)arg;
	while (!g_yc_hog_done && g_yc_ticks < 1000) {
		g_yc_ticks++;
		xtc_yield();
	}
	return 0;
}

static MunitResult
test_yield_watchdog(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_task_t *hog = NULL, *tick = NULL;
	(void)p; (void)d;

	g_yc_over = g_yc_ticks = g_yc_hog_done = 0;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	/* Budget set before the first dispatch so the hog's quanta are
	 * timed from the start. */
	xtc_yield_set_budget(loop, 2LL * 1000 * 1000);   /* 2ms */
	munit_assert_int(xtc_async(loop, yc_hog, NULL, &hog), ==, XTC_OK);
	munit_assert_int(xtc_async(loop, yc_ticker, NULL, &tick), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	munit_assert_int(g_yc_over, ==, 1);                 /* saw over-budget */
	munit_assert_uint64(xtc_yield_due_count(loop), >, 0);
	munit_assert_int(g_yc_ticks, >=, 2);                /* peer interleaved */
	munit_assert_int(g_yc_hog_done, ==, 1);

	/* Off a loop / no budget: never due. */
	munit_assert_int(xtc_yield_check(), ==, 0);

	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/C1_C2_basic",       test_basic,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/C3_nested",         test_nested,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/C4_yield",          test_yield,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/C5_locals_survive", test_locals_survive,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/C9_many_coros",     test_many_coros,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/C7_coro_done",      test_coro_done,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/P1_pt_basic",       test_pt_basic,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/yield_watchdog",    test_yield_watchdog,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m4/async", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
