/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m99/test_rexis_budgets.c
 *	Budget enforcement: each --max-* cap is checked by MEASURING the
 *	running server, not by trusting its replies or its exit code.
 *
 *	  memory   server RSS growth while SETting 4x the cap stays under
 *	           1.5x the cap, and INFO used_memory <= cap
 *	  keys     exactly max-keys SETs succeed, DBSIZE == max-keys
 *	  clients  exactly max-clients of 15 connections are SERVED, and a
 *	           slot freed by a disconnect is reusable (a concurrency cap,
 *	           not a lifetime one)
 *	  iops     8x the limit's worth of pipelined commands run at a
 *	           measured rate <= 1.5x the limit (analytic worst case
 *	           1.33x -- see test_iops_limit)
 *	  cores    the server's CPU affinity mask has exactly N CPUs
 *
 *	Every test also requires a CLEAN shutdown: SIGTERM must make the
 *	server exit 0 within REXIS_STOP_MS (rexis_harness.h).  Before this
 *	was checked the server ignored SIGTERM and every test hung.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "munit.h"
#include "rexis_harness.h"

static rexis_srv_t g_srv;

/* Start with `extra` args or fail the test. */
#define START(...) do {                                                   \
	const char *const a_[] = { __VA_ARGS__ };                          \
	munit_assert_int(rexis_start(&g_srv, a_,                           \
	    (int)(sizeof a_ / sizeof a_[0])), ==, 0);                      \
} while (0)

#define STOP_CLEAN() munit_assert_int(rexis_stop(&g_srv), ==, 0)

/* Send the whole command, read one reply chunk.  <= 0 on failure. */
static int
roundtrip(int fd, const char *cmd, int len, char *resp, size_t cap)
{
	ssize_t n;

	if (send(fd, cmd, (size_t)len, MSG_NOSIGNAL) != len)
		return -1;
	n = recv(fd, resp, cap - 1, 0);
	if (n <= 0)
		return -1;
	resp[n] = '\0';
	return (int)n;
}

/* Integer field `key:` from an INFO reply, or -1. */
static long long
info_field(int fd, const char *key)
{
	char cmd[64], resp[4096];
	const char *p;
	int n;

	n = rexis_build(cmd, sizeof cmd, 1, "INFO");
	if (roundtrip(fd, cmd, n, resp, sizeof resp) <= 0)
		return -1;
	if ((p = strstr(resp, key)) == NULL)
		return -1;
	return atoll(p + strlen(key));
}

#ifdef __linux__
/* A "Key:\t<n> kB"-style or "Key:\t<text>" line from /proc/<pid>/status. */
static int
proc_status(pid_t pid, const char *key, char *out, size_t cap)
{
	char path[64], line[512];
	size_t klen = strlen(key);
	FILE *f;
	int found = -1;

	snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
	if ((f = fopen(path, "r")) == NULL)
		return -1;
	while (fgets(line, sizeof line, f) != NULL) {
		if (strncmp(line, key, klen) == 0 && line[klen] == ':') {
			const char *v = line + klen + 1;
			while (*v == ' ' || *v == '\t')
				v++;
			snprintf(out, cap, "%s", v);
			out[strcspn(out, "\n")] = '\0';
			found = 0;
			break;
		}
	}
	fclose(f);
	return found;
}

static long long
rss_bytes(pid_t pid)
{
	char v[64];
	return proc_status(pid, "VmRSS", v, sizeof v) == 0 ?
	    atoll(v) * 1024 : -1;
}

/* Number of CPUs in a "0-1,4,6-7" list. */
static int
cpu_list_count(const char *s)
{
	int n = 0;
	while (*s != '\0') {
		char *end;
		long a = strtol(s, &end, 10), b = a;
		if (end == s)
			return -1;
		s = end;
		if (*s == '-') {
			b = strtol(s + 1, &end, 10);
			s = end;
		}
		n += (int)(b - a + 1);
		if (*s == ',')
			s++;
	}
	return n;
}
#endif

/* ----- memory ----- */

#define MEM_CAP   (4LL * 1024 * 1024)
#define MEM_VAL   1024

