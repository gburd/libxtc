/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m13/test_prob.c -- verifies M14 xtc_bloom + xtc_hll.
 *
 * Statistical, not exact: the Bloom test asserts ZERO false negatives
 * (a hard invariant) and a measured false-positive rate within ~2x of
 * the configured target (a probabilistic bound).  The HLL test asserts
 * the estimate is within ~2-3% relative error across cardinalities from
 * 100 to 1e6, and that merging two disjoint sketches estimates the
 * union.  A fixed munit seed keeps the run deterministic.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_prob.h"

/* ---------- Bloom: no false negatives, FP rate near target ---------- */

#define BLOOM_N   50000

static MunitResult
test_bloom_basic(const MunitParameter p[], void *d)
{
	xtc_bloom_t *b;
	(void)p; (void)d;

	/* NULL / invalid args. */
	munit_assert_int(xtc_bloom_create(100, 0.01, NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_bloom_create(100, 0.0, &b), ==, XTC_E_INVAL);
	munit_assert_int(xtc_bloom_create(100, 1.0, &b), ==, XTC_E_INVAL);
	munit_assert_int(xtc_bloom_create(100, -0.5, &b), ==, XTC_E_INVAL);
	munit_assert_int(xtc_bloom_maybe_contains(NULL, "x", 1), ==, 0);

	munit_assert_int(xtc_bloom_create(100, 0.01, &b), ==, XTC_OK);
	/* Empty filter: everything definitely absent. */
	munit_assert_int(xtc_bloom_maybe_contains(b, "x", 1), ==, 0);
	xtc_bloom_add(b, "x", 1);
	munit_assert_int(xtc_bloom_maybe_contains(b, "x", 1), ==, 1);
	/* Idempotent add + empty key. */
	xtc_bloom_add(b, "x", 1);
	munit_assert_int(xtc_bloom_maybe_contains(b, "x", 1), ==, 1);
	xtc_bloom_add(b, NULL, 0);
	munit_assert_int(xtc_bloom_maybe_contains(b, NULL, 0), ==, 1);
	xtc_bloom_destroy(b);
	xtc_bloom_destroy(NULL);   /* NULL is a no-op */
	return MUNIT_OK;
}

/* ---- Bloom sizing edge cases: tiny n, extreme fp_rate (k caps) ---- */
static MunitResult
test_bloom_sizing_edges(const MunitParameter p[], void *d)
{
	xtc_bloom_t *b;
	(void)p; (void)d;

	/* n_expected == 0 is bumped to 1 internally (no divide-by-zero). */
	munit_assert_int(xtc_bloom_create(0, 0.01, &b), ==, XTC_OK);
	xtc_bloom_add(b, "a", 1);
	munit_assert_int(xtc_bloom_maybe_contains(b, "a", 1), ==, 1);
	xtc_bloom_destroy(b);

	/* fp_rate very close to 1: m_opt tiny (clamped to >=1), k clamped
	 * to >=1.  Still no false negatives. */
	munit_assert_int(xtc_bloom_create(1, 0.999, &b), ==, XTC_OK);
	xtc_bloom_add(b, "a", 1);
	munit_assert_int(xtc_bloom_maybe_contains(b, "a", 1), ==, 1);
	xtc_bloom_destroy(b);

	/* fp_rate extremely small with a huge n: k would exceed 64, so it
	 * is capped at 64.  A handful of keys still test present. */
	munit_assert_int(xtc_bloom_create(1000000, 1e-18, &b), ==, XTC_OK);
	xtc_bloom_add(b, "a", 1);
	xtc_bloom_add(b, "b", 1);
	munit_assert_int(xtc_bloom_maybe_contains(b, "a", 1), ==, 1);
	munit_assert_int(xtc_bloom_maybe_contains(b, "b", 1), ==, 1);
	xtc_bloom_destroy(b);
	return MUNIT_OK;
}

static MunitResult
test_bloom_stats(const MunitParameter p[], void *d)
{
	xtc_bloom_t *b;
	const double target = 0.01;
	uint64_t i;
	uint64_t false_pos = 0, trials = 0;
	double rate;
	(void)p; (void)d;

	munit_assert_int(xtc_bloom_create(BLOOM_N, target, &b), ==, XTC_OK);

	/* Add keys 0..BLOOM_N-1 (as raw 8-byte spans). */
	for (i = 0; i < BLOOM_N; i++)
		xtc_bloom_add(b, &i, sizeof i);

	/* ZERO false negatives: every added key must test present. */
	for (i = 0; i < BLOOM_N; i++)
		munit_assert_int(xtc_bloom_maybe_contains(b, &i, sizeof i),
		    ==, 1);

	/* FP rate: probe a disjoint range never added. */
	for (i = BLOOM_N; i < BLOOM_N + 200000; i++) {
		trials++;
		if (xtc_bloom_maybe_contains(b, &i, sizeof i))
			false_pos++;
	}
	rate = (double)false_pos / (double)trials;
	munit_logf(MUNIT_LOG_INFO,
	    "bloom: target=%.4f measured=%.4f (%llu/%llu)",
	    target, rate, (unsigned long long)false_pos,
	    (unsigned long long)trials);

	/* Within ~2x of target (statistical bound). */
	munit_assert_double(rate, <, target * 2.0);

	xtc_bloom_destroy(b);
	return MUNIT_OK;
}

/* ---------- HLL: within a few % across cardinalities, merge = union ---- */

static double
hll_relerr(int precision, uint64_t n)
{
	xtc_hll_t *h;
	uint64_t i, est;
	double err;

	munit_assert_int(xtc_hll_create(precision, &h), ==, XTC_OK);
	for (i = 0; i < n; i++)
		xtc_hll_add(h, &i, sizeof i);
	est = xtc_hll_count(h);
	xtc_hll_destroy(h);
	err = ((double)est - (double)n) / (double)n;
	if (err < 0.0) err = -err;
	munit_logf(MUNIT_LOG_INFO,
	    "hll p=%d n=%llu est=%llu relerr=%.4f",
	    precision, (unsigned long long)n, (unsigned long long)est, err);
	return err;
}

static MunitResult
test_hll_basic(const MunitParameter p[], void *d)
{
	xtc_hll_t *h;
	(void)p; (void)d;

	munit_assert_int(xtc_hll_create(12, NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_hll_create(3, &h), ==, XTC_E_INVAL);
	munit_assert_int(xtc_hll_create(19, &h), ==, XTC_E_INVAL);
	munit_assert_uint64(xtc_hll_count(NULL), ==, 0);

	munit_assert_int(xtc_hll_create(12, &h), ==, XTC_OK);
	munit_assert_uint64(xtc_hll_count(h), ==, 0);   /* empty */
	xtc_hll_add(h, "one", 3);
	munit_assert_uint64(xtc_hll_count(h), >=, 1);
	munit_assert_uint64(xtc_hll_count(h), <=, 2);
	xtc_hll_add(h, NULL, 0);   /* empty key ok */
	xtc_hll_destroy(h);
	xtc_hll_destroy(NULL);        /* NULL no-op */
	return MUNIT_OK;
}

/* Low precisions 4/5/6 hit the special-cased alpha constants (m ==
 * 16/32/64).  A small distinct set still estimates in a sane range. */
static MunitResult
test_hll_low_precision(const MunitParameter p[], void *d)
{
	int prec;
	(void)p; (void)d;

	for (prec = 4; prec <= 6; prec++) {
		xtc_hll_t *h;
		uint64_t i, est;
		munit_assert_int(xtc_hll_create(prec, &h), ==, XTC_OK);
		for (i = 0; i < 50; i++)
			xtc_hll_add(h, &i, sizeof i);
		est = xtc_hll_count(h);
		/* p in [4,6] is coarse; just assert a sane, non-degenerate
		 * estimate (small-range linear counting keeps it positive). */
		munit_assert_uint64(est, >=, 20);
		munit_assert_uint64(est, <=, 120);
		xtc_hll_destroy(h);
	}
	return MUNIT_OK;
}

static MunitResult
test_hll_accuracy(const MunitParameter p[], void *d)
{
	(void)p; (void)d;

	/* p=14 => rel std error ~0.65%; allow ~3% to cover the tail and
	 * the small-range corrections at n=100/1000. */
	munit_assert_double(hll_relerr(14, 100),      <, 0.05);
	munit_assert_double(hll_relerr(14, 1000),     <, 0.03);
	munit_assert_double(hll_relerr(14, 10000),    <, 0.03);
	munit_assert_double(hll_relerr(14, 100000),   <, 0.03);
	munit_assert_double(hll_relerr(14, 1000000),  <, 0.03);
	return MUNIT_OK;
}

static MunitResult
test_hll_merge(const MunitParameter p[], void *d)
{
	xtc_hll_t *a, *b, *mismatch;
	uint64_t i, est;
	double err;
	const uint64_t half = 500000;
	(void)p; (void)d;

	munit_assert_int(xtc_hll_create(14, &a), ==, XTC_OK);
	munit_assert_int(xtc_hll_create(14, &b), ==, XTC_OK);

	/* Two DISJOINT key ranges: a gets [0,half), b gets [half,2*half). */
	for (i = 0; i < half; i++)          xtc_hll_add(a, &i, sizeof i);
	for (i = half; i < 2 * half; i++)   xtc_hll_add(b, &i, sizeof i);

	/* NULL / mismatch. */
	munit_assert_int(xtc_hll_merge(NULL, b), ==, XTC_E_INVAL);
	munit_assert_int(xtc_hll_merge(a, NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_hll_create(10, &mismatch), ==, XTC_OK);
	munit_assert_int(xtc_hll_merge(a, mismatch), ==, XTC_E_INVAL);
	xtc_hll_destroy(mismatch);

	munit_assert_int(xtc_hll_merge(a, b), ==, XTC_OK);
	est = xtc_hll_count(a);
	err = ((double)est - (double)(2 * half)) / (double)(2 * half);
	if (err < 0.0) err = -err;
	munit_logf(MUNIT_LOG_INFO,
	    "hll merge: union n=%llu est=%llu relerr=%.4f",
	    (unsigned long long)(2 * half), (unsigned long long)est, err);
	munit_assert_double(err, <, 0.03);

	xtc_hll_destroy(a);
	xtc_hll_destroy(b);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/bloom_basic",    test_bloom_basic,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/bloom_sizing",   test_bloom_sizing_edges, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/bloom_stats",    test_bloom_stats,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hll_basic",      test_hll_basic,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hll_low_prec",   test_hll_low_precision, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hll_accuracy",   test_hll_accuracy,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hll_merge",      test_hll_merge,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m13/prob", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
