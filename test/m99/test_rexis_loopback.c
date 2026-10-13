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

/* ----- Regressions (1.55 review, rexis findings E-M1 / E-M2) ----- */

/* An n-byte value of c's. */
static char *
build_value(size_t n, char c)
{
	char *v = malloc(n + 1);
	munit_assert_ptr_not_null(v);
	memset(v, c, n);
	v[n] = '\0';
	return v;
}

/* SET key to an n-byte value (one round trip). */
static void
set_big(int fd, const char *key, size_t n)
{
	char *v = build_value(n, 'y'), *cmd = malloc(n + 128), ok[8];
	int len;

	munit_assert_ptr_not_null(cmd);
	len = build_cmd(cmd, n + 128, 3, "SET", key, v);
	munit_assert_int(rexis_send_all(fd, cmd, (size_t)len), ==, 0);
	munit_assert_size(rexis_read_n(fd, ok, 5, 3000), ==, 5);
	munit_assert_memory_equal(5, ok, "+OK\r\n");
	free(v);
	free(cmd);
}

/* A reply larger than the connection's 64 KiB write buffer used to be
 * truncated to its header: GET of a 100000 B value returned the 9 bytes
 * "$100000\r\n" and nothing else (exorc/rexis_attack.py hugeget). */
static MunitResult
test_big_reply(const MunitParameter p[], void *d)
{
	const size_t n = 100000;
	char cmd[64], *want, *got;
	int fd, len, hl;
	(void)p; (void)d;

	fd = connect_server();
	munit_assert_int(fd, >=, 0);
	set_big(fd, "huge", n);
	want = malloc(n + 32);
	got = malloc(n + 32);
	munit_assert_ptr_not_null(want);
	munit_assert_ptr_not_null(got);
	hl = snprintf(want, 32, "$%zu\r\n", n);
	memset(want + hl, 'y', n);
	memcpy(want + hl + n, "\r\n", 2);
	len = build_cmd(cmd, sizeof cmd, 2, "GET", "huge");
	munit_assert_int(rexis_send_all(fd, cmd, (size_t)len), ==, 0);
	munit_assert_size(rexis_read_n(fd, got, (size_t)hl + n + 2, 5000), ==,
	    (size_t)hl + n + 2);
	munit_assert_memory_equal((size_t)hl + n + 2, got, want);
	/* and the stream is still in sync */
	len = build_cmd(cmd, sizeof cmd, 1, "PING");
	munit_assert_int(rexis_send_all(fd, cmd, (size_t)len), ==, 0);
	munit_assert_size(rexis_read_n(fd, got, 7, 2000), ==, 7);
	munit_assert_memory_equal(7, got, "+PONG\r\n");
	free(want);
	free(got);
	close(fd);
	return MUNIT_OK;
}

/* A client that pipelines many replies' worth of commands and does not
 * read for a while.  The server used to drop whatever did not fit its
 * write buffer: 20000 pipelined GETs of a 1000 B value delivered 599
 * complete replies and a corrupted stream.  Every byte must arrive, in
 * order, exactly. */
static MunitResult
test_slow_reader_pipeline(const MunitParameter p[], void *d)
{
	enum { N = 20000, V = 1000 };
	char one[V + 16], *pipe_buf, *got;
	size_t one_len, cmd_len, i;
	int fd, len;
	char cmd[64];
	(void)p; (void)d;

	fd = connect_server();
	munit_assert_int(fd, >=, 0);
	set_big(fd, "v", V);
	len = snprintf(one, sizeof one, "$%d\r\n", V);
	memset(one + len, 'y', V);
	memcpy(one + len + V, "\r\n", 2);
	one_len = (size_t)len + V + 2;

	cmd_len = (size_t)build_cmd(cmd, sizeof cmd, 2, "GET", "v");
	pipe_buf = malloc(cmd_len * N);
	got = malloc(one_len * N);
	munit_assert_ptr_not_null(pipe_buf);
	munit_assert_ptr_not_null(got);
	for (i = 0; i < N; i++)
		memcpy(pipe_buf + i * cmd_len, cmd, cmd_len);
	munit_assert_int(rexis_send_all(fd, pipe_buf, cmd_len * N), ==, 0);
	usleep(1500 * 1000);              /* do not read for a while */
	munit_assert_size(rexis_read_n(fd, got, one_len * N, 20000), ==,
	    one_len * N);
	for (i = 0; i < N; i++)
		if (memcmp(got + i * one_len, one, one_len) != 0)
			munit_errorf("reply %zu of %d corrupted", i, N);
	free(pipe_buf);
	free(got);
	close(fd);
	return MUNIT_OK;
}

/* Count "$<n>\r\n" bulk headers for exactly-n-byte elements. */
static int
count_bulks(const char *buf, size_t len, const char *hdr)
{
	size_t hl = strlen(hdr), i;
	int c = 0;

	for (i = 0; i + hl <= len; i++)
		if (memcmp(buf + i, hdr, hl) == 0)
			c++;
	return c;
}

/* LRANGE / KEYS / HGETALL used to stop at 1024 / 1024 / 512 elements
 * (fixed stack arrays) under a header that claimed only that many:
 * LRANGE of 2000 elements answered "*1024", KEYS * over 3000 keys
 * "*1024" -- wrong results, no error. */
