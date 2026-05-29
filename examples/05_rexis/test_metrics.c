/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test_metrics.c -- exercises the rexis xtc_stats integration
 * without a running server.  Mirrors what cmd_execute and cmd_info
 * do: register the counters and histogram, record into them, then
 * read them back the same way the INFO command does.
 */

#include <stdio.h>
#include <stdlib.h>

#include "xtc_stats.h"

#define ASSERT(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s:%d: %s\n", \
		    __FILE__, __LINE__, #cond); \
		exit(1); \
	} \
} while (0)

/* These are the objects metrics.c defines; here we create local
 * ones with the same shape to prove the API contract the example
 * relies on. */
static void
test_counter_roundtrip(void)
{
	xtc_counter_t *c = NULL;
	int i;

	ASSERT(xtc_counter_create("test_cmd_total", &c) == 0);
	ASSERT(c != NULL);
	for (i = 0; i < 100; i++)
		xtc_counter_inc(c);
	ASSERT(xtc_counter_read(c) == 100);
	xtc_counter_add(c, 50);
	ASSERT(xtc_counter_read(c) == 150);
	xtc_counter_destroy(c);
	printf("  ok   counter_roundtrip\n");
}

static void
test_histogram_quantiles(void)
{
	xtc_hist_t *h = NULL;
	int i;
	int64_t p50, p99;

	ASSERT(xtc_hist_create("test_cmd_latency_ns", &h) == 0);
	ASSERT(h != NULL);

	/* Record a known distribution: 1000 values from 1us to 1ms. */
	for (i = 0; i < 1000; i++)
		xtc_hist_record(h, 1000 + (int64_t)i * 1000);

	p50 = xtc_hist_quantile(h, 0.50);
	p99 = xtc_hist_quantile(h, 0.99);

	/* p50 should land near the middle (~500us), p99 near the top.
	 * Log-linear buckets give bounded error, so check ranges. */
	ASSERT(p50 > 100000 && p50 < 900000);
	ASSERT(p99 > p50);
	xtc_hist_destroy(h);
	printf("  ok   histogram_quantiles (p50=%lld p99=%lld)\n",
	    (long long)p50, (long long)p99);
}

static void
test_gauge_set_read(void)
{
	xtc_gauge_t *g = NULL;

	ASSERT(xtc_gauge_create("test_db_keys", &g) == 0);
	ASSERT(g != NULL);
	xtc_gauge_set(g, 42);
	ASSERT(xtc_gauge_read(g) == 42);
	xtc_gauge_add(g, 8);
	ASSERT(xtc_gauge_read(g) == 50);
	xtc_gauge_add(g, -10);
	ASSERT(xtc_gauge_read(g) == 40);
	xtc_gauge_destroy(g);
	printf("  ok   gauge_set_read\n");
}

int
main(void)
{
	test_counter_roundtrip();
	test_histogram_quantiles();
	test_gauge_set_read();
	printf("All rexis metrics tests passed.\n");
	return 0;
}
