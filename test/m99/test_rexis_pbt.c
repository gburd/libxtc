/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m99/test_rexis_pbt.c
 *	Property-based tests for the Redis-compatible server.
 *
 *	Properties:
 *	  P1: SET/GET round-trip: any key + value, GET returns SET value
 *	  P2: DEL idempotent: DEL twice is same as DEL once
 *	  P3: INCR atomic: N concurrent INCRs -> final value = sum of INCRs
 *	  P4: LPUSH+RPOP preserves FIFO across messages
 *	  P5: Random command stress: no crash or corruption
 *	  P6: a pipelined mix of SET/GET/DEL over values either side of the
 *	      by-reference GET threshold returns, for every GET, exactly the
 *	      bytes the model says (a later SET/DEL in the pipeline must not
 *	      reach a reply still queued)
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "munit.h"
#include "rexis_harness.h"

/* Simplified property-based testing harness */
#define PBT_ITERATIONS 50

static unsigned int g_seed;

static void
pbt_init_seed(void)
{
	g_seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
}

static int
pbt_rand(void)
{
	g_seed = g_seed * 1103515245 + 12345;
	return (int)((g_seed / 65536) % 32768);
}

static int
pbt_rand_range(int min, int max)
{
	return min + pbt_rand() % (max - min + 1);
}

static void
pbt_rand_string(char *buf, size_t max_len, size_t *out_len)
{
	size_t len = (size_t)pbt_rand_range(1, (int)max_len - 1);
	size_t i;
	for (i = 0; i < len; i++) {
		/* ASCII printable, avoiding special RESP chars */
		buf[i] = (char)(32 + pbt_rand() % 94);
		if (buf[i] == '\r' || buf[i] == '\n')
			buf[i] = 'x';
	}
	buf[len] = '\0';
	*out_len = len;
}

/* Server management: see rexis_harness.h (free port, readiness line,
 * bounded clean stop). */
static rexis_srv_t g_srv;

static int
start_server(void)
{
	return rexis_start(&g_srv, NULL, 0);
}

/* 0 iff the server exited 0 on SIGTERM within the bound. */
static int
stop_server(void)
{
	return rexis_stop(&g_srv);
}

static int
connect_server(void)
{
	return rexis_connect(&g_srv, 2000);
}

static int
send_cmd(int fd, const char *cmd, size_t cmd_len, char *resp, size_t resp_cap)
{
	ssize_t n;

	n = send(fd, cmd, cmd_len, MSG_NOSIGNAL);
	if (n != (ssize_t)cmd_len)
		return -1;

	n = recv(fd, resp, resp_cap - 1, 0);
	if (n > 0) {
		resp[n] = '\0';
		return (int)n;
	}
	return -1;
}

#define build_cmd rexis_build

/* ----- Property 1: SET/GET round-trip ----- */

static MunitResult
test_prop_set_get_roundtrip(const MunitParameter p[], void *d)
{
	int fd;
	char cmd[4096], resp[4096];
	char key[64], value[512];
	size_t key_len, value_len;
	int n, i;
	(void)p; (void)d;

	pbt_init_seed();

	munit_assert_int(start_server(), ==, 0);

	fd = connect_server();
	munit_assert_int(fd, >=, 0);

	for (i = 0; i < PBT_ITERATIONS; i++) {
		pbt_rand_string(key, 32, &key_len);
		pbt_rand_string(value, 256, &value_len);

		/* SET */
		n = build_cmd(cmd, sizeof cmd, 3, "SET", key, value);
		munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
		munit_assert_string_equal(resp, "+OK\r\n");

		/* GET */
		n = build_cmd(cmd, sizeof cmd, 2, "GET", key);
		munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);

		/* Verify value matches */
		munit_assert_ptr_not_null(strstr(resp, value));

		/* Clean up */
		n = build_cmd(cmd, sizeof cmd, 2, "DEL", key);
		send_cmd(fd, cmd, (size_t)n, resp, sizeof resp);
	}

	printf("\n  P1 SET/GET round-trip: %d iterations passed\n", i);

	close(fd);
	munit_assert_int(stop_server(), ==, 0);
	return MUNIT_OK;
}

/* ----- Property 2: DEL idempotent ----- */

