/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m1/test_tls.c — verifies M1_CLAIMS.md L1–L5.
 */

#include "munit.h"
#include "xtc_int.h"

/* [L1, L2, L3] */
static MunitResult
test_set_get_unset(const MunitParameter p[], void *d)
{
	__os_tls_key_t k;
	int x = 7;
	(void)p; (void)d;
	munit_assert_int(__os_tls_create(&k, NULL), ==, XTC_OK);
	munit_assert_ptr(__os_tls_get(k), ==, NULL);   /* L3 */
	munit_assert_int(__os_tls_set(k, &x), ==, XTC_OK);
	munit_assert_ptr(__os_tls_get(k), ==, &x);
	munit_assert_int(__os_tls_destroy(k), ==, XTC_OK);
	return MUNIT_OK;
}

/* [L4] thread isolation */
static __os_tls_key_t l4_key;
static int            l4_a, l4_b;
static int            l4_observed_a, l4_observed_b;

static void *
l4_worker(void *arg)
{
	int *value = arg;
	(void)__os_tls_set(l4_key, value);
	__os_thread_yield();
	if (value == &l4_a) l4_observed_a = *(int *)__os_tls_get(l4_key);
	else                l4_observed_b = *(int *)__os_tls_get(l4_key);
	return NULL;
}

static MunitResult
test_thread_isolation(const MunitParameter p[], void *d)
{
	__os_thread_t a = {0}, b = {0};
	(void)p; (void)d;
	munit_assert_int(__os_tls_create(&l4_key, NULL), ==, XTC_OK);
	l4_a = 11; l4_b = 22;
	munit_assert_int(__os_thread_create(&a, l4_worker, &l4_a), ==, XTC_OK);
	munit_assert_int(__os_thread_create(&b, l4_worker, &l4_b), ==, XTC_OK);
	munit_assert_int(__os_thread_join(&a, NULL), ==, XTC_OK);
	munit_assert_int(__os_thread_join(&b, NULL), ==, XTC_OK);
	munit_assert_int(l4_observed_a, ==, 11);
	munit_assert_int(l4_observed_b, ==, 22);
	munit_assert_int(__os_tls_destroy(l4_key), ==, XTC_OK);
	return MUNIT_OK;
}

/* [L5] destructor */
static int l5_dtor_calls;

static void
l5_dtor(void *value)
{
	(void)value;
	(void)__os_atomic_fetch_add_i32(&l5_dtor_calls, 1);
}

static __os_tls_key_t l5_key;

static void *
l5_worker(void *arg)
{
	(void)arg;
	(void)__os_tls_set(l5_key, (void *)0xdeadbeef);
	return NULL;
}

static MunitResult
test_destructor(const MunitParameter p[], void *d)
{
	__os_thread_t thr = {0};
	(void)p; (void)d;
	__os_atomic_store_i32(&l5_dtor_calls, 0);
	munit_assert_int(__os_tls_create(&l5_key, l5_dtor), ==, XTC_OK);
	munit_assert_int(__os_thread_create(&thr, l5_worker, NULL), ==, XTC_OK);
	munit_assert_int(__os_thread_join(&thr, NULL), ==, XTC_OK);
	munit_assert_int32(__os_atomic_load_i32(&l5_dtor_calls), ==, 1);
	munit_assert_int(__os_tls_destroy(l5_key), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/L1_L2_L3_set_get",  test_set_get_unset,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/L4_isolation",      test_thread_isolation, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/L5_destructor",     test_destructor,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m1/tls", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
