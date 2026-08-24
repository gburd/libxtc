/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/11_lorb/driver.c
 *	Self-contained generate-and-process driver for the lorb engine.
 *	Seeds the book with an initial population of resting orders, then
 *	replays a mixed order stream (the same order-type mix and normal
 *	price distribution as the C++ original's generator), timing every
 *	order and reporting throughput plus p50/p99/p999 latency.
 *
 *	Usage: lorb [n_orders] [n_initial] [seed]
 *	         n_orders  processed orders to time  (default 1000000)
 *	         n_initial resting orders to seed    (default 11000)
 *	         seed      PRNG seed                  (default 12345)
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lob.h"
#include "xtc.h"

/* ---- deterministic PRNG (xorshift64* + Box-Muller normal) ---------- */

static uint64_t g_state;

static uint64_t
rng_next(void)
{
	uint64_t x = g_state;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	g_state = x;
	return x * 0x2545F4914F6CDD1DULL;
}

/* Uniform int in [lo, hi]. */
static int
rng_uniform(int lo, int hi)
{
	return lo + (int)(rng_next() % (uint64_t)(hi - lo + 1));
}

/* Uniform double in [0, 1). */
static double
rng_unit(void)
{
	return (double)(rng_next() >> 11) * (1.0 / 9007199254740992.0);
}

/* Normal(mean, sd) via Box-Muller. */
static int
rng_normal(double mean, double sd)
{
	double u1 = rng_unit();
	double u2 = rng_unit();
	if (u1 < 1e-12)
		u1 = 1e-12;
	double z = sqrt(-2.0 * log(u1)) *
	    cos(6.28318530717958647692 * u2);
	return (int)(mean + sd * z);
}

/* ---- latency sample -> percentiles --------------------------------- */

static int
cmp_i64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
	return (x > y) - (x < y);
}

static int64_t
percentile(const int64_t *sorted, size_t n, double p)
{
	if (n == 0)
		return 0;
	size_t idx = (size_t)(p * (double)(n - 1) + 0.5);
	if (idx >= n)
		idx = n - 1;
	return sorted[idx];
}

/* ---- generator ----------------------------------------------------- */

/* Seed the book with resting limit + stop orders around a centre. */
static int
seed_book(lob_book_t *b, int n, int centre, int next_id)
{
	for (int i = 0; i < n; i++) {
		int shares = rng_uniform(1, 1000);
		int price = rng_normal(centre, 50);
		if (price < 1)
			price = 1;
		bool buy = price < centre;
		lob_add_limit_order(b, next_id++, buy, shares, price);
	}
	/* A smaller batch of stop / stop-limit orders (10% of n).  As in
	 * the C++ initial-orders generator: buy stops sit above the
	 * centre, sell stops below, so the two sides occupy disjoint
	 * prices (the single stop price map cannot host both a buy and a
	 * sell level at one price). */
	int n_stop = n / 10;
	for (int i = 0; i < n_stop; i++) {
		int shares = rng_uniform(1, 1000);
		int price = rng_normal(centre, 50);
		if (price == centre)
			price += 1;
		if (price < 1)
			price = 1;
		bool buy = price > centre;
		if (rng_uniform(0, 1)) {
			lob_add_stop_order(b, next_id++, buy, shares, price);
		} else {
			int lp = buy ? price + rng_uniform(1, 5)
			             : price - rng_uniform(1, 5);
			if (lp < 1)
				lp = 1;
			lob_add_stop_limit_order(b, next_id++, buy, shares, lp,
			    price);
		}
	}
	return next_id;
}

/*
 * Best-bid/offer, guarding against a momentarily empty side.  The
 * generator keeps new resting orders from crossing the book (as the
 * C++ generator does), falling back to the centre when a side is empty.
 */
static int
best_buy(const lob_book_t *b, int centre)
{
	return b->highest_buy ? b->highest_buy->price : centre - 1;
}

static int
best_sell(const lob_book_t *b, int centre)
{
	return b->lowest_sell ? b->lowest_sell->price : centre + 1;
}

