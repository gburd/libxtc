/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m2/test_net_frame.c -- xtc_net_send_frame / xtc_net_recv_frame.
 *	Two processes on one loop exchange length-framed messages over a
 *	socketpair (the loop-aware yield path), and a peer's oversized
 *	claimed length is rejected without allocating.
 *
 *	framev: xtc_net_send_framev frames assembled from pieces (with
 *	empty pieces and a 1 MiB piece that forces partial writes and
 *	parks) arrive as ordinary frames; bad vectors are refused.
 *	one_segment: a small frame's prefix and payload leave in ONE send
 *	-- on a SOCK_DGRAM pair every send is one record, so the
 *	receiver's first recv must return 4 + len bytes (the old
 *	send-prefix-then-payload code delivered a 4-byte record first).
 *	(DGRAM, not SEQPACKET: macOS has no AF_UNIX SOCK_SEQPACKET.)
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_net.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "os_alloc.h"

struct fr_state {
	int  sv0, sv1;
	int  ok;        /* receiver: count of correctly-received frames */
	int  cap_range; /* receiver: oversized frame -> XTC_E_RANGE */
	int  trunc_ok;  /* receiver: truncated frame rejected, no buffer */
};

/* ---- round-trip: empty, small, and a 4000-byte frame ---- */
static void
fr_sender(void *arg)
{
	struct fr_state *s = arg;
	static uint8_t big[4000];
	size_t i;
	for (i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i & 0xff);
	(void)xtc_net_send_frame(s->sv0, "", 0);
	(void)xtc_net_send_frame(s->sv0, "hello", 5);
	(void)xtc_net_send_frame(s->sv0, big, sizeof big);
}

static void
fr_receiver(void *arg)
{
	struct fr_state *s = arg;
	void *m = NULL; size_t n = 0;
	/* empty frame */
	if (xtc_net_recv_frame(s->sv1, &m, &n, 1u << 20,
	    2LL * 1000000000) == XTC_OK && n == 0 && m == NULL)
		s->ok++;
	/* "hello" */
	if (xtc_net_recv_frame(s->sv1, &m, &n, 1u << 20,
	    2LL * 1000000000) == XTC_OK && n == 5 &&
	    memcmp(m, "hello", 5) == 0) { s->ok++; }
	if (m) { __os_free(m); m = NULL; }
	/* 4000-byte frame, content check */
	if (xtc_net_recv_frame(s->sv1, &m, &n, 1u << 20,
	    2LL * 1000000000) == XTC_OK && n == 4000 &&
	    ((uint8_t *)m)[0] == 0 && ((uint8_t *)m)[3999] == (uint8_t)(3999 & 0xff))
		s->ok++;
	if (m) __os_free(m);
}