static MunitResult
test_prop_del_idempotent(const MunitParameter p[], void *d)
{
	int fd;
	char cmd[256], resp[256];
	char key[32];
	size_t key_len;
	int n, i;
	(void)p; (void)d;

	pbt_init_seed();

	munit_assert_int(start_server(), ==, 0);

	fd = connect_server();
	munit_assert_int(fd, >=, 0);

	for (i = 0; i < PBT_ITERATIONS; i++) {
		pbt_rand_string(key, 16, &key_len);

		/* SET key */
		n = build_cmd(cmd, sizeof cmd, 3, "SET", key, "val");
		send_cmd(fd, cmd, (size_t)n, resp, sizeof resp);

		/* DEL once - should return 1 */
		n = build_cmd(cmd, sizeof cmd, 2, "DEL", key);
		munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
		munit_assert_string_equal(resp, ":1\r\n");

		/* DEL twice - should return 0 */
		munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
		munit_assert_string_equal(resp, ":0\r\n");

		/* DEL third time - still 0 */
		munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
		munit_assert_string_equal(resp, ":0\r\n");
	}

	printf("\n  P2 DEL idempotent: %d iterations passed\n", i);

	close(fd);
	munit_assert_int(stop_server(), ==, 0);
	return MUNIT_OK;
}

/* ----- Property 3: INCR atomic under concurrency ----- */

#define INCR_THREADS    10
#define INCR_PER_THREAD 100

static atomic_int g_incr_errors;

static void *
incr_thread(void *arg)
{
	int fd;
	char cmd[64], resp[64];
	int n, i;

	(void)arg;
	fd = connect_server();
	if (fd < 0) {
		atomic_fetch_add(&g_incr_errors, 1);
		return NULL;
	}

	for (i = 0; i < INCR_PER_THREAD; i++) {
		n = build_cmd(cmd, sizeof cmd, 2, "INCR", "atomic_counter");
		if (send_cmd(fd, cmd, (size_t)n, resp, sizeof resp) < 0) {
			atomic_fetch_add(&g_incr_errors, 1);
		} else if (resp[0] != ':') {
			atomic_fetch_add(&g_incr_errors, 1);
		}
	}

	close(fd);
	return NULL;
}

static MunitResult
test_prop_incr_atomic(const MunitParameter p[], void *d)
{
	pthread_t threads[INCR_THREADS];
	int fd;
	char cmd[64], resp[64];
	int n, i;
	int64_t final_value;
	int64_t expected = INCR_THREADS * INCR_PER_THREAD;
	(void)p; (void)d;

	atomic_store(&g_incr_errors, 0);

	munit_assert_int(start_server(), ==, 0);

	/* Initialize counter to 0 */
	fd = connect_server();
	munit_assert_int(fd, >=, 0);
	n = build_cmd(cmd, sizeof cmd, 3, "SET", "atomic_counter", "0");
	send_cmd(fd, cmd, (size_t)n, resp, sizeof resp);
	close(fd);

	/* Spawn threads */
	for (i = 0; i < INCR_THREADS; i++) {
		munit_assert_int(pthread_create(&threads[i], NULL, incr_thread,
		    NULL), ==, 0);
	}

	/* Wait for completion */
	for (i = 0; i < INCR_THREADS; i++) {
		pthread_join(threads[i], NULL);
	}

	/* Check final value */
	fd = connect_server();
	munit_assert_int(fd, >=, 0);
	n = build_cmd(cmd, sizeof cmd, 2, "GET", "atomic_counter");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);

	/* Parse value from $N\r\n<value>\r\n */
	{
		char *p = strchr(resp, '\n');
		if (p) final_value = atoll(p + 1);
		else final_value = 0;
	}

	printf("\n  P3 INCR atomic: %d threads x %d = %lld expected, got %lld"
	       " (errors: %d)\n",
	       INCR_THREADS, INCR_PER_THREAD,
	       (long long)expected, (long long)final_value,
	       atomic_load(&g_incr_errors));

	munit_assert_int(atomic_load(&g_incr_errors), ==, 0);
	munit_assert_llong(final_value, ==, expected);

	close(fd);
	munit_assert_int(stop_server(), ==, 0);
	return MUNIT_OK;
}

/* ----- Property 4: LPUSH + RPOP preserves FIFO ----- */

