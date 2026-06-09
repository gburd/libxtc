/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m1/test_cpu.c
 *	CPU topology probes: total / performance / efficiency core
 *	counts and the reactor QoS hint.
 */

#include "munit.h"
#include "xtc.h"
#include "xtc_int.h"

/* ncpus is always at least 1. */
static MunitResult
test_ncpus(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	munit_assert_int(__os_ncpus(), >=, 1);
	return MUNIT_OK;
}

/*
 * Perf/efficiency split is internally consistent:
 *   1 <= perf <= total
 *   effic == 0  (symmetric hardware), or
 *   perf + effic == total  (asymmetric: P + E partition the CPUs).
 */
static MunitResult
test_perf_effic(const MunitParameter p[], void *d)
{
	int total, perf, effic;
	(void)p; (void)d;

	total = __os_ncpus();
	perf  = __os_ncpus_perf();
	effic = __os_ncpus_effic();

	munit_assert_int(perf, >=, 1);
	munit_assert_int(perf, <=, total);
	munit_assert_int(effic, >=, 0);

	if (effic == 0)
		munit_assert_int(perf, ==, total);
	else
		munit_assert_int(perf + effic, ==, total);

	return MUNIT_OK;
}

/* The reactor QoS hint is best-effort and must never fault or fail;
 * it is a no-op on platforms without a QoS API. */
static MunitResult
test_qos_hint(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	__os_thread_apply_default_qos();
	__os_thread_apply_default_qos();   /* idempotent */
	return MUNIT_OK;
}

/*
 * The CPU-affinity hint: a negative CPU is rejected; a valid CPU on the
 * calling thread returns OK (Linux pin, macOS advisory tag) or NOSYS on
 * an unported platform.  Either way it must not fault.
 */
static MunitResult
test_affinity_hint(const MunitParameter p[], void *d)
{
	int rc;
	(void)p; (void)d;

	munit_assert_int(__os_thread_set_affinity(-1), ==, XTC_E_INVAL);

	rc = __os_thread_set_affinity(0);
	munit_assert_true(rc == XTC_OK || rc == XTC_E_NOSYS);

	/* A CPU index within the machine's count behaves the same. */
	rc = __os_thread_set_affinity(__os_ncpus() - 1);
	munit_assert_true(rc == XTC_OK || rc == XTC_E_NOSYS);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/ncpus",       test_ncpus,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/perf_effic",  test_perf_effic,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/qos_hint",    test_qos_hint,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/affinity",    test_affinity_hint, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m1/cpu", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
