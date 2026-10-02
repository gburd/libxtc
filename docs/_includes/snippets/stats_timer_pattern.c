/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/stats_timer_pattern.c -- the "Timer" pattern.
 *
 * A Dropwizard "Timer" is not a distinct type: it is a rate counter
 * (how many operations) composed with a latency histogram (how long
 * each took) behind one call site.  libxtc ships the two primitives
 * -- xtc_counter and xtc_hist -- and leaves the composition to the
 * consumer, so this snippet IS the documentation of that pattern.
 *
 * THE RULE THIS SNIPPET EXISTS TO ENFORCE: timing uses xtc_clock_mono
 * (the PUBLIC monotonic clock, nanoseconds), and the two updates --
 * xtc_hist_record(latency, delta) and xtc_counter_inc(events) -- happen
 * together at the one call site, so the count and the distribution can
 * never drift apart.  We assert the count equals what we recorded; we
 * deliberately do NOT assert exact latency values (wall time is not
 * reproducible).
 */

/* !region full */
#include <assert.h>
#include <stdio.h>

#include "xtc.h"         /* XTC_OK, xtc_clock_mono */
#include "xtc_stats.h"   /* xtc_counter, xtc_hist */

/* The "Timer": wrap the two primitives in one struct and one call so a
 * call site records rate and latency together and cannot forget one. */
struct timer {
	xtc_counter_t *events;   /* how many operations (the rate) */
	xtc_hist_t    *latency;  /* how long each took, ns (the distribution) */
};

static void
timer_observe(struct timer *t, int64_t ns)
{
	xtc_hist_record(t->latency, ns);   /* latency distribution */
	xtc_counter_inc(t->events);        /* one more event (the rate) */
}

int
main(void)
{
	struct timer t;
	const int    iters = 1000;
	int          i;
	volatile int sink = 0;

	if (xtc_counter_create("demo.ops", &t.events) != XTC_OK)
		return 1;
	if (xtc_hist_create("demo.op_latency_ns", &t.latency) != XTC_OK)
		return 1;

	for (i = 0; i < iters; i++) {
		int64_t t0 = xtc_clock_mono();
		int     j;

		/* A tiny, cheap operation standing in for real work. */
		for (j = 0; j < 32; j++)
			sink += j;

		timer_observe(&t, xtc_clock_mono() - t0);
	}

	/* Read back: rate (count) from the counter, distribution from the
	 * histogram.  One cohesive "Timer" view from two primitives. */
	{
		uint64_t count = xtc_counter_read(t.events);
		uint64_t hc    = xtc_hist_count(t.latency);
		int64_t  p99   = xtc_hist_quantile(t.latency, 0.99);

		printf("ops=%llu hist_count=%llu p99_ns=%lld\n",
		    (unsigned long long)count,
		    (unsigned long long)hc,
		    (long long)p99);

		/* The composition is correct iff both sides saw every event. */
		assert(count == (uint64_t)iters);
		assert(hc == (uint64_t)iters);
		assert(p99 >= 0);
		(void)sink;
	}

	xtc_counter_destroy(t.events);
	xtc_hist_destroy(t.latency);
	printf("ok\n");
	return 0;
}
/* !endregion full */
