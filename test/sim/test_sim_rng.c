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
#include "xtc_exec.h"
#include "xtc_io.h"
#include "sim_int.h"
#include "os_time.h"

#define N 64

static MunitResult
test_inactive_by_default(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	xtc_sim_deactivate();
	munit_assert_int(__xtc_sim_active(), ==, 0);
	munit_assert_int(xtc_sim_active(), ==, 0);
	/* Outside a run the application draw is xtc_rand_u64 % bound. */
	munit_assert_uint64(xtc_sim_rand(0), ==, 0);
	munit_assert_uint64(xtc_sim_rand(10), <, 10);
	return MUNIT_OK;
}

static void
count_call(void *arg)
{
	(*(int *)arg)++;
}

/* Off a sim build the simulator refuses to run instead of reporting a
 * replay it did not make (2.1 returned XTC_OK after stepping the loops
 * over the native backend), the fd seam refuses too, and an external
 * completion is delivered at once on the completing thread. */
static MunitResult
test_native_build(const MunitParameter p[], void *d)
{
	xtc_exec_t *e = NULL;
	xtc_sim_ext_t *t = NULL;
	int hits = 0;
	(void)p; (void)d;

	if (strcmp(xtc_io_backend_name(), "sim") == 0)
		return MUNIT_SKIP;
	munit_assert_int(xtc_exec_create(&(xtc_exec_opts_t){
	    .size = sizeof(xtc_exec_opts_t), .n_loops = 1 }, &e), ==, XTC_OK);
	munit_assert_int(xtc_sim_exec_run(e, 7, 1000), ==, XTC_E_NOSYS);
	munit_assert_int(xtc_sim_active(), ==, 0);
	xtc_exec_destroy(e);
	munit_assert_int(xtc_sim_fd_ready_at(0, 1, XTC_IO_READABLE), ==,
	    XTC_E_NOSYS);
	munit_assert_int(xtc_sim_fd_ready_at(-1, 1, XTC_IO_READABLE), ==,
	    XTC_E_INVAL);
	munit_assert_int(xtc_sim_external_begin(0, 1000, &t), ==, XTC_OK);
	munit_assert_ptr_null(t);
	munit_assert_int(xtc_sim_external_complete(t, count_call, &hits), ==,
	    XTC_OK);
	munit_assert_int(hits, ==, 1);
	return MUNIT_OK;
}

/* xtc_sim_rand is the APP stream under activation: the same draws a
 * runtime-internal __xtc_sim_rng_range(XTC_SIM_RNG_APP, ...) gets, and
 * replayed by the seed. */
static MunitResult
test_app_rand(const MunitParameter p[], void *d)
{
	uint64_t a[N], b[N];
	int i;
	(void)p; (void)d;

	xtc_sim_activate(0xA99);
	munit_assert_int(xtc_sim_active(), ==, 1);
	for (i = 0; i < N; i++) a[i] = xtc_sim_rand(1000);
	xtc_sim_activate(0xA99);
	for (i = 0; i < N; i++)
		b[i] = __xtc_sim_rng_range(XTC_SIM_RNG_APP, 1000);
	munit_assert_memory_equal(sizeof a, a, b);
	xtc_sim_deactivate();
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

/* Virtual clock controls __os_clock_mono deterministically, and
 * reverting hands control back to the real monotonic clock. */
static MunitResult
test_virtual_clock(const MunitParameter p[], void *d)
{
	int64_t a = 0, b = 0;
	(void)p; (void)d;

	xtc_sim_clock_enable(1000000);
	munit_assert_int(__os_clock_mono(&a), ==, XTC_OK);
	munit_assert_int64(a, ==, 1000000);

	xtc_sim_clock_advance(500);
	munit_assert_int(__os_clock_mono(&b), ==, XTC_OK);
	munit_assert_int64(b, ==, 1000500);

	xtc_sim_clock_set(42);
	munit_assert_int(__os_clock_mono(&a), ==, XTC_OK);
	munit_assert_int64(a, ==, 42);

	/* Disabled: real monotonic clock, which only moves forward. */
	xtc_sim_clock_disable();
	munit_assert_int(__os_clock_mono(&a), ==, XTC_OK);
	munit_assert_int(__os_clock_mono(&b), ==, XTC_OK);
	munit_assert_int64(b, >=, a);
	munit_assert_int64(a, !=, 42);   /* not the virtual value anymore */
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/inactive_default",   test_inactive_by_default, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/seed_reproducible",  test_seed_reproducible,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/seed_differs",       test_seed_differs,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/app_rand",           test_app_rand,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/native_build",       test_native_build,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/streams_independent",test_streams_independent, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/rng_range",          test_rng_range,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/virtual_clock",      test_virtual_clock,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/sim/rng", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char **argv) { return munit_suite_main(&suite, NULL, argc, argv); }
