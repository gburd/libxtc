/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m7/test_res_meter.c -- xtc_res caps must bound what they name.
 *
 *	PLAN 19.27.11: a MEM_BYTES cap bounded slab chunks and nothing
 *	else, and an FDS cap bounded nothing at all.  These tests set a
 *	cap and assert the SECOND kind of consumer -- an mctx allocation,
 *	an xtc_net socket -- is REFUSED past it, that the units come back
 *	on free/close, and that nothing is charged without an attach.
 */

#include <stdint.h>
#include <string.h>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "munit.h"
#include "xtc.h"
#include "xtc_res.h"
#include "xtc_mctx.h"
#include "xtc_net.h"

static MunitResult
test_mctx_mem_cap(const MunitParameter p[], void *d)
{
	xtc_res_t r;
	xtc_res_caps_t caps = XTC_RES_CAPS_DEFAULT;
	xtc_mctx_t *m = NULL, *child = NULL, *plain = NULL;
	void *a, *b;
	int64_t hdr;
	(void)p; (void)d;

	caps.mem_bytes = 4096;
	munit_assert_int(xtc_res_init(&r, &caps), ==, XTC_OK);
	munit_assert_int(xtc_mctx_create(NULL, "metered", 0, &m), ==, XTC_OK);
	munit_assert_int(xtc_res_attach_mctx(&r, m), ==, XTC_OK);
	munit_assert_int(xtc_res_attach_mctx(&r, NULL), ==, XTC_E_INVAL);

	/* Under the cap: charged, header included. */
	a = xtc_mctx_alloc(m, 1000);
	munit_assert_ptr_not_null(a);
	hdr = xtc_res_used(&r, XTC_RES_MEM_BYTES) - 1000;
	munit_assert_int64(hdr, >, 0);

	/* THE regression: past the cap the allocation is refused. */
	b = xtc_mctx_alloc(m, 4000);
	munit_assert_ptr_null(b);
	munit_assert_int64(xtc_res_rejects(&r, XTC_RES_MEM_BYTES), ==, 1);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_MEM_BYTES), ==, 1000 + hdr);
	munit_assert_ptr_null(xtc_mctx_calloc(m, 4, 1000));
	munit_assert_int64(xtc_res_rejects(&r, XTC_RES_MEM_BYTES), ==, 2);

	/* free gives it back; the big one now fits. */
	xtc_mctx_free(m, a);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_MEM_BYTES), ==, 0);
	b = xtc_mctx_alloc(m, 4000);
	munit_assert_ptr_not_null(b);

	/* A child created while attached inherits the accountant. */
	munit_assert_int(xtc_mctx_create(m, "child", 0, &child), ==, XTC_OK);
	munit_assert_ptr_null(xtc_mctx_alloc(child, 1000));

	/* reset releases the footprint. */
	xtc_mctx_reset(m);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_MEM_BYTES), ==, 0);
	munit_assert_ptr_not_null(xtc_mctx_alloc(child, 1000));
	munit_assert_int64(xtc_res_used(&r, XTC_RES_MEM_BYTES), ==, 1000 + hdr);

	/* An unattached context is not charged at all. */
	munit_assert_int(xtc_mctx_create(NULL, "plain", 0, &plain), ==, XTC_OK);
	munit_assert_ptr_not_null(xtc_mctx_alloc(plain, 100000));
	munit_assert_int64(xtc_res_used(&r, XTC_RES_MEM_BYTES), ==, 1000 + hdr);

	/* Attaching a context already over the cap fails, changes nothing. */
	munit_assert_int(xtc_res_attach_mctx(&r, plain), ==, XTC_E_RESOURCE);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_MEM_BYTES), ==, 1000 + hdr);
	munit_assert_ptr_not_null(xtc_mctx_alloc(plain, 100000));

	/* destroy (parent first, recursing into the child) returns it all. */
	xtc_mctx_destroy(m);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_MEM_BYTES), ==, 0);

	/* Detach moves the footprint off the accountant. */
	munit_assert_int(xtc_mctx_create(NULL, "detach", 0, &m), ==, XTC_OK);
	munit_assert_int(xtc_res_attach_mctx(&r, m), ==, XTC_OK);
	munit_assert_ptr_not_null(xtc_mctx_alloc(m, 500));
	munit_assert_int64(xtc_res_used(&r, XTC_RES_MEM_BYTES), ==, 500 + hdr);
	munit_assert_int(xtc_res_attach_mctx(NULL, m), ==, XTC_OK);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_MEM_BYTES), ==, 0);
	xtc_mctx_destroy(m);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_MEM_BYTES), ==, 0);
	xtc_mctx_destroy(plain);
	return MUNIT_OK;
}

