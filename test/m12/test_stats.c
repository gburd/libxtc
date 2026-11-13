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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

/*
 * Check a page against the Prometheus text format (0.0.4) the way a
 * scraper reads it: every sample name matches [a-zA-Z_:][a-zA-Z0-9_:]*
 * and belongs to the family the last # TYPE named, with only the
 * suffixes that type allows; a histogram's buckets are cumulative
 * (never decreasing, le ascending), end in le="+Inf", and that bucket
 * equals _count.  (promtool check metrics agrees on every page the
 * tests here produce; it is not a build dependency.)
 */
static int
prom_name_ok(const char *a, size_t n)
{
	size_t i;
	if (n == 0 || (a[0] >= '0' && a[0] <= '9'))
		return 0;
	for (i = 0; i < n; i++)
		if (!((a[i] >= 'a' && a[i] <= 'z') ||
		        (a[i] >= 'A' && a[i] <= 'Z') ||
		        (a[i] >= '0' && a[i] <= '9') || a[i] == '_' ||
		        a[i] == ':'))
			return 0;
	return 1;
}

static void
prom_check(const char *page)
{
	char fam[128] = "", type[16] = "", name[160];
	const char *l = page, *e, *sp;
	double last_le = -1.0, le;
	unsigned long long last_cum = 0, cum, inf = 0, count;
	int saw_inf = 0, in_hist = 0;
	size_t nl;

	while (*l != '\0') {
		e = strchr(l, '\n');
		munit_assert_not_null(e); /* every line ends in a newline */
		if (strncmp(l, "# TYPE ", 7) == 0) {
			if (in_hist)
				munit_assert_true(saw_inf);
			munit_assert_int(sscanf(l, "# TYPE %127s %15s", fam,
			                     type),
			    ==, 2);
			munit_assert_true(prom_name_ok(fam, strlen(fam)));
			munit_assert_true(strcmp(type, "counter") == 0 ||
			    strcmp(type, "gauge") == 0 ||
			    strcmp(type, "histogram") == 0 ||
			    strcmp(type, "summary") == 0);
			in_hist = strcmp(type, "histogram") == 0;
			last_le = -1.0;
			last_cum = 0;
			saw_inf = 0;
		} else if (strncmp(l, "# HELP ", 7) == 0) {
			munit_assert_true(
			    prom_name_ok(l + 7, strcspn(l + 7, " ")));
		} else {
			sp = l + strcspn(l, "{ ");
			nl = (size_t)(sp - l);
			munit_assert_size(nl, <, sizeof name);
			memcpy(name, l, nl);
			name[nl] = '\0';
			munit_assert_true(prom_name_ok(name, nl));
			munit_assert_size(strlen(fam), >, 0);
			munit_assert_int(strncmp(name, fam, strlen(fam)), ==,
			    0);
			if (strcmp(type, "counter") == 0 ||
			    strcmp(type, "gauge") == 0)
				munit_assert_string_equal(name, fam);
			else if (strcmp(type, "summary") == 0)
				munit_assert_true(
				    strcmp(name + strlen(fam), "_sum") == 0 ||
				    strcmp(name + strlen(fam), "_count") == 0);
			else if (strcmp(name + strlen(fam), "_bucket") == 0) {
				munit_assert_false(saw_inf);
				if (strncmp(sp, "{le=\"+Inf\"} ", 12) == 0) {
					munit_assert_int(sscanf(sp + 12, "%llu",
					                     &cum),
					    ==, 1);
					saw_inf = 1;
					inf = cum;
				} else {
					munit_assert_int(
					    sscanf(sp, "{le=\"%lf\"} %llu", &le,
					        &cum),
					    ==, 2);
					munit_assert_true(le > last_le);
					last_le = le;
				}
				munit_assert_true(cum >= last_cum);
				last_cum = cum;
			} else if (strcmp(name + strlen(fam), "_count") == 0) {
				munit_assert_true(saw_inf);
				munit_assert_int(sscanf(sp, " %llu", &count),
				    ==, 1);
				munit_assert_ullong(count, ==, inf);
			} else {
				munit_assert_string_equal(name + strlen(fam),
				    "_sum");
				munit_assert_true(saw_inf);
			}
		}
		l = e + 1;
	}
	if (in_hist)
		munit_assert_true(saw_inf);
}

