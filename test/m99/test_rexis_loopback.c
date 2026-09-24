/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m99/test_rexis_loopback.c
 *	Loopback tests: spawn server, send commands via TCP, verify responses.
 */

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "munit.h"

#include "rexis_harness.h"

static rexis_srv_t g_srv;

/* Connect to the server started by setup() */
static int
connect_server(void)
{
	return rexis_connect(&g_srv, 2000);
}

/* Send command and receive response (the socket's 2s SO_RCVTIMEO
 * bounds the wait, so a wedged server fails the test, not hangs it). */
static int
send_cmd(int fd, const char *cmd, size_t cmd_len, char *resp, size_t resp_cap)
{
	ssize_t n;

	n = send(fd, cmd, cmd_len, MSG_NOSIGNAL);
	if (n != (ssize_t)cmd_len)
		return -1;
	n = recv(fd, resp, resp_cap - 1, 0);
	if (n <= 0)
		return -1;
	resp[n] = '\0';
	return (int)n;
}

#define build_cmd rexis_build

/* ----- Test cases ----- */

static void *
setup(const MunitParameter params[], void *user_data)
{
	(void)params;
	(void)user_data;

	const char *const args[] = { "--max-clients=100" };

	if (rexis_start(&g_srv, args, 1) != 0)
		munit_error("failed to start server");
	return &g_srv;
}

static void
teardown(void *fixture)
{
	(void)fixture;
	/* A clean exit on SIGTERM is part of every test's contract. */
	munit_assert_int(rexis_stop(&g_srv), ==, 0);
}

static MunitResult
test_ping(const MunitParameter p[], void *d)
{
	int fd;
	char cmd[64], resp[128];
	int n;
	(void)p; (void)d;

	fd = connect_server();
	munit_assert_int(fd, >=, 0);

	n = build_cmd(cmd, sizeof cmd, 1, "PING");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, "+PONG\r\n");

	close(fd);
	return MUNIT_OK;
}

static MunitResult
test_ping_with_message(const MunitParameter p[], void *d)
{
	int fd;
	char cmd[64], resp[128];
	int n;
	(void)p; (void)d;

	fd = connect_server();
	munit_assert_int(fd, >=, 0);

	n = build_cmd(cmd, sizeof cmd, 2, "PING", "hello");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, "$5\r\nhello\r\n");

	close(fd);
	return MUNIT_OK;
}

static MunitResult
test_set_get(const MunitParameter p[], void *d)
{
	int fd;
	char cmd[128], resp[128];
	int n;
	(void)p; (void)d;

	fd = connect_server();
	munit_assert_int(fd, >=, 0);

	/* SET */
	n = build_cmd(cmd, sizeof cmd, 3, "SET", "testkey", "testvalue");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, "+OK\r\n");

	/* GET */
	n = build_cmd(cmd, sizeof cmd, 2, "GET", "testkey");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, "$9\r\ntestvalue\r\n");

	close(fd);
	return MUNIT_OK;
}

static MunitResult
test_del(const MunitParameter p[], void *d)
{
	int fd;
	char cmd[128], resp[128];
	int n;
	(void)p; (void)d;

	fd = connect_server();
	munit_assert_int(fd, >=, 0);

	/* SET */
	n = build_cmd(cmd, sizeof cmd, 3, "SET", "delkey", "value");
	send_cmd(fd, cmd, (size_t)n, resp, sizeof resp);

	/* DEL */
	n = build_cmd(cmd, sizeof cmd, 2, "DEL", "delkey");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, ":1\r\n");

	/* GET should return null */
	n = build_cmd(cmd, sizeof cmd, 2, "GET", "delkey");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, "$-1\r\n");

	close(fd);
	return MUNIT_OK;
}

