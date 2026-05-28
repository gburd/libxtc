/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/m2/test_net_udp.c -- UDP + DNS resolution tests.
 */

#include "munit.h"
#include "xtc.h"
#include "xtc_net.h"
#include "xtc_int.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

static MunitResult
test_udp_send_recv(const MunitParameter p[], void *d)
{
	int srv, cli;
	int port;
	char host[64];
	size_t n;
	char buf[64] = {0};
	(void)p; (void)d;

	munit_assert_int(xtc_net_udp_socket(XTC_NET_INET, "127.0.0.1", 0,
	    &srv), ==, XTC_OK);
	munit_assert_int(xtc_net_udp_socket(XTC_NET_INET, NULL, 0, &cli),
	    ==, XTC_OK);

	{
		struct sockaddr_in sa;
		socklen_t slen = sizeof sa;
		munit_assert_int(getsockname(srv, (struct sockaddr *)&sa,
		    &slen), ==, 0);
		port = ntohs(sa.sin_port);
	}

	munit_assert_int(xtc_net_udp_sendto(cli, "hello", 5, "127.0.0.1",
	    port), ==, XTC_OK);

	/* Brief retry loop -- UDP can occasionally re-order. */
	{
		int attempts;
		for (attempts = 0; attempts < 50; attempts++) {
			int rc = xtc_net_udp_recvfrom(srv, buf, sizeof buf,
			    host, sizeof host, NULL, &n);
			if (rc == XTC_OK) break;
			if (rc != XTC_E_AGAIN) {
				munit_errorf("recv unexpected rc=%d", rc);
			}
			{ struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL); }
		}
	}
	munit_assert_size(n, ==, 5);
	munit_assert_int(memcmp(buf, "hello", 5), ==, 0);
	munit_assert_string_equal(host, "127.0.0.1");

	xtc_net_close(srv);
	xtc_net_close(cli);
	return MUNIT_OK;
}

static MunitResult
test_dns_resolve_localhost(const MunitParameter p[], void *d)
{
	char addr[64];
	(void)p; (void)d;
	munit_assert_int(xtc_dns_resolve("localhost", 80, XTC_NET_INET,
	    addr, sizeof addr), ==, XTC_OK);
	/* "localhost" usually resolves to 127.0.0.1; some systems map
	 * it to ::1 first.  Accept either. */
	munit_assert_true(strcmp(addr, "127.0.0.1") == 0 ||
	                  strcmp(addr, "::1") == 0 ||
	                  /* Some hosts have IPv4-mapped or alternate
	                   * loopback variants in /etc/hosts. */
	                  strncmp(addr, "127.", 4) == 0);
	return MUNIT_OK;
}

static MunitResult
test_dns_resolve_bad(const MunitParameter p[], void *d)
{
	char addr[64];
	(void)p; (void)d;
	/* Bogus name should fail. */
	munit_assert_int(xtc_dns_resolve("xtc-no-such-host.example.invalid",
	    80, XTC_NET_INET, addr, sizeof addr), ==, XTC_E_INVAL);
	/* NULL args. */
	munit_assert_int(xtc_dns_resolve(NULL, 80, XTC_NET_INET, addr,
	    sizeof addr), ==, XTC_E_INVAL);
	munit_assert_int(xtc_dns_resolve("localhost", 80, XTC_NET_INET,
	    NULL, sizeof addr), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/udp_send_recv",          test_udp_send_recv,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/dns_resolve_localhost",  test_dns_resolve_localhost,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/dns_resolve_bad",        test_dns_resolve_bad,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = {
	"/m2/net_udp", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
