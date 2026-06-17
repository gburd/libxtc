/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/m12/test_runtime.c -- xtc_runtime_info verification.
 */

#include "munit.h"
#include "xtc.h"
#include "xtc_runtime.h"

#include <stdint.h>

static MunitResult
test_runtime_basic(const MunitParameter p[], void *d)
{
	xtc_runtime_info_t ri;
	(void)p; (void)d;

	/* Off any loop (the test thread is not on an executor loop):
	 * the call still succeeds and reports a sane aggregate. */
	munit_assert_int(xtc_runtime_info(&ri), ==, XTC_OK);

	/* CPU topology: at least one online CPU; perf/effic each
	 * non-negative; perf+effic does not exceed the online count. */
	munit_assert_int(ri.n_cpus_online, >=, 1);
	munit_assert_int(ri.n_cpus_perf,   >=, 0);
	munit_assert_int(ri.n_cpus_effic,  >=, 0);
	munit_assert_int(ri.n_cpus_perf + ri.n_cpus_effic, <=, ri.n_cpus_online);

	/* At least one NUMA node always. */
	munit_assert_int(ri.numa_nodes, >=, 1);

	/* Off-loop default is 1 loop. */
	munit_assert_int(ri.n_loops, >=, 1);

	/* No global resource accountant -> memory fields are 0. */
	munit_assert_int64(ri.mem_cap_bytes,  ==, 0);
	munit_assert_int64(ri.mem_used_bytes, ==, 0);

	return MUNIT_OK;
}

static MunitResult
test_runtime_null(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	munit_assert_int(xtc_runtime_info(NULL), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/basic", test_runtime_basic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/null",  test_runtime_null,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = {
	"/m12/runtime", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
