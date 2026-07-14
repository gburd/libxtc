/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m1/test_cpu_cgroup.c
 *	Container/cgroup awareness (PLAN.md 19.15): __os_ncpus() reading
 *	cgroup v2 cpu.max, and __os_mem_max() reading cgroup v2
 *	memory.max, via the Linux-only fixture-path test seam
 *	(__xtc_os_cgroup_cpu_path_override / _mem_path_override) so this
 *	runs without root or a real cgroup.  On non-Linux platforms the
 *	seam does not exist (no cgroup concept), so this whole suite is
 *	a no-op there -- see the guard around main().
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1       /* mkstemp/fdopen */
#endif

#include "munit.h"
#include "xtc.h"
#include "xtc_int.h"

#if defined(__linux__)

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Write "contents" to a fresh temp file, return its path in "path"
 * (caller-sized buffer).  Aborts the test on failure. */
static void
write_fixture(char *path, size_t pathsize, const char *contents)
{
	char tmpl[] = "/tmp/xtc_cgroup_fixture_XXXXXX";
	int fd;
	FILE *f;

	fd = mkstemp(tmpl);
	munit_assert_int(fd, >=, 0);
	munit_assert_size(strlen(tmpl) + 1, <=, pathsize);
	memcpy(path, tmpl, strlen(tmpl) + 1);

	f = fdopen(fd, "w");
	munit_assert_not_null(f);
	munit_assert_true(fputs(contents, f) >= 0);
	munit_assert_int(fclose(f), ==, 0);
}

/* --- __os_ncpus() cgroup cases ------------------------------------- */

/* "100000 100000" (100ms quota / 100ms period) -> exactly 1 CPU. */
static MunitResult
test_ncpus_cgroup_one(const MunitParameter p[], void *d)
{
	char path[64];
	(void)p; (void)d;

	write_fixture(path, sizeof path, "100000 100000\n");
	__xtc_os_cgroup_cpu_path_override(path);
	munit_assert_int(__os_ncpus(), ==, 1);
	__xtc_os_cgroup_cpu_path_override(NULL);
	(void)unlink(path);
	return MUNIT_OK;
}

/* "max 100000" (unlimited quota) -> falls back to the real hw count. */
static MunitResult
test_ncpus_cgroup_max(const MunitParameter p[], void *d)
{
	char path[64];
	int hw;
	(void)p; (void)d;

	__xtc_os_cgroup_cpu_path_override(NULL);
	hw = __os_ncpus();   /* the uncapped baseline, seam off */

	write_fixture(path, sizeof path, "max 100000\n");
	__xtc_os_cgroup_cpu_path_override(path);
	munit_assert_int(__os_ncpus(), ==, hw);
	__xtc_os_cgroup_cpu_path_override(NULL);
	(void)unlink(path);
	return MUNIT_OK;
}

/* A missing fixture file -> falls back to the real hw count. */
static MunitResult
test_ncpus_cgroup_missing(const MunitParameter p[], void *d)
{
	int hw;
	(void)p; (void)d;

	__xtc_os_cgroup_cpu_path_override(NULL);
	hw = __os_ncpus();

	__xtc_os_cgroup_cpu_path_override("/tmp/xtc_cgroup_fixture_does_not_exist");
	munit_assert_int(__os_ncpus(), ==, hw);
	__xtc_os_cgroup_cpu_path_override(NULL);
	return MUNIT_OK;
}

/* A quota bigger than one period (e.g. 2.5 cores worth) rounds UP:
 * ceil(250000/100000) == 3, clamped to the real hw count. */
static MunitResult
test_ncpus_cgroup_ceil(const MunitParameter p[], void *d)
{
	char path[64];
	int hw, want;
	(void)p; (void)d;

	__xtc_os_cgroup_cpu_path_override(NULL);
	hw = __os_ncpus();
	want = hw < 3 ? hw : 3;

	write_fixture(path, sizeof path, "250000 100000\n");
	__xtc_os_cgroup_cpu_path_override(path);
	munit_assert_int(__os_ncpus(), ==, want);
	__xtc_os_cgroup_cpu_path_override(NULL);
	(void)unlink(path);
	return MUNIT_OK;
}

/* --- __os_mem_max() cgroup cases ------------------------------------ */

/* A numeric memory.max is returned verbatim. */
static MunitResult
test_mem_max_cgroup_numeric(const MunitParameter p[], void *d)
{
	char path[64];
	(void)p; (void)d;

	write_fixture(path, sizeof path, "104857600\n");   /* 100 MiB */
	__xtc_os_cgroup_mem_path_override(path);
	munit_assert_int64(__os_mem_max(), ==, 104857600);
	__xtc_os_cgroup_mem_path_override(NULL);
	(void)unlink(path);
	return MUNIT_OK;
}

/* "max" (unlimited) -> falls back to the host total. */
static MunitResult
test_mem_max_cgroup_max(const MunitParameter p[], void *d)
{
	char path[64];
	int64_t host;
	(void)p; (void)d;

	__xtc_os_cgroup_mem_path_override(NULL);
	host = __os_mem_max();
	munit_assert_int64(host, >, 0);

	write_fixture(path, sizeof path, "max\n");
	__xtc_os_cgroup_mem_path_override(path);
	munit_assert_int64(__os_mem_max(), ==, host);
	__xtc_os_cgroup_mem_path_override(NULL);
	(void)unlink(path);
	return MUNIT_OK;
}

/* A missing fixture file -> falls back to the host total. */
static MunitResult
test_mem_max_cgroup_missing(const MunitParameter p[], void *d)
{
	int64_t host;
	(void)p; (void)d;

	__xtc_os_cgroup_mem_path_override(NULL);
	host = __os_mem_max();

	__xtc_os_cgroup_mem_path_override(
	    "/tmp/xtc_cgroup_fixture_does_not_exist");
	munit_assert_int64(__os_mem_max(), ==, host);
	__xtc_os_cgroup_mem_path_override(NULL);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/ncpus_cgroup_one",     test_ncpus_cgroup_one,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/ncpus_cgroup_max",     test_ncpus_cgroup_max,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/ncpus_cgroup_missing", test_ncpus_cgroup_missing, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/ncpus_cgroup_ceil",    test_ncpus_cgroup_ceil,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/mem_max_cgroup_numeric", test_mem_max_cgroup_numeric, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/mem_max_cgroup_max",     test_mem_max_cgroup_max,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/mem_max_cgroup_missing", test_mem_max_cgroup_missing, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m1/cpu_cgroup", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }

#else /* !__linux__: no cgroup seam; nothing to test here. */

int
main(void)
{
	return 0;
}

#endif
