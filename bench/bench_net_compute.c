/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * bench/bench_net_compute.c
 *	Mixed network + compute echo server: the workload that decides
 *	whether libxtc should partition cores the way Seastar's asymmetric
 *	io_uring backend does.
 *
 *	WHY THIS EXISTS.  Seastar's asymmetric backend moves the async I/O
 *	execution pipeline onto dedicated "networking cores" so application
 *	cores keep their cycles.  Their published measurements show the win
 *	is NARROW and workload-shaped: +~15% at one shard, a REGRESSION at
 *	four shards on pure I/O (the single networking core spends 40-50% of
 *	its cycles in copy_to_user), a consistent win only when each request
 *	carries roughly 1-4 us of compute, and convergence into the error
 *	margin by 8 us.  At the realistic 7 shards : 1 networking core they
 *	still land BELOW linux-aio on throughput and claim only reclaimed
 *	app-core CPU.
 *
 *	So the question "should libxtc do this?" is not answerable from
 *	first principles -- it needs the discriminating workload, which is
 *	request-response traffic with a TUNABLE per-request compute cost.
 *	bench_net.c has no compute knob, so that workload did not exist in
 *	this tree and the question kept getting re-asked.  This bench exists
 *	to answer it durably, with numbers, and to keep answering it when
 *	the scheduler changes.
 *
 *	WHAT IT MEASURES.  Throughput, p50/p99 request latency, and -- the
 *	metric that actually decides the Seastar question -- CPU cost per
 *	request (CLOCK_PROCESS_CPUTIME_ID over a fixed request count).
 *	Seastar's realistic-ratio claim is not "more throughput", it is
 *	"the same throughput while giving application cores their cycles
 *	back".  A core-partitioning change in libxtc must be judged on that
 *	number, not on throughput alone.
 *
 *	A NOTE ON WHAT libxtc HAS TO GAIN, MEASURED BEFORE BUILDING THIS.
 *	Seastar's win came from moving NETWORK processing off application
 *	cores.  libxtc's network path never reaches io_uring's io-wq at all:
 *	the only ops it submits are poll/poll_multishot/poll_remove (never
 *	io-wq), a preempt timeout (never io-wq), and file read/write/fsync.
 *	Sockets are polled for readiness and then read/written INLINE on the
 *	fiber's own thread.  Therefore io-wq affinity cannot relocate
 *	libxtc's network work -- there is none there to relocate -- and the
 *	expected outcome of this sweep is that core partitioning is NOT
 *	warranted here.  Recording that up front so a negative result reads
 *	as the answer it is, not as a failed experiment.
 *
 *	Usage:
 *	  bench_net_compute server <port> <n_loops> <compute_us>
 *	  bench_net_compute client <addr> <port> <n_conns> <msgs_each> <bytes>
 *
 *	The server spawns one fiber per accepted connection on an N-loop
 *	executor; each request does <compute_us> of real work before the
 *	echo, so the I/O-to-compute ratio is the swept variable.
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_io.h"
#include "xtc_loop.h"
#include "xtc_net.h"
#include "xtc_proc.h"
#include "os_thread.h"   /* portable thread wrapper (bench, not a consumer) */

#define MAX_LOOPS   64
#define BUF_SIZE    65536

/* ---------------------------------------------------------------- shared */

static volatile sig_atomic_t g_stop;

static void
on_sigint(int sig)
{
	(void)sig;
	g_stop = 1;
}

