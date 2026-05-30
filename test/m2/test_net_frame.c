/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m2/test_net_frame.c -- xtc_net_send_frame / xtc_net_recv_frame.
 *	Two processes on one loop exchange length-framed messages over a
 *	socketpair (the loop-aware yield path), and a peer's oversized
 *	claimed length is rejected without allocating.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

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
	xtc_proc_opts_t opts = { 0 };
	struct fr_state s = { -1, -1, 0, 0 };
	int sv[2];
	xtc_pid_t a, b;
	(void)p; (void)d;

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
	xtc_proc_opts_t opts = { 0 };
	struct fr_state s = { -1, -1, 0, 0 };
	int sv[2];
	xtc_pid_t a, b;
	(void)p; (void)d;

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

static MunitTest tests[] = {
	{ "/roundtrip", test_frame_roundtrip, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/cap",       test_frame_cap,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = {
	"/m2/net_frame", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
