/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m1/test_tuning.c
 *	Power/kernel-tuning advisor (PLAN.md 19.21): __os_tuning_check()
 *	via the Linux-only fixture-path test seam
 *	(__xtc_os_tuning_*_path_override / __xtc_os_tuning_uring_force),
 *	the same pattern test/m1/test_cpu_cgroup.c uses for the cgroup
 *	probes.  A private xtc_log_t with a capture sink lets each case
 *	assert a probe's advisory line is present ("needs recommendation"
 *	fixture), absent ("already tuned" fixture), or absent with no
 *	error ("missing file" -- skip silently).  Runs without root or a
 *	real kernel-tuning knob.  On non-Linux the seam does not exist
 *	(every probe targets a Linux-only /proc or /sys path), so this
 *	whole suite is a no-op there -- see the guard around main().
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1       /* mkstemp/fdopen */
#endif

#include "munit.h"
#include "xtc.h"
#include "xtc_int.h"
#include "xtc_log.h"
#include "os_tuning.h"

#if defined(__linux__)

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ----- fixture + log-capture plumbing ------------------------------ */

static char   g_log_buf[8192];
static size_t g_log_n;

static int
sink_capture(void *user, xtc_log_level_t lvl, const char *buf, size_t len)
{
	(void)user; (void)lvl;
	if (g_log_n + len < sizeof g_log_buf) {
		memcpy(g_log_buf + g_log_n, buf, len);
		g_log_n += len;
	}
	return 0;
}

/* Run __os_tuning_check() with a fresh log and return the captured
 * text (NUL-terminated, valid until the next call). */
static const char *
run_check(void)
{
	xtc_log_t *log;
	xtc_log_opts_t opts = XTC_LOG_OPTS_DEFAULT;

	g_log_n = 0;
	memset(g_log_buf, 0, sizeof g_log_buf);
	opts.sink = sink_capture;
	opts.sink_fd = -1;
	munit_assert_int(xtc_log_create(&opts, &log), ==, XTC_OK);
	xtc_log_set_default(log);

	__os_tuning_check();

	munit_assert_int(xtc_log_drain(log), >=, 0);
	xtc_log_set_default(NULL);
	xtc_log_destroy(log);
	return g_log_buf;
}

/* Write "contents" to a fresh temp file, return its path in "path"
 * (caller-sized buffer).  Aborts the test on failure. */
