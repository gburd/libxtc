/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/stats_prometheus.c -- a /metrics page.
 *
 * A server's serving metrics as Prometheus histograms with fixed bucket
 * bounds (the TTFT bounds vLLM publishes, so a dashboard written for it
 * keys on the same `le` labels), a nanosecond latency histogram exposed
 * in seconds, and a counter, rendered with xtc_metrics_write_prometheus
 * into the caller's buffer.  The snippet checks the lines a scraper
 * relies on are there.
 *
 * Compiled AND run by test/docs/test_doc_snippets.sh (a release gate): it
 * must exit 0.
 */

/* !region full */
#include <stdio.h>
#include <string.h>

#include "xtc.h"
#include "xtc_stats.h"

static const double TTFT_BOUNDS[] = {
	0.001, 0.005, 0.01, 0.02, 0.04, 0.06, 0.08, 0.1, 0.25, 0.5, 0.75,
	1.0, 2.5, 5.0, 7.5, 10.0, 20.0, 40.0, 80.0, 160.0, 640.0, 2560.0
};

/* The page goes wherever the HTTP handler wants it; here, a buffer. */
struct page {
	char   text[16384];
	size_t len;
};

static int
to_page(const char *buf, size_t len, void *user)
{
	struct page *pg = user;
	if (len >= sizeof pg->text)
		return -1;   /* the call returns XTC_E_IO */
	memcpy(pg->text, buf, len + 1);
	pg->len = len;
	return 0;
}

int
main(void)
{
	xtc_hist_t *ttft, *step;
	xtc_counter_t *reqs;
	static struct page pg;

	/* Fixed bounds, values in seconds. */
	if (xtc_hist_create_bounds("server_time_to_first_token_seconds",
	        TTFT_BOUNDS, (int)(sizeof TTFT_BOUNDS / sizeof TTFT_BOUNDS[0]),
	        &ttft) != XTC_OK)
		return 1;
	(void)xtc_metrics_set_help(ttft,
	    "Histogram of time to first token in seconds.");

	/* Log-linear, recorded in nanoseconds, exposed in seconds. */
	if (xtc_hist_create("server_step_seconds", &step) != XTC_OK)
		return 1;
	(void)xtc_hist_set_scale(step, 1e-9);
	(void)xtc_metrics_set_help(step, "One batched engine step.");

	if (xtc_counter_create("server_requests_total", &reqs) != XTC_OK)
		return 1;
	(void)xtc_metrics_set_help(reqs, "Requests finished.");

	/* The hot path: any thread, one or two atomic adds each. */
	xtc_hist_record_value(ttft, 0.042);
	xtc_hist_record_value(ttft, 0.3);
	xtc_hist_record(step, 7500000);   /* 7.5 ms */
	xtc_counter_add(reqs, 2);

	/* The scrape. */
	if (xtc_metrics_write_prometheus(to_page, &pg) < 0)
		return 1;
	fputs(pg.text, stdout);
	/* !endregion full */

	if (strstr(pg.text, "server_time_to_first_token_seconds_bucket"
	        "{le=\"0.06\"} 1\n") == NULL ||
	    strstr(pg.text, "server_time_to_first_token_seconds_bucket"
	        "{le=\"+Inf\"} 2\n") == NULL ||
	    strstr(pg.text, "server_time_to_first_token_seconds_count 2\n") ==
	        NULL ||
	    strstr(pg.text, "server_step_seconds_sum 0.0075\n") == NULL ||
	    strstr(pg.text, "# TYPE server_requests_total counter\n"
	        "server_requests_total 2\n") == NULL)
		return 1;
	xtc_hist_destroy(ttft);
	xtc_hist_destroy(step);
	xtc_counter_destroy(reqs);
	return 0;
}