static int
collect_page(const char *buf, size_t len, void *user)
{
	char **out = user;
	munit_assert_size(strlen(buf), ==, len);
	*out = strdup(buf);
	return 0;
}

static int
refuse_page(const char *buf, size_t len, void *user)
{
	(void)buf;
	(void)len;
	(void)user;
	return -1;
}

/* vLLM's TTFT bucket bounds (seconds): the le labels a dashboard keys
 * on must come out exactly so, and a value on a bound counts in it. */
static const double TTFT[] = { 0.001, 0.005, 0.01, 0.02, 0.04, 0.06, 0.08, 0.1,
	0.25, 0.5, 0.75, 1.0, 2.5, 5.0, 7.5, 10.0, 20.0, 40.0, 80.0, 160.0,
	640.0, 2560.0 };

struct bucket_rec {
	int n;
	double lo[4], hi[4];
	uint64_t c[4];
};

static int
bucket_cb(double lower, double upper, uint64_t count, void *user)
{
	struct bucket_rec *r = user;
	if (r->n < 4) {
		r->lo[r->n] = lower;
		r->hi[r->n] = upper;
		r->c[r->n] = count;
	}
	r->n++;
	return 0;
}

static MunitResult
test_hist_bounds(const MunitParameter p[], void *d)
{
	static const double bad1[] = { 1.0, 1.0 }, bad2[] = { -1.0 };
	xtc_hist_t *h, *bad;
	char *page = NULL;
	double nan = 0.0 / 0.0;
	struct bucket_rec r;
	(void)p;
	(void)d;

	munit_assert_int(xtc_hist_create_bounds("ttft", TTFT, 0, &bad), ==,
	    XTC_E_INVAL);
	munit_assert_int(xtc_hist_create_bounds("ttft", TTFT, 65, &bad), ==,
	    XTC_E_INVAL);
	munit_assert_int(xtc_hist_create_bounds("ttft", bad1, 2, &bad), ==,
	    XTC_E_INVAL);
	munit_assert_int(xtc_hist_create_bounds("ttft", bad2, 1, &bad), ==,
	    XTC_E_INVAL);
	munit_assert_int(xtc_hist_create_bounds("ttft", &nan, 1, &bad), ==,
	    XTC_E_INVAL);
	munit_assert_int(
	    xtc_hist_create_bounds("vllm:time_to_first_token_seconds", TTFT,
	        (int)(sizeof TTFT / sizeof TTFT[0]), &h),
	    ==, XTC_OK);
	munit_assert_int(xtc_metrics_set_help(h,
	                     "Histogram of time to first token in seconds."),
	    ==, XTC_OK);

	xtc_hist_record_value(h, 0.003);  /* (0.001, 0.005] */
	xtc_hist_record_value(h, 0.005);  /* on a bound: le="0.005" */
	xtc_hist_record_value(h, 0.3);    /* (0.25, 0.5] */
	xtc_hist_record_value(h, 3000.0); /* above every bound: +Inf only */
	xtc_hist_record_value(h, -2.0);   /* counts as 0: le="0.001" */
	xtc_hist_record(h, 2);            /* an integer: (1.0, 2.5] */
	munit_assert_uint64(xtc_hist_count(h), ==, 6);
	munit_assert_double_equal(xtc_hist_sum(h), 3002.308, 9);
	munit_assert_int64(xtc_hist_quantile(h, 0.5), ==, 0);
	munit_assert_int64(xtc_hist_max(h), ==, 0);

	memset(&r, 0, sizeof r);
	munit_assert_int(xtc_hist_iterate(h, bucket_cb, &r), ==, 23);
	munit_assert_double(r.lo[0], ==, 0.0);
	munit_assert_double(r.hi[0], ==, 0.001);
	munit_assert_uint64(r.c[0], ==, 1);
	munit_assert_double(r.lo[1], ==, 0.001);
	munit_assert_uint64(r.c[1], ==, 2);

	munit_assert_int(xtc_metrics_write_prometheus(collect_page, &page), >,
	    0);
	munit_assert_not_null(page);
	munit_assert_not_null(strstr(page,
	    "# HELP vllm:time_to_first_token_seconds Histogram of time to "
	    "first token in seconds.\n"
	    "# TYPE vllm:time_to_first_token_seconds histogram\n"
	    "vllm:time_to_first_token_seconds_bucket{le=\"0.001\"} 1\n"
	    "vllm:time_to_first_token_seconds_bucket{le=\"0.005\"} 3\n"
	    "vllm:time_to_first_token_seconds_bucket{le=\"0.01\"} 3\n"));
	munit_assert_not_null(strstr(page,
	    "vllm:time_to_first_token_seconds_bucket{le=\"0.5\"} 4\n"));
	munit_assert_not_null(strstr(page,
	    "vllm:time_to_first_token_seconds_bucket{le=\"2.5\"} 5\n"));
	munit_assert_not_null(strstr(page,
	    "vllm:time_to_first_token_seconds_bucket{le=\"2560.0\"} 5\n"
	    "vllm:time_to_first_token_seconds_bucket{le=\"+Inf\"} 6\n"
	    "vllm:time_to_first_token_seconds_sum 3002.308\n"
	    "vllm:time_to_first_token_seconds_count 6\n"));
	prom_check(page);
	free(page);
	xtc_hist_destroy(h);
	return MUNIT_OK;
}

