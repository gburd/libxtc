/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m1/test_cpu.c
 *	CPU topology probes: total / performance / efficiency core
 *	counts, the reactor QoS hint, and the CPU-affinity hint
 *	(including an enforcement check that fails on a host that
 *	claims to pin but does not).
 */

#if defined(__linux__)
#  ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#  endif
#  include <sched.h>
#  include <pthread.h>
#endif

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

/*
 * Enforcement check: this FAILS when a host that promises to pin does
 * not actually do so.  "Properly supporting the CPU-pin hint" is
 * platform-specific:
 *   - Linux: __os_thread_set_affinity is a hard pin, so we verify the
 *     thread's effective affinity mask is exactly {cpu} AND that it is
 *     actually running on that CPU.  A restrictive cpuset/seccomp that
 *     silently refuses the pin makes this fail -- which is the intent.
 *   - Apple Silicon: the kernel accepts the affinity tag but ignores
 *     it (no enforcement is promised), so physical placement is
 *     unverifiable; we assert the API is accepted and SKIP the
 *     enforcement assertion rather than fail on a guarantee the
 *     platform never makes.
 *   - Other platforms: SKIP until an enforcement check is ported.
 */
static MunitResult
test_affinity_enforced(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
#if defined(__linux__)
	{
		cpu_set_t mask;
		int cpu = 0;            /* CPU 0 always exists */
		int i, set_count;

		munit_assert_int(__os_thread_set_affinity(cpu), ==, XTC_OK);

		CPU_ZERO(&mask);
		munit_assert_int(
		    pthread_getaffinity_np(pthread_self(), sizeof mask, &mask),
		    ==, 0);
		munit_assert_true(CPU_ISSET(cpu, &mask));
		set_count = 0;
		for (i = 0; i < CPU_SETSIZE; i++)
			if (CPU_ISSET(i, &mask))
				set_count++;
		munit_assert_int(set_count, ==, 1);   /* exactly {cpu} */

		for (i = 0; i < 1000 && sched_getcpu() != cpu; i++)
			sched_yield();
		munit_assert_int(sched_getcpu(), ==, cpu);
		return MUNIT_OK;
	}
#elif defined(__APPLE__)
	munit_assert_int(__os_thread_set_affinity(0), ==, XTC_OK);
#  if defined(__aarch64__) || defined(__arm64__)
	/* Apple Silicon ignores affinity tags; enforcement unverifiable. */
	return MUNIT_SKIP;
#  else
	return MUNIT_OK;        /* Intel macOS: advisory tag accepted */
#  endif
#else
	munit_assert_int(__os_thread_set_affinity(0), ==, XTC_OK);
	return MUNIT_SKIP;
#endif
}

static MunitTest tests[] = {
	{ "/ncpus",       test_ncpus,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/perf_effic",  test_perf_effic,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/qos_hint",    test_qos_hint,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/affinity",    test_affinity_hint, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/affinity_enforced", test_affinity_enforced, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m1/cpu", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
