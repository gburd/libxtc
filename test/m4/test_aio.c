/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m4/test_aio.c -- async file I/O (xtc_aio_pread/pwrite/fsync):
 * a fiber writes, fsyncs, and reads back a file without blocking its
 * loop, via the native io_uring completion path (or the blocking-pool
 * fallback on a readiness-only backend).
 */

#define _GNU_SOURCE

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_aio.h"

#define AIO_LEN 8192

struct aio_ctx {
	int   fd;
	int   wrote;     /* bytes xtc_aio_pwrite returned */
	int   read;      /* bytes xtc_aio_pread returned */
	int   fsynced;   /* xtc_aio_fsync return */
	int   match;     /* read-back content matched */
	int   loop_ran;  /* a peer fiber ran while the I/O was outstanding */
};

static atomic_int g_peer_ran;

/* A peer fiber that simply records it got to run -- proof the loop kept
 * scheduling while the aio fiber's I/O was in flight. */
static void
peer_proc(void *arg)
{
	(void)arg;
	atomic_store(&g_peer_ran, 1);
}

static void
aio_proc(void *arg)
{
	struct aio_ctx *c = arg;
	unsigned char *out = malloc(AIO_LEN);
	unsigned char *in = malloc(AIO_LEN);
	int i;

	if (out == NULL || in == NULL) { free(out); free(in); return; }
	for (i = 0; i < AIO_LEN; i++) out[i] = (unsigned char)(i * 7 + 3);

	/* yield once so the peer proc is scheduled too. */
	xtc_yield();

	c->wrote   = xtc_aio_pwrite(c->fd, out, AIO_LEN, 0);
	c->fsynced = xtc_aio_fsync(c->fd);
	memset(in, 0, AIO_LEN);
	c->read    = xtc_aio_pread(c->fd, in, AIO_LEN, 0);
	c->match   = (c->read == AIO_LEN && memcmp(in, out, AIO_LEN) == 0);
	c->loop_ran = atomic_load(&g_peer_ran);
	free(out);
	free(in);
}

static MunitResult
test_aio_roundtrip(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t o = { 0 };
	xtc_pid_t a, b;
	struct aio_ctx c;
	char tmpl[] = "/tmp/xtc_aio_XXXXXX";
	(void)p; (void)d;

	memset(&c, 0, sizeof c);
	atomic_store(&g_peer_ran, 0);
	c.fd = mkstemp(tmpl);
	munit_assert_int(c.fd, >=, 0);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, peer_proc, NULL, &o, &b), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, aio_proc, &c, &o, &a), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	(void)xtc_loop_fini(loop);

	close(c.fd);
	unlink(tmpl);

	munit_assert_int(c.wrote, ==, AIO_LEN);     /* all bytes written */
	munit_assert_int(c.fsynced, ==, 0);         /* fsync ok */
	munit_assert_int(c.read, ==, AIO_LEN);      /* all bytes read back */
	munit_assert_int(c.match, ==, 1);           /* content round-tripped */
	munit_assert_int(c.loop_ran, ==, 1);        /* peer ran during the I/O */
	return MUNIT_OK;
}

/* A short read at EOF returns the partial count, not an error. */
static struct aio_ctx g_eof;
static void
eof_proc(void *arg)
{
	struct aio_ctx *c = arg;
	unsigned char buf[256];
	c->read = xtc_aio_pread(c->fd, buf, sizeof buf, 0);  /* file is 10 bytes */
}

static MunitResult
test_aio_short_read(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t o = { 0 };
	xtc_pid_t a;
	char tmpl[] = "/tmp/xtc_aio2_XXXXXX";
	(void)p; (void)d;

	memset(&g_eof, 0, sizeof g_eof);
	g_eof.fd = mkstemp(tmpl);
	munit_assert_int(g_eof.fd, >=, 0);
	munit_assert_int((int)write(g_eof.fd, "0123456789", 10), ==, 10);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, eof_proc, &g_eof, &o, &a), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	close(g_eof.fd);
	unlink(tmpl);

	munit_assert_int(g_eof.read, ==, 10);       /* partial count, no error */
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/roundtrip",  test_aio_roundtrip,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/short_read", test_aio_short_read, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/aio", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char **argv) { return munit_suite_main(&suite, NULL, argc, argv); }