static int
set_nb(int fd)
{
	int f = fcntl(fd, F_GETFL);
	if (f < 0)
		return -1;
	return fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

static int64_t
now_ns(void)
{
	struct timespec ts;
	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/*
 * Per-request synthetic compute.  Calibrated ONCE against the monotonic
 * clock rather than assuming a fixed iteration cost: the whole point of
 * the sweep is the compute-per-request axis, so if the units are wrong
 * the sweep measures the wrong thing.  A fixed iteration count would
 * mean something different on every machine and would silently drift
 * with compiler version.
 */
static uint64_t g_iters_per_us;
static volatile uint64_t g_sink;

static uint64_t
spin_iters(uint64_t iters)
{
	uint64_t acc = 0, k;
	for (k = 0; k < iters; k++)
		acc += (k ^ (k << 1)) + (acc >> 3);
	return acc;
}

static void
calibrate(void)
{
	int64_t t0, t1;
	uint64_t iters = 200000;
	double per_ns;

	/* Warm up so the first timed pass is not measuring page faults and
	 * frequency ramp. */
	g_sink += spin_iters(iters);

	t0 = now_ns();
	g_sink += spin_iters(iters);
	t1 = now_ns();

	per_ns = (double)(t1 - t0) / (double)iters;
	if (per_ns <= 0.0)
		per_ns = 1.0;
	g_iters_per_us = (uint64_t)(1000.0 / per_ns);
	if (g_iters_per_us == 0)
		g_iters_per_us = 1;
	fprintf(stderr, "[calib] %.3f ns/iter -> %llu iters per us\n",
	    per_ns, (unsigned long long)g_iters_per_us);
}

static void
compute_us(unsigned us)
{
	if (us == 0)
		return;
	g_sink += spin_iters((uint64_t)us * g_iters_per_us);
}

/* ---------------------------------------------------------------- server */

struct srv_cfg {
	int      listen_fd;
	unsigned compute;
};

static struct srv_cfg g_srv;
static _Atomic uint64_t g_requests;
static _Atomic uint64_t g_bytes;

/*
 * One fiber per connection.  Reads a request, burns <compute> us, echoes
 * it back.  This is the request-response shape Seastar's rpc_tester
 * drives, which is what makes their compute-per-request axis meaningful.
 */
static void
conn_fiber(void *arg)
{
	int fd = (int)(intptr_t)arg;
	char *buf = malloc(BUF_SIZE);

	if (buf == NULL) {
		(void)close(fd);
		return;
	}
	for (;;) {
		uint32_t revents = 0;
		ssize_t n;

		if (xtc_proc_wait_fd(fd, XTC_IO_READABLE, -1, &revents)
		    != XTC_OK)
			break;
		n = read(fd, buf, BUF_SIZE);
		if (n == 0)
			break;              /* peer closed */
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			break;
		}

		/* The swept variable: per-request application work. */
		compute_us(g_srv.compute);

		{
			ssize_t off = 0;
			while (off < n) {
				ssize_t w = write(fd, buf + off,
				    (size_t)(n - off));
				if (w > 0) {
					off += w;
					continue;
				}
				if (w < 0 && (errno == EAGAIN ||
				    errno == EINTR)) {
					revents = 0;
					if (xtc_proc_wait_fd(fd,
					    XTC_IO_WRITABLE, -1, &revents)
					    != XTC_OK)
						goto done;
					continue;
				}
				goto done;
			}
		}
		atomic_fetch_add_explicit(&g_requests, 1,
		    memory_order_relaxed);
		atomic_fetch_add_explicit(&g_bytes, (uint64_t)n,
		    memory_order_relaxed);
	}
done:
	free(buf);
	(void)close(fd);
}

/* Accept loop: hands each connection to a MIGRATABLE fiber so the
 * executor's work stealing can rebalance connections across loops, which
 * is libxtc's answer to the imbalance Seastar attacks with core
 * partitioning.  Comparing those two answers is the point. */
static void
accept_fiber(void *arg)
{
	xtc_loop_t *self = (xtc_loop_t *)arg;

	for (;;) {
		uint32_t revents = 0;
		int cfd;

		if (g_stop)
			break;
		if (xtc_proc_wait_fd(g_srv.listen_fd, XTC_IO_READABLE,
		    200LL * 1000 * 1000, &revents) != XTC_OK)
			continue;
		cfd = accept(g_srv.listen_fd, NULL, NULL);
		if (cfd < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			break;
		}
		{
			int one = 1;
			(void)setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY,
			    &one, sizeof one);
		}
		if (set_nb(cfd) != 0) {
			(void)close(cfd);
			continue;
		}
		{
			xtc_proc_opts_t o;
			memset(&o, 0, sizeof o);
			o.name = "conn";
			o.migratable = 1;
			if (xtc_proc_spawn(self, conn_fiber,
			    (void *)(intptr_t)cfd, &o, NULL) != XTC_OK)
				(void)close(cfd);
		}
	}
}

