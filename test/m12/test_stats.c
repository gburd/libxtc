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
#include <stdio.h>
#include <string.h>
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

/* ---- dist (mergeable online mean/variance) ---- */

static MunitResult
test_dist_basic(const MunitParameter p[], void *d)
{
	/* Textbook Welford set: mean 5, population variance 4. */
	static const double sample[] = { 2, 4, 4, 4, 5, 5, 7, 9 };
	xtc_dist_t *dist;
	size_t i;
	(void)p; (void)d;
	munit_assert_int(xtc_dist_create("test.d", &dist), ==, XTC_OK);

	/* (c) empty dist: no divide-by-zero, all zero. */
	munit_assert_uint64(xtc_dist_count(dist), ==, 0);
	munit_assert_double(xtc_dist_mean(dist), ==, 0.0);
	munit_assert_double(xtc_dist_variance(dist), ==, 0.0);
	munit_assert_double(xtc_dist_stddev(dist), ==, 0.0);

	/* (d) count<2 variance is 0.0. */
	xtc_dist_record(dist, 42.0);
	munit_assert_uint64(xtc_dist_count(dist), ==, 1);
	munit_assert_double(xtc_dist_mean(dist), ==, 42.0);
	munit_assert_double(xtc_dist_variance(dist), ==, 0.0);

	xtc_dist_destroy(dist);

	/* (a) known sequence: mean/variance/stddev to a small epsilon. */
	munit_assert_int(xtc_dist_create("test.d2", &dist), ==, XTC_OK);
	for (i = 0; i < sizeof sample / sizeof sample[0]; i++)
		xtc_dist_record(dist, sample[i]);
	munit_assert_uint64(xtc_dist_count(dist), ==, 8);
	munit_assert_double_equal(xtc_dist_mean(dist), 5.0, 9);
	munit_assert_double_equal(xtc_dist_variance(dist), 4.0, 9);
	munit_assert_double_equal(xtc_dist_stddev(dist), 2.0, 9);
	xtc_dist_destroy(dist);
	xtc_dist_destroy(NULL);   /* NULL no-op */
	return MUNIT_OK;
}

#define N_DIST_THREADS  8
#define N_DIST_REC      50000

static xtc_dist_t *g_concurrent_d;

static void *
dist_worker(void *arg)
{
	int n = (int)(intptr_t)arg;
	int i;
	/* Every thread records the same constant so the merged mean is
	 * exactly that constant and the variance is exactly 0 regardless
	 * of how the samples land across shards -- a clean merge oracle. */
	for (i = 0; i < n; i++) xtc_dist_record(g_concurrent_d, 10.0);
	return NULL;
}

