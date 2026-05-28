/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/m12/test_stats.c -- xtc_stats verification.
 */

#include "munit.h"
#include "xtc.h"
#include "xtc_stats.h"
#include "xtc_int.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>

/* ---- counter ---- */

static MunitResult
test_counter_basic(const MunitParameter p[], void *d)
{
	xtc_counter_t *c;
	(void)p; (void)d;
	munit_assert_int(xtc_counter_create("test.c", &c), ==, XTC_OK);
	munit_assert_uint64(xtc_counter_read(c), ==, 0);
	xtc_counter_inc(c);
	munit_assert_uint64(xtc_counter_read(c), ==, 1);
	xtc_counter_add(c, 100);
	munit_assert_uint64(xtc_counter_read(c), ==, 101);
	xtc_counter_destroy(c);
	return MUNIT_OK;
}

#define N_THREADS  8
#define N_INC      100000

static xtc_counter_t *g_concurrent_c;

static void *
inc_worker(void *arg)
{
	int n = (int)(intptr_t)arg;
	int i;
	for (i = 0; i < n; i++) xtc_counter_inc(g_concurrent_c);
	return NULL;
}

static MunitResult
test_counter_concurrent(const MunitParameter p[], void *d)
{
	pthread_t th[N_THREADS];
	int i;
	(void)p; (void)d;
	munit_assert_int(xtc_counter_create("test.cc", &g_concurrent_c), ==,
	    XTC_OK);
	for (i = 0; i < N_THREADS; i++)
		pthread_create(&th[i], NULL, inc_worker,
		    (void *)(intptr_t)N_INC);
	for (i = 0; i < N_THREADS; i++) pthread_join(th[i], NULL);
	munit_assert_uint64(xtc_counter_read(g_concurrent_c), ==,
	    (uint64_t)N_THREADS * N_INC);
	xtc_counter_destroy(g_concurrent_c);
	return MUNIT_OK;
}

/* ---- gauge ---- */

static MunitResult
test_gauge_basic(const MunitParameter p[], void *d)
{
	xtc_gauge_t *g;
	(void)p; (void)d;
	munit_assert_int(xtc_gauge_create("test.g", &g), ==, XTC_OK);
	munit_assert_int64(xtc_gauge_read(g), ==, 0);
	xtc_gauge_set(g, 42);
	munit_assert_int64(xtc_gauge_read(g), ==, 42);
	xtc_gauge_add(g, -10);
	munit_assert_int64(xtc_gauge_read(g), ==, 32);
	xtc_gauge_destroy(g);
	return MUNIT_OK;
}

/* ---- histogram ---- */

static MunitResult
test_hist_basic(const MunitParameter p[], void *d)
{
	xtc_hist_t *h;
	int i;
	int64_t p50, p99;
	(void)p; (void)d;
	munit_assert_int(xtc_hist_create("test.h", &h), ==, XTC_OK);
	munit_assert_uint64(xtc_hist_count(h), ==, 0);

	/* Record 1..1000 (microseconds expressed as ns). */
	for (i = 1; i <= 1000; i++)
		xtc_hist_record(h, (int64_t)i * 1000);

	munit_assert_uint64(xtc_hist_count(h), ==, 1000);
	p50 = xtc_hist_quantile(h, 0.50);
	p99 = xtc_hist_quantile(h, 0.99);
	/* p50 should be near 500us; p99 near 990us.  Tolerate the
	 * histogram's bucketing imprecision. */
	munit_assert_int64(p50, >, 100000);
	munit_assert_int64(p50, <, 1000000);
	munit_assert_int64(p99, >=, p50);

	xtc_hist_destroy(h);
	return MUNIT_OK;
}

/* ---- iteration ---- */

struct visit_count { int n; };

static int
__count_visit(const char *name, xtc_metric_kind_t kind,
    const void *handle, void *user)
{
	struct visit_count *c = user;
	(void)name; (void)kind; (void)handle;
	c->n++;
	return 0;
}

static MunitResult
test_iterate(const MunitParameter p[], void *d)
{
	xtc_counter_t *c1, *c2;
	xtc_gauge_t   *g1;
	xtc_hist_t    *h1;
	struct visit_count vc = { 0 };
	(void)p; (void)d;
	munit_assert_int(xtc_counter_create("it.c1", &c1), ==, XTC_OK);
	munit_assert_int(xtc_counter_create("it.c2", &c2), ==, XTC_OK);
	munit_assert_int(xtc_gauge_create  ("it.g1", &g1), ==, XTC_OK);
	munit_assert_int(xtc_hist_create   ("it.h1", &h1), ==, XTC_OK);

	munit_assert_int(xtc_metrics_iterate(__count_visit, &vc), >=, 4);
	munit_assert_int(vc.n, >=, 4);

	xtc_counter_destroy(c1);
	xtc_counter_destroy(c2);
	xtc_gauge_destroy(g1);
	xtc_hist_destroy(h1);
	return MUNIT_OK;
}

/* ---- prometheus dump ---- */

static MunitResult
test_dump_prometheus(const MunitParameter p[], void *d)
{
	xtc_counter_t *c;
	xtc_gauge_t   *g;
	int pipefd[2];
	char buf[1024] = {0};
	ssize_t n;
	(void)p; (void)d;
	munit_assert_int(pipe(pipefd), ==, 0);
	munit_assert_int(xtc_counter_create("prom.c", &c), ==, XTC_OK);
	munit_assert_int(xtc_gauge_create  ("prom.g", &g), ==, XTC_OK);
	xtc_counter_add(c, 7);
	xtc_gauge_set(g, 99);

	munit_assert_int(xtc_metrics_dump_prometheus(pipefd[1]), >, 0);
	close(pipefd[1]);

	n = read(pipefd[0], buf, sizeof buf - 1);
	munit_assert_size((size_t)n, >, 0);
	munit_assert_ptr_not_null(strstr(buf, "prom.c"));
	munit_assert_ptr_not_null(strstr(buf, "prom.g"));

	close(pipefd[0]);
	xtc_counter_destroy(c);
	xtc_gauge_destroy(g);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/counter_basic",      test_counter_basic,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/counter_concurrent", test_counter_concurrent, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/gauge_basic",        test_gauge_basic,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hist_basic",         test_hist_basic,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/iterate",            test_iterate,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/dump_prometheus",    test_dump_prometheus,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = {
	"/m12/stats", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