static MunitResult
test_prop_list_fifo(const MunitParameter p[], void *d)
{
	int fd;
	char cmd[256], resp[256];
	char value[32];
	int n, i, j;
	int num_items;
	(void)p; (void)d;

	pbt_init_seed();

	munit_assert_int(start_server(), ==, 0);

	fd = connect_server();
	munit_assert_int(fd, >=, 0);

	for (i = 0; i < PBT_ITERATIONS / 5; i++) {
		num_items = pbt_rand_range(1, 20);

		/* Clear any existing list */
		n = build_cmd(cmd, sizeof cmd, 2, "DEL", "fifo_list");
		send_cmd(fd, cmd, (size_t)n, resp, sizeof resp);

		/* LPUSH item0, item1, ... to the head; the tail is then the
		 * OLDEST push, so RPOP must return them in push order.  (This
		 * test used to push in reverse and expect the reverse, and it
		 * failed the first time anyone ran it.) */
		for (j = 0; j < num_items; j++) {
			snprintf(value, sizeof value, "item%d", j);
			n = build_cmd(cmd, sizeof cmd, 3, "LPUSH", "fifo_list", value);
			munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
		}

		for (j = 0; j < num_items; j++) {
			char want[64];
			n = build_cmd(cmd, sizeof cmd, 2, "RPOP", "fifo_list");
			munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);

			snprintf(value, sizeof value, "item%d", j);
			snprintf(want, sizeof want, "$%zu\r\n%s\r\n",
			    strlen(value), value);
			munit_assert_string_equal(resp, want);   /* exact: item1 != item10 */
		}

		/* List should be empty now */
		n = build_cmd(cmd, sizeof cmd, 2, "LLEN", "fifo_list");
		munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
		munit_assert_string_equal(resp, ":0\r\n");
	}

	printf("\n  P4 LPUSH/RPOP FIFO: %d iterations passed\n", i);

	close(fd);
	munit_assert_int(stop_server(), ==, 0);
	return MUNIT_OK;
}

/* ----- Property 5: Random command stress ----- */

static const char *RANDOM_COMMANDS[] = {
	"PING", "GET", "SET", "DEL", "EXISTS", "INCR", "DECR",
	"LPUSH", "RPUSH", "LPOP", "RPOP", "LLEN",
	"HSET", "HGET", "HDEL", "HLEN",
	"DBSIZE", "KEYS", "INFO"
};
#define N_RANDOM_COMMANDS (sizeof(RANDOM_COMMANDS) / sizeof(RANDOM_COMMANDS[0]))

