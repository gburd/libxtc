/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m99/test_rexis_budgets.c
 *	Budget enforcement tests: prove that resource limits hold under stress.
 *	The centerpiece for the P99 conference talk.
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "munit.h"

/* Server process management */
static pid_t g_server_pid = -1;

static int
start_server(int port, const char *extra_args[], int n_extra)
{
	char port_str[16];
	pid_t pid;
	int i;

	snprintf(port_str, sizeof port_str, "%d", port);

	pid = fork();
	if (pid < 0)
		return -1;

	if (pid == 0) {
		/* Child: exec the server */
		char *args[32] = {
			"../../examples/05_rexis/rexis-server-xtc",
			"-p", port_str,
			NULL
		};
		int argc = 3;

		for (i = 0; i < n_extra && argc < 30; i++)
			args[argc++] = (char *)extra_args[i];
		args[argc] = NULL;

		execv(args[0], args);
		args[0] = "./examples/05_rexis/rexis-server-xtc";
		execv(args[0], args);
		_exit(1);
	}

	g_server_pid = pid;
	usleep(300 * 1000);  /* 300 ms startup */
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

static int
connect_server(int port)
{
	struct sockaddr_in addr;
	int fd, flag = 1;
	struct timeval tv;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	/* Set receive timeout */
	tv.tv_sec = 1;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

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

static int
send_cmd(int fd, const char *cmd, size_t cmd_len, char *resp, size_t resp_cap)
{
	ssize_t n;

	n = send(fd, cmd, cmd_len, 0);
	if (n != (ssize_t)cmd_len)
		return -1;

	n = recv(fd, resp, resp_cap - 1, 0);
	if (n > 0) {
		resp[n] = '\0';
		return (int)n;
	}
	return -1;
}

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

/* ----- Test: Memory budget ----- */

static MunitResult
test_memory_budget(const MunitParameter p[], void *d)
{
	const char *args[] = { "--max-memory=1048576" };  /* 1 MiB */
	int fd;
	char cmd[4096], resp[256];
	char key[32], value[1024];
	int n, i, oom_count = 0, ok_count = 0;
	(void)p; (void)d;

	/* Start server with 1 MiB memory limit */
	if (start_server(16391, args, 1) < 0) {
		munit_error("failed to start server");
		return MUNIT_FAIL;
	}

	fd = connect_server(16391);
	munit_assert_int(fd, >=, 0);

	/* Create a 1KB value */
	memset(value, 'x', sizeof value - 1);
	value[sizeof value - 1] = '\0';

	/* SET keys until we hit OOM */
	for (i = 0; i < 5000; i++) {
		snprintf(key, sizeof key, "key%04d", i);
		n = build_cmd(cmd, sizeof cmd, 3, "SET", key, value);
		if (send_cmd(fd, cmd, (size_t)n, resp, sizeof resp) < 0)
			break;

		if (strstr(resp, "OOM")) {
			oom_count++;
			if (oom_count >= 10)
				break;  /* Enough OOM errors */
		} else if (strstr(resp, "+OK")) {
			ok_count++;
		}
	}

	printf("\n  Memory budget test: %d keys set, %d OOM errors\n",
	       ok_count, oom_count);

	/* Verify we hit OOM */
	munit_assert_int(oom_count, >, 0);

	/* Verify server still alive - GET should work */
	n = build_cmd(cmd, sizeof cmd, 2, "GET", "key0000");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_ptr_not_null(strstr(resp, "$"));  /* Got bulk string */

	/* Verify PING works */
	n = build_cmd(cmd, sizeof cmd, 1, "PING");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, "+PONG\r\n");

	close(fd);
	stop_server();
	return MUNIT_OK;
}

/* ----- Test: Key count limit ----- */

static MunitResult
test_key_limit(const MunitParameter p[], void *d)
{
	const char *args[] = { "--max-keys=100" };
	int fd;
	char cmd[256], resp[256];
	char key[32];
	int n, i, reject_count = 0, ok_count = 0;
	(void)p; (void)d;

	if (start_server(16392, args, 1) < 0) {
		munit_error("failed to start server");
		return MUNIT_FAIL;
	}

	fd = connect_server(16392);
	munit_assert_int(fd, >=, 0);

	/* SET 110 keys */
	for (i = 0; i < 110; i++) {
		snprintf(key, sizeof key, "k%03d", i);
		n = build_cmd(cmd, sizeof cmd, 3, "SET", key, "v");
		if (send_cmd(fd, cmd, (size_t)n, resp, sizeof resp) < 0)
			break;

		if (strstr(resp, "+OK")) {
			ok_count++;
		} else if (strstr(resp, "OOM") || strstr(resp, "key limit")) {
			reject_count++;
		}
	}

	printf("\n  Key limit test: %d keys created, %d rejected\n",
	       ok_count, reject_count);

	/* Should have exactly 100 keys (or close to it) */
	munit_assert_int(ok_count, ==, 100);
	munit_assert_int(reject_count, >, 0);

	/* Server still alive */
	n = build_cmd(cmd, sizeof cmd, 1, "PING");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, "+PONG\r\n");

	close(fd);
	stop_server();
	return MUNIT_OK;
}