static MunitResult
test_frame_roundtrip(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = XTC_PROC_OPTS_INIT;
	struct fr_state s;
	int sv[2];
	xtc_pid_t a, b;
	(void)p; (void)d;

	memset(&s, 0, sizeof s);
	s.sv0 = s.sv1 = -1;

	munit_assert_int(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
	(void)xtc_net_setnonblock(sv[0]);
	(void)xtc_net_setnonblock(sv[1]);
	s.sv0 = sv[0]; s.sv1 = sv[1];

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "recv";
	munit_assert_int(xtc_proc_spawn(loop, fr_receiver, &s, &opts, &b),
	    ==, XTC_OK);
	opts.name = "send";
	munit_assert_int(xtc_proc_spawn(loop, fr_sender, &s, &opts, &a),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	xtc_net_close(sv[0]); xtc_net_close(sv[1]);

	munit_assert_int(s.ok, ==, 3);
	return MUNIT_OK;
}

/* ---- cap: a frame larger than max_len is rejected, no alloc ---- */
static void
cap_sender(void *arg)
{
	struct fr_state *s = arg;
	static uint8_t buf[100];
	(void)xtc_net_send_frame(s->sv0, buf, sizeof buf);
}

static void
cap_receiver(void *arg)
{
	struct fr_state *s = arg;
	void *m = NULL; size_t n = 0;
	int rc = xtc_net_recv_frame(s->sv1, &m, &n, 50, 2LL * 1000000000);
	if (rc == XTC_E_RANGE && m == NULL)
		s->cap_range = 1;
	if (m) __os_free(m);
}

static MunitResult
test_frame_cap(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = XTC_PROC_OPTS_INIT;
	struct fr_state s;
	int sv[2];
	xtc_pid_t a, b;
	(void)p; (void)d;

	memset(&s, 0, sizeof s);
	s.sv0 = s.sv1 = -1;

	munit_assert_int(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
	(void)xtc_net_setnonblock(sv[0]);
	(void)xtc_net_setnonblock(sv[1]);
	s.sv0 = sv[0]; s.sv1 = sv[1];
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "crecv";
	munit_assert_int(xtc_proc_spawn(loop, cap_receiver, &s, &opts, &b),
	    ==, XTC_OK);
	opts.name = "csend";
	munit_assert_int(xtc_proc_spawn(loop, cap_sender, &s, &opts, &a),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	xtc_net_close(sv[0]); xtc_net_close(sv[1]);

	munit_assert_int(s.cap_range, ==, 1);
	return MUNIT_OK;
}


/* ---- peer closes MID-FRAME: the recv loop's cleanup paths -----------
 *
 * xtc_net_recv_frame reads a 4-byte length prefix and then loops on the
 * payload.  If the peer disappears after the header but before the whole
 * payload arrives, the loop must notice recv()==0, FREE the buffer it
 * already allocated for the claimed length, and report the truncation
 * rather than returning a partly-filled frame or leaking.  Neither of
 * the existing cases reaches that: /roundtrip always completes a frame
 * and /cap is rejected before any allocation.
 *
 * The sender hand-writes a header claiming more bytes than it will send,
 * writes a short payload, then closes -- so the receiver is guaranteed
 * to be mid-payload when the peer vanishes.  ASan/valgrind runs of this
 * case are what prove the buffer is released on that path.
 */
static void
trunc_sender(void *arg)
{
	struct fr_state *s = arg;
	unsigned char hdr[4];
	const char body[] = "only-4";

	/* Claim 4096 bytes, then send 6 and hang up.  Raw send(): the point
	 * is to emit a DELIBERATELY inconsistent frame, which the public
	 * xtc_net_send_frame cannot (and should not) produce. */
	hdr[0] = 0; hdr[1] = 0; hdr[2] = 0x10; hdr[3] = 0x00;   /* 4096 */
	(void)send(s->sv0, hdr, sizeof hdr, 0);
	(void)send(s->sv0, body, sizeof body - 1, 0);
	xtc_net_close(s->sv0);
	s->sv0 = -1;
}

static void
trunc_receiver(void *arg)
{
	struct fr_state *s = arg;
	void  *buf = NULL;
	size_t len = 0;
	int rc;

	rc = xtc_net_recv_frame(s->sv1, &buf, &len, 65536,
	    2000LL * 1000 * 1000);
	/* Must NOT report success on a truncated frame, and must not hand
	 * back a buffer for the caller to free. */
	if (rc != XTC_OK && buf == NULL)
		s->trunc_ok = 1;
	xtc_free(buf);   /* no-op on NULL; correct even if a frame appeared */
}

static MunitResult
test_frame_truncated(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = XTC_PROC_OPTS_INIT;
	struct fr_state s;
	int sv[2];
	xtc_pid_t a, b;
	(void)p; (void)d;

	memset(&s, 0, sizeof s);
	s.sv0 = s.sv1 = -1;

	munit_assert_int(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
	(void)xtc_net_setnonblock(sv[0]);
	(void)xtc_net_setnonblock(sv[1]);
	s.sv0 = sv[0]; s.sv1 = sv[1];
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "trecv";
	munit_assert_int(xtc_proc_spawn(loop, trunc_receiver, &s, &opts, &b),
	    ==, XTC_OK);
	opts.name = "tsend";
	munit_assert_int(xtc_proc_spawn(loop, trunc_sender, &s, &opts, &a),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	munit_assert_int(s.trunc_ok, ==, 1);
	if (s.sv0 >= 0) xtc_net_close(sv[0]);
	xtc_net_close(sv[1]);
	return MUNIT_OK;
}

/* ---- framev: scatter-gather frames ---- */
#define FV_BIG (1u << 20)
static uint8_t g_fv_big[FV_BIG];

static void
fv_sender(void *arg)
{
	struct fr_state *s = arg;
	struct iovec v[4];
	size_t i;
	for (i = 0; i < FV_BIG; i++)
		g_fv_big[i] = (uint8_t)(i * 13);
	v[0].iov_base = "head:"; v[0].iov_len = 5;
	v[1].iov_base = NULL;    v[1].iov_len = 0;      /* skipped */
	v[2].iov_base = "body";  v[2].iov_len = 4;
	(void)xtc_net_send_framev(s->sv0, v, 3);
	(void)xtc_net_send_framev(s->sv0, NULL, 0);     /* empty frame */
	v[0].iov_base = "<";       v[0].iov_len = 1;
	v[1].iov_base = g_fv_big;  v[1].iov_len = FV_BIG;
	v[2].iov_base = ">";       v[2].iov_len = 1;
	(void)xtc_net_send_framev(s->sv0, v, 3);
}

static void
fv_receiver(void *arg)
{
	struct fr_state *s = arg;
	void *m = NULL; size_t n = 0, i;
	if (xtc_net_recv_frame(s->sv1, &m, &n, 0, 5LL * 1000000000) ==
	    XTC_OK && n == 9 && memcmp(m, "head:body", 9) == 0)
		s->ok++;
	if (m) { __os_free(m); m = NULL; }
	if (xtc_net_recv_frame(s->sv1, &m, &n, 0, 5LL * 1000000000) ==
	    XTC_OK && n == 0 && m == NULL)
		s->ok++;
	if (xtc_net_recv_frame(s->sv1, &m, &n, 0, 5LL * 1000000000) ==
	    XTC_OK && n == FV_BIG + 2) {
		const uint8_t *b = m;
		int good = b[0] == '<' && b[FV_BIG + 1] == '>';
		for (i = 0; good && i < FV_BIG; i++)
			good = b[1 + i] == (uint8_t)(i * 13);
		s->ok += good;
	}
	if (m) __os_free(m);
}

static MunitResult
test_frame_framev(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	struct fr_state s;
	struct iovec bad[64];
	int sv[2];
	(void)p; (void)d;

	memset(&s, 0, sizeof s);
	munit_assert_int(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
	(void)xtc_net_setnonblock(sv[0]);
	(void)xtc_net_setnonblock(sv[1]);
	s.sv0 = sv[0]; s.sv1 = sv[1];
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, fv_receiver, &s, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, fv_sender, &s, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	munit_assert_int(s.ok, ==, 3);

	/* Refused before anything is sent. */
	memset(bad, 0, sizeof bad);
	munit_assert_int(xtc_net_send_framev(sv[0], bad, 64), ==,
	    XTC_E_INVAL);                                 /* > 63 pieces */
	bad[0].iov_len = 3;                               /* NULL base */
	munit_assert_int(xtc_net_send_framev(sv[0], bad, 1), ==,
	    XTC_E_INVAL);
	munit_assert_int(xtc_net_send_framev(sv[0], NULL, 1), ==,
	    XTC_E_INVAL);
	munit_assert_int(xtc_net_send_framev(-1, NULL, 0), ==, XTC_E_INVAL);
	xtc_net_close(sv[0]); xtc_net_close(sv[1]);
	return MUNIT_OK;
}

/* ---- one_segment: prefix + payload in one send ---- */
static MunitResult
test_frame_one_segment(const MunitParameter p[], void *d)
{
	uint8_t rec[64];
	ssize_t r;
	int sv[2];
	struct iovec v[2];
	(void)p; (void)d;
	munit_assert_int(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv), ==, 0);
	munit_assert_int(xtc_net_send_frame(sv[0], "hello", 5), ==, XTC_OK);
	r = recv(sv[1], rec, sizeof rec, 0);
	munit_assert_int((int)r, ==, 4 + 5);
	munit_assert_memory_equal(5, rec + 4, "hello");
	v[0].iov_base = "ab"; v[0].iov_len = 2;
	v[1].iov_base = "cde"; v[1].iov_len = 3;
	munit_assert_int(xtc_net_send_framev(sv[0], v, 2), ==, XTC_OK);
	r = recv(sv[1], rec, sizeof rec, 0);
	munit_assert_int((int)r, ==, 4 + 5);
	munit_assert_memory_equal(5, rec + 4, "abcde");
	close(sv[0]); close(sv[1]);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/roundtrip", test_frame_roundtrip, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/cap",       test_frame_cap,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/truncated", test_frame_truncated, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/framev",    test_frame_framev,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/one_segment", test_frame_one_segment, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = {
	"/m2/net_frame", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