static MunitResult
test_dist_concurrent(const MunitParameter p[], void *d)
{
	pthread_t th[N_DIST_THREADS];
	int i;
	(void)p; (void)d;
	munit_assert_int(xtc_dist_create("test.dc", &g_concurrent_d), ==,
	    XTC_OK);
	for (i = 0; i < N_DIST_THREADS; i++)
		pthread_create(&th[i], NULL, dist_worker,
		    (void *)(intptr_t)N_DIST_REC);
	for (i = 0; i < N_DIST_THREADS; i++) pthread_join(th[i], NULL);
	/* (b) count is exact under contention; mean is the constant,
	 * variance ~0 -- proves the Chan merge is correct across shards. */
	munit_assert_uint64(xtc_dist_count(g_concurrent_d), ==,
	    (uint64_t)N_DIST_THREADS * N_DIST_REC);
	munit_assert_double_equal(xtc_dist_mean(g_concurrent_d), 10.0, 9);
	munit_assert_double(xtc_dist_variance(g_concurrent_d), <, 1e-9);
	xtc_dist_destroy(g_concurrent_d);
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
	xtc_dist_t    *d1;
	struct visit_count vc = { 0 };
	(void)p; (void)d;
	munit_assert_int(xtc_counter_create("it.c1", &c1), ==, XTC_OK);
	munit_assert_int(xtc_counter_create("it.c2", &c2), ==, XTC_OK);
	munit_assert_int(xtc_gauge_create  ("it.g1", &g1), ==, XTC_OK);
	munit_assert_int(xtc_hist_create   ("it.h1", &h1), ==, XTC_OK);
	munit_assert_int(xtc_dist_create   ("it.d1", &d1), ==, XTC_OK);

	munit_assert_int(xtc_metrics_iterate(__count_visit, &vc), >=, 5);
	munit_assert_int(vc.n, >=, 5);

	xtc_counter_destroy(c1);
	xtc_counter_destroy(c2);
	xtc_gauge_destroy(g1);
	xtc_hist_destroy(h1);
	xtc_dist_destroy(d1);
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

/* Prometheus dump including a histogram (the XTC_METRIC_HIST arm). */
static MunitResult
test_dump_prometheus_hist(const MunitParameter p[], void *d)
{
	xtc_hist_t *h;
	int pipefd[2];
	char buf[1024] = {0};
	ssize_t n;
	int i;
	(void)p; (void)d;
	munit_assert_int(pipe(pipefd), ==, 0);
	munit_assert_int(xtc_hist_create("prom.h", &h), ==, XTC_OK);
	for (i = 1; i <= 100; i++)
		xtc_hist_record(h, i);

	munit_assert_int(xtc_metrics_dump_prometheus(pipefd[1]), >, 0);
	close(pipefd[1]);
	n = read(pipefd[0], buf, sizeof buf - 1);
	munit_assert_size((size_t)n, >, 0);
	munit_assert_ptr_not_null(strstr(buf, "prom.h_count"));
	munit_assert_ptr_not_null(strstr(buf, "prom.h_p50"));
	munit_assert_ptr_not_null(strstr(buf, "prom.h_p99"));
	close(pipefd[0]);
	xtc_hist_destroy(h);
	return MUNIT_OK;
}

/* Histogram edge values: small (base < sub-bits), an empty quantile,
 * and a very large value push the bucket-bound math through its arms. */
static MunitResult
test_hist_edges(const MunitParameter p[], void *d)
{
	xtc_hist_t *h;
	int i;
	(void)p; (void)d;
	munit_assert_int(xtc_hist_create("edge.h", &h), ==, XTC_OK);

	/* Empty histogram: quantile is well-defined (0), count is 0. */
	munit_assert_uint64(xtc_hist_count(h), ==, 0);
	munit_assert_int64(xtc_hist_quantile(h, 0.5), >=, 0);
	/* Only zeros: no quantile above the max (0). */
	xtc_hist_record(h, 0);
	munit_assert_int64(xtc_hist_quantile(h, 1.0), ==, 0);
	munit_assert_int64(xtc_hist_max(h), ==, 0);
	xtc_hist_destroy(h);
	munit_assert_int(xtc_hist_create("edge.h", &h), ==, XTC_OK);

	/* Small values (1..8) live in low base buckets -- exercises the
	 * small-base bucket-index and lower-bound arms. */
	for (i = 1; i <= 8; i++)
		xtc_hist_record(h, i);
	/* A negative value is clamped to bucket 0, not a crash. */
	xtc_hist_record(h, -5);
	/* A very large value lands in the top bucket. */
	xtc_hist_record(h, (int64_t)1 << 60);
	munit_assert_uint64(xtc_hist_count(h), ==, 10);

	/* Quantiles across the range are monotonic and in bounds. */
	munit_assert_int64(xtc_hist_quantile(h, 0.0), >=, 0);
	munit_assert_int64(xtc_hist_quantile(h, 0.5), >=, 0);
	munit_assert_int64(xtc_hist_quantile(h, 1.0), >=,
	    xtc_hist_quantile(h, 0.5));
	xtc_hist_destroy(h);
	xtc_hist_destroy(NULL);   /* NULL no-op */
	return MUNIT_OK;
}

/* A latency far above the old 2^24 ns (16.8 ms) top bucket must be
 * reported as itself, not clamped: 99 x 1 ms + 1 x 500 ms, the shape of
 * a checkpoint stall in a stream of fast commits. */
static MunitResult
test_hist_range(const MunitParameter p[], void *d)
{
	xtc_hist_t *h;
	int64_t ms = 1000000, q50, q99, q995, q100;
	int i;
	(void)p; (void)d;
	munit_assert_int(xtc_hist_create("range.h", &h), ==, XTC_OK);
	munit_assert_int64(xtc_hist_max(h), ==, 0);
	for (i = 0; i < 99; i++)
		xtc_hist_record(h, 1 * ms);
	xtc_hist_record(h, 500 * ms);
	q50 = xtc_hist_quantile(h, 0.50);
	q99 = xtc_hist_quantile(h, 0.99);
	q995 = xtc_hist_quantile(h, 0.995);
	q100 = xtc_hist_quantile(h, 1.0);
	/* Quantiles are bucket UPPER bounds (never below the true value),
	 * within one sub-bucket (1/16 of the octave) of it. */
	munit_assert_int64(q50, >=, 1 * ms);
	munit_assert_int64(q50, <=, 1 * ms + ms / 16);
	munit_assert_int64(q99, >=, 1 * ms);
	munit_assert_int64(q99, <=, 1 * ms + ms / 16);
	munit_assert_int64(q995, >=, 500 * ms);
	/* ... and never above the largest value recorded. */
	munit_assert_int64(q995, <=, 500 * ms);
	munit_assert_int64(q100, ==, 500 * ms);
	munit_assert_int64(xtc_hist_max(h), ==, 500 * ms);

	/* A value past the widest bucket is still counted and is the max. */
	xtc_hist_record(h, (int64_t)1 << 60);
	munit_assert_uint64(xtc_hist_count(h), ==, 101);
	munit_assert_int64(xtc_hist_max(h), ==, (int64_t)1 << 60);
	munit_assert_int64(xtc_hist_quantile(h, 1.0), ==, (int64_t)1 << 60);
	munit_assert_int64(xtc_hist_quantile(h, 0.995), >=, 500 * ms);
	munit_assert_int64(xtc_hist_quantile(h, 0.50), <=, 1 * ms + ms / 16);
	xtc_hist_destroy(h);
	munit_assert_int64(xtc_hist_max(NULL), ==, 0);
	return MUNIT_OK;
}

/* Every recorded value v satisfies v <= quantile(rank(v)) <= v + v/8,
 * at every octave the histogram covers (1 ns .. 2^40 ns ~ 18 min). */
static MunitResult
test_hist_upper_bound(const MunitParameter p[], void *d)
{
	xtc_hist_t *h;
	int k;
	int64_t v, q;
	(void)p; (void)d;
	for (k = 0; k < 40; k++) {
		munit_assert_int(xtc_hist_create("ub.h", &h), ==, XTC_OK);
		v = ((int64_t)1 << k) + ((int64_t)1 << k) / 3;
		xtc_hist_record(h, 1);
		xtc_hist_record(h, v);
		xtc_hist_record(h, v * 2);
		q = xtc_hist_quantile(h, 0.5);   /* rank 2 of 3: v */
		munit_assert_int64(q, >=, v);
		munit_assert_int64(q, <=, v + v / 8);
		munit_assert_int64(xtc_hist_quantile(h, 1.0), ==, v * 2);
		xtc_hist_destroy(h);
	}
	return MUNIT_OK;
}

#define N_HIST_THREADS  8
#define N_HIST_REC      50000

static xtc_hist_t *g_concurrent_h;

static void *
hist_worker(void *arg)
{
	int id = (int)(intptr_t)arg;
	int i;
	for (i = 0; i < N_HIST_REC; i++)
		xtc_hist_record(g_concurrent_h, (int64_t)(id * N_HIST_REC + i));
	return NULL;
}

/* The max is exact under concurrent recorders on many shards. */
static MunitResult
test_hist_max_concurrent(const MunitParameter p[], void *d)
{
	pthread_t th[N_HIST_THREADS];
	int i;
	(void)p; (void)d;
	munit_assert_int(xtc_hist_create("test.hc", &g_concurrent_h), ==,
	    XTC_OK);
	for (i = 0; i < N_HIST_THREADS; i++)
		pthread_create(&th[i], NULL, hist_worker, (void *)(intptr_t)i);
	for (i = 0; i < N_HIST_THREADS; i++) pthread_join(th[i], NULL);
	munit_assert_uint64(xtc_hist_count(g_concurrent_h), ==,
	    (uint64_t)N_HIST_THREADS * N_HIST_REC);
	munit_assert_int64(xtc_hist_max(g_concurrent_h), ==,
	    (int64_t)N_HIST_THREADS * N_HIST_REC - 1);
	xtc_hist_destroy(g_concurrent_h);
	return MUNIT_OK;
}

/* A 63-byte metric name (the longest the registry keeps) still dumps
 * every histogram line, including the max. */
static MunitResult
test_dump_prometheus_long_name(const MunitParameter p[], void *d)
{
	static const char name[] =
	    "a_metric_name_that_is_exactly_sixty_three_bytes_long_0123456789";
	xtc_hist_t *h;
	xtc_dist_t *dd;
	int pipefd[2];
	char buf[4096] = {0}, want[128];
	size_t got = 0;
	ssize_t n;
	(void)p; (void)d;
	munit_assert_size(sizeof name - 1, ==, 63);
	munit_assert_int(pipe(pipefd), ==, 0);
	munit_assert_int(xtc_hist_create(name, &h), ==, XTC_OK);
	munit_assert_int(xtc_dist_create("long.d", &dd), ==, XTC_OK);
	xtc_hist_record(h, 123456789);
	xtc_dist_record(dd, 1.0);
	munit_assert_int(xtc_metrics_dump_prometheus(pipefd[1]), >, 0);
	close(pipefd[1]);
	while (got < sizeof buf - 1 &&
	    (n = read(pipefd[0], buf + got, sizeof buf - 1 - got)) > 0)
		got += (size_t)n;
	close(pipefd[0]);
	snprintf(want, sizeof want, "%s_p999 ", name);
	munit_assert_ptr_not_null(strstr(buf, want));
	snprintf(want, sizeof want, "%s_max 123456789\n", name);
	munit_assert_ptr_not_null(strstr(buf, want));
	/* The next metric's lines are intact (no garbage, no NUL). */
	munit_assert_size(strlen(buf), ==, got);
	munit_assert_ptr_not_null(strstr(buf, "long.d_stddev "));
	xtc_hist_destroy(h);
	xtc_dist_destroy(dd);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/counter_basic",      test_counter_basic,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/counter_concurrent", test_counter_concurrent, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/gauge_basic",        test_gauge_basic,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hist_basic",         test_hist_basic,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/dist_basic",         test_dist_basic,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/dist_concurrent",    test_dist_concurrent,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/iterate",            test_iterate,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/dump_prometheus",    test_dump_prometheus,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/dump_prom_hist",     test_dump_prometheus_hist, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hist_edges",         test_hist_edges,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hist_range",         test_hist_range,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hist_upper_bound",   test_hist_upper_bound,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hist_max_concurrent", test_hist_max_concurrent, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/dump_prom_long_name", test_dump_prometheus_long_name, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = {
	"/m12/stats", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