/* A non-crossing limit price for a resting order of the given side. */
static int
noncross_price(const lob_book_t *b, int centre, bool buy)
{
	int price = rng_normal(centre, 50);
	if (buy) {
		int cap = best_sell(b, centre);
		if (price >= cap)
			price = cap - 1;
	} else {
		int flr = best_buy(b, centre);
		if (price <= flr)
			price = flr + 1;
	}
	return price < 1 ? 1 : price;
}

/*
 * A stop trigger price on the correct side of the book.  Buy stops sit
 * strictly above the lowest sell (they fire as the price rises); sell
 * stops strictly below the highest buy (they fire as it falls).  Since
 * the resting book never crosses (lowest_sell > highest_buy), those two
 * regions are disjoint, so a buy stop and a sell stop can never share a
 * price -- which matters because the engine (faithful to the C++
 * original) keys all stops in a single price map that cannot host both
 * a buy and a sell level at one price.
 */
static int
stop_price(const lob_book_t *b, int centre, bool buy)
{
	int price = rng_normal(centre, 50);
	if (buy) {
		int flr = best_sell(b, centre);
		if (price <= flr)
			price = flr + 1;
	} else {
		int cap = best_buy(b, centre);
		if (price >= cap)
			price = cap - 1;
	}
	return price < 1 ? 1 : price;
}

/*
 * A live order id in the wanted category to cancel/modify: probe a
 * handful of past ids for one still resting in that category.  Returns
 * 0 if none found (caller no-ops, which the engine handles).  This
 * stands in for the C++ generator's getRandomOrder over its three
 * separate live-order sets -- cancelling/modifying a limit order must
 * not touch a stop order, and vice versa.
 */
static int
pick_live(const lob_book_t *b, int max_id, lob_cat_t want)
{
	for (int tries = 0; tries < 8; tries++) {
		int id = rng_uniform(1, max_id > 1 ? max_id - 1 : 1);
		const lob_order_t *o = lob_search_order(b, id);
		if (o != NULL && o->cat == want)
			return id;
	}
	return 0;
}

/* One generated + processed order.  Returns the id consumed (or the
 * same id for actions that reuse an existing order). */
static int
gen_one(lob_book_t *b, int centre, int *next_id)
{
	double r = rng_unit();
	int id = *next_id;
	int tid;
	const lob_order_t *o;

	/* Cumulative weights matching the C++ generator's mix. */
	if (r < 0.025) {                       /* market */
		lob_market_order(b, id, rng_uniform(0, 1), rng_uniform(1, 1000));
		(*next_id)++;
	} else if (r < 0.217) {                /* add limit */
		bool buy = rng_uniform(0, 1);
		int shares = rng_uniform(1, 1000);
		lob_add_limit_order(b, id, buy, shares,
		    noncross_price(b, centre, buy));
		(*next_id)++;
	} else if (r < 0.336) {                /* cancel limit */
		if ((tid = pick_live(b, id, LOB_LIMIT)) != 0)
			lob_cancel_limit_order(b, tid);
	} else if (r < 0.515) {                /* modify limit */
		if ((tid = pick_live(b, id, LOB_LIMIT)) != 0 &&
		    (o = lob_search_order(b, tid)) != NULL)
			lob_modify_limit_order(b, tid, rng_uniform(1, 1000),
		        noncross_price(b, centre, o->buy));
	} else if (r < 0.540) {                /* market limit (crosses) */
		bool buy = rng_uniform(0, 1);
		int shares = rng_uniform(1, 1000);
		int price = buy ? best_sell(b, centre) + 1
		                : best_buy(b, centre) - 1;
		if (price < 1)
			price = 1;
		lob_add_limit_order(b, id, buy, shares, price);
		(*next_id)++;
	} else if (r < 0.633) {                /* add stop */
		bool buy = rng_uniform(0, 1);
		int shares = rng_uniform(1, 1000);
		lob_add_stop_order(b, id, buy, shares,
		    stop_price(b, centre, buy));
		(*next_id)++;
	} else if (r < 0.706) {                /* cancel stop */
		if ((tid = pick_live(b, id, LOB_STOP)) != 0)
			lob_cancel_stop_order(b, tid);
	} else if (r < 0.779) {                /* modify stop */
		if ((tid = pick_live(b, id, LOB_STOP)) != 0 &&
		    (o = lob_search_order(b, tid)) != NULL)
			lob_modify_stop_order(b, tid, rng_uniform(1, 1000),
		        stop_price(b, centre, o->buy));
	} else if (r < 0.869) {                /* add stop limit */
		bool buy = rng_uniform(0, 1);
		int shares = rng_uniform(1, 1000);
		int sp = stop_price(b, centre, buy);
		int lp = buy ? sp + rng_uniform(1, 5)
		             : sp - rng_uniform(1, 5);
		if (lp < 1)
			lp = 1;
		lob_add_stop_limit_order(b, id, buy, shares, lp, sp);
		(*next_id)++;
	} else if (r < 0.940) {                /* cancel stop limit */
		if ((tid = pick_live(b, id, LOB_STOP_LIMIT)) != 0)
			lob_cancel_stop_limit_order(b, tid);
	} else {                               /* modify stop limit */
		if ((tid = pick_live(b, id, LOB_STOP_LIMIT)) != 0 &&
		    (o = lob_search_order(b, tid)) != NULL) {
			bool buy = o->buy;
			int sp = stop_price(b, centre, buy);
			int lp = buy ? sp + rng_uniform(1, 5)
			             : sp - rng_uniform(1, 5);
			if (lp < 1)
				lp = 1;
			lob_modify_stop_limit_order(b, tid,
			    rng_uniform(1, 1000), lp, sp);
		}
	}
	return id;
}