static MunitResult
test_hist_sum_quantiles_scale(const MunitParameter p[], void *d)
{
	static const double qs[3] = { 0.5, 0.99, 1.0 };
	xtc_hist_t *h;
	xtc_counter_t *c;
	char *page = NULL;
	int64_t v[3];
	uint64_t n = 0;
	struct bucket_rec r;
	int i;
	(void)p;
	(void)d;

	munit_assert_int(xtc_hist_create("req.latency-seconds", &h), ==,
	    XTC_OK);
	for (i = 1; i <= 1000; i++)
		xtc_hist_record(h, (int64_t)i * 1000);
	xtc_hist_record(h, -7); /* counts as 0, in bucket 0 and the sum */
	munit_assert_double(xtc_hist_sum(h), ==, 500500000.0);

	/* One merge answers several quantiles, the same as one at a time. */
	munit_assert_int(xtc_hist_quantiles(h, qs, 3, v, &n), ==, XTC_OK);
	munit_assert_uint64(n, ==, 1001);
	for (i = 0; i < 3; i++)
		munit_assert_int64(v[i], ==, xtc_hist_quantile(h, qs[i]));
	munit_assert_int64(v[2], ==, 1000000);
	munit_assert_int(xtc_hist_quantiles(h, NULL, 0, NULL, &n), ==, XTC_OK);
	munit_assert_int(xtc_hist_quantiles(h, NULL, 1, v, NULL), ==,
	    XTC_E_INVAL);
	munit_assert_int(xtc_hist_quantiles(NULL, qs, 1, v, NULL), ==,
	    XTC_E_INVAL);

	/* Non-empty buckets only, lowest first, (lower, upper]. */
	memset(&r, 0, sizeof r);
	munit_assert_int(xtc_hist_iterate(h, bucket_cb, &r), >, 2);
	munit_assert_double(r.lo[0], ==, 0.0);
	munit_assert_double(r.hi[0], ==, 1.0);
	munit_assert_uint64(r.c[0], ==, 1);
	munit_assert_double(r.lo[1], ==, 991.0); /* 1000 is in [992, 1023] */
	munit_assert_double(r.hi[1], ==, 1023.0);
	munit_assert_uint64(r.c[1], ==, 1);
	munit_assert_int(xtc_hist_iterate(h, NULL, NULL), ==, XTC_E_INVAL);

	/* Nanoseconds exposed in seconds: bounds and sum scale, the
	 * recorded values do not. */
	munit_assert_int(xtc_hist_set_scale(h, 0.0), ==, XTC_E_INVAL);
	munit_assert_int(xtc_hist_set_scale(h, 1e-9), ==, XTC_OK);
	munit_assert_int(xtc_counter_create("req.total", &c), ==, XTC_OK);
	munit_assert_int(xtc_metrics_set_help(c, "a\\b\nc"), ==, XTC_OK);
	munit_assert_int(xtc_metrics_set_help(&n, "x"), ==, XTC_E_NOTFOUND);
	munit_assert_int(xtc_metrics_set_help(NULL, "x"), ==, XTC_E_INVAL);
	munit_assert_int(xtc_metrics_write_prometheus(collect_page, &page), >,
	    0);
	munit_assert_not_null(strstr(page,
	    "# TYPE req_latency_seconds histogram\n"
	    "req_latency_seconds_bucket{le=\"1e-09\"} 1\n"
	    "req_latency_seconds_bucket{le=\"1.023e-06\"} 2\n"));
	munit_assert_not_null(strstr(page,
	    "req_latency_seconds_bucket{le=\"+Inf\"} 1001\n"
	    "req_latency_seconds_sum 0.5005\n"
	    "req_latency_seconds_count 1001\n"));
	/* Help text escapes backslash and newline. */
	munit_assert_not_null(strstr(page,
	    "# HELP req_total a\\\\b\\nc\n# TYPE req_total counter\n"));
	munit_assert_int64(xtc_hist_quantile(h, 1.0), ==, 1000000);
	prom_check(page);
	free(page);
	munit_assert_int(xtc_metrics_write_prometheus(refuse_page, NULL), ==,
	    XTC_E_IO);
	munit_assert_int(xtc_metrics_write_prometheus(NULL, NULL), ==,
	    XTC_E_INVAL);
	xtc_counter_destroy(c);
	xtc_hist_destroy(h);
	return MUNIT_OK;
}


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
	/* '.' is not allowed in a Prometheus name: it becomes '_'. */
	munit_assert_ptr_not_null(
	    strstr(buf, "# TYPE prom_c counter\nprom_c 7\n"));
	munit_assert_ptr_not_null(
	    strstr(buf, "# TYPE prom_g gauge\nprom_g 99\n"));
	munit_assert_null(strstr(buf, "prom.c"));
	prom_check(buf);

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
	char buf[8192] = { 0 };
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
	/* A real histogram family: cumulative buckets (1..16 are exact
	 * buckets, so le="16" holds 16 values), +Inf, _sum, _count. */
	munit_assert_ptr_not_null(strstr(buf, "# TYPE prom_h histogram\n"));
	munit_assert_ptr_not_null(
	    strstr(buf, "prom_h_bucket{le=\"16.0\"} 16\n"));
	munit_assert_ptr_not_null(
	    strstr(buf, "prom_h_bucket{le=\"+Inf\"} 100\n"));
	munit_assert_ptr_not_null(strstr(buf, "prom_h_sum 5050.0\n"));
	munit_assert_ptr_not_null(strstr(buf, "prom_h_count 100\n"));
	munit_assert_null(strstr(buf, "_p50"));
	prom_check(buf);
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
 * every histogram line. */
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
	snprintf(want, sizeof want, "%s_sum 123456789.0\n", name);
	munit_assert_ptr_not_null(strstr(buf, want));
	snprintf(want, sizeof want, "%s_count 1\n", name);
	munit_assert_ptr_not_null(strstr(buf, want));
	/* The next metric's lines are intact (no garbage, no NUL), and a
	 * dist is a summary (mean = _sum / _count) plus a _stddev gauge. */
	munit_assert_size(strlen(buf), ==, got);
	munit_assert_ptr_not_null(strstr(buf,
	    "# TYPE long_d summary\n"
	    "long_d_sum 1.0\nlong_d_count 1\n# TYPE long_d_stddev gauge\n"
	    "long_d_stddev 0.0\n"));
	prom_check(buf);
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
	{ "/hist_bounds",        test_hist_bounds,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hist_sum_quantiles_scale", test_hist_sum_quantiles_scale, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = {
	"/m12/stats", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