static MunitResult
test_incr(const MunitParameter p[], void *d)
{
	int fd;
	char cmd[128], resp[128];
	int n;
	(void)p; (void)d;

	fd = connect_server();
	munit_assert_int(fd, >=, 0);

	/* INCR new key */
	n = build_cmd(cmd, sizeof cmd, 2, "INCR", "counter");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, ":1\r\n");

	/* INCR again */
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, ":2\r\n");

	/* INCR third time */
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, ":3\r\n");

	close(fd);
	return MUNIT_OK;
}

static MunitResult
test_list_ops(const MunitParameter p[], void *d)
{
	int fd;
	char cmd[256], resp[256];
	int n;
	(void)p; (void)d;

	fd = connect_server();
	munit_assert_int(fd, >=, 0);

	/* RPUSH */
	n = build_cmd(cmd, sizeof cmd, 3, "RPUSH", "mylist", "one");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, ":1\r\n");

	n = build_cmd(cmd, sizeof cmd, 3, "RPUSH", "mylist", "two");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, ":2\r\n");

	n = build_cmd(cmd, sizeof cmd, 3, "RPUSH", "mylist", "three");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, ":3\r\n");

	/* LLEN */
	n = build_cmd(cmd, sizeof cmd, 2, "LLEN", "mylist");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, ":3\r\n");

	/* LRANGE */
	n = build_cmd(cmd, sizeof cmd, 4, "LRANGE", "mylist", "0", "-1");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_ptr_not_null(strstr(resp, "*3\r\n"));
	munit_assert_ptr_not_null(strstr(resp, "one"));
	munit_assert_ptr_not_null(strstr(resp, "two"));
	munit_assert_ptr_not_null(strstr(resp, "three"));

	close(fd);
	return MUNIT_OK;
}

static MunitResult
test_hash_ops(const MunitParameter p[], void *d)
{
	int fd;
	char cmd[256], resp[256];
	int n;
	(void)p; (void)d;

	fd = connect_server();
	munit_assert_int(fd, >=, 0);

	/* HSET */
	n = build_cmd(cmd, sizeof cmd, 4, "HSET", "myhash", "field1", "value1");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, ":1\r\n");

	/* HGET */
	n = build_cmd(cmd, sizeof cmd, 3, "HGET", "myhash", "field1");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, "$6\r\nvalue1\r\n");

	/* HGET non-existent field */
	n = build_cmd(cmd, sizeof cmd, 3, "HGET", "myhash", "nofield");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, "$-1\r\n");

	close(fd);
	return MUNIT_OK;
}

static MunitResult
test_expire_ttl(const MunitParameter p[], void *d)
{
	int fd;
	char cmd[128], resp[128];
	int n;
	(void)p; (void)d;

	fd = connect_server();
	munit_assert_int(fd, >=, 0);

	/* SET key */
	n = build_cmd(cmd, sizeof cmd, 3, "SET", "expkey", "val");
	send_cmd(fd, cmd, (size_t)n, resp, sizeof resp);

	/* TTL before EXPIRE (should be -1) */
	n = build_cmd(cmd, sizeof cmd, 2, "TTL", "expkey");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, ":-1\r\n");

	/* EXPIRE */
	n = build_cmd(cmd, sizeof cmd, 3, "EXPIRE", "expkey", "10");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, ":1\r\n");

	/* TTL after EXPIRE (should be positive) */
	n = build_cmd(cmd, sizeof cmd, 2, "TTL", "expkey");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	/* Should be :9 or :10 */
	munit_assert_true(resp[0] == ':');
	munit_assert_true(resp[1] >= '1' && resp[1] <= '9');

	close(fd);
	return MUNIT_OK;
}

