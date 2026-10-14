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
		int probe = (int)socket(AF_INET, SOCK_STREAM, 0);
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

	accept_fd = (int)accept(listen_fd, (struct sockaddr *)&sa, &salen);
	munit_assert_int(accept_fd, >=, 0);

	xtc_net_close(accept_fd);
	xtc_net_close(client_fd);
	xtc_net_close(listen_fd);
	return MUNIT_OK;
}

/* ----- listen backlog ------------------------------------------
 * xtc_net_listen hard-coded listen(fd, 128): a burst of connections
 * beyond that overflowed the accept queue (20k clients: 5926 had not
 * completed one echo after 10 s).  The default is now SOMAXCONN / 4096
 * (kernel-clamped), and xtc_net_listen_backlog sets it.  Measured by
 * connecting N clients without accepting and counting how many the
 * kernel completed (Linux: SYNs beyond backlog+1 are dropped, so the
 * dial stays in progress).  Fails on 1.55.0 (129 of 1000). */
#if defined(__linux__)
#include <poll.h>
#include <sys/resource.h>

static int
backlog_fill(int (*listen_fn)(int, int *), int arg, int n)
{
	xtc_tcp_opts_t opts = XTC_TCP_OPTS_DEFAULT;
	struct sockaddr_in s;
	socklen_t l = sizeof s;
	struct pollfd *pf;
	int lfd = -1, i, ok = 0, port;

	munit_assert_int(listen_fn(arg, &lfd), ==, XTC_OK);
	memset(&s, 0, sizeof s);
	munit_assert_int(getsockname(lfd, (struct sockaddr *)&s, &l), ==, 0);
	port = ntohs(s.sin_port);
	pf = calloc((size_t)n, sizeof *pf);
	munit_assert_not_null(pf);
	for (i = 0; i < n; i++) {
		int fd = -1;
		munit_assert_int(xtc_net_dial(XTC_NET_INET, "127.0.0.1", port,
		    &opts, &fd), ==, XTC_OK);
		pf[i].fd = fd;
		pf[i].events = POLLOUT;
	}
	(void)poll(pf, (nfds_t)n, 500);   /* let the handshakes land */
	for (i = 0; i < n; i++) {
		int err = 0;
		socklen_t el = sizeof err;
		pf[i].revents = 0;
		if (poll(&pf[i], 1, 0) == 1 && (pf[i].revents & POLLOUT) &&
		    getsockopt(pf[i].fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 &&
		    err == 0)
			ok++;
		xtc_net_close(pf[i].fd);
	}
	free(pf);
	xtc_net_close(lfd);
	return ok;
}

static int g_bl_port;

static int
bl_default(int unused, int *out)
{
	xtc_tcp_opts_t opts = XTC_TCP_OPTS_DEFAULT;
	(void)unused;
	return xtc_net_listen(XTC_NET_INET, "127.0.0.1", g_bl_port, &opts, out);
}

static int
bl_explicit(int backlog, int *out)
{
	xtc_tcp_opts_t opts = XTC_TCP_OPTS_DEFAULT;
	return xtc_net_listen_backlog(XTC_NET_INET, "127.0.0.1", g_bl_port,
	    &opts, backlog, out);
}

static int
bl_free_port(void)
{
	int probe = (int)socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in s;
	socklen_t l = sizeof s;
	memset(&s, 0, sizeof s);
	s.sin_family = AF_INET;
	s.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	(void)bind(probe, (struct sockaddr *)&s, sizeof s);
	(void)getsockname(probe, (struct sockaddr *)&s, &l);
	close(probe);
	return ntohs(s.sin_port);
}

static MunitResult
test_listen_backlog(const MunitParameter p[], void *d)
{
	enum { N = 1000 };
	struct rlimit rl;
	FILE *f;
	int somax = 0, got, fd = -1;
	xtc_tcp_opts_t opts = XTC_TCP_OPTS_DEFAULT;
	(void)p; (void)d;

	f = fopen("/proc/sys/net/core/somaxconn", "r");
	if (f == NULL || fscanf(f, "%d", &somax) != 1)
		somax = 0;
	if (f != NULL)
		fclose(f);
	if (somax < N)
		return MUNIT_SKIP;   /* host clamps the backlog below N */
	munit_assert_int(getrlimit(RLIMIT_NOFILE, &rl), ==, 0);
	if (rl.rlim_cur < N + 64) {
		rl.rlim_cur = rl.rlim_max >= N + 64 ? N + 64 : rl.rlim_max;
		(void)setrlimit(RLIMIT_NOFILE, &rl);
		(void)getrlimit(RLIMIT_NOFILE, &rl);
		if (rl.rlim_cur < N + 64)
			return MUNIT_SKIP;
	}

	/* The default must accept a burst far beyond 128. */
	g_bl_port = bl_free_port();
	got = backlog_fill(bl_default, 0, N);
	munit_assert_int(got, ==, N);

	/* An explicit small backlog is honored (Linux completes backlog+1). */
	g_bl_port = bl_free_port();
	got = backlog_fill(bl_explicit, 16, N);
	munit_assert_int(got, <=, 17 + 4);
	munit_assert_int(got, >=, 1);

	munit_assert_int(xtc_net_listen_backlog(XTC_NET_INET, "127.0.0.1",
	    bl_free_port(), &opts, -1, &fd), ==, XTC_E_INVAL);
	return MUNIT_OK;
}
#endif

/* ----- TCP knobs apply ---------------------------------------- */

static MunitResult
test_tcp_knobs(const MunitParameter p[], void *d)
{
	int fd;
	xtc_tcp_opts_t opts = XTC_TCP_OPTS_DEFAULT;
	int v;
	socklen_t l;
	(void)p; (void)d;
	fd = (int)socket(AF_INET, SOCK_STREAM, 0);
	munit_assert_int(fd, >=, 0);
	opts.nodelay = 1; opts.reuseaddr = 1; opts.keepalive = 1;
	munit_assert_int(xtc_net_apply_tcp_opts(fd, &opts), ==, XTC_OK);
	l = sizeof v;
	munit_assert_int(getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *)&v, &l), ==, 0);
	munit_assert_int(v, !=, 0);
#if defined(_WIN32)
	closesocket((SOCKET)fd);  /* a SOCKET, not a CRT fd: _close() fastfails */
#else
	close(fd);
#endif
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
	accept_fd = (int)accept(listen_fd, NULL, NULL);
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
	fd = (int)socket(AF_INET, SOCK_STREAM, 0);
	munit_assert_int(fd, >=, 0);
	munit_assert_int(xtc_net_setnonblock(fd), ==, XTC_OK);
#if defined(_WIN32)
	closesocket((SOCKET)fd);
#else
	close(fd);
#endif
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/tcp_listen_dial", test_tcp_listen_dial, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/tcp_knobs",       test_tcp_knobs,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#if !defined(_WIN32)
	{ "/uds_listen_dial", test_uds_listen_dial, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#endif
	{ "/setnonblock",     test_setnonblock,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#if defined(__linux__)
	{ "/listen_backlog",  test_listen_backlog,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#endif
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m2/net", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
