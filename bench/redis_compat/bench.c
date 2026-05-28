/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * bench/redis_compat/bench.c
 *	Simple TCP client benchmark for the Redis-compatible server.
 *	Measures GET/SET ops/sec and latency percentiles.
 *
 *	Reports in M17 conformance format for integration with the
 *	xtc benchmark suite.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define MAX_CLIENTS  1000
#define MAX_SAMPLES  1000000

/* Configuration */
static struct {
	const char *host;
	int         port;
	int         clients;
	int         requests;
	int         pipeline;
	int         keysize;
	int         datasize;
	int         quiet;
	int         compare;   /* compare with real redis */
} g_cfg = {
	.host     = "127.0.0.1",
	.port     = 6379,
	.clients  = 10,
	.requests = 10000,
	.pipeline = 1,
	.keysize  = 16,
	.datasize = 64,
	.quiet    = 0,
	.compare  = 0
};

/* Stats */
static atomic_int g_completed;
static atomic_int g_errors;
static int64_t   *g_latencies;
static atomic_int g_lat_idx;
static int64_t    g_start_ns;
static int64_t    g_end_ns;

static int64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int
connect_server(const char *host, int port)
{
	struct sockaddr_in addr;
	int fd, flag = 1;
	struct timeval tv;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof flag);
	tv.tv_sec = 5;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	inet_pton(AF_INET, host, &addr.sin_addr);

	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

static int
build_set(char *buf, size_t cap, const char *key, const char *val)
{
	return snprintf(buf, cap,
	    "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
	    strlen(key), key, strlen(val), val);
}

static int
build_get(char *buf, size_t cap, const char *key)
{
	return snprintf(buf, cap,
	    "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
	    strlen(key), key);
}

/* Client thread */
static void *
client_thread(void *arg)
{
	int thread_id = (int)(intptr_t)arg;
	int fd;
	char cmd[4096], resp[4096], key[128], value[1024];
	int requests_per_client;
	int i, n;
	int64_t t0, t1;

	requests_per_client = g_cfg.requests / g_cfg.clients;

	fd = connect_server(g_cfg.host, g_cfg.port);
	if (fd < 0) {
		atomic_fetch_add(&g_errors, requests_per_client);
		return NULL;
	}

	/* Prepare value */
	memset(value, 'x', (size_t)g_cfg.datasize);
	value[g_cfg.datasize] = '\0';

	for (i = 0; i < requests_per_client; i++) {
		/* Generate key */
		snprintf(key, sizeof key, "bench:%d:%d", thread_id, i);

		/* Alternate SET and GET */
		if (i % 2 == 0) {
			n = build_set(cmd, sizeof cmd, key, value);
		} else {
			n = build_get(cmd, sizeof cmd, key);
		}

		t0 = now_ns();

		if (send(fd, cmd, (size_t)n, 0) != n) {
			atomic_fetch_add(&g_errors, 1);
			continue;
		}

		n = (int)recv(fd, resp, sizeof resp - 1, 0);
		if (n <= 0) {
			atomic_fetch_add(&g_errors, 1);
			continue;
		}

		t1 = now_ns();

		/* Record latency */
		{
			int idx = atomic_fetch_add(&g_lat_idx, 1);
			if (idx < MAX_SAMPLES)
				g_latencies[idx] = t1 - t0;
		}

		atomic_fetch_add(&g_completed, 1);
	}

	close(fd);
	return NULL;
}

static int
cmp_i64(const void *a, const void *b)
{
	int64_t va = *(const int64_t *)a;
	int64_t vb = *(const int64_t *)b;
	if (va < vb) return -1;
	if (va > vb) return 1;
	return 0;
}

static void
print_stats(const char *label, int port)
{
	int completed = atomic_load(&g_completed);
	int errors = atomic_load(&g_errors);
	int n_lat = atomic_load(&g_lat_idx);
	double elapsed_s = (g_end_ns - g_start_ns) / 1000000000.0;
	double ops_per_sec = completed / elapsed_s;
	int64_t p50, p99, p999;

	if (n_lat > MAX_SAMPLES)
		n_lat = MAX_SAMPLES;

	/* Sort latencies for percentiles */
	qsort(g_latencies, (size_t)n_lat, sizeof(int64_t), cmp_i64);

	p50  = n_lat > 0 ? g_latencies[n_lat * 50 / 100]  : 0;
	p99  = n_lat > 0 ? g_latencies[n_lat * 99 / 100]  : 0;
	p999 = n_lat > 0 ? g_latencies[n_lat * 999 / 1000] : 0;

	if (!g_cfg.quiet) {
		printf("\n=== %s (port %d) ===\n", label, port);
		printf("  Requests:    %d completed, %d errors\n", completed, errors);
		printf("  Duration:    %.2f seconds\n", elapsed_s);
		printf("  Throughput:  %.2f ops/sec\n", ops_per_sec);
		printf("  Latency p50: %.2f us\n", p50 / 1000.0);
		printf("  Latency p99: %.2f us\n", p99 / 1000.0);
		printf("  Latency p999: %.2f us\n", p999 / 1000.0);
	}

	/* M17 format output */
	printf("\n# M17 conformance output\n");
	printf("bench.%s.ops_per_sec=%.2f\n", label, ops_per_sec);
	printf("bench.%s.p50_us=%.2f\n", label, p50 / 1000.0);
	printf("bench.%s.p99_us=%.2f\n", label, p99 / 1000.0);
	printf("bench.%s.p999_us=%.2f\n", label, p999 / 1000.0);
	printf("bench.%s.completed=%d\n", label, completed);
	printf("bench.%s.errors=%d\n", label, errors);
}

