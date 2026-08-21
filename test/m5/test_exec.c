/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m5/test_exec.c -- verifies M5_CLAIMS.md Ex1-Ex4, Sp1-Sp3.
 */

#include <stdint.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_int.h"
/* [Ex1, Ex2] init/fini round-trip. */
static MunitResult
test_init_fini(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	(void)p; (void)d;
	munit_assert_int(xtc_exec_init(&e, 4), ==, XTC_OK);
	munit_assert_int(xtc_exec_n_loops(e), ==, 4);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);

	/* Ex1.b: n_loops <= 0 defaults to __os_ncpus(). */
	munit_assert_int(xtc_exec_init(&e, 0), ==, XTC_OK);
	{
		int expect = __os_ncpus();
		if (expect <= 0) expect = 4;
		munit_assert_int(xtc_exec_n_loops(e), ==, expect);
	}
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);

	munit_assert_int(xtc_exec_init(NULL, 4), ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_fini(NULL), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* [Ex3] run-until-done with no tasks should return immediately. */
static MunitResult
test_run_until_done(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	(void)p; (void)d;
	munit_assert_int(xtc_exec_init(&e, 2), ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Ex4] n_loops sanity. */
static MunitResult
test_n_loops(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	(void)p; (void)d;
	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	munit_assert_int(xtc_exec_n_loops(e), ==, 1);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	munit_assert_int(xtc_exec_init(&e, 7), ==, XTC_OK);
	munit_assert_int(xtc_exec_n_loops(e), ==, 7);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Sp1] basic spawn+run. */
static int sp1_ran;
static int sp1_task(xtc_task_t *self, void *u) {
	(void)self; (void)u;
	__os_atomic_fetch_add_i32(&sp1_ran, 1);
	return XTC_TASK_DONE;
}

static MunitResult
test_spawn(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	(void)p; (void)d;
	__os_atomic_store_i32(&sp1_ran, 0);
	munit_assert_int(xtc_exec_init(&e, 4), ==, XTC_OK);
	munit_assert_int(xtc_exec_spawn(e, sp1_task, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(__os_atomic_load_i32(&sp1_ran), ==, 1);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Sp2] spawn_on places on the requested loop. */
static int sp2_observed_id;
static int sp2_task(xtc_task_t *self, void *u) {
	(void)self; (void)u;
	sp2_observed_id = xtc_exec_loop_id();
	return XTC_TASK_DONE;
}

static MunitResult
test_spawn_on(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	(void)p; (void)d;
	munit_assert_int(xtc_exec_init(&e, 4), ==, XTC_OK);
	sp2_observed_id = -1;
	munit_assert_int(xtc_exec_spawn_on(e, 2, sp2_task, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(sp2_observed_id, ==, 2);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Sp3] N spawned tasks: shared atomic counter sums correctly. */
#define SP3_N 64
static int64_t sp3_counter;
static int sp3_task(xtc_task_t *self, void *u) {
	int64_t inc = (intptr_t)u;
	(void)self;
	(void)__os_atomic_fetch_add_i64(&sp3_counter, inc);
	return XTC_TASK_DONE;
}

static MunitResult
test_n_spawned_sum(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	int i;
	int64_t expected = 0;
	(void)p; (void)d;
	munit_assert_int(xtc_exec_init(&e, 4), ==, XTC_OK);
	__os_atomic_store_i64(&sp3_counter, 0);
	for (i = 0; i < SP3_N; i++) {
		expected += i + 1;
		munit_assert_int(xtc_exec_spawn(e, sp3_task,
		    (void *)(intptr_t)(i + 1), NULL), ==, XTC_OK);
	}
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int64(__os_atomic_load_i64(&sp3_counter), ==, expected);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* A12: Seastar-style per-shard API. */
static int g_shard_ran[32];            /* one slot per shard -- no contention */
static int g_shard_count_seen;
static int
shard_probe(xtc_task_t *self, void *arg)
{
	int id = xtc_shard_id();
	(void)self; (void)arg;
	if (id >= 0 && id < 32)
		__os_atomic_store_i32(&g_shard_ran[id], 1);
	__os_atomic_store_i32(&g_shard_count_seen, xtc_shard_count());
	return XTC_TASK_DONE;
}

static MunitResult
test_shard_id(const MunitParameter p[], void *d)
{
	xtc_exec_t *e = NULL;
	int i, n = 4;
	(void)p; (void)d;

	/* Off a loop: id -1, count 0. */
	munit_assert_int(xtc_shard_id(), ==, -1);
	munit_assert_int(xtc_shard_count(), ==, 0);

	for (i = 0; i < 32; i++) __os_atomic_store_i32(&g_shard_ran[i], 0);
	__os_atomic_store_i32(&g_shard_count_seen, 0);
	munit_assert_int(xtc_exec_init(&e, n), ==, XTC_OK);
	/* Pin one probe to each shard so every shard id is observed. */
	for (i = 0; i < n; i++)
		munit_assert_int(xtc_exec_spawn_on(e, i, shard_probe, NULL, NULL),
		    ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);

	/* Every shard 0..n-1 ran and reported the full count. */
	for (i = 0; i < n; i++)
		munit_assert_int(__os_atomic_load_i32(&g_shard_ran[i]), ==, 1);
	munit_assert_int(__os_atomic_load_i32(&g_shard_count_seen), ==, n);
	return MUNIT_OK;
}

/* Policy knobs: set/get round-trip and NULL-guards. */
static MunitResult
test_policy_knobs(const MunitParameter p[], void *d)
{
	xtc_exec_t *e = NULL;
	(void)p; (void)d;

	/* NULL exec: setters are no-ops, getters return 0 (never crash). */
	xtc_exec_set_service_mode(NULL, 1);
	xtc_exec_set_eager_rebalance(NULL, 1);
	xtc_exec_set_steal_backoff(NULL, 1);
	munit_assert_int(xtc_exec_get_service_mode(NULL), ==, 0);
	munit_assert_int(xtc_exec_get_eager_rebalance(NULL), ==, 0);
	munit_assert_int(xtc_exec_get_steal_backoff(NULL), ==, 0);

	munit_assert_int(xtc_exec_init(&e, 2), ==, XTC_OK);

	/* Each knob toggles and reads back both states. */
	munit_assert_int(xtc_exec_get_service_mode(e), ==, 0);
	xtc_exec_set_service_mode(e, 1);
	munit_assert_int(xtc_exec_get_service_mode(e), ==, 1);
	xtc_exec_set_service_mode(e, 0);
	munit_assert_int(xtc_exec_get_service_mode(e), ==, 0);

	munit_assert_int(xtc_exec_get_eager_rebalance(e), ==, 0);
	xtc_exec_set_eager_rebalance(e, 1);
	munit_assert_int(xtc_exec_get_eager_rebalance(e), ==, 1);
	xtc_exec_set_eager_rebalance(e, 0);
	munit_assert_int(xtc_exec_get_eager_rebalance(e), ==, 0);

	munit_assert_int(xtc_exec_get_steal_backoff(e), ==, 0);
	xtc_exec_set_steal_backoff(e, 1);
	munit_assert_int(xtc_exec_get_steal_backoff(e), ==, 1);
	xtc_exec_set_steal_backoff(e, 0);
	munit_assert_int(xtc_exec_get_steal_backoff(e), ==, 0);

	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* Accessors and error paths: loop, loop_stats, n_loops, spawn/async NULL
 * and out-of-range branches. */
static int noop_task(xtc_task_t *self, void *u)
{ (void)self; (void)u; return XTC_TASK_DONE; }
static intptr_t noop_coro(void *arg) { (void)arg; return 0; }

static MunitResult
test_accessors_errors(const MunitParameter p[], void *d)
{
	xtc_exec_t *e = NULL;
	xtc_loop_stats_t st;
	(void)p; (void)d;

	/* NULL exec on every accessor. */
	munit_assert_int(xtc_exec_n_loops(NULL), ==, 0);
	munit_assert_null(xtc_exec_loop(NULL, 0));
	munit_assert_int(xtc_exec_loop_stats(NULL, 0, &st), ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_stop(NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_spawn(NULL, noop_task, NULL, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_spawn_on(NULL, 0, noop_task, NULL, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_async(NULL, noop_coro, NULL, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_async_on(NULL, 0, noop_coro, NULL, NULL),
	    ==, XTC_E_INVAL);

	munit_assert_int(xtc_exec_init(&e, 3), ==, XTC_OK);

	/* loop: valid idx returns non-NULL, out-of-range returns NULL. */
	munit_assert_not_null(xtc_exec_loop(e, 0));
	munit_assert_not_null(xtc_exec_loop(e, 2));
	munit_assert_null(xtc_exec_loop(e, -1));
	munit_assert_null(xtc_exec_loop(e, 3));

	/* loop_stats: NULL out, out-of-range idx, then a valid read. */
	munit_assert_int(xtc_exec_loop_stats(e, 0, NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_loop_stats(e, -1, &st), ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_loop_stats(e, 3, &st), ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_loop_stats(e, 1, &st), ==, XTC_OK);
	/* Fresh executor: both counters start at zero. */
	munit_assert_uint64(st.tasks_run, ==, 0);
	munit_assert_uint64(st.steals, ==, 0);

	/* spawn_on / async_on out-of-range idx rejected. */
	munit_assert_int(xtc_exec_spawn_on(e, -1, noop_task, NULL, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_spawn_on(e, 3, noop_task, NULL, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_async_on(e, -1, noop_coro, NULL, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_async_on(e, 3, noop_coro, NULL, NULL),
	    ==, XTC_E_INVAL);

	/* Valid default-placement spawn + async, then run drains them. */
	munit_assert_int(xtc_exec_spawn(e, noop_task, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_async(e, noop_coro, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_async_on(e, 1, noop_coro, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);

	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* Proportional-share class accessors (v1.35.0 API): the create contract
 * (shares 1..1000, latency_ns >= 0, non-NULL loop/out) and the read-back
 * accessors incl. their NULL-returns-0 error path.  These are exercised
 * by the DST/PBT sched-shares tests but had no standard-make-check unit
 * coverage. */
static MunitResult
test_exec_class_accessors(const MunitParameter p[], void *d)
{
	xtc_exec_t *e = NULL;
	xtc_loop_t *loop;
	xtc_exec_class_t c = NULL;
	(void)p; (void)d;

	/* Accessors are NULL-safe and return 0. */
	munit_assert_int(xtc_exec_class_shares(NULL), ==, 0);
	munit_assert_int64(xtc_exec_class_latency(NULL), ==, 0);
	munit_assert_int64(xtc_exec_class_runs(NULL), ==, 0);
	munit_assert_int64(xtc_exec_class_vruntime(NULL), ==, 0);

	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	loop = xtc_exec_loop(e, 0);
	munit_assert_not_null(loop);

	/* create validation: NULL args, shares out of 1..1000, negative
	 * latency all rejected. */
	munit_assert_int(xtc_exec_class_create(NULL, 3, 0, &c), ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_class_create(loop, 3, 0, NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_class_create(loop, 0, 0, &c), ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_class_create(loop, 1001, 0, &c), ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_class_create(loop, 3, -1, &c), ==, XTC_E_INVAL);

	/* Valid create; accessors read back what was set. */
	munit_assert_int(xtc_exec_class_create(loop, 7, 250000, &c), ==, XTC_OK);
	munit_assert_not_null(c);
	munit_assert_int(xtc_exec_class_shares(c), ==, 7);
	munit_assert_int64(xtc_exec_class_latency(c), ==, 250000);
	munit_assert_int64(xtc_exec_class_runs(c), ==, 0);      /* not run yet */
	munit_assert_int64(xtc_exec_class_vruntime(c), ==, 0);

	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/Ex1_Ex2_init_fini",       test_init_fini,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Ex3_run_until_done",      test_run_until_done,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Ex4_n_loops",             test_n_loops,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Sp1_spawn",               test_spawn,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Sp2_spawn_on",            test_spawn_on,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Sp3_n_spawned_sum",       test_n_spawned_sum,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/A12_shard_id",            test_shard_id,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/policy_knobs",            test_policy_knobs,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/accessors_errors",        test_accessors_errors,NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/class_accessors",         test_exec_class_accessors, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m5/exec", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
