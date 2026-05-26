/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * test/m1/test_atomic.c — verifies M1_CLAIMS.md A1–A8.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#include "munit.h"
#include "xtc_int.h"

/* [A1, A2] */
static MunitResult
test_load_store(const MunitParameter p[], void *d)
{
	int32_t  v32 = 0; int64_t v64 = 0;
	uint32_t u32 = 0; uint64_t u64 = 0;
	(void)p; (void)d;

	__os_atomic_store_i32(&v32, 0x12345678);
	munit_assert_int32(__os_atomic_load_i32(&v32), ==, 0x12345678);

	__os_atomic_store_i64(&v64, 0x1234567890abcdefLL);
	munit_assert_int64(__os_atomic_load_i64(&v64), ==, 0x1234567890abcdefLL);

	__os_atomic_store_u32(&u32, 0xdeadbeef);
	munit_assert_uint32(__os_atomic_load_u32(&u32), ==, 0xdeadbeef);

	__os_atomic_store_u64(&u64, 0xfeedfacecafebabeULL);
	munit_assert_uint64(__os_atomic_load_u64(&u64), ==, 0xfeedfacecafebabeULL);
	return MUNIT_OK;
}

/* [A3] CAS success and failure paths. */
static MunitResult
test_cas(const MunitParameter p[], void *d)
{
	int32_t v = 100, expect, ok;
	(void)p; (void)d;

	expect = 100;
	ok = __os_atomic_cas_i32(&v, &expect, 200);
	munit_assert_int(ok, ==, 1);
	munit_assert_int32(v, ==, 200);
	munit_assert_int32(expect, ==, 100);

	expect = 999;        /* wrong */
	ok = __os_atomic_cas_i32(&v, &expect, 0);
	munit_assert_int(ok, ==, 0);
	munit_assert_int32(v, ==, 200);
	munit_assert_int32(expect, ==, 200);   /* updated to actual */
	return MUNIT_OK;
}

/* [A4] */
static MunitResult
test_fetch_add(const MunitParameter p[], void *d)
{
	int64_t v = 10;
	int64_t prev;
	(void)p; (void)d;
	prev = __os_atomic_fetch_add_i64(&v, 5);
	munit_assert_int64(prev, ==, 10);
	munit_assert_int64(v,    ==, 15);
	prev = __os_atomic_fetch_add_i64(&v, -3);
	munit_assert_int64(prev, ==, 15);
	munit_assert_int64(v,    ==, 12);
	return MUNIT_OK;
}

/* [A7] Linearizability via fetch_add. */
#define A7_THREADS  8
#define A7_ITERS    50000

static int64_t a7_counter;

static void *
a7_worker(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < A7_ITERS; i++)
		(void)__os_atomic_fetch_add_i64(&a7_counter, 1);
	return NULL;
}

static MunitResult
test_linearizable_counter(const MunitParameter p[], void *d)
{
	pthread_t th[A7_THREADS];
	int i;
	(void)p; (void)d;
	a7_counter = 0;
	for (i = 0; i < A7_THREADS; i++)
		munit_assert_int(pthread_create(&th[i], NULL, a7_worker, NULL),
		    ==, 0);
	for (i = 0; i < A7_THREADS; i++)
		munit_assert_int(pthread_join(th[i], NULL), ==, 0);
	munit_assert_int64(a7_counter, ==, (int64_t)A7_THREADS * A7_ITERS);
	return MUNIT_OK;
}

/* [A8] Linearizability via CAS-loop. */
#define A8_THREADS  8
#define A8_ITERS    25000

static int64_t a8_counter;

static void *
a8_worker(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < A8_ITERS; i++) {
		int64_t cur, next;
		do {
			cur  = __os_atomic_load_i64(&a8_counter);
			next = cur + 1;
		} while (!__os_atomic_cas_i64(&a8_counter, &cur, next));
	}
	return NULL;
}

static MunitResult
test_linearizable_cas(const MunitParameter p[], void *d)
{
	pthread_t th[A8_THREADS];
	int i;
	(void)p; (void)d;
	a8_counter = 0;
	for (i = 0; i < A8_THREADS; i++)
		munit_assert_int(pthread_create(&th[i], NULL, a8_worker, NULL),
		    ==, 0);
	for (i = 0; i < A8_THREADS; i++)
		munit_assert_int(pthread_join(th[i], NULL), ==, 0);
	munit_assert_int64(a8_counter, ==, (int64_t)A8_THREADS * A8_ITERS);
	return MUNIT_OK;
}

/* Pointer atomics smoke test. */
static MunitResult
test_atomic_ptr(const MunitParameter p[], void *d)
{
	void *slot = NULL;
	void *ex;
	int x = 1, y = 2, ok;
	(void)p; (void)d;
	__os_atomic_store_ptr(&slot, &x);
	munit_assert_ptr(__os_atomic_load_ptr(&slot), ==, &x);
	ex = &x;
	ok = __os_atomic_cas_ptr(&slot, &ex, &y);
	munit_assert_int(ok, ==, 1);
	munit_assert_ptr(slot, ==, &y);
	ex = NULL;
	ok = __os_atomic_cas_ptr(&slot, &ex, &x);
	munit_assert_int(ok, ==, 0);
	munit_assert_ptr(ex, ==, &y);
	return MUNIT_OK;
}

/* fence + pause: presence-asserted (compiles + links). */
static MunitResult
test_fence_pause(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	__os_atomic_fence();
	__os_pause();
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/A1_A2_load_store",         test_load_store, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/A3_cas",                    test_cas, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/A4_fetch_add",              test_fetch_add, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/A7_linearizable_counter",   test_linearizable_counter, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/A8_linearizable_cas",       test_linearizable_cas, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/ptr",                       test_atomic_ptr, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/A5_A6_fence_pause",         test_fence_pause, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m1/atomic", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
