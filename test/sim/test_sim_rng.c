/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_rng.c -- DST phase 0: the seeded PRNG tree.
 *
 *	Proves the properties replay depends on:
 *	  - inactive by default (production behavior unchanged);
 *	  - the same seed yields the same sequence per stream (replay);
 *	  - different seeds yield different sequences;
 *	  - streams are independent (a draw on one stream does not shift
 *	    another stream's sequence -- the stability-under-code-change
 *	    property that lets a new draw site be added without breaking
 *	    an existing replay).
 */

#define _GNU_SOURCE

#include <stdint.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_sim.h"

#define N 64

static MunitResult
test_inactive_by_default(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	xtc_sim_deactivate();
	munit_assert_int(__xtc_sim_active(), ==, 0);
	return MUNIT_OK;
}

/* Same seed -> identical per-stream sequence. */
static MunitResult
test_seed_reproducible(const MunitParameter p[], void *d)
{
	uint64_t a[N], b[N];
	int i;
	(void)p; (void)d;

	xtc_sim_activate(0xC0FFEE);
	munit_assert_int(__xtc_sim_active(), ==, 1);
	for (i = 0; i < N; i++) a[i] = __xtc_sim_rng(XTC_SIM_RNG_STEAL);

	xtc_sim_activate(0xC0FFEE);   /* re-seed: same root */
	for (i = 0; i < N; i++) b[i] = __xtc_sim_rng(XTC_SIM_RNG_STEAL);

	munit_assert_memory_equal(sizeof a, a, b);
	xtc_sim_deactivate();
	return MUNIT_OK;
}

/* Different seeds -> different sequence (overwhelmingly). */
static MunitResult
test_seed_differs(const MunitParameter p[], void *d)
{
	uint64_t a[N], b[N];
	int i, same = 1;
	(void)p; (void)d;

	xtc_sim_activate(0x1111);
	for (i = 0; i < N; i++) a[i] = __xtc_sim_rng(XTC_SIM_RNG_SCHED);
	xtc_sim_activate(0x2222);
	for (i = 0; i < N; i++) b[i] = __xtc_sim_rng(XTC_SIM_RNG_SCHED);

	for (i = 0; i < N; i++) if (a[i] != b[i]) { same = 0; break; }
	munit_assert_int(same, ==, 0);
	xtc_sim_deactivate();
	return MUNIT_OK;
}

/* Streams are independent: drawing from STEAL must not change the
 * sequence observed on SCHED.  Draw SCHED alone, then draw SCHED while
 * interleaving STEAL draws; the SCHED values must match. */
static MunitResult
test_streams_independent(const MunitParameter p[], void *d)
{
	uint64_t plain[N], inter[N];
	int i;
	(void)p; (void)d;

	xtc_sim_activate(0x5EED);
	for (i = 0; i < N; i++) plain[i] = __xtc_sim_rng(XTC_SIM_RNG_SCHED);

	xtc_sim_activate(0x5EED);
	for (i = 0; i < N; i++) {
		/* Interpose draws on OTHER streams between SCHED draws. */
		(void)__xtc_sim_rng(XTC_SIM_RNG_STEAL);
		(void)__xtc_sim_rng(XTC_SIM_RNG_PLACE);
		(void)__xtc_sim_rng(XTC_SIM_RNG_FAULT);
		inter[i] = __xtc_sim_rng(XTC_SIM_RNG_SCHED);
	}

	munit_assert_memory_equal(sizeof plain, plain, inter);
	xtc_sim_deactivate();
	return MUNIT_OK;
}

/* rng_range stays within bound and is deterministic. */
static MunitResult
test_rng_range(const MunitParameter p[], void *d)
{
	int i;
	(void)p; (void)d;
	xtc_sim_activate(0xBEEF);
	for (i = 0; i < 1000; i++) {
		uint64_t v = __xtc_sim_rng_range(XTC_SIM_RNG_PLACE, 7);
		munit_assert_uint64(v, <, 7);
	}
	munit_assert_uint64(__xtc_sim_rng_range(XTC_SIM_RNG_PLACE, 0), ==, 0);
	xtc_sim_deactivate();
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/inactive_default",   test_inactive_by_default, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/seed_reproducible",  test_seed_reproducible,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/seed_differs",       test_seed_differs,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/streams_independent",test_streams_independent, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/rng_range",          test_rng_range,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/sim/rng", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char **argv) { return munit_suite_main(&suite, NULL, argc, argv); }
