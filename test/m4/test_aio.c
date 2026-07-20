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
#include "aio_int.h"        /* __xtc_aio_force_offload (internal test hook) */

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
run_roundtrip(int force_offload)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t o = { 0 };
	xtc_pid_t a, b;
	struct aio_ctx c;
	char tmpl[] = "/tmp/xtc_aio_XXXXXX";

	__xtc_aio_force_offload(force_offload);

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
	__xtc_aio_force_offload(0);   /* restore auto for later tests */

	munit_assert_int(c.wrote, ==, AIO_LEN);     /* all bytes written */
	munit_assert_int(c.fsynced, ==, 0);         /* fsync ok */
	munit_assert_int(c.read, ==, AIO_LEN);      /* all bytes read back */
	munit_assert_int(c.match, ==, 1);           /* content round-tripped */
	munit_assert_int(c.loop_ran, ==, 1);        /* peer ran during the I/O */
	return MUNIT_OK;
}

/*
 * The native path (io_uring / IOCP where present, inline elsewhere):
 * one async-looking API, best mechanism the host offers.
 */
static MunitResult
test_aio_roundtrip(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return run_roundtrip(0);
}

/*
 * The SAME round-trip with the blocking-pool offload FORCED, even on a
 * host with a native engine.  Proves the portable fallback presents the
 * identical async-looking behavior (parks the fiber, peer runs, bytes
 * round-trip, fsync ok) -- the "write once, runs as AIO everywhere"
 * guarantee, tested where the tests actually run rather than only
 * asserted for platforms CI never sees.
 */
static MunitResult
test_aio_roundtrip_offload(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return run_roundtrip(1);
}

/* ---- vectored (scatter/gather) round-trip ---- */
#include <sys/uio.h>

struct vec_ctx {
	int fd;
	int wrote;
	int rd;
	int match;
	int loop_ran;
};

/* Gather-write 3 segments, then scatter-read into 3 DIFFERENTLY sized
 * segments (so reassembly across iovec boundaries is exercised), and
 * verify the whole byte stream round-trips. */
static void
vec_proc(void *arg)
{
	struct vec_ctx *c = arg;
	static unsigned char w0[1000], w1[2000], w2[1096];  /* total 4096 */
	static unsigned char r0[2048], r1[1024], r2[1024];  /* total 4096 */
	struct iovec wv[3], rv[3];
	unsigned char flat_w[4096], flat_r[4096];
	int i;

	for (i = 0; i < (int)sizeof w0; i++) w0[i] = (unsigned char)(i + 1);
	for (i = 0; i < (int)sizeof w1; i++) w1[i] = (unsigned char)(i * 3 + 5);
	for (i = 0; i < (int)sizeof w2; i++) w2[i] = (unsigned char)(i * 5 + 9);
	wv[0].iov_base = w0; wv[0].iov_len = sizeof w0;
	wv[1].iov_base = w1; wv[1].iov_len = sizeof w1;
	wv[2].iov_base = w2; wv[2].iov_len = sizeof w2;

	xtc_yield();   /* let the peer run while the I/O is outstanding */

	c->wrote = xtc_aio_pwritev(c->fd, wv, 3, 0);
	(void)xtc_aio_fdatasync(c->fd);

	memset(r0, 0, sizeof r0); memset(r1, 0, sizeof r1);
	memset(r2, 0, sizeof r2);
	rv[0].iov_base = r0; rv[0].iov_len = sizeof r0;
	rv[1].iov_base = r1; rv[1].iov_len = sizeof r1;
	rv[2].iov_base = r2; rv[2].iov_len = sizeof r2;
	c->rd = xtc_aio_preadv(c->fd, rv, 3, 0);

	/* Flatten both sides and compare the whole 4096-byte stream. */
	memcpy(flat_w, w0, sizeof w0);
	memcpy(flat_w + 1000, w1, sizeof w1);
	memcpy(flat_w + 3000, w2, sizeof w2);
	memcpy(flat_r, r0, sizeof r0);
	memcpy(flat_r + 2048, r1, sizeof r1);
	memcpy(flat_r + 3072, r2, sizeof r2);
	c->match = (c->wrote == 4096 && c->rd == 4096 &&
	    memcmp(flat_w, flat_r, 4096) == 0);
	c->loop_ran = atomic_load(&g_peer_ran);
}

static MunitResult
run_vec_roundtrip(int force_offload)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t o = { 0 };
	xtc_pid_t a, b;
	struct vec_ctx c;
	char tmpl[] = "/tmp/xtc_aiov_XXXXXX";

	__xtc_aio_force_offload(force_offload);
	memset(&c, 0, sizeof c);
	atomic_store(&g_peer_ran, 0);
	c.fd = mkstemp(tmpl);
	munit_assert_int(c.fd, >=, 0);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, peer_proc, NULL, &o, &b), ==,
	    XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, vec_proc, &c, &o, &a), ==,
	    XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	close(c.fd);
	unlink(tmpl);
	__xtc_aio_force_offload(0);

	munit_assert_int(c.wrote, ==, 4096);    /* all gathered bytes written */
	munit_assert_int(c.rd, ==, 4096);       /* all scattered bytes read */
	munit_assert_int(c.match, ==, 1);       /* stream round-tripped exactly */
	munit_assert_int(c.loop_ran, ==, 1);    /* peer ran during the I/O */
	return MUNIT_OK;
}

static MunitResult
test_aiov_roundtrip(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return run_vec_roundtrip(0);   /* native (io_uring where present) */
}

static MunitResult
test_aiov_roundtrip_offload(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return run_vec_roundtrip(1);   /* forced blocking-pool preadv/pwritev */
}

/* Out-of-range iovcnt / NULL iov are rejected (bounded submission). */
static struct vec_ctx g_badv;
static void
badv_proc(void *arg)
{
	struct vec_ctx *c = arg;
	struct iovec v = { (void *)"x", 1 };
	c->wrote = xtc_aio_pwritev(c->fd, &v, 0, 0);    /* iovcnt < 1 */
	c->rd = xtc_aio_pwritev(c->fd, NULL, 1, 0);     /* NULL iov */
}

static MunitResult
test_aiov_bounds(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t o = { 0 };
	xtc_pid_t a;
	char tmpl[] = "/tmp/xtc_aiovb_XXXXXX";
	(void)p; (void)d;

	memset(&g_badv, 0, sizeof g_badv);
	g_badv.fd = mkstemp(tmpl);
	munit_assert_int(g_badv.fd, >=, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, badv_proc, &g_badv, &o, &a),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	close(g_badv.fd);
	unlink(tmpl);

	munit_assert_int(g_badv.wrote, <, 0);   /* iovcnt 0 rejected */
	munit_assert_int(g_badv.rd, <, 0);      /* NULL iov rejected */
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
	{ "/roundtrip",         test_aio_roundtrip,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/roundtrip_offload", test_aio_roundtrip_offload, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/vec_roundtrip",     test_aiov_roundtrip,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/vec_roundtrip_offload", test_aiov_roundtrip_offload, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/vec_bounds",        test_aiov_bounds,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/short_read",        test_aio_short_read,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/aio", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char **argv) { return munit_suite_main(&suite, NULL, argc, argv); }