int
main(int argc, char **argv)
{
	long n_orders = (argc > 1) ? atol(argv[1]) : 1000000;
	long n_initial = (argc > 2) ? atol(argv[2]) : 11000;
	uint64_t seed = (argc > 3) ? strtoull(argv[3], NULL, 10) : 12345;
	const int centre = 300;

	g_state = seed ? seed : 1;

	lob_book_t *book = NULL;
	if (lob_book_create(&book) != XTC_OK) {
		fprintf(stderr, "book create failed\n");
		return 1;
	}

	int next_id = seed_book(book, (int)n_initial, centre, 1);
	printf("seeded %ld resting orders (+ ~%ld stops); centre=%d\n",
	    n_initial, n_initial / 10, centre);

	int64_t *lat = xtc_malloc((size_t)n_orders * sizeof(int64_t));
	if (lat == NULL) {
		fprintf(stderr, "latency buffer alloc failed\n");
		lob_book_destroy(book);
		return 1;
	}

	int64_t wall_start = xtc_clock_mono();
	for (long i = 0; i < n_orders; i++) {
		int64_t t0 = xtc_clock_mono();
		gen_one(book, centre, &next_id);
		int64_t t1 = xtc_clock_mono();
		lat[i] = t1 - t0;
	}
	int64_t wall_end = xtc_clock_mono();

	double wall_s = (double)(wall_end - wall_start) / 1e9;
	double tps = wall_s > 0 ? (double)n_orders / wall_s : 0;

	qsort(lat, (size_t)n_orders, sizeof(int64_t), cmp_i64);
	int64_t p50 = percentile(lat, (size_t)n_orders, 0.50);
	int64_t p99 = percentile(lat, (size_t)n_orders, 0.99);
	int64_t p999 = percentile(lat, (size_t)n_orders, 0.999);

	double sum = 0;
	for (long i = 0; i < n_orders; i++)
		sum += (double)lat[i];
	double mean = sum / (double)n_orders;

	printf("\nprocessed %ld orders in %.3f s\n", n_orders, wall_s);
	printf("throughput: %.0f orders/s (%.2f M TPS)\n", tps, tps / 1e6);
	printf("latency ns: mean=%.0f  p50=%lld  p99=%lld  p999=%lld  max=%lld\n",
	    mean, (long long)p50, (long long)p99, (long long)p999,
	    (long long)lat[n_orders - 1]);

	xtc_free(lat);
	lob_book_destroy(book);
	return 0;
}