static MunitResult
test_multi_element_replies_uncapped(const MunitParameter p[], void *d)
{
	enum { NL = 2000, NK = 3000, NH = 1500 };
	static char cmd[NK * 64], buf[NL * 120 + 64];
	char key[32], fld[32], one[160], v[101];
	size_t want, off = 0;
	int fd, i, len, n;
	(void)p; (void)d;

	fd = connect_server();
	munit_assert_int(fd, >=, 0);

	/* list: 2000 x 100 B */
	memset(v, 'e', 100);
	v[100] = '\0';
	for (i = 0; i < NL; i++) {
		len = build_cmd(one, sizeof one, 3, "RPUSH", "L", v);
		memcpy(cmd + off, one, (size_t)len);
		off += (size_t)len;
		if (off > sizeof cmd - sizeof one || i == NL - 1) {
			munit_assert_int(rexis_send_all(fd, cmd, off), ==, 0);
			off = 0;
		}
	}
	/* drain the NL integer replies ":1\r\n".. ":2000\r\n" */
	want = 0;
	for (i = 1; i <= NL; i++)
		want += (size_t)snprintf(one, sizeof one, ":%d\r\n", i);
	munit_assert_size(rexis_read_n(fd, buf, want, 5000), ==, want);

	len = build_cmd(one, sizeof one, 4, "LRANGE", "L", "0", "-1");
	n = build_cmd(one + len, sizeof one - (size_t)len, 1, "PING");
	munit_assert_int(rexis_send_all(fd, one, (size_t)(len + n)), ==, 0);
	want = 7 + (size_t)NL * (6 + 100 + 2) + 7;   /* "*2000\r\n" .. +PONG */
	munit_assert_size(rexis_read_n(fd, buf, want, 5000), ==, want);
	munit_assert_memory_equal(7, buf, "*2000\r\n");
	munit_assert_int(count_bulks(buf, want, "$100\r\n"), ==, NL);
	munit_assert_memory_equal(7, buf + want - 7, "+PONG\r\n");

	/* keys: 3000 more ("L" is one already) */
	off = 0;
	for (i = 0; i < NK; i++) {
		snprintf(key, sizeof key, "key%05d", i);
		len = build_cmd(one, sizeof one, 3, "SET", key, "v");
		memcpy(cmd + off, one, (size_t)len);
		off += (size_t)len;
	}
	munit_assert_int(rexis_send_all(fd, cmd, off), ==, 0);
	munit_assert_size(rexis_read_n(fd, buf, 5 * NK, 5000), ==, 5 * NK);
	len = build_cmd(one, sizeof one, 2, "KEYS", "key*");
	munit_assert_int(rexis_send_all(fd, one, (size_t)len), ==, 0);
	want = 7 + (size_t)NK * (4 + 8 + 2);          /* "$8\r\nkeyNNNNN\r\n" */
	munit_assert_size(rexis_read_n(fd, buf, want, 5000), ==, want);
	munit_assert_memory_equal(7, buf, "*3000\r\n");
	munit_assert_int(count_bulks(buf, want, "$8\r\nkey"), ==, NK);

	/* hash: 1500 fields -> HGETALL is 3000 bulks */
	off = 0;
	for (i = 0; i < NH; i++) {
		snprintf(fld, sizeof fld, "f%05d", i);
		len = build_cmd(one, sizeof one, 4, "HSET", "H", fld, "x");
		memcpy(cmd + off, one, (size_t)len);
		off += (size_t)len;
	}
	munit_assert_int(rexis_send_all(fd, cmd, off), ==, 0);
	munit_assert_size(rexis_read_n(fd, buf, 4 * NH, 5000), ==, 4 * NH);
	len = build_cmd(one, sizeof one, 2, "HGETALL", "H");
	munit_assert_int(rexis_send_all(fd, one, (size_t)len), ==, 0);
	want = 7 + (size_t)NH * ((4 + 6 + 2) + (4 + 1 + 2));
	munit_assert_size(rexis_read_n(fd, buf, want, 5000), ==, want);
	munit_assert_memory_equal(7, buf, "*3000\r\n");
	munit_assert_int(count_bulks(buf, want, "$6\r\nf"), ==, NH);
	len = build_cmd(one, sizeof one, 2, "HKEYS", "H");
	munit_assert_int(rexis_send_all(fd, one, (size_t)len), ==, 0);
	want = 7 + (size_t)NH * (4 + 6 + 2);
	munit_assert_size(rexis_read_n(fd, buf, want, 5000), ==, want);
	munit_assert_memory_equal(7, buf, "*1500\r\n");

	close(fd);
	return MUNIT_OK;
}

/* Inline commands (telnet / nc / redis-cli's raw mode) used to be
 * answered "-ERR protocol error" and the connection closed. */
static MunitResult
test_inline_commands(const MunitParameter p[], void *d)
{
	static const char in[] = "PING\r\nSET a  b\r\n\r\nGET a\nECHO hi\r\n";
	static const char want[] = "+PONG\r\n+OK\r\n$1\r\nb\r\n$2\r\nhi\r\n";
	char got[64];
	int fd;
	(void)p; (void)d;

	fd = connect_server();
	munit_assert_int(fd, >=, 0);
	munit_assert_int(rexis_send_all(fd, in, sizeof in - 1), ==, 0);
	munit_assert_size(rexis_read_n(fd, got, sizeof want - 1, 3000), ==,
	    sizeof want - 1);
	munit_assert_memory_equal(sizeof want - 1, got, want);
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
	{ "/big_reply",        test_big_reply,        setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/slow_reader_pipeline", test_slow_reader_pipeline, setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/multi_element_replies_uncapped", test_multi_element_replies_uncapped, setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/inline_commands",  test_inline_commands,  setup, teardown, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m99/rexis_loopback", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
