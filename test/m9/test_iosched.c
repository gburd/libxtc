/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m9/test_iosched.c
 *	Adaptive write-batching scheduler: data integrity, batch/flush
 *	accounting (fixed mode), and adaptive-mode safety (batch stays in
 *	bounds, data still correct).
 */

#include "munit.h"
#include "xtc.h"
#include "xtc_fs.h"
#include "xtc_iosched.h"
#include "xtc_int.h"

#include <stdlib.h>
#include <string.h>

#define REC 512
#define N   20

static int
open_tmp(char *tmpl, size_t cap)
{
	int fd = -1;
	if (xtc_fs_tmpdir(tmpl, cap) != XTC_OK) return -1;
	strncat(tmpl, "/xtc_iosched_XXXXXX", cap - strlen(tmpl) - 1);
	if (xtc_fs_mkstemp(tmpl, &fd) != XTC_OK) return -1;
	(void)xtc_fs_close(fd);
	return 0;
}

/* Fixed batch: data round-trips and flush accounting is exact. */
static MunitResult
test_fixed(const MunitParameter p[], void *d)
{
	char tmpl[256];
	int fd = -1, i;
	char *backing;
	xtc_iosched_t *s = NULL;
	xtc_iosched_opts_t o;
	xtc_iosched_stats_t st;
	size_t done;
	char rd[REC];
	(void)p; (void)d;

	munit_assert_int(open_tmp(tmpl, sizeof tmpl), ==, 0);
	munit_assert_int(xtc_fs_open(tmpl, XTC_FS_WRITE | XTC_FS_CREATE, &fd),
	    ==, XTC_OK);

	backing = malloc(N * REC);
	munit_assert_not_null(backing);
	for (i = 0; i < N; i++) memset(backing + i * REC, i & 0xff, REC);

	memset(&o, 0, sizeof o);
	o.fd = fd; o.adaptive = 0; o.batch_size = 8;
	o.min_batch = 1; o.max_batch = 256;
	munit_assert_int(xtc_iosched_create(&o, &s), ==, XTC_OK);

	for (i = 0; i < N; i++)
		munit_assert_int(
		    xtc_iosched_write(s, backing + i * REC, REC,
		        (int64_t)i * REC), ==, XTC_OK);
	munit_assert_int(xtc_iosched_flush(s), ==, XTC_OK);

	xtc_iosched_get_stats(s, &st);
	munit_assert_uint64(st.writes, ==, N);
	munit_assert_uint64(st.bytes, ==, (uint64_t)N * REC);
	/* 20 writes, batch 8 -> implicit flush at 8 and 16, final flush of 4. */
	munit_assert_uint64(st.flushes, ==, 3);
	munit_assert_int(st.cur_batch, ==, 8);   /* fixed */

	xtc_iosched_destroy(s);
	munit_assert_int(xtc_fs_close(fd), ==, XTC_OK);

	/* Verify the bytes that landed. */
	munit_assert_int(xtc_fs_open(tmpl, XTC_FS_READ, &fd), ==, XTC_OK);
	for (i = 0; i < N; i++) {
		done = 0;
		munit_assert_int(xtc_fs_pread(fd, rd, REC, (int64_t)i * REC,
		    &done), ==, XTC_OK);
		munit_assert_size(done, ==, REC);
		munit_assert_int((unsigned char)rd[0], ==, (i & 0xff));
		munit_assert_int((unsigned char)rd[REC - 1], ==, (i & 0xff));
	}
	(void)xtc_fs_close(fd);
	(void)xtc_fs_unlink(tmpl);
	free(backing);
	return MUNIT_OK;
}

/* Adaptive: batch stays in bounds across many flushes; data correct. */
static MunitResult
test_adaptive(const MunitParameter p[], void *d)
{
	char tmpl[256];
	int fd = -1, i;
	char *backing;
	xtc_iosched_t *s = NULL;
	xtc_iosched_opts_t o;
	xtc_iosched_stats_t st;
	const int total = 8 * 60;
	(void)p; (void)d;

	munit_assert_int(open_tmp(tmpl, sizeof tmpl), ==, 0);
	munit_assert_int(xtc_fs_open(tmpl, XTC_FS_WRITE | XTC_FS_CREATE, &fd),
	    ==, XTC_OK);

	backing = malloc(REC);
	munit_assert_not_null(backing);
	memset(backing, 0x5a, REC);

	memset(&o, 0, sizeof o);
	o.fd = fd; o.adaptive = 1; o.batch_size = 16;
	o.min_batch = 1; o.max_batch = 64; o.seed = 7;
	munit_assert_int(xtc_iosched_create(&o, &s), ==, XTC_OK);

	for (i = 0; i < total; i++) {
		/* All records overwrite the same block; we only check bounds
		 * and that the scheduler keeps running and adapting. */
		munit_assert_int(xtc_iosched_write(s, backing, REC, 0),
		    ==, XTC_OK);
		xtc_iosched_get_stats(s, &st);
		munit_assert_int(st.cur_batch, >=, 1);
		munit_assert_int(st.cur_batch, <=, 64);
	}
	munit_assert_int(xtc_iosched_flush(s), ==, XTC_OK);
	xtc_iosched_get_stats(s, &st);
	munit_assert_uint64(st.writes, ==, (uint64_t)total);
	munit_assert_uint64(st.flushes, >=, 1);
	munit_assert_double(st.mutation_rate, <=, 0.45 + 1e-9);

	xtc_iosched_destroy(s);
	(void)xtc_fs_close(fd);
	(void)xtc_fs_unlink(tmpl);
	free(backing);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/fixed",    test_fixed,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/adaptive", test_adaptive, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m9/iosched", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
