/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m1/test_fs.c -- portable filesystem helpers (xtc_fs_*).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1       /* setenv/unsetenv */
#endif

#include "munit.h"
#include "xtc_int.h"
#include "xtc_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* open -> pwrite -> pread round-trip, size, fsync, truncate. */
static MunitResult
test_file_io(const MunitParameter p[], void *d)
{
	char dir[512], tmpl[600];
	int fd = -1;
	int64_t sz = -1;
	size_t done = 0;
	char buf[64];
	static const char msg[] = "xtc_fs round-trip";
	(void)p; (void)d;

	munit_assert_int(xtc_fs_tmpdir(dir, sizeof dir), ==, XTC_OK);
	munit_assert_size(strlen(dir), >, 0);
	snprintf(tmpl, sizeof tmpl, "%s/xtcfs-io-XXXXXX", dir);

	munit_assert_int(xtc_fs_mkstemp(tmpl, &fd), ==, XTC_OK);
	munit_assert_int(fd, >=, 0);

	munit_assert_int(xtc_fs_pwrite(fd, msg, sizeof msg, 0, &done), ==, XTC_OK);
	munit_assert_size(done, ==, sizeof msg);
	munit_assert_int(xtc_fs_fsync(fd), ==, XTC_OK);

	munit_assert_int(xtc_fs_fsize(fd, &sz), ==, XTC_OK);
	munit_assert_int64(sz, ==, (int64_t)sizeof msg);

	memset(buf, 0, sizeof buf);
	done = 0;
	munit_assert_int(xtc_fs_pread(fd, buf, sizeof msg, 0, &done), ==, XTC_OK);
	munit_assert_size(done, ==, sizeof msg);
	munit_assert_string_equal(buf, msg);

	/* short read at EOF returns a partial count, not an error */
	done = 99;
	munit_assert_int(xtc_fs_pread(fd, buf, sizeof buf, (int64_t)sizeof msg,
	    &done), ==, XTC_OK);
	munit_assert_size(done, ==, 0);

	munit_assert_int(xtc_fs_ftruncate(fd, 4), ==, XTC_OK);
	munit_assert_int(xtc_fs_fsize(fd, &sz), ==, XTC_OK);
	munit_assert_int64(sz, ==, 4);

	munit_assert_int(xtc_fs_close(fd), ==, XTC_OK);
	munit_assert_int(xtc_fs_unlink(tmpl), ==, XTC_OK);
	munit_assert_int(xtc_fs_exists(tmpl), ==, 0);
	return MUNIT_OK;
}

/* stat, exists, rename, unlink on a named file. */
static MunitResult
test_namespace(const MunitParameter p[], void *d)
{
	char dir[512], a[600], b[600];
	int fd = -1;
	size_t done = 0;
	xtc_fs_stat_t st;
	(void)p; (void)d;

	munit_assert_int(xtc_fs_tmpdir(dir, sizeof dir), ==, XTC_OK);
	snprintf(a, sizeof a, "%s/xtcfs-ns-XXXXXX", dir);
	munit_assert_int(xtc_fs_mkstemp(a, &fd), ==, XTC_OK);
	munit_assert_int(xtc_fs_pwrite(fd, "abcd", 4, 0, &done), ==, XTC_OK);
	munit_assert_int(xtc_fs_close(fd), ==, XTC_OK);

	munit_assert_int(xtc_fs_exists(a), ==, 1);
	munit_assert_int(xtc_fs_stat(a, &st), ==, XTC_OK);
	munit_assert_int64(st.size, ==, 4);
	munit_assert_int(st.is_dir, ==, 0);

	/* rename to a sibling, old name gone, new name present */
	snprintf(b, sizeof b, "%s.renamed", a);
	munit_assert_int(xtc_fs_rename(a, b), ==, XTC_OK);
	munit_assert_int(xtc_fs_exists(a), ==, 0);
	munit_assert_int(xtc_fs_exists(b), ==, 1);

	/* stat of an absent path is NOTFOUND */
	munit_assert_int(xtc_fs_stat(a, &st), ==, XTC_E_NOTFOUND);

	munit_assert_int(xtc_fs_unlink(b), ==, XTC_OK);
	return MUNIT_OK;
}