static MunitResult
test_keys_pattern(const MunitParameter p[], void *d)
{
	int fd;
	char cmd[128], resp[512];
	int n;
	(void)p; (void)d;

	fd = connect_server();
	munit_assert_int(fd, >=, 0);

	/* Create some keys */
	n = build_cmd(cmd, sizeof cmd, 3, "SET", "user:1", "alice");
	send_cmd(fd, cmd, (size_t)n, resp, sizeof resp);
	n = build_cmd(cmd, sizeof cmd, 3, "SET", "user:2", "bob");
	send_cmd(fd, cmd, (size_t)n, resp, sizeof resp);
	n = build_cmd(cmd, sizeof cmd, 3, "SET", "item:1", "widget");
	send_cmd(fd, cmd, (size_t)n, resp, sizeof resp);

	/* KEYS user:* */
	n = build_cmd(cmd, sizeof cmd, 2, "KEYS", "user:*");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_true(strstr(resp, "*2\r\n") != NULL ||
	                  strstr(resp, "*1\r\n") != NULL);

	close(fd);
	return MUNIT_OK;
}

static MunitResult
test_info(const MunitParameter p[], void *d)
{
	int fd;
	char cmd[128], resp[2048];
	int n;
	(void)p; (void)d;

	fd = connect_server();
	munit_assert_int(fd, >=, 0);

	n = build_cmd(cmd, sizeof cmd, 1, "INFO");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);

	/* Should return a bulk string with server info */
	munit_assert_true(resp[0] == '$');
	munit_assert_ptr_not_null(strstr(resp, "redis_version"));

	close(fd);
	return MUNIT_OK;
}

static MunitResult
test_quit(const MunitParameter p[], void *d)
{
	int fd;
	char cmd[64], resp[128];
	ssize_t n;
	(void)p; (void)d;

	fd = connect_server();
	munit_assert_int(fd, >=, 0);

	/* QUIT answers +OK, then the server closes the connection. */
	n = build_cmd(cmd, sizeof cmd, 1, "QUIT");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, "+OK\r\n");
	n = recv(fd, resp, sizeof resp, 0);   /* bounded by SO_RCVTIMEO */
	munit_assert_int((int)n, ==, 0);      /* EOF, not a timeout (-1) */

	close(fd);
	return MUNIT_OK;
}

static MunitResult
test_unknown_command(const MunitParameter p[], void *d)
{
	int fd;
	char cmd[64], resp[128];
	int n;
	(void)p; (void)d;

	fd = connect_server();
	munit_assert_int(fd, >=, 0);

	n = build_cmd(cmd, sizeof cmd, 1, "NOTACMD");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_true(resp[0] == '-');  /* Error response */
	munit_assert_ptr_not_null(strstr(resp, "unknown"));

	close(fd);
	return MUNIT_OK;
}

/* One connection, far more than 64 KiB of replies.  The server's write
 * buffer used to never rewind, so after 65536 bytes of replies (13107
 * "+OK\r\n") every further reply was dropped and the client hung. */
static MunitResult
test_many_replies_one_conn(const MunitParameter p[], void *d)
{
	char cmd[128], resp[128], key[32];
	int fd, n, i;
	(void)p; (void)d;

	fd = connect_server();
	munit_assert_int(fd, >=, 0);
	for (i = 0; i < 20000; i++) {
		snprintf(key, sizeof key, "k%d", i % 64);
		n = build_cmd(cmd, sizeof cmd, 3, "SET", key, "v");
		if (send_cmd(fd, cmd, (size_t)n, resp, sizeof resp) <= 0 ||
		    strcmp(resp, "+OK\r\n") != 0)
			munit_errorf("reply %d (after %d bytes) missing or wrong",
			    i, i * 5);
	}
	close(fd);
	return MUNIT_OK;
}

/* ----- Test suite ----- */

static MunitTest tests[] = {
	{ "/ping",             test_ping,             setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/ping_with_message", test_ping_with_message, setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/set_get",          test_set_get,          setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/del",              test_del,              setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/incr",             test_incr,             setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/list_ops",         test_list_ops,         setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hash_ops",         test_hash_ops,         setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/expire_ttl",       test_expire_ttl,       setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/keys_pattern",     test_keys_pattern,     setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/info",             test_info,             setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/quit",             test_quit,             setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/unknown_command",  test_unknown_command,  setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/many_replies_one_conn", test_many_replies_one_conn, setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m99/rexis_loopback", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