static MunitResult
test_net_fds_cap(const MunitParameter p[], void *d)
{
	xtc_res_t r;
	xtc_res_caps_t caps = XTC_RES_CAPS_DEFAULT;
	int a = -1, b = -1, c = -1, e = -1;
	(void)p; (void)d;

	caps.fds = 2;
	munit_assert_int(xtc_res_init(&r, &caps), ==, XTC_OK);
	munit_assert_int(xtc_res_attach_net(&r), ==, XTC_OK);

	munit_assert_int(xtc_net_udp_socket(XTC_NET_INET, NULL, 0, &a), ==, XTC_OK);
	munit_assert_int(xtc_net_udp_socket(XTC_NET_INET, NULL, 0, &b), ==, XTC_OK);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_FDS), ==, 2);

	/* THE regression: the third socket is refused, and nothing is
	 * created (out_fd untouched) -- neither a socket nor a dial. */
	munit_assert_int(xtc_net_udp_socket(XTC_NET_INET, NULL, 0, &c), ==,
	    XTC_E_RESOURCE);
	munit_assert_int(c, ==, -1);
	munit_assert_int(xtc_net_dial(XTC_NET_INET, "127.0.0.1", 9, NULL, &c),
	    ==, XTC_E_RESOURCE);
	munit_assert_int(c, ==, -1);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_FDS), ==, 2);
	munit_assert_int64(xtc_res_rejects(&r, XTC_RES_FDS), ==, 2);

	/* close gives it back. */
	xtc_net_close(a);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_FDS), ==, 1);
	munit_assert_int(xtc_net_udp_socket(XTC_NET_INET, NULL, 0, &c), ==, XTC_OK);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_FDS), ==, 2);

	/* A failed create refunds its charge. */
	xtc_net_close(c);
	munit_assert_int(xtc_net_udp_socket((xtc_net_family_t)99, NULL, 0, &c),
	    !=, XTC_OK);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_FDS), ==, 1);

	/* Detached: not charged, and closing an uncharged fd releases
	 * nothing.  The fd charged before the detach still refunds. */
	munit_assert_int(xtc_res_attach_net(NULL), ==, XTC_OK);
	munit_assert_int(xtc_net_udp_socket(XTC_NET_INET, NULL, 0, &e), ==, XTC_OK);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_FDS), ==, 1);
	xtc_net_close(e);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_FDS), ==, 1);
	xtc_net_close(b);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_FDS), ==, 0);
	return MUNIT_OK;
}

#if !defined(_WIN32)
/* Inbound connections are metered (1.51): xtc_net_accept charges FDS
 * before accepting, so at the cap the connection is REFUSED and stays in
 * the kernel backlog (not accepted-then-dropped), and a close frees room
 * for it.  Before 1.51 xtc_net had no accept, so a server's accepted fds
 * -- its main fd growth -- were invisible to the FDS cap. */
static MunitResult
test_net_accept_fds_cap(const MunitParameter p[], void *d)
{
	xtc_res_t r;
	xtc_res_caps_t caps = XTC_RES_CAPS_DEFAULT;
	xtc_tcp_opts_t to = XTC_TCP_OPTS_DEFAULT;
	struct sockaddr_in sa;
	socklen_t sl = sizeof sa;
	int lfd = -1, c1 = -1, c2 = -1, a1 = -1, a2 = -1, i, rc;
	(void)p; (void)d;

	/* The listener is a RAW socket on an ephemeral port (xtc_net_listen
	 * wants a fixed port), so it is not charged: the cap counts only the
	 * two dialed clients and the accepted connections. */
	lfd = socket(AF_INET, SOCK_STREAM, 0);
	munit_assert_int(lfd, >=, 0);
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	munit_assert_int(bind(lfd, (struct sockaddr *)&sa, sizeof sa), ==, 0);
	munit_assert_int(listen(lfd, 8), ==, 0);
	munit_assert_int(getsockname(lfd, (struct sockaddr *)&sa, &sl), ==, 0);
	munit_assert_int(xtc_net_setnonblock(lfd), ==, XTC_OK);
	caps.fds = 2;              /* 2 clients, 0 room */
	munit_assert_int(xtc_res_init(&r, &caps), ==, XTC_OK);
	munit_assert_int(xtc_res_attach_net(&r), ==, XTC_OK);
	munit_assert_int(xtc_net_dial(XTC_NET_INET, "127.0.0.1",
	    ntohs(sa.sin_port), &to, &c1), ==, XTC_OK);
	munit_assert_int(xtc_net_dial(XTC_NET_INET, "127.0.0.1",
	    ntohs(sa.sin_port), &to, &c2), ==, XTC_OK);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_FDS), ==, 2);

	/* At the cap: the pending connection is refused, not accepted. */
	munit_assert_int(xtc_net_accept(lfd, &a1), ==, XTC_E_RESOURCE);
	munit_assert_int(a1, ==, -1);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_FDS), ==, 2);

	/* Room: close a client; the queued connection is now accepted and
	 * charged.  (Retry briefly: the handshake completes asynchronously.) */
	xtc_net_close(c2);
	for (i = 0, rc = XTC_E_AGAIN; i < 200 && rc == XTC_E_AGAIN; i++) {
		rc = xtc_net_accept(lfd, &a1);
		if (rc == XTC_E_AGAIN) usleep(5000);
	}
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_int(a1, >=, 0);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_FDS), ==, 2);

	/* Nothing pending (both handshakes consumed or refused): AGAIN with
	 * the tentative charge refunded. */
	xtc_net_close(a1);
	rc = xtc_net_accept(lfd, &a2);
	munit_assert_true(rc == XTC_E_AGAIN || rc == XTC_OK);
	if (rc == XTC_OK) xtc_net_close(a2);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_FDS), ==, 1);

	munit_assert_int(xtc_net_accept(-1, &a2), ==, XTC_E_INVAL);
	munit_assert_int(xtc_net_accept(lfd, NULL), ==, XTC_E_INVAL);
	xtc_net_close(c1);
	(void)close(lfd);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_FDS), ==, 0);
	munit_assert_int(xtc_res_attach_net(NULL), ==, XTC_OK);
	return MUNIT_OK;
}
#endif

static MunitTest tests[] = {
	{ "/mctx_mem_cap", test_mctx_mem_cap, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/net_fds_cap",  test_net_fds_cap,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#if !defined(_WIN32)
	{ "/net_accept_fds_cap", test_net_accept_fds_cap, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#endif
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m7/res_meter", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