static int
run_server(int port, int n_loops, unsigned compute)
{
	xtc_exec_t *e = NULL;
	struct sockaddr_in sa;
	int fd, one = 1, i;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}
	(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons((uint16_t)port);
	if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
		perror("bind");
		(void)close(fd);
		return 1;
	}
	if (listen(fd, 4096) != 0) {
		perror("listen");
		(void)close(fd);
		return 1;
	}
	if (set_nb(fd) != 0) {
		perror("set_nb");
		(void)close(fd);
		return 1;
	}

	g_srv.listen_fd = fd;
	g_srv.compute = compute;

	if (xtc_exec_init(&e, n_loops) != XTC_OK) {
		fprintf(stderr, "xtc_exec_init failed\n");
		(void)close(fd);
		return 1;
	}
	/* One accept fiber per loop, all on the same listen fd: the kernel
	 * distributes accepts, so no single loop is the accept bottleneck. */
	for (i = 0; i < n_loops; i++) {
		xtc_loop_t *lp = xtc_exec_loop(e, i);
		xtc_proc_opts_t o;
		memset(&o, 0, sizeof o);
		o.name = "accept";
		if (xtc_proc_spawn(lp, accept_fiber, lp, &o, NULL) != XTC_OK) {
			fprintf(stderr, "spawn accept failed\n");
			(void)xtc_exec_fini(e);
			(void)close(fd);
			return 1;
		}
	}

	fprintf(stderr, "[server] port=%d loops=%d compute=%uus "
	    "backend=%s\n", port, n_loops, compute, xtc_io_backend_name());
	fprintf(stderr, "[server] SIGINT to stop\n");

	(void)xtc_exec_run(e);

	/*
	 * Worker-thread CPU time is the metric the Seastar question turns
	 * on ("same throughput, app cores get their cycles back").  This
	 * bench is a CONSUMER, so it uses only the public xtc_* API and does
	 * NOT reach into the executor for per-worker clocks -- there is no
	 * public accessor for them and adding one just to instrument a bench
	 * would be the wrong reason to grow the ABI.  Process-wide CPU time
	 * is the honest consumer-visible proxy: with a fixed request count
	 * and a fixed compute cost, a core-partitioning change shows up as a
	 * change in CPU-seconds per request.  Per-thread detail, if ever
	 * needed, comes from `perf` or /proc/<pid>/task/<tid>/stat outside
	 * this process.
	 */
	{
		uint64_t reqs = atomic_load(&g_requests);
		uint64_t bytes = atomic_load(&g_bytes);
		struct timespec cpu;
		double cpu_s = 0.0;

		if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu) == 0)
			cpu_s = (double)cpu.tv_sec +
			    (double)cpu.tv_nsec / 1e9;
		printf("requests=%llu bytes=%llu cpu=%.3fs",
		    (unsigned long long)reqs, (unsigned long long)bytes,
		    cpu_s);
		if (reqs > 0)
			printf(" cpu_us_per_req=%.3f",
			    cpu_s * 1e6 / (double)reqs);
		printf("\n");
	}
	(void)xtc_exec_fini(e);
	(void)close(fd);
	return 0;
}

/* ---------------------------------------------------------------- client */

static int
cmp_i64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
	return x < y ? -1 : (x > y ? 1 : 0);
}

