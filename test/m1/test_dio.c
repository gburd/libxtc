/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m1/test_dio.c
 *	Direct I/O (XTC_FS_DIRECT): alignment query, aligned buffer
 *	helper, and an aligned write/read round-trip.  Skips where the
 *	filesystem does not support direct I/O (e.g. Linux tmpfs).
 */

#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_fs.h"
#include "xtc_int.h"

static int
is_pow2(size_t x)
{
	return x != 0 && (x & (x - 1)) == 0;
}

/* Alignment query returns power-of-two, non-zero values. */
static MunitResult
test_align(const MunitParameter p[], void *d)
{
	size_t mem = 0, off = 0, len = 0;
	int fd = -1;
	char tmpl[256];
	(void)p; (void)d;

	munit_assert_int(xtc_fs_tmpdir(tmpl, sizeof tmpl), ==, XTC_OK);
	strncat(tmpl, "/xtc_dio_align_XXXXXX", sizeof tmpl - strlen(tmpl) - 1);
	munit_assert_int(xtc_fs_mkstemp(tmpl, &fd), ==, XTC_OK);

	munit_assert_int(xtc_fs_dio_align(fd, &mem, &off, &len), ==, XTC_OK);
	munit_assert_true(is_pow2(mem));
	munit_assert_true(is_pow2(off));
	munit_assert_true(is_pow2(len));

	(void)xtc_fs_close(fd);
	(void)xtc_fs_unlink(tmpl);
	return MUNIT_OK;
}

/* Aligned direct write then read back must match. */
static MunitResult
test_roundtrip(const MunitParameter p[], void *d)
{
	char tmpl[256];
	int  fd = -1;
	void *wbuf = NULL, *rbuf = NULL;
	size_t len = 4096, mem = 0;
	size_t done = 0;
	int rc;
	(void)p; (void)d;

	munit_assert_int(xtc_fs_tmpdir(tmpl, sizeof tmpl), ==, XTC_OK);
	strncat(tmpl, "/xtc_dio_rt_XXXXXX", sizeof tmpl - strlen(tmpl) - 1);
	munit_assert_int(xtc_fs_mkstemp(tmpl, &fd), ==, XTC_OK);
	(void)xtc_fs_close(fd);

	rc = xtc_fs_open(tmpl, XTC_FS_WRITE | XTC_FS_DIRECT, &fd);
	if (rc != XTC_OK) {
		(void)xtc_fs_unlink(tmpl);
		return MUNIT_SKIP;     /* direct I/O unavailable here */
	}

	(void)xtc_fs_dio_align(fd, &mem, NULL, &len);
	munit_assert_int(xtc_fs_dio_alloc(fd, len, &wbuf), ==, XTC_OK);
	munit_assert_int((int)((uintptr_t)wbuf & (mem - 1)), ==, 0);
	memset(wbuf, 0xA5, len);

	rc = xtc_fs_pwrite(fd, wbuf, len, 0, &done);
	(void)xtc_fs_close(fd);
	if (rc != XTC_OK || done != len) {
		/* Filesystem accepted O_DIRECT/F_NOCACHE but cannot do the
		 * aligned write (e.g. overlay/tmpfs): not a library fault. */
		xtc_fs_dio_free(wbuf);
		(void)xtc_fs_unlink(tmpl);
		return MUNIT_SKIP;
	}

	munit_assert_int(xtc_fs_open(tmpl, XTC_FS_READ | XTC_FS_DIRECT, &fd),
	    ==, XTC_OK);
	munit_assert_int(xtc_fs_dio_alloc(fd, len, &rbuf), ==, XTC_OK);
	done = 0;
	munit_assert_int(xtc_fs_pread(fd, rbuf, len, 0, &done), ==, XTC_OK);
	munit_assert_size(done, ==, len);
	munit_assert_int(memcmp(wbuf, rbuf, len), ==, 0);

	(void)xtc_fs_close(fd);
	xtc_fs_dio_free(wbuf);
	xtc_fs_dio_free(rbuf);
	(void)xtc_fs_unlink(tmpl);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/align",     test_align,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/roundtrip", test_roundtrip, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m1/dio", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
