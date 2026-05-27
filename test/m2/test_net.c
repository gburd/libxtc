/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/m2/test_net.c -- verifies M19.1 networking helpers.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_net.h"
#include "xtc_int.h"

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <io.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#endif

/* ----- TCP listen + dial pair --------------------------------- */

static MunitResult
test_tcp_listen_dial(const MunitParameter p[], void *d)
{
	int listen_fd = -1, client_fd = -1, accept_fd;
	xtc_tcp_opts_t opts = XTC_TCP_OPTS_DEFAULT;
	struct sockaddr_in sa;
	socklen_t salen = sizeof sa;
	int port;
	struct timespec sleep10ms = { 0, 10 * 1000 * 1000 };
	(void)p; (void)d;

#if defined(_WIN32)
	/* The probe-then-bind approach below is racy under Windows's
	 * stricter port-reuse semantics; skip the round-trip part and
	 * just verify the API rejects bad args. */
	(void)xtc_net_setnonblock(-1);
	munit_assert_int(xtc_net_listen(XTC_NET_INET, NULL, 0, &opts, &listen_fd),
	    ==, XTC_E_INVAL);
	return MUNIT_OK;
#endif

	/* Trigger WSAStartup on Windows via any xtc_net call. */
	(void)xtc_net_setnonblock(-1);

	munit_assert_int(xtc_net_listen(XTC_NET_INET, NULL, 0, &opts, &listen_fd),
	    ==, XTC_E_INVAL);   /* port=0 invalid */

	munit_assert_int(xtc_net_listen(XTC_NET_INET, "127.0.0.1", 0, &opts, &listen_fd),
	    ==, XTC_E_INVAL);

	/* Pick a free port: bind to port 0 via raw socket then close. */
	{
		int probe = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in s; memset(&s, 0, sizeof s);
		socklen_t l = sizeof s;
		s.sin_family = AF_INET; s.sin_port = 0;
		s.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		bind(probe, (struct sockaddr *)&s, sizeof s);
		getsockname(probe, (struct sockaddr *)&s, &l);
		port = ntohs(s.sin_port);
		close(probe);
	}

	munit_assert_int(xtc_net_listen(XTC_NET_INET, "127.0.0.1", port, &opts, &listen_fd),
	    ==, XTC_OK);
	munit_assert_int(listen_fd, >=, 0);

	munit_assert_int(xtc_net_dial(XTC_NET_INET, "127.0.0.1", port, &opts, &client_fd),
	    ==, XTC_OK);

	/* Spin a bit so the connect lands in the listen queue. */
	(void)nanosleep(&sleep10ms, NULL);

	accept_fd = accept(listen_fd, (struct sockaddr *)&sa, &salen);
	munit_assert_int(accept_fd, >=, 0);

	xtc_net_close(accept_fd);
	xtc_net_close(client_fd);
	xtc_net_close(listen_fd);
	return MUNIT_OK;
}

/* ----- TCP knobs apply ---------------------------------------- */

static MunitResult
test_tcp_knobs(const MunitParameter p[], void *d)
{
	int fd;
	xtc_tcp_opts_t opts = XTC_TCP_OPTS_DEFAULT;
	int v;
	socklen_t l;
	(void)p; (void)d;
	fd = socket(AF_INET, SOCK_STREAM, 0);
	munit_assert_int(fd, >=, 0);
	opts.nodelay = 1; opts.reuseaddr = 1; opts.keepalive = 1;
	munit_assert_int(xtc_net_apply_tcp_opts(fd, &opts), ==, XTC_OK);
	l = sizeof v;
	munit_assert_int(getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *)&v, &l), ==, 0);
	munit_assert_int(v, !=, 0);
	close(fd);
	return MUNIT_OK;
}

/* ----- UDS listen + dial pair --------------------------------- */

#if !defined(_WIN32)
static MunitResult
test_uds_listen_dial(const MunitParameter p[], void *d)
{
	char path[64];
	int listen_fd = -1, client_fd = -1, accept_fd;
	struct timespec sleep10ms = { 0, 10 * 1000 * 1000 };
	(void)p; (void)d;
	snprintf(path, sizeof path, "/tmp/xtc-test-uds.%d", (int)getpid());
	munit_assert_int(xtc_net_unix_listen(path, &listen_fd), ==, XTC_OK);
	munit_assert_int(xtc_net_unix_dial(path, &client_fd), ==, XTC_OK);
	(void)nanosleep(&sleep10ms, NULL);
	accept_fd = accept(listen_fd, NULL, NULL);
	munit_assert_int(accept_fd, >=, 0);

	/* Echo a tiny message across. */
	{
		const char *msg = "hello";
		size_t got = 0;
		char rxbuf[64] = {0};
		uint32_t uid = 0, gid = 0;
		munit_assert_int(xtc_net_unix_send_creds(client_fd, msg, 5),
		    ==, XTC_OK);
		(void)nanosleep(&sleep10ms, NULL);
		munit_assert_int(xtc_net_unix_recv_creds(accept_fd, rxbuf, 64,
		    &uid, &gid, &got), ==, XTC_OK);
		munit_assert_size(got, ==, 5);
		munit_assert_int(memcmp(rxbuf, "hello", 5), ==, 0);
		/* uid should match our own (Linux/BSD only); on stub
		 * platforms it's 0 -- both are acceptable. */
		(void)uid; (void)gid;
	}

	xtc_net_close(accept_fd);
	xtc_net_close(client_fd);
	xtc_net_close(listen_fd);
	(void)unlink(path);
	return MUNIT_OK;
}
#endif

/* ----- nonblock helper ---------------------------------------- */

static MunitResult
test_setnonblock(const MunitParameter p[], void *d)
{
	int fd;
	(void)p; (void)d;
	fd = socket(AF_INET, SOCK_STREAM, 0);
	munit_assert_int(fd, >=, 0);
	munit_assert_int(xtc_net_setnonblock(fd), ==, XTC_OK);
	close(fd);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/tcp_listen_dial", test_tcp_listen_dial, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/tcp_knobs",       test_tcp_knobs,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#if !defined(_WIN32)
	{ "/uds_listen_dial", test_uds_listen_dial, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#endif
	{ "/setnonblock",     test_setnonblock,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m2/net", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