/*
 * Load generator.  One OS THREAD per connection group, each keeping its
 * connections continuously busy, because the shape of the driver decides
 * whether the numbers mean anything.
 *
 * An earlier version round-robined all connections from a single thread
 * with one request in flight at a time.  That makes the CLIENT the
 * bottleneck, not the server: measured throughput then reflects the
 * driver's serial loop, and the sweep produced non-monotonic nonsense
 * (4 loops at 8 us of compute "beating" 0 us by 8.5x).  A benchmark whose
 * driver saturates before the subject is worse than no benchmark, because
 * it produces numbers that look like data.
 */
struct cl_thread {
	__os_thread_t th;
	int          *fds;
	int           n_fds;
	int           msgs;
	int           bytes;
	int64_t      *lat;      /* n_fds * msgs samples */
	int           nlat;
	int           err;
};

static void *
cl_worker(void *arg)
{
	struct cl_thread *t = arg;
	char *buf = malloc((size_t)t->bytes);
	int i, j;

	if (buf == NULL) {
		t->err = 1;
		return NULL;
	}
	memset(buf, 'x', (size_t)t->bytes);

	for (j = 0; j < t->msgs; j++) {
		for (i = 0; i < t->n_fds; i++) {
			int64_t s = now_ns();
			ssize_t off = 0, n;

			while (off < t->bytes) {
				n = write(t->fds[i], buf + off,
				    (size_t)(t->bytes - off));
				if (n <= 0) {
					if (n < 0 && errno == EINTR)
						continue;
					t->err = 1;
					free(buf);
					return NULL;
				}
				off += n;
			}
			off = 0;
			while (off < t->bytes) {
				n = read(t->fds[i], buf + off,
				    (size_t)(t->bytes - off));
				if (n <= 0) {
					if (n < 0 && errno == EINTR)
						continue;
					t->err = 1;
					free(buf);
					return NULL;
				}
				off += n;
			}
			t->lat[t->nlat++] = now_ns() - s;
		}
	}
	free(buf);
	return NULL;
}