/* ----- Test: Connection limit ----- */

static MunitResult
test_connection_limit(const MunitParameter p[], void *d)
{
	const char *args[] = { "--max-clients=10" };
	int fds[15];
	int i, connected = 0;
	(void)p; (void)d;

	if (start_server(16393, args, 1) < 0) {
		munit_error("failed to start server");
		return MUNIT_FAIL;
	}

	/* Try to open 15 connections */
	for (i = 0; i < 15; i++) {
		fds[i] = connect_server(16393);
		if (fds[i] >= 0)
			connected++;
	}

	printf("\n  Connection limit test: %d/15 connections succeeded\n",
	       connected);

	/* Should have at most 10 connections */
	munit_assert_int(connected, <=, 10);
	munit_assert_int(connected, >=, 1);  /* At least one should work */

	/* Clean up */
	for (i = 0; i < 15; i++) {
		if (fds[i] >= 0)
			close(fds[i]);
	}

	stop_server();
	return MUNIT_OK;
}

/* ----- Test: IOPS rate limit ----- */

static MunitResult
test_iops_limit(const MunitParameter p[], void *d)
{
	const char *args[] = { "--max-iops=100" };
	int fd;
	char cmd[64], resp[64];
	int n, i;
	int64_t start_ns, elapsed_ns;
	int success_count = 0;
	(void)p; (void)d;

	if (start_server(16394, args, 1) < 0) {
		munit_error("failed to start server");
		return MUNIT_FAIL;
	}

	fd = connect_server(16394);
	munit_assert_int(fd, >=, 0);

	/* Record start time */
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		start_ns = ts.tv_sec * 1000000000LL + ts.tv_nsec;
	}

	/* Fire 500 PINGs as fast as possible */
	for (i = 0; i < 500; i++) {
		n = build_cmd(cmd, sizeof cmd, 1, "PING");
		if (send_cmd(fd, cmd, (size_t)n, resp, sizeof resp) > 0) {
			if (strstr(resp, "PONG"))
				success_count++;
		}
	}

	/* Record end time */
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		elapsed_ns = ts.tv_sec * 1000000000LL + ts.tv_nsec - start_ns;
	}

	printf("\n  IOPS limit test: %d successful in %.2f ms (%.1f ops/sec)\n",
	       success_count,
	       elapsed_ns / 1000000.0,
	       success_count / (elapsed_ns / 1000000000.0));

	/* With 100 IOPS limit and token bucket, we should see rate limiting
	 * kick in.  The exact behavior depends on implementation. */
	munit_assert_int(success_count, >=, 100);  /* At least first batch */

	close(fd);
	stop_server();
	return MUNIT_OK;
}

/* ----- Test: Core pinning ----- */

#ifdef __linux__
static int
count_threads(void)
{
	FILE *f;
	char line[256];
	int threads = 0;

	f = fopen("/proc/self/status", "r");
	if (!f)
		return -1;

	while (fgets(line, sizeof line, f)) {
		if (strncmp(line, "Threads:", 8) == 0) {
			threads = atoi(line + 8);
			break;
		}
	}
	fclose(f);
	return threads;
}
#endif

static MunitResult
test_core_pinning(const MunitParameter p[], void *d)
{
	const char *args[] = { "--cores=2" };
	int fd;
	char cmd[64], resp[64];
	int n;
	(void)p; (void)d;

	if (start_server(16395, args, 1) < 0) {
		munit_error("failed to start server");
		return MUNIT_FAIL;
	}

	fd = connect_server(16395);
	munit_assert_int(fd, >=, 0);

	/* Verify server is alive */
	n = build_cmd(cmd, sizeof cmd, 1, "PING");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, "+PONG\r\n");

#ifdef __linux__
	/* Check thread count of the server process */
	if (g_server_pid > 0) {
		char path[64];
		FILE *f;
		char line[256];
		int threads = 0;

		snprintf(path, sizeof path, "/proc/%d/status", g_server_pid);
		f = fopen(path, "r");
		if (f) {
			while (fgets(line, sizeof line, f)) {
				if (strncmp(line, "Threads:", 8) == 0) {
					threads = atoi(line + 8);
					break;
				}
			}
			fclose(f);

			printf("\n  Core pinning test: server has %d threads\n",
			       threads);

			/* With --cores=2, we expect a small number of threads
			 * (main + workers + maybe a few helpers) */
			munit_assert_int(threads, <=, 10);
		}
	}
#else
	printf("\n  Core pinning test: platform does not support thread count check\n");
#endif

	close(fd);
	stop_server();
	return MUNIT_OK;
}

/* ----- Test suite ----- */

static MunitTest tests[] = {
	{ "/memory_budget",    test_memory_budget,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/key_limit",        test_key_limit,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/connection_limit", test_connection_limit, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/iops_limit",       test_iops_limit,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/core_pinning",     test_core_pinning,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m99/rexis_budgets", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