static void
run_bench(const char *label, const char *host, int port)
{
	pthread_t threads[MAX_CLIENTS];
	int i;

	atomic_store(&g_completed, 0);
	atomic_store(&g_errors, 0);
	atomic_store(&g_lat_idx, 0);
	memset(g_latencies, 0, MAX_SAMPLES * sizeof(int64_t));

	if (!g_cfg.quiet)
		printf("Starting benchmark: %d clients, %d requests, %s:%d\n",
		       g_cfg.clients, g_cfg.requests, host, port);

	g_start_ns = now_ns();

	for (i = 0; i < g_cfg.clients; i++) {
		pthread_create(&threads[i], NULL, client_thread, (void *)(intptr_t)i);
	}

	for (i = 0; i < g_cfg.clients; i++) {
		pthread_join(threads[i], NULL);
	}

	g_end_ns = now_ns();

	print_stats(label, port);
}

static void
usage(const char *prog)
{
	fprintf(stderr,
	    "Usage: %s [options]\n"
	    "\n"
	    "Options:\n"
	    "  --host HOST      Server hostname (default: 127.0.0.1)\n"
	    "  --port PORT      Server port (default: 6379)\n"
	    "  --clients N      Number of parallel clients (default: 10)\n"
	    "  --requests N     Total number of requests (default: 10000)\n"
	    "  --datasize N     Value size in bytes (default: 64)\n"
	    "  --quiet          Minimal output (M17 format only)\n"
	    "  --compare        Also benchmark real redis-server on port 6379\n"
	    "  --help           Show this help\n",
	    prog);
}

int
main(int argc, char **argv)
{
	static struct option longopts[] = {
		{ "host",      required_argument, NULL, 'h' },
		{ "port",      required_argument, NULL, 'p' },
		{ "clients",   required_argument, NULL, 'c' },
		{ "requests",  required_argument, NULL, 'n' },
		{ "datasize",  required_argument, NULL, 'd' },
		{ "quiet",     no_argument,       NULL, 'q' },
		{ "compare",   no_argument,       NULL, 'C' },
		{ "help",      no_argument,       NULL, '?' },
		{ NULL, 0, NULL, 0 }
	};
	int c;

	while ((c = getopt_long(argc, argv, "h:p:c:n:d:qC", longopts, NULL)) != -1) {
		switch (c) {
		case 'h':
			g_cfg.host = optarg;
			break;
		case 'p':
			g_cfg.port = atoi(optarg);
			break;
		case 'c':
			g_cfg.clients = atoi(optarg);
			if (g_cfg.clients > MAX_CLIENTS)
				g_cfg.clients = MAX_CLIENTS;
			break;
		case 'n':
			g_cfg.requests = atoi(optarg);
			break;
		case 'd':
			g_cfg.datasize = atoi(optarg);
			if (g_cfg.datasize > 900)
				g_cfg.datasize = 900;
			break;
		case 'q':
			g_cfg.quiet = 1;
			break;
		case 'C':
			g_cfg.compare = 1;
			break;
		case '?':
		default:
			usage(argv[0]);
			return 1;
		}
	}

	/* Allocate latency array */
	g_latencies = malloc(MAX_SAMPLES * sizeof(int64_t));
	if (!g_latencies) {
		fprintf(stderr, "failed to allocate latency array\n");
		return 1;
	}

	/* Run benchmark against xtc-redis */
	run_bench("xtc_redis", g_cfg.host, g_cfg.port);

	/* Optionally compare with real Redis */
	if (g_cfg.compare) {
		int redis_fd = connect_server("127.0.0.1", 6379);
		if (redis_fd >= 0) {
			close(redis_fd);
			g_cfg.host = "127.0.0.1";
			g_cfg.port = 6379;
			run_bench("real_redis", g_cfg.host, g_cfg.port);
		} else {
			printf("\nNote: real redis-server not running on port 6379\n");
		}
	}

	free(g_latencies);
	return 0;
}