static int
run_client(const char *host, int port, int n_conns, int msgs, int bytes,
    int n_threads)
{
	struct sockaddr_in sa;
	struct cl_thread *ts;
	int *fds;
	int64_t *lat, t0, t1;
	long total;
	int i, per, nlat = 0, rc = 0;

	if (n_conns <= 0 || msgs <= 0 || bytes <= 0 || n_threads <= 0)
		return 1;
	if (n_threads > n_conns)
		n_threads = n_conns;
	if (n_conns % n_threads != 0) {
		fprintf(stderr, "n_conns (%d) must be a multiple of "
		    "n_threads (%d) so every driver thread carries an equal "
		    "share -- otherwise the slowest thread sets the elapsed "
		    "time and skews throughput\n", n_conns, n_threads);
		return 2;
	}
	per = n_conns / n_threads;
	total = (long)n_conns * msgs;

	fds = calloc((size_t)n_conns, sizeof *fds);
	lat = calloc((size_t)total, sizeof *lat);
	ts = calloc((size_t)n_threads, sizeof *ts);
	if (fds == NULL || lat == NULL || ts == NULL) {
		fprintf(stderr, "oom\n");
		free(fds); free(lat); free(ts);
		return 1;
	}

	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
		fprintf(stderr, "bad addr %s\n", host);
		free(fds); free(lat); free(ts);
		return 1;
	}
	for (i = 0; i < n_conns; i++) {
		int one = 1;
		fds[i] = socket(AF_INET, SOCK_STREAM, 0);
		if (fds[i] < 0 ||
		    connect(fds[i], (struct sockaddr *)&sa, sizeof sa) != 0) {
			perror("connect");
			free(fds); free(lat); free(ts);
			return 1;
		}
		(void)setsockopt(fds[i], IPPROTO_TCP, TCP_NODELAY, &one,
		    sizeof one);
	}

	/* Connections are established BEFORE the timer starts, so setup cost
	 * is not charged to throughput. */
	for (i = 0; i < n_threads; i++) {
		ts[i].fds = fds + (long)i * per;
		ts[i].n_fds = per;
		ts[i].msgs = msgs;
		ts[i].bytes = bytes;
		ts[i].lat = lat + (long)i * per * msgs;
		ts[i].nlat = 0;
		ts[i].err = 0;
	}
	t0 = now_ns();
	for (i = 0; i < n_threads; i++) {
		if (__os_thread_create(&ts[i].th, cl_worker, &ts[i])
		    != XTC_OK) {
			fprintf(stderr, "thread create failed\n");
			rc = 1;
			n_threads = i;
			break;
		}
	}
	for (i = 0; i < n_threads; i++)
		(void)__os_thread_join(&ts[i].th, NULL);
	t1 = now_ns();

	for (i = 0; i < n_threads; i++) {
		if (ts[i].err)
			rc = 1;
		nlat += ts[i].nlat;
	}
	/* Compact the per-thread latency runs into one sorted array.  Each
	 * thread wrote into its own slice, so gaps are possible only on
	 * error; drop zeros rather than let them skew the percentiles. */
	{
		int w = 0;
		for (i = 0; i < (int)total; i++)
			if (lat[i] > 0)
				lat[w++] = lat[i];
		nlat = w;
	}
	if (nlat == 0) {
		fprintf(stderr, "no samples\n");
		free(fds); free(lat); free(ts);
		return 1;
	}
	qsort(lat, (size_t)nlat, sizeof *lat, cmp_i64);
	{
		double secs = (double)(t1 - t0) / 1e9;
		printf("conns=%d threads=%d msgs_each=%d bytes=%d\n",
		    n_conns, n_threads, msgs, bytes);
		printf("requests=%d elapsed=%.3fs throughput=%.0f req/s\n",
		    nlat, secs, secs > 0 ? (double)nlat / secs : 0.0);
		printf("p50=%.1fus p99=%.1fus\n",
		    (double)lat[nlat / 2] / 1000.0,
		    (double)lat[(nlat * 99) / 100] / 1000.0);
	}
	for (i = 0; i < n_conns; i++)
		(void)close(fds[i]);
	free(fds); free(lat); free(ts);
	return rc;
}

/* ------------------------------------------------------------------ main */

static void
usage(const char *p)
{
	fprintf(stderr,
	    "usage: %s server <port> <n_loops> <compute_us>\n"
	    "       %s client <addr> <port> <n_conns> <msgs_each> <bytes> "
	    "[n_threads]\n"
	    "\n"
	    "NOTE: throughput from this bench is only meaningful on an\n"
	    "IDLE machine.  Check the load average first -- on a box already\n"
	    "running a build the numbers are noise (measured: 8x\n"
	    "non-monotonic swings at load ~30 on 8 cores).\n",
	    p, p);
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}
	(void)signal(SIGPIPE, SIG_IGN);
	(void)signal(SIGINT, on_sigint);

	if (strcmp(argv[1], "server") == 0) {
		int port = argc > 2 ? atoi(argv[2]) : 9998;
		int loops = argc > 3 ? atoi(argv[3]) : 4;
		unsigned cu = argc > 4 ? (unsigned)atoi(argv[4]) : 0;
		if (loops < 1 || loops > MAX_LOOPS) {
			fprintf(stderr, "n_loops must be 1..%d\n", MAX_LOOPS);
			return 2;
		}
		calibrate();
		return run_server(port, loops, cu);
	}
	if (strcmp(argv[1], "client") == 0) {
		if (argc < 7) {
			usage(argv[0]);
			return 2;
		}
		return run_client(argv[2], atoi(argv[3]), atoi(argv[4]),
		    atoi(argv[5]), atoi(argv[6]),
		    argc > 7 ? atoi(argv[7]) : 4);
	}
	usage(argv[0]);
	return 2;
}
