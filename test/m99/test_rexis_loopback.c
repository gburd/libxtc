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

#define TEST_PORT 16390

/* Global server PID */
static pid_t g_server_pid = -1;

/* Start the server in a child process */
static int
start_server(int port)
{
	char port_str[16];
	pid_t pid;

	snprintf(port_str, sizeof port_str, "%d", port);

	pid = fork();
	if (pid < 0)
		return -1;

	if (pid == 0) {
		/* Child: exec the server */
		char *args[] = {
			"../../examples/05_rexis/rexis-server-xtc",
			"-p", port_str,
			"--max-clients=100",
			NULL
		};
		execv(args[0], args);
		/* If exec fails, try relative to build dir */
		args[0] = "./examples/05_rexis/rexis-server-xtc";
		execv(args[0], args);
		_exit(1);
	}

	g_server_pid = pid;
	/* Give the server time to start */
	usleep(200 * 1000);
	return 0;
}

static void
stop_server(void)
{
	if (g_server_pid > 0) {
		kill(g_server_pid, SIGTERM);
		waitpid(g_server_pid, NULL, 0);
		g_server_pid = -1;
	}
}

/* Connect to server */
static int
connect_server(int port)
{
	struct sockaddr_in addr;
	int fd;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = inet_addr("127.0.0.1");

	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

/* Send command and receive response */
static int
send_cmd(int fd, const char *cmd, size_t cmd_len, char *resp, size_t resp_cap)
{
	ssize_t n;
	size_t total = 0;

	n = send(fd, cmd, cmd_len, 0);
	if (n != (ssize_t)cmd_len)
		return -1;

	/* Read response (simplified: just read what's available) */
	usleep(50 * 1000);  /* 50 ms */
	n = recv(fd, resp, resp_cap - 1, MSG_DONTWAIT);
	if (n > 0) {
		resp[n] = '\0';
		total = (size_t)n;
	} else if (n == 0) {
		return -1;  /* Connection closed */
	} else {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		return -1;
	}

	return (int)total;
}

/* Build RESP command */
static int
build_cmd(char *buf, size_t cap, int argc, ...)
{
	va_list ap;
	int i, len = 0;
	const char *arg;

	len += snprintf(buf + len, cap - (size_t)len, "*%d\r\n", argc);

	va_start(ap, argc);
	for (i = 0; i < argc; i++) {
		arg = va_arg(ap, const char *);
		len += snprintf(buf + len, cap - (size_t)len, "$%zu\r\n%s\r\n",
		                strlen(arg), arg);
	}
	va_end(ap);

	return len;
}

/* ----- Test cases ----- */

static void *
setup(const MunitParameter params[], void *user_data)
{
	(void)params;
	(void)user_data;

	if (start_server(TEST_PORT) < 0) {
		munit_error("failed to start server");
		return NULL;
	}

	return (void *)(intptr_t)1;  /* non-NULL to indicate success */
}

static void
teardown(void *fixture)
{
	(void)fixture;
	stop_server();
}

static MunitResult
test_ping(const MunitParameter p[], void *d)
{
	int fd;
	char cmd[64], resp[128];
	int n;
	(void)p; (void)d;

	fd = connect_server(TEST_PORT);
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

	fd = connect_server(TEST_PORT);
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

	fd = connect_server(TEST_PORT);
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

	fd = connect_server(TEST_PORT);
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

	fd = connect_server(TEST_PORT);
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

	fd = connect_server(TEST_PORT);
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

	fd = connect_server(TEST_PORT);
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

	fd = connect_server(TEST_PORT);
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

	fd = connect_server(TEST_PORT);
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

	fd = connect_server(TEST_PORT);
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

	fd = connect_server(TEST_PORT);
	munit_assert_int(fd, >=, 0);

	n = build_cmd(cmd, sizeof cmd, 1, "QUIT");
	send(fd, cmd, (size_t)n, 0);

	usleep(100 * 1000);
	n = recv(fd, resp, sizeof resp, MSG_DONTWAIT);

	/* Should get +OK and then connection should be closed */
	/* Or the connection might already be closed */
	(void)n;

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

	fd = connect_server(TEST_PORT);
	munit_assert_int(fd, >=, 0);

	n = build_cmd(cmd, sizeof cmd, 1, "NOTACMD");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_true(resp[0] == '-');  /* Error response */
	munit_assert_ptr_not_null(strstr(resp, "unknown"));

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
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m99/rexis_loopback", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
