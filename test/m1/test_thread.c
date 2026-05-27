/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m1/test_thread.c — verifies M1_CLAIMS.md T1–T7.
 */

#include "munit.h"
#include "xtc_int.h"

/* [T1, T2] */
static void *
worker_return_arg(void *arg)
{
	return arg;
}

static MunitResult
test_create_join(const MunitParameter p[], void *d)
{
	__os_thread_t thr = {0};
	void *ret = NULL;
	int marker = 0xC0FFEE;
	(void)p; (void)d;
	munit_assert_int(__os_thread_create(&thr, worker_return_arg, &marker),
	    ==, XTC_OK);
	munit_assert_int(__os_thread_join(&thr, &ret), ==, XTC_OK);
	munit_assert_ptr(ret, ==, &marker);
	return MUNIT_OK;
}

/* [T3] detach */
static int detach_ran;

static void *
detach_worker(void *arg)
{
	(void)arg;
	__os_atomic_store_i32(&detach_ran, 1);
	return NULL;
}

static MunitResult
test_detach(const MunitParameter p[], void *d)
{
	__os_thread_t thr = {0};
	int spins = 0;
	(void)p; (void)d;
	__os_atomic_store_i32(&detach_ran, 0);
	munit_assert_int(__os_thread_create(&thr, detach_worker, NULL),
	    ==, XTC_OK);
	munit_assert_int(__os_thread_detach(&thr), ==, XTC_OK);
	while (__os_atomic_load_i32(&detach_ran) == 0 && spins++ < 1000) {
		(void)__os_sleep_ns(1 * XTC_NS_PER_MS);
	}
	munit_assert_int32(__os_atomic_load_i32(&detach_ran), ==, 1);
	return MUNIT_OK;
}

/* [T4] self */
static MunitResult
test_self(const MunitParameter p[], void *d)
{
	__os_thread_t me = {0};
	(void)p; (void)d;
	munit_assert_int(__os_thread_self(&me), ==, XTC_OK);
	munit_assert_not_null(me.opaque);
	__os_free(me.opaque);  /* allocator-symmetric: self() malloc'd it */
	return MUNIT_OK;
}

/* [T5] yield */
static MunitResult
test_yield_returns(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	__os_thread_yield();
	return MUNIT_OK;
}

/* [T6] setname */
static MunitResult
test_setname(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	munit_assert_int(__os_thread_setname("xtc-test"), ==, XTC_OK);
	munit_assert_int(__os_thread_setname(NULL), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* [T7] N independent partial sums. */
#define T7_THREADS  4
#define T7_PER      10000

struct t7_job { int64_t sum; int from; int to; };

static void *
t7_worker(void *arg)
{
	struct t7_job *j = arg;
	int i;
	j->sum = 0;
	for (i = j->from; i < j->to; i++)
		j->sum += i;
	return NULL;
}

static MunitResult
test_n_threads_independent(const MunitParameter p[], void *d)
{
	__os_thread_t thr[T7_THREADS] = {{0}};
	struct t7_job  jobs[T7_THREADS];
	int64_t total = 0, expected = 0;
	int i;
	(void)p; (void)d;
	for (i = 0; i < T7_THREADS; i++) {
		jobs[i].from = i * T7_PER;
		jobs[i].to   = (i + 1) * T7_PER;
		munit_assert_int(__os_thread_create(&thr[i], t7_worker,
		    &jobs[i]), ==, XTC_OK);
	}
	for (i = 0; i < T7_THREADS; i++) {
		munit_assert_int(__os_thread_join(&thr[i], NULL), ==, XTC_OK);
		total += jobs[i].sum;
	}
	for (i = 0; i < T7_THREADS * T7_PER; i++) expected += i;
	munit_assert_int64(total, ==, expected);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/T1_T2_create_join",  test_create_join, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/T3_detach",          test_detach,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/T4_self",            test_self,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/T5_yield",           test_yield_returns,NULL, NULL,MUNIT_TEST_OPTION_NONE, NULL },
	{ "/T6_setname",         test_setname,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/T7_independent",     test_n_threads_independent, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m1/thread", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