static MunitResult
test_prop_random_stress(const MunitParameter p[], void *d)
{
	int fd;
	char cmd[512], resp[4096];
	char key[32], value[64], field[32];
	size_t key_len, value_len, field_len;
	int n, i;
	int cmd_idx;
	const char *command;
	(void)p; (void)d;

	pbt_init_seed();

	munit_assert_int(start_server(), ==, 0);

	fd = connect_server();
	munit_assert_int(fd, >=, 0);

	for (i = 0; i < PBT_ITERATIONS * 10; i++) {
		cmd_idx = pbt_rand() % (int)N_RANDOM_COMMANDS;
		command = RANDOM_COMMANDS[cmd_idx];

		pbt_rand_string(key, 16, &key_len);
		pbt_rand_string(value, 32, &value_len);
		pbt_rand_string(field, 8, &field_len);

		/* Build command based on type */
		if (strcmp(command, "PING") == 0) {
			n = build_cmd(cmd, sizeof cmd, 1, "PING");
		} else if (strcmp(command, "GET") == 0 ||
		           strcmp(command, "DEL") == 0 ||
		           strcmp(command, "EXISTS") == 0 ||
		           strcmp(command, "INCR") == 0 ||
		           strcmp(command, "DECR") == 0 ||
		           strcmp(command, "LPOP") == 0 ||
		           strcmp(command, "RPOP") == 0 ||
		           strcmp(command, "LLEN") == 0 ||
		           strcmp(command, "HLEN") == 0) {
			n = build_cmd(cmd, sizeof cmd, 2, command, key);
		} else if (strcmp(command, "SET") == 0 ||
		           strcmp(command, "LPUSH") == 0 ||
		           strcmp(command, "RPUSH") == 0) {
			n = build_cmd(cmd, sizeof cmd, 3, command, key, value);
		} else if (strcmp(command, "HGET") == 0 ||
		           strcmp(command, "HDEL") == 0) {
			n = build_cmd(cmd, sizeof cmd, 3, command, key, field);
		} else if (strcmp(command, "HSET") == 0) {
			n = build_cmd(cmd, sizeof cmd, 4, command, key, field, value);
		} else if (strcmp(command, "DBSIZE") == 0 ||
		           strcmp(command, "INFO") == 0) {
			n = build_cmd(cmd, sizeof cmd, 1, command);
		} else if (strcmp(command, "KEYS") == 0) {
			n = build_cmd(cmd, sizeof cmd, 2, "KEYS", "*");
		} else {
			continue;
		}

		/* Send and verify we get a response (not a crash) */
		if (send_cmd(fd, cmd, (size_t)n, resp, sizeof resp) < 0) {
			/* Reconnect if disconnected */
			close(fd);
			fd = connect_server();
			if (fd < 0) {
				munit_error("server crashed or became unreachable");
				stop_server();
				return MUNIT_FAIL;
			}
		}

		/* Response should start with a valid RESP type */
		munit_assert_true(resp[0] == '+' || resp[0] == '-' ||
		                  resp[0] == ':' || resp[0] == '$' ||
		                  resp[0] == '*');
	}

	printf("\n  P5 Random stress: %d commands executed without crash\n", i);

	/* Final sanity check */
	n = build_cmd(cmd, sizeof cmd, 1, "PING");
	munit_assert_int(send_cmd(fd, cmd, (size_t)n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, "+PONG\r\n");

	close(fd);
	munit_assert_int(stop_server(), ==, 0);
	return MUNIT_OK;
}

/* ----- Property 6: pipelined SET/GET/DEL of values around the
 * by-reference threshold match a model ----- */

/* Sizes either side of the server's DB_REF_MIN (4096): a GET of a value
 * at or above it is sent from the keyspace's memory by reference, so a
 * later SET / DEL in the same pipeline must not change or free the bytes
 * of a reply that is still queued. */
static const size_t P6_SIZES[] = { 0, 1, 4095, 4096, 4097, 20000, 70000 };
#define P6_N_SIZES  (sizeof P6_SIZES / sizeof P6_SIZES[0])
#define P6_KEYS     3
#define P6_OPS      40
#define P6_ROUNDS   15

struct p6_send { int fd; const char *buf; size_t len; int rc; };

static void *
p6_send_fn(void *arg)
{
	struct p6_send *ps = arg;
	size_t off = 0;
	ssize_t r;

	ps->rc = 0;
	while (off < ps->len) {
		r = send(ps->fd, ps->buf + off, ps->len - off, MSG_NOSIGNAL);
		if (r <= 0) {
			ps->rc = -1;
			break;
		}
		off += (size_t)r;
	}
	return NULL;
}

/* Value bytes are a function of (round, op, offset), so a reply carrying
 * another op's bytes, or freed-and-reused memory, does not match. */
static char
p6_byte(int round, int op, size_t i)
{
	return (char)('A' + (unsigned)(round * 131 + op * 31 + (int)(i % 7)) %
	    26);
}

static MunitResult
test_prop_pipelined_overwrite(const MunitParameter p[], void *d)
{
	/* model: size of each key's value (-1 = absent) and the op that
	 * wrote it */
	long msize[P6_KEYS];
	int mop[P6_KEYS], kind[P6_OPS], wop[P6_OPS];
	long esize[P6_OPS];
	char *buf, *got, hdr[64], key[8];
	size_t off, cap = (size_t)P6_OPS * (70000 + 64), sz, i, hl;
	int fd, round, op, k, n;
	struct p6_send ps;
	pthread_t th;
	(void)p; (void)d;

	pbt_init_seed();
	buf = malloc(cap);
	got = malloc(70000 + 64);
	munit_assert_ptr_not_null(buf);
	munit_assert_ptr_not_null(got);
	munit_assert_int(start_server(), ==, 0);
	fd = connect_server();
	munit_assert_int(fd, >=, 0);

	for (round = 0; round < P6_ROUNDS; round++) {
		for (k = 0; k < P6_KEYS; k++) {
			msize[k] = -1;
			mop[k] = -1;
		}
		off = 0;
		/* DEL every key first, so the model starts empty. */
		for (k = 0; k < P6_KEYS; k++) {
			snprintf(key, sizeof key, "p6k%d", k);
			off += (size_t)sprintf(buf + off, "*2\r\n$3\r\nDEL\r\n"
			    "$%zu\r\n%s\r\n", strlen(key), key);
		}
		for (op = 0; op < P6_OPS; op++) {
			k = pbt_rand() % P6_KEYS;
			kind[op] = pbt_rand() % 5;  /* 0,1 SET; 2,3 GET; 4 DEL */
			snprintf(key, sizeof key, "p6k%d", k);
			if (kind[op] <= 1) {
				sz = P6_SIZES[pbt_rand() % P6_N_SIZES];
				off += (size_t)sprintf(buf + off, "*3\r\n$3\r\nSET"
				    "\r\n$%zu\r\n%s\r\n$%zu\r\n", strlen(key),
				    key, sz);
				for (i = 0; i < sz; i++)
					buf[off + i] = p6_byte(round, op, i);
				off += sz;
				memcpy(buf + off, "\r\n", 2);
				off += 2;
				msize[k] = (long)sz;
				mop[k] = op;
			} else {
				off += (size_t)sprintf(buf + off, "*2\r\n$3\r\n%s"
				    "\r\n$%zu\r\n%s\r\n", kind[op] == 4 ?
				    "DEL" : "GET", strlen(key), key);
				esize[op] = msize[k];
				wop[op] = mop[k];
				if (kind[op] == 4) {
					msize[k] = -1;
					mop[k] = -1;
				}
			}
		}
		/* Send the whole pipeline from a thread while this one reads
		 * slowly, so replies pile up unsent on the server. */
		ps.fd = fd; ps.buf = buf; ps.len = off; ps.rc = -1;
		munit_assert_int(pthread_create(&th, NULL, p6_send_fn, &ps),
		    ==, 0);
		usleep(20 * 1000);
		for (k = 0; k < P6_KEYS; k++) {     /* the DELs */
			munit_assert_size(rexis_read_n(fd, got, 4, 10000), ==, 4);
			munit_assert_true(got[0] == ':');
		}
		for (op = 0; op < P6_OPS; op++) {
			if (kind[op] <= 1) {
				munit_assert_size(rexis_read_n(fd, got, 5, 10000),
				    ==, 5);
				munit_assert_memory_equal(5, got, "+OK\r\n");
			} else if (kind[op] == 4) {
				munit_assert_size(rexis_read_n(fd, got, 4, 10000),
				    ==, 4);
				munit_assert_memory_equal(4, got,
				    esize[op] >= 0 ? ":1\r\n" : ":0\r\n");
			} else if (esize[op] < 0) {
				munit_assert_size(rexis_read_n(fd, got, 5, 10000),
				    ==, 5);
				munit_assert_memory_equal(5, got, "$-1\r\n");
			} else {
				hl = (size_t)snprintf(hdr, sizeof hdr, "$%ld\r\n",
				    esize[op]);
				n = (int)(hl + (size_t)esize[op] + 2);
				munit_assert_size(rexis_read_n(fd, got, (size_t)n,
				    20000), ==, (size_t)n);
				munit_assert_memory_equal(hl, got, hdr);
				for (i = 0; i < (size_t)esize[op]; i++)
					if (got[hl + i] != p6_byte(round,
					    wop[op], i))
						munit_errorf("round %d op %d: GET "
						    "byte %zu of %ld wrong (seed "
						    "%u)", round, op, i,
						    esize[op], g_seed);
				munit_assert_memory_equal(2, got + hl +
				    (size_t)esize[op], "\r\n");
			}
		}
		munit_assert_int(pthread_join(th, NULL), ==, 0);
		munit_assert_int(ps.rc, ==, 0);
	}
	printf("\n  P6 pipelined overwrite: %d rounds x %d ops passed\n",
	    P6_ROUNDS, P6_OPS);
	close(fd);
	free(buf);
	free(got);
	munit_assert_int(stop_server(), ==, 0);
	return MUNIT_OK;
}

/* ----- Test suite ----- */

static MunitTest tests[] = {
	{ "/set_get_roundtrip", test_prop_set_get_roundtrip, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/del_idempotent",    test_prop_del_idempotent,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/incr_atomic",       test_prop_incr_atomic,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/list_fifo",         test_prop_list_fifo,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/random_stress",     test_prop_random_stress,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/pipelined_overwrite", test_prop_pipelined_overwrite, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m99/rexis_pbt", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
