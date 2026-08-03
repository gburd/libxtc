/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m2/test_bdev.c -- portable block-device I/O (xtc_bdev_*):
 * opens a temp REGULAR FILE (no privileged device / root), verifies the
 * fallback geometry it reports, and does a sector-aligned pread/pwrite
 * round-trip plus a flush through xtc_aio inside a proc/loop.  A
 * misaligned request is rejected with XTC_E_INVAL.
 */

#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_fs.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_bdev.h"

/* Fallback logical sector for a regular file; the round-trip uses one
 * logical sector so the offset and length are trivially aligned. */
#define SECT 512
#define BLKS 8
#define LEN  (SECT * BLKS)

static char g_tmpl[520] = "/tmp/xtc_bdev_XXXXXX";
static int  g_fd = -1;

struct ctx {
	int      opened;      /* xtc_bdev_open return */
	uint32_t logical;
	uint32_t physical;
	uint64_t capacity;
	ssize_t  wrote;
	ssize_t  rd;
	int      flushed;
	int      match;
	int      misaligned_off;   /* XTC_E_INVAL expected */
	int      misaligned_len;   /* XTC_E_INVAL expected */
};

static void
bdev_proc(void *arg)
{
	struct ctx *c = arg;
	xtc_bdev_t *b = NULL;
	unsigned char *out = malloc(LEN);
	unsigned char *in = malloc(LEN);
	int i;

	if (out == NULL || in == NULL) { free(out); free(in); return; }
	for (i = 0; i < LEN; i++) out[i] = (unsigned char)(i * 11 + 5);

	c->opened = xtc_bdev_open(g_tmpl, XTC_BDEV_READ | XTC_BDEV_WRITE, &b);
	if (c->opened != XTC_OK || b == NULL) { free(out); free(in); return; }

	c->logical = xtc_bdev_logical_sector(b);
	c->physical = xtc_bdev_physical_sector(b);

	/* misalignment must be rejected before any I/O happens. */
	c->misaligned_off = (int)xtc_bdev_pwrite(b, out, SECT, 1);
	c->misaligned_len = (int)xtc_bdev_pwrite(b, out, SECT - 1, 0);

	c->wrote = xtc_bdev_pwrite(b, out, LEN, 0);
	c->flushed = xtc_bdev_flush(b);
	memset(in, 0, LEN);
	c->rd = xtc_bdev_pread(b, in, LEN, 0);
	c->match = (c->rd == LEN && memcmp(in, out, LEN) == 0);
	c->capacity = xtc_bdev_capacity(b);   /* open-time size snapshot */

	xtc_bdev_close(b);
	free(out);
	free(in);
}

static MunitResult
test_roundtrip(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t o = { 0 };
	xtc_pid_t a;
	struct ctx c;
	(void)p; (void)d;

	memset(&c, 0, sizeof c);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, bdev_proc, &c, &o, &a), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	(void)xtc_loop_fini(loop);

	munit_assert_int(c.opened, ==, XTC_OK);
	munit_assert_uint32(c.logical, ==, SECT);        /* fallback logical */
	munit_assert_uint32(c.physical, ==, 4096);       /* fallback physical */
	munit_assert_true(c.misaligned_off < 0);         /* XTC_E_INVAL */
	munit_assert_true(c.misaligned_len < 0);         /* XTC_E_INVAL */
	munit_assert_int((int)c.wrote, ==, LEN);         /* all bytes written */
	munit_assert_int(c.flushed, ==, XTC_OK);         /* fsync ok */
	munit_assert_int((int)c.rd, ==, LEN);            /* all bytes read back */
	munit_assert_int(c.match, ==, 1);                /* content round-tripped */
	/* capacity is probed at open; the temp file was pre-sized to LEN in
	 * setup, so fstat's st_size fallback must report exactly that. */
	munit_assert_uint64(c.capacity, ==, (uint64_t)LEN);
	return MUNIT_OK;
}

/* NULL / bad-arg rejection outside a loop (runs synchronously). */
static MunitResult
test_badargs(const MunitParameter p[], void *d)
{
	xtc_bdev_t *b = NULL;
	(void)p; (void)d;
	munit_assert_int(xtc_bdev_open(NULL, XTC_BDEV_READ, &b), ==, XTC_E_INVAL);
	munit_assert_int(xtc_bdev_open("x", XTC_BDEV_READ, NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_bdev_open("x", 0, &b), ==, XTC_E_INVAL);
	munit_assert_true(xtc_bdev_pread(NULL, (void *)"z", SECT, 0) < 0);
	munit_assert_int(xtc_bdev_flush(NULL), ==, XTC_E_INVAL);
	xtc_bdev_close(NULL);   /* no-op, must not crash */
	return MUNIT_OK;
}

static void *
suite_setup(const MunitParameter p[], void *ud)
{
	unsigned char zero[LEN];
	char tmpdir[512];
	(void)p; (void)ud;
	munit_assert_int(xtc_fs_tmpdir(tmpdir, sizeof tmpdir), ==, XTC_OK);
	snprintf(g_tmpl, sizeof g_tmpl, "%s/xtc_bdev_XXXXXX", tmpdir);
	g_fd = mkstemp(g_tmpl);
	munit_assert_int(g_fd, >=, 0);
	/* Pre-size the file to LEN so the fstat capacity fallback has a
	 * non-zero, known value to report at open time. */
	memset(zero, 0, sizeof zero);
	munit_assert_int((int)write(g_fd, zero, LEN), ==, LEN);
	return NULL;
}

static void
suite_teardown(void *fixture)
{
	(void)fixture;
	if (g_fd >= 0) { close(g_fd); g_fd = -1; }
	(void)unlink(g_tmpl);
}

static MunitTest tests[] = {
	{ "/roundtrip", test_roundtrip, suite_setup, suite_teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/badargs",   test_badargs,   NULL,        NULL,           MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m2/bdev", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char **argv) { return munit_suite_main(&suite, NULL, argc, argv); }