static MunitResult
test_memory_budget(const MunitParameter p[], void *d)
{
	char cap_arg[48], cmd[MEM_VAL + 128], resp[256];
	char key[32], value[MEM_VAL + 1];
	int fd, n, i, oom = 0, ok = 0;
	long long used, rss0 = -1, rss1 = -1;
	(void)p; (void)d;

	snprintf(cap_arg, sizeof cap_arg, "--max-memory=%lld", MEM_CAP);
	START(cap_arg);
	fd = rexis_connect(&g_srv, 2000);
	munit_assert_int(fd, >=, 0);
	n = rexis_build(cmd, sizeof cmd, 1, "PING");
	munit_assert_int(roundtrip(fd, cmd, n, resp, sizeof resp), >, 0);
#ifdef __linux__
	rss0 = rss_bytes(g_srv.pid);
	munit_assert_llong(rss0, >, 0);
#endif

	/* Offer 4x the cap.  Without enforcement every SET succeeds and
	 * the server grows by >= 4x the cap. */
	memset(value, 'x', MEM_VAL);
	value[MEM_VAL] = '\0';
	for (i = 0; i < (int)(4 * MEM_CAP / MEM_VAL); i++) {
		snprintf(key, sizeof key, "key%06d", i);
		n = rexis_build(cmd, sizeof cmd, 3, "SET", key, value);
		munit_assert_int(roundtrip(fd, cmd, n, resp, sizeof resp), >, 0);
		if (strstr(resp, "OOM") != NULL) {
			if (++oom >= 20)
				break;
		} else if (strncmp(resp, "+OK", 3) == 0)
			ok++;
	}
	used = info_field(fd, "used_memory:");
#ifdef __linux__
	rss1 = rss_bytes(g_srv.pid);
#endif
	printf("\n  memory: cap %lld, %d SET ok, %d OOM, used_memory %lld, "
	    "RSS %lld -> %lld (+%lld)\n", MEM_CAP, ok, oom, used,
	    rss0, rss1, rss1 - rss0);

	munit_assert_int(oom, >, 0);
	munit_assert_llong(used, >, 0);
	munit_assert_llong(used, <=, MEM_CAP);
#ifdef __linux__
	/* The measurement.  The accounted bytes are <= the cap; malloc
	 * chunk headers (~3% at 1 KiB values) and the lazily-touched
	 * pages of the second bucket array add a little (measured: 1.10x
	 * the cap on glibc).  1.5x allows for allocator differences and
	 * still fails a server that under-counts by half (grows ~2.1x)
	 * or does not enforce at all (grows 4x). */
	munit_assert_llong(rss1 - rss0, <=, MEM_CAP + MEM_CAP / 2);
#endif

	/* Still serving after refusing writes. */
	n = rexis_build(cmd, sizeof cmd, 2, "EXISTS", "key000000");
	munit_assert_int(roundtrip(fd, cmd, n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, ":1\r\n");
	n = rexis_build(cmd, sizeof cmd, 1, "PING");
	munit_assert_int(roundtrip(fd, cmd, n, resp, sizeof resp), >, 0);
	munit_assert_string_equal(resp, "+PONG\r\n");

	close(fd);
	STOP_CLEAN();
	return MUNIT_OK;
}

/* ----- keys ----- */

static MunitResult
test_key_limit(const MunitParameter p[], void *d)
{
	char cmd[256], resp[256], key[32];
	int fd, n, i, ok = 0, rejected = 0;
	(void)p; (void)d;

	START("--max-keys=100");
	fd = rexis_connect(&g_srv, 2000);
	munit_assert_int(fd, >=, 0);

	for (i = 0; i < 110; i++) {
		snprintf(key, sizeof key, "k%03d", i);
		n = rexis_build(cmd, sizeof cmd, 3, "SET", key, "v");
		munit_assert_int(roundtrip(fd, cmd, n, resp, sizeof resp), >, 0);
		if (strncmp(resp, "+OK", 3) == 0)
			ok++;
		else if (resp[0] == '-')
			rejected++;
	}
	n = rexis_build(cmd, sizeof cmd, 1, "DBSIZE");
	munit_assert_int(roundtrip(fd, cmd, n, resp, sizeof resp), >, 0);
	printf("\n  keys: %d created, %d rejected, DBSIZE %s", ok, rejected,
	    resp);

	munit_assert_int(ok, ==, 100);
	munit_assert_int(rejected, ==, 10);
	munit_assert_string_equal(resp, ":100\r\n");

	close(fd);
	STOP_CLEAN();
	return MUNIT_OK;
}

/* ----- clients ----- */

/* 1 if the connection answers PING, 0 if the server dropped it. */
static int
served(int fd)
{
	char cmd[32], resp[64];
	int n = rexis_build(cmd, sizeof cmd, 1, "PING");
	return roundtrip(fd, cmd, n, resp, sizeof resp) > 0 &&
	    strcmp(resp, "+PONG\r\n") == 0;
}

static MunitResult
test_connection_limit(const MunitParameter p[], void *d)
{
	int fds[15], ok[15];
	int i, n_served = 0, first = -1, reused = 0;
	int64_t deadline;
	(void)p; (void)d;

	START("--max-clients=10");

	/* connect() completes in the kernel backlog even for a connection
	 * the server will refuse, so "connected" proves nothing: count the
	 * connections that are actually SERVED. */
	for (i = 0; i < 15; i++)
		munit_assert_int((fds[i] = rexis_connect(&g_srv, 2000)), >=, 0);
	for (i = 0; i < 15; i++) {
		ok[i] = served(fds[i]);
		n_served += ok[i];
		if (ok[i] && first < 0)
			first = i;
	}
	printf("\n  clients: %d/15 served with --max-clients=10\n", n_served);
	munit_assert_int(n_served, ==, 10);

	/* Free one slot; a new client must get it.  (conn_count used to be
	 * incremented per accept and never decremented, so after
	 * max-clients connections in the server's LIFETIME it refused
	 * everyone.)  The server frees the slot when it sees the EOF;
	 * allow it up to 2s. */
	close(fds[first]);
	fds[first] = -1;
	deadline = rexis_now_ms() + 2000;
	while (!reused && rexis_now_ms() < deadline) {
		int fd = rexis_connect(&g_srv, 1000);
		if (fd >= 0) {
			reused = served(fd);
			close(fd);
		}
		if (!reused)
			usleep(20 * 1000);
	}
	munit_assert_true(reused);

	for (i = 0; i < 15; i++)
		if (fds[i] >= 0)
			close(fds[i]);
	STOP_CLEAN();
	return MUNIT_OK;
}

/* ----- iops ----- */

#define IOPS_LIMIT   200
#define IOPS_N       (8 * IOPS_LIMIT)
#define IOPS_SLACK   1.5

static MunitResult
test_iops_limit(const MunitParameter p[], void *d)
{
	static char out[IOPS_N * 16], in[IOPS_N * 8];
	char arg[32], one[32];
	const char pong[] = "+PONG\r\n";
	size_t want = (size_t)IOPS_N * (sizeof pong - 1), got = 0, k;
	int fd, n, i, len = 0;
	int64_t t0, t1;
	double secs, rate;
	(void)p; (void)d;

	/*
	 * Why these numbers.  rexis's bucket holds IOPS_LIMIT tokens and is
	 * RESET to full at most once per second (main.c rate_limit_refill:
	 * "now - last >= 1s").  The reset's phase is fixed at server init,
	 * not when this test starts, so in the worst case a reset lands
	 * right after the initial burst: in any window of T seconds at most
	 * IOPS_LIMIT * (floor(T) + 2) commands can run.  Finishing
	 * IOPS_N = 8 * IOPS_LIMIT therefore takes T >= 6 s, so a working
	 * limiter can never exceed 8/6 = 1.33x the limit, whatever the
	 * phase; the client clock only ADDS time (it starts before the first
	 * command reaches the server).  The burst allowance is 2/8 of the
	 * work, too little to dominate.  IOPS_SLACK = 1.5 is above that
	 * worst case (headroom for nothing: a correct server cannot trip
	 * it) and ~1000x below an unlimited server on loopback (this
	 * pipeline completes in a few ms without the limiter).  The lower
	 * bound (half the limit) catches a limiter that stalls instead of
	 * pacing.
	 */
	snprintf(arg, sizeof arg, "--max-iops=%d", IOPS_LIMIT);
	START(arg);
	fd = rexis_connect(&g_srv, 3000);
	munit_assert_int(fd, >=, 0);

	/* Pipeline every command at once: the client never waits, so the
	 * server's limiter is the only thing pacing the replies. */
	n = rexis_build(one, sizeof one, 1, "PING");
	for (i = 0; i < IOPS_N; i++) {
		memcpy(out + len, one, (size_t)n);
		len += n;
	}
	t0 = rexis_now_ms();
	munit_assert_int(send(fd, out, (size_t)len, MSG_NOSIGNAL), ==, len);
	while (got < want) {
		ssize_t r = recv(fd, in + got, sizeof in - got, 0);
		if (r <= 0)
			break;
		got += (size_t)r;
	}
	t1 = rexis_now_ms();
	secs = (double)(t1 - t0) / 1000.0;
	rate = secs > 0 ? (double)(got / (sizeof pong - 1)) / secs : 1e9;
	printf("\n  iops: %zu/%d replies in %.3fs = %.1f/s (limit %d, "
	    "assert <= %.0f)\n", got / (sizeof pong - 1), IOPS_N, secs, rate,
	    IOPS_LIMIT, IOPS_LIMIT * IOPS_SLACK);

	munit_assert_size(got, ==, want);
	for (k = 0; k < got; k += sizeof pong - 1)
		munit_assert_memory_equal(sizeof pong - 1, in + k, pong);
	munit_assert_double(rate, <=, IOPS_LIMIT * IOPS_SLACK);
	munit_assert_double(rate, >=, IOPS_LIMIT * 0.5);

	close(fd);
	STOP_CLEAN();
	return MUNIT_OK;
}

/* ----- cores ----- */

static MunitResult
test_core_pinning(const MunitParameter p[], void *d)
{
#ifdef __linux__
	char mine[256], theirs[256];
	int n_cpus;
	(void)p; (void)d;

	/* rexis pins to CPUs 0..N-1; that is only possible if we may run
	 * on both CPU 0 and CPU 1 ourselves. */
	munit_assert_int(proc_status(getpid(), "Cpus_allowed_list", mine,
	    sizeof mine), ==, 0);
	if (cpu_list_count(mine) < 2 || strncmp(mine, "0-", 2) != 0)
		return MUNIT_SKIP;

	START("--cores=2");
	munit_assert_int(proc_status(g_srv.pid, "Cpus_allowed_list", theirs,
	    sizeof theirs), ==, 0);
	n_cpus = cpu_list_count(theirs);
	printf("\n  cores: ours %s, server %s (%d CPUs)\n", mine, theirs,
	    n_cpus);
	munit_assert_int(n_cpus, ==, 2);
	STOP_CLEAN();
	return MUNIT_OK;
#else
	(void)p; (void)d;
	return MUNIT_SKIP;   /* affinity is read from /proc */
#endif
}

/* ----- shutdown ----- */

/* An idle server and a server with an open client both exit 0 on
 * SIGTERM.  (The per-test STOP_CLEAN covers the idle case; this pins
 * the connected case, where a parked connection proc must see the
 * shutdown too.) */
static MunitResult
test_sigterm_with_client(const MunitParameter p[], void *d)
{
	int fd;
	(void)p; (void)d;

	START("--max-clients=10");
	fd = rexis_connect(&g_srv, 2000);
	munit_assert_int(fd, >=, 0);
	munit_assert_true(served(fd));
	STOP_CLEAN();
	close(fd);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/memory_budget",    test_memory_budget,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/key_limit",        test_key_limit,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/connection_limit", test_connection_limit, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/iops_limit",       test_iops_limit,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/core_pinning",     test_core_pinning,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/sigterm_with_client", test_sigterm_with_client, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m99/rexis_budgets", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
