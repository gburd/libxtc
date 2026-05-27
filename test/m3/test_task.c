/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m3/test_task.c -- verifies M3_CLAIMS.md Ts1-Ts6.
 */

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"

/* [Ts1] */
static int dummy(xtc_task_t *s, void *u) { (void)s; (void)u; return XTC_TASK_DONE; }

static MunitResult
test_spawn_bad_args(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_task_spawn(NULL, dummy, NULL, NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_task_spawn(loop, NULL,  NULL, NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Ts2] */
static int observed_int;
static int task_record(xtc_task_t *s, void *u) {
	(void)s;
	observed_int = *(int *)u;
	return XTC_TASK_DONE;
}

static MunitResult
test_user_pointer(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	int marker = 0xC0DE;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	observed_int = 0;
	munit_assert_int(xtc_task_spawn(loop, task_record, &marker, NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(observed_int, ==, 0xC0DE);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Ts3] */
static int call_count;
static int task_count_then_done(xtc_task_t *s, void *u) {
	(void)s; (void)u; call_count++; return XTC_TASK_DONE;
}

static MunitResult
test_done_runs_once(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	(void)p; (void)d;
	call_count = 0;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_task_spawn(loop, task_count_then_done, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(call_count, ==, 1);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Ts4] */
struct rs { int n; int target; };
static int task_resched_n(xtc_task_t *s, void *u) {
	struct rs *c = u;
	(void)s;
	c->n++;
	if (c->n >= c->target) return XTC_TASK_DONE;
	return XTC_TASK_RESCHED;
}

static MunitResult
test_resched_loops(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct rs c = { 0, 100 };
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_task_spawn(loop, task_resched_n, &c, NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(c.n, ==, 100);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Ts5] spawn from inside */
static int leaf_hits;
static int leaf(xtc_task_t *s, void *u) { (void)s; (void)u; leaf_hits++; return XTC_TASK_DONE; }

struct sp { xtc_loop_t *loop; int spawned; };
static int task_spawner(xtc_task_t *s, void *u) {
	struct sp *c = u;
	(void)s;
	if (!c->spawned) {
		(void)xtc_task_spawn(c->loop, leaf, NULL, NULL);
		(void)xtc_task_spawn(c->loop, leaf, NULL, NULL);
		(void)xtc_task_spawn(c->loop, leaf, NULL, NULL);
		c->spawned = 1;
	}
	return XTC_TASK_DONE;
}

static MunitResult
test_spawn_inside_task(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct sp c;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	c.loop = loop; c.spawned = 0;
	leaf_hits = 0;
	munit_assert_int(xtc_task_spawn(loop, task_spawner, &c, NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(leaf_hits, ==, 3);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Ts6] N independent tasks */
#define TS6_N  20
struct ind { int id; int counter; int target; };
static int task_indep(xtc_task_t *s, void *u) {
	struct ind *c = u;
	(void)s;
	c->counter++;
	if (c->counter >= c->target) return XTC_TASK_DONE;
	return XTC_TASK_RESCHED;
}

static MunitResult
test_n_tasks_independent(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct ind ctx[TS6_N];
	int i;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	for (i = 0; i < TS6_N; i++) {
		ctx[i].id = i; ctx[i].counter = 0; ctx[i].target = 1 + i * 3;
		munit_assert_int(xtc_task_spawn(loop, task_indep, &ctx[i], NULL),
		    ==, XTC_OK);
	}
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	for (i = 0; i < TS6_N; i++)
		munit_assert_int(ctx[i].counter, ==, ctx[i].target);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/Ts1_bad_args",          test_spawn_bad_args,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Ts2_user_pointer",      test_user_pointer,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Ts3_done_runs_once",    test_done_runs_once,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Ts4_resched_loops",     test_resched_loops,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Ts5_spawn_inside",      test_spawn_inside_task,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Ts6_independent",       test_n_tasks_independent,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m3/task", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