static void
write_fixture(char *path, size_t pathsize, const char *contents)
{
	char tmpl[] = "/tmp/xtc_tuning_fixture_XXXXXX";
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

/* Clear every override back to "use the real path", and un-force the
 * io_uring probe -- run before AND after every case so one test's
 * fixture never leaks into the next. */
static void
clear_overrides(void)
{
	__xtc_os_tuning_governor_path_override(NULL);
	__xtc_os_tuning_pstate_status_path_override(NULL);
	__xtc_os_tuning_thp_path_override(NULL);
	__xtc_os_tuning_swappiness_path_override(NULL);
	__xtc_os_tuning_autogroup_path_override(NULL);
	__xtc_os_tuning_uring_force(0);
}

static void *
setup(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	clear_overrides();
	return NULL;
}

static void
teardown(void *d)
{
	(void)d;
	clear_overrides();
}

/* ----- governor ------------------------------------------------- */

static MunitResult
test_governor_needs_rec(const MunitParameter p[], void *d)
{
	char path[64];
	(void)p; (void)d;
	write_fixture(path, sizeof path, "powersave\n");
	__xtc_os_tuning_governor_path_override(path);
	munit_assert_not_null(strstr(run_check(), "cpu governor"));
	(void)unlink(path);
	return MUNIT_OK;
}

static MunitResult
test_governor_already_tuned(const MunitParameter p[], void *d)
{
	char path[64];
	(void)p; (void)d;
	write_fixture(path, sizeof path, "performance\n");
	__xtc_os_tuning_governor_path_override(path);
	munit_assert_null(strstr(run_check(), "cpu governor"));
	(void)unlink(path);
	return MUNIT_OK;
}

static MunitResult
test_governor_missing(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	__xtc_os_tuning_governor_path_override(
	    "/tmp/xtc_tuning_fixture_does_not_exist");
	munit_assert_null(strstr(run_check(), "cpu governor"));
	return MUNIT_OK;
}

/* ----- intel_pstate ---------------------------------------------- */

static MunitResult
test_pstate_needs_rec(const MunitParameter p[], void *d)
{
	char path[64];
	(void)p; (void)d;
	write_fixture(path, sizeof path, "passive\n");
	__xtc_os_tuning_pstate_status_path_override(path);
	munit_assert_not_null(strstr(run_check(), "intel_pstate"));
	(void)unlink(path);
	return MUNIT_OK;
}

static MunitResult
test_pstate_already_tuned(const MunitParameter p[], void *d)
{
	char path[64];
	(void)p; (void)d;
	write_fixture(path, sizeof path, "active\n");
	__xtc_os_tuning_pstate_status_path_override(path);
	munit_assert_null(strstr(run_check(), "intel_pstate"));
	(void)unlink(path);
	return MUNIT_OK;
}

/* Driver not loaded (file absent) -- deliberately not a finding: see
 * the intel_pstate reasoning in src/os/os_tuning.c. */
static MunitResult
test_pstate_missing(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	__xtc_os_tuning_pstate_status_path_override(
	    "/tmp/xtc_tuning_fixture_does_not_exist");
	munit_assert_null(strstr(run_check(), "intel_pstate"));
	return MUNIT_OK;
}

/* ----- transparent hugepage ---------------------------------------- */

static MunitResult
test_thp_needs_rec_always(const MunitParameter p[], void *d)
{
	char path[128];
	(void)p; (void)d;
	write_fixture(path, sizeof path, "[always] madvise never\n");
	__xtc_os_tuning_thp_path_override(path);
	munit_assert_not_null(strstr(run_check(), "transparent_hugepage"));
	(void)unlink(path);
	return MUNIT_OK;
}

static MunitResult
test_thp_needs_rec_never(const MunitParameter p[], void *d)
{
	char path[128];
	(void)p; (void)d;
	write_fixture(path, sizeof path, "always madvise [never]\n");
	__xtc_os_tuning_thp_path_override(path);
	munit_assert_not_null(strstr(run_check(), "transparent_hugepage"));
	(void)unlink(path);
	return MUNIT_OK;
}

static MunitResult
test_thp_already_tuned(const MunitParameter p[], void *d)
{
	char path[128];
	(void)p; (void)d;
	write_fixture(path, sizeof path, "always [madvise] never\n");
	__xtc_os_tuning_thp_path_override(path);
	munit_assert_null(strstr(run_check(), "transparent_hugepage"));
	(void)unlink(path);
	return MUNIT_OK;
}

static MunitResult
test_thp_missing(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	__xtc_os_tuning_thp_path_override(
	    "/tmp/xtc_tuning_fixture_does_not_exist");
	munit_assert_null(strstr(run_check(), "transparent_hugepage"));
	return MUNIT_OK;
}

/* ----- vm.swappiness ------------------------------------------- */

static MunitResult
test_swappiness_needs_rec(const MunitParameter p[], void *d)
{
	char path[64];
	(void)p; (void)d;
	write_fixture(path, sizeof path, "60\n");
	__xtc_os_tuning_swappiness_path_override(path);
	munit_assert_not_null(strstr(run_check(), "swappiness"));
	(void)unlink(path);
	return MUNIT_OK;
}

/* Exactly the threshold (10) is NOT a finding: the rule is "above 10". */
static MunitResult
test_swappiness_at_threshold(const MunitParameter p[], void *d)
{
	char path[64];
	(void)p; (void)d;
	write_fixture(path, sizeof path, "10\n");
	__xtc_os_tuning_swappiness_path_override(path);
	munit_assert_null(strstr(run_check(), "swappiness"));
	(void)unlink(path);
	return MUNIT_OK;
}

static MunitResult
test_swappiness_already_tuned(const MunitParameter p[], void *d)
{
	char path[64];
	(void)p; (void)d;
	write_fixture(path, sizeof path, "1\n");
	__xtc_os_tuning_swappiness_path_override(path);
	munit_assert_null(strstr(run_check(), "swappiness"));
	(void)unlink(path);
	return MUNIT_OK;
}

static MunitResult
test_swappiness_missing(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	__xtc_os_tuning_swappiness_path_override(
	    "/tmp/xtc_tuning_fixture_does_not_exist");
	munit_assert_null(strstr(run_check(), "swappiness"));
	return MUNIT_OK;
}

/* ----- kernel.sched_autogroup_enabled --------------------------- */

static MunitResult
test_autogroup_needs_rec(const MunitParameter p[], void *d)
{
	char path[64];
	(void)p; (void)d;
	write_fixture(path, sizeof path, "1\n");
	__xtc_os_tuning_autogroup_path_override(path);
	munit_assert_not_null(strstr(run_check(), "sched_autogroup_enabled"));
	(void)unlink(path);
	return MUNIT_OK;
}

static MunitResult
test_autogroup_already_tuned(const MunitParameter p[], void *d)
{
	char path[64];
	(void)p; (void)d;
	write_fixture(path, sizeof path, "0\n");
	__xtc_os_tuning_autogroup_path_override(path);
	munit_assert_null(strstr(run_check(), "sched_autogroup_enabled"));
	(void)unlink(path);
	return MUNIT_OK;
}

static MunitResult
test_autogroup_missing(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	__xtc_os_tuning_autogroup_path_override(
	    "/tmp/xtc_tuning_fixture_does_not_exist");
	munit_assert_null(strstr(run_check(), "sched_autogroup_enabled"));
	return MUNIT_OK;
}

/* ----- io_uring under seccomp ------------------------------------ */

static MunitResult
test_uring_blocked(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	__xtc_os_tuning_uring_force(1);   /* pretend EPERM */
	munit_assert_not_null(strstr(run_check(), "io_uring_setup"));
	return MUNIT_OK;
}

static MunitResult
test_uring_usable(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	__xtc_os_tuning_uring_force(-1);  /* pretend usable */
	munit_assert_null(strstr(run_check(), "io_uring_setup"));
	return MUNIT_OK;
}

/* ----- everything already tuned: total silence -------------------- */

static MunitResult
test_all_tuned_is_silent(const MunitParameter p[], void *d)
{
	char gov[64], pst[64], thp[128], swp[64], ag[64];
	const char *out;
	(void)p; (void)d;

	write_fixture(gov, sizeof gov, "performance\n");
	write_fixture(pst, sizeof pst, "active\n");
	write_fixture(thp, sizeof thp, "always [madvise] never\n");
	write_fixture(swp, sizeof swp, "1\n");
	write_fixture(ag, sizeof ag, "0\n");
	__xtc_os_tuning_governor_path_override(gov);
	__xtc_os_tuning_pstate_status_path_override(pst);
	__xtc_os_tuning_thp_path_override(thp);
	__xtc_os_tuning_swappiness_path_override(swp);
	__xtc_os_tuning_autogroup_path_override(ag);
	__xtc_os_tuning_uring_force(-1);

	out = run_check();
	munit_assert_size(strlen(out), ==, 0);

	(void)unlink(gov); (void)unlink(pst); (void)unlink(thp);
	(void)unlink(swp); (void)unlink(ag);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/governor_needs_rec",     test_governor_needs_rec,     setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/governor_already_tuned", test_governor_already_tuned, setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/governor_missing",       test_governor_missing,       setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/pstate_needs_rec",       test_pstate_needs_rec,       setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/pstate_already_tuned",   test_pstate_already_tuned,   setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/pstate_missing",         test_pstate_missing,         setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/thp_needs_rec_always",   test_thp_needs_rec_always,   setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/thp_needs_rec_never",    test_thp_needs_rec_never,    setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/thp_already_tuned",      test_thp_already_tuned,      setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/thp_missing",            test_thp_missing,            setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/swappiness_needs_rec",   test_swappiness_needs_rec,   setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/swappiness_at_threshold",test_swappiness_at_threshold,setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/swappiness_already_tuned", test_swappiness_already_tuned, setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/swappiness_missing",     test_swappiness_missing,     setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/autogroup_needs_rec",    test_autogroup_needs_rec,    setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/autogroup_already_tuned",test_autogroup_already_tuned,setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/autogroup_missing",      test_autogroup_missing,      setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/uring_blocked",          test_uring_blocked,          setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/uring_usable",           test_uring_usable,           setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/all_tuned_is_silent",    test_all_tuned_is_silent,    setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m1/tuning", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }

#else /* !__linux__: no tuning seam; nothing to test here. */

int
main(void)
{
	return 0;
}

#endif