/* mkdir, directory iteration (sees created entries, skips . and ..), rmdir. */
static MunitResult
test_dir(const MunitParameter p[], void *d)
{
	char dir[512], sub[600], f1[700], f2[700];
	int fd = -1, seen1 = 0, seen2 = 0, n = 0;
	const char *name;
	xtc_fs_dir_t *it = NULL;
	(void)p; (void)d;

	munit_assert_int(xtc_fs_tmpdir(dir, sizeof dir), ==, XTC_OK);
	/* a unique subdir via mkstemp-then-replace */
	snprintf(sub, sizeof sub, "%s/xtcfs-dir-XXXXXX", dir);
	munit_assert_int(xtc_fs_mkstemp(sub, &fd), ==, XTC_OK);
	munit_assert_int(xtc_fs_close(fd), ==, XTC_OK);
	munit_assert_int(xtc_fs_unlink(sub), ==, XTC_OK);  /* free the name */
	munit_assert_int(xtc_fs_mkdir(sub), ==, XTC_OK);

	snprintf(f1, sizeof f1, "%s/one", sub);
	snprintf(f2, sizeof f2, "%s/two", sub);
	munit_assert_int(xtc_fs_open(f1, XTC_FS_WRITE | XTC_FS_CREATE, &fd), ==, XTC_OK);
	munit_assert_int(xtc_fs_close(fd), ==, XTC_OK);
	munit_assert_int(xtc_fs_open(f2, XTC_FS_WRITE | XTC_FS_CREATE, &fd), ==, XTC_OK);
	munit_assert_int(xtc_fs_close(fd), ==, XTC_OK);

	munit_assert_int(xtc_fs_dir_open(sub, &it), ==, XTC_OK);
	for (;;) {
		munit_assert_int(xtc_fs_dir_next(it, &name), ==, XTC_OK);
		if (name == NULL) break;
		munit_assert_string_not_equal(name, ".");
		munit_assert_string_not_equal(name, "..");
		if (strcmp(name, "one") == 0) seen1 = 1;
		if (strcmp(name, "two") == 0) seen2 = 1;
		n++;
	}
	xtc_fs_dir_close(it);
	munit_assert_int(seen1, ==, 1);
	munit_assert_int(seen2, ==, 1);
	munit_assert_int(n, ==, 2);

	munit_assert_int(xtc_fs_unlink(f1), ==, XTC_OK);
	munit_assert_int(xtc_fs_unlink(f2), ==, XTC_OK);
	munit_assert_int(xtc_fs_rmdir(sub), ==, XTC_OK);
	return MUNIT_OK;
}

/* tmpdir honors $TMPDIR. */
static MunitResult
test_tmpdir_env(const MunitParameter p[], void *d)
{
	char dir[512];
	(void)p; (void)d;
#if !defined(_WIN32)
	setenv("TMPDIR", "/var/tmp/", 1);
	munit_assert_int(xtc_fs_tmpdir(dir, sizeof dir), ==, XTC_OK);
	munit_assert_string_equal(dir, "/var/tmp");   /* trailing slash trimmed */
	unsetenv("TMPDIR");
#else
	munit_assert_int(xtc_fs_tmpdir(dir, sizeof dir), ==, XTC_OK);
	munit_assert_size(strlen(dir), >, 0);
#endif
	munit_assert_int(xtc_fs_tmpdir(NULL, 10), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* argument validation. */
static MunitResult
test_einval(const MunitParameter p[], void *d)
{
	int fd = -1;
	(void)p; (void)d;
	munit_assert_int(xtc_fs_open(NULL, XTC_FS_READ, &fd), ==, XTC_E_INVAL);
	munit_assert_int(xtc_fs_open("x", XTC_FS_READ, NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_fs_stat("/no/such/xtc/path", NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_fs_exists("/no/such/xtc/path/zzz"), ==, 0);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/file_io",     test_file_io,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/namespace",   test_namespace,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/dir",         test_dir,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/tmpdir_env",  test_tmpdir_env,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/einval",      test_einval,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m1/fs", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
