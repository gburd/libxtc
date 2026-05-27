/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * bench/conformance/w2_echo/xtc/main.c
 *   M17 W2 — TCP echo server, xtc runtime.
 *
 *   Architecture:
 *     - Server thread: xtc_io_t (level-triggered epoll/poll) driving
 *       accept + non-blocking read/write echo.  Uses xtc_net_listen
 *       and xtc_net_setnonblock for the server-side fds.
 *     - Client threads: N pthreads, each opening a raw POSIX TCP socket,
 *       connecting to the server, sending and receiving --msgs messages,
 *       recording per-RTT latency in a thread-local histogram, then
 *       merging into a global histogram under a mutex.
 *
 *   Default: clients=1000, msgs=10 (= 10 000 total round-trips).
 *
 * Build:
 *   cd bench/conformance/w2_echo/xtc && make
 *
 * Usage:
 *   ./bench                             # defaults
 *   ./bench --clients=100 --msgs=20
 *   ./bench --params=clients=100:msgs=20
 */

#define _DEFAULT_SOURCE 1
#define _XOPEN_SOURCE   700

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

/* hist.h implementation is compiled via hist.c; include declarations only. */
#include "hist.h"

#include "xtc.h"
#include "xtc_io.h"
#include "xtc_net.h"

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */

#define MAX_EVENTS      256
#define ECHO_BUF_SIZE   64
#define MAX_CLIENTS     65536

/* Echo message: exactly 8 bytes, no null terminator included. */
#define MSG_PAYLOAD     "xtcping!"
#define MSG_LEN         8

/* -------------------------------------------------------------------------
 * Monotonic clock helper
 * ------------------------------------------------------------------------- */

static uint64_t
mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

/* -------------------------------------------------------------------------
 * Global coordination state
 * ------------------------------------------------------------------------- */

static int              g_listen_fd  = -1;
static int              g_port       = 0;
static sem_t            g_server_ready;   /* server posts when listening     */
static atomic_int       g_clients_done;   /* incremented by each client done */
static int              g_n_clients  = 0; /* total clients (set by main)     */

/* Global latency histogram (protected by g_hist_mu) */
static hist_t           g_hist;
static pthread_mutex_t  g_hist_mu = PTHREAD_MUTEX_INITIALIZER;

/* -------------------------------------------------------------------------
 * Server thread
 *
 * Runs an xtc_io_t poll loop: accepts connections on g_listen_fd, then
 * echoes every incoming byte-stream chunk back on the same fd.  Exits
 * once all client threads have finished (checked via g_clients_done).
 * ------------------------------------------------------------------------- */

static void *
server_thread(void *arg)
{
	xtc_io_t       *io   = NULL;
	xtc_io_event_t  evs[MAX_EVENTS];
	int             n, i;
	(void)arg;

	if (xtc_io_init(&io) != XTC_OK) {
		fprintf(stderr, "w2/xtc: xtc_io_init failed\n");
		return NULL;
	}

	/* Register the already-open listen fd for readability. */
	if (xtc_io_reg_fd(io, g_listen_fd, XTC_IO_READABLE,
	                  (void *)(intptr_t)(-1)) != XTC_OK) {
		fprintf(stderr, "w2/xtc: xtc_io_reg_fd(listen_fd) failed\n");
		xtc_io_fini(io);
		return NULL;
	}

	/* Tell main/clients we're ready. */
	sem_post(&g_server_ready);

	/*
	 * Poll loop.  We run until all clients are done AND the accept
	 * backlog is fully drained (we let one extra pass run after
	 * g_clients_done reaches g_n_clients so lingering echo fds get
	 * final reads/writes processed).
	 */
	for (;;) {
		int all_done = (atomic_load_explicit(&g_clients_done,
		                    memory_order_acquire) >= g_n_clients);

		/* Use a short timeout so we notice when clients are done. */
		int64_t timeout_ns = all_done ? 0 : (int64_t)5000000LL; /* 5 ms */

		n = 0;
		(void)xtc_io_poll(io, evs, MAX_EVENTS, timeout_ns, &n);

		for (i = 0; i < n; i++) {
			intptr_t tag   = (intptr_t)evs[i].tag;
			uint32_t flags = evs[i].flags;

			if (flags & (XTC_IO_HUP | XTC_IO_ERR)) {
				/* Closed or errored client fd. */
				if (tag != -1) {
					xtc_io_del_fd(io, (int)tag);
					close((int)tag);
				}
				continue;
			}

			if (tag == -1) {
				/* listen_fd is readable: drain the accept queue. */
				for (;;) {
					int cfd = accept(g_listen_fd, NULL, NULL);
					if (cfd < 0) {
						/* EAGAIN / EWOULDBLOCK: queue empty */
						break;
					}
					xtc_net_setnonblock(cfd);
					if (xtc_io_reg_fd(io, cfd, XTC_IO_READABLE,
					                  (void *)(intptr_t)cfd) != XTC_OK) {
						close(cfd);
					}
				}
			} else {
				/* Client fd: echo. */
				int cfd = (int)tag;
				char buf[ECHO_BUF_SIZE];
				ssize_t nr;

				for (;;) {
					nr = read(cfd, buf, sizeof buf);
					if (nr > 0) {
						ssize_t sent = 0;
						while (sent < nr) {
							ssize_t nw = write(cfd,
							    buf + sent,
							    (size_t)(nr - sent));
							if (nw < 0) {
								if (errno == EAGAIN ||
								    errno == EWOULDBLOCK)
									continue;
								break;
							}
							sent += nw;
						}
					} else if (nr == 0) {
						/* EOF: client closed. */
						xtc_io_del_fd(io, cfd);
						close(cfd);
						break;
					} else {
						/* nr < 0 */
						if (errno == EAGAIN ||
						    errno == EWOULDBLOCK)
							break; /* no more data right now */
						/* real error */
						xtc_io_del_fd(io, cfd);
						close(cfd);
						break;
					}
				}
			}
		}

		if (all_done && n == 0)
			break;
	}

	xtc_io_fini(io);
	return NULL;
}

/* -------------------------------------------------------------------------
 * Client thread arguments
 * ------------------------------------------------------------------------- */

struct client_args {
	int     msgs;   /* messages to send per client */
};

/* -------------------------------------------------------------------------
 * Client thread
 *
 * Opens a blocking POSIX socket, connects to the echo server, and
 * sends/receives --msgs messages.  Records per-RTT latency.
 * ------------------------------------------------------------------------- */

static void *
client_thread(void *arg)
{
	struct client_args *ca = arg;
	int    fd;
	struct sockaddr_in sa;
	hist_t local_hist;
	int    m;

	/* Block until server is ready. */
	sem_wait(&g_server_ready);
	sem_post(&g_server_ready); /* re-post so next thread unblocks too */

	if (hist_init(&local_hist, HIST_SUB_BITS_DEFAULT) != 0) {
		fprintf(stderr, "w2/xtc: client hist_init failed\n");
		atomic_fetch_add_explicit(&g_clients_done, 1,
		    memory_order_release);
		return NULL;
	}

	/* Create and connect a blocking socket with a receive timeout. */
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		hist_fini(&local_hist);
		atomic_fetch_add_explicit(&g_clients_done, 1,
		    memory_order_release);
		return NULL;
	}

	/* 10-second receive timeout prevents deadlock if the server cannot
	 * accept our connection (e.g. fd exhaustion on server side). */
	{
		struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
		(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
		    &tv, sizeof tv);
	}

	memset(&sa, 0, sizeof sa);
	sa.sin_family      = AF_INET;
	sa.sin_port        = htons((uint16_t)g_port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
		close(fd);
		hist_fini(&local_hist);
		atomic_fetch_add_explicit(&g_clients_done, 1,
		    memory_order_release);
		return NULL;
	}

	/* Send/receive --msgs messages, measuring RTT. */
	for (m = 0; m < ca->msgs; m++) {
		char rxbuf[MSG_LEN];
		ssize_t nw, nr;
		uint64_t t0, t1;

		t0 = mono_ns();

		nw = write(fd, MSG_PAYLOAD, MSG_LEN);
		if (nw != (ssize_t)MSG_LEN)
			break;

		/* Read exactly MSG_LEN bytes back. */
		nr = 0;
		while (nr < (ssize_t)MSG_LEN) {
			ssize_t r = read(fd, rxbuf + nr,
			    (size_t)((ssize_t)MSG_LEN - nr));
			if (r <= 0)
				goto done;
			nr += r;
		}

		t1 = mono_ns();
		hist_record(&local_hist, t1 - t0);
	}

done:
	close(fd);

	/* Merge local histogram into global under lock. */
	pthread_mutex_lock(&g_hist_mu);
	{
		uint32_t bi;
		for (bi = 0; bi < local_hist.n_buckets; bi++) {
			if (local_hist.buckets[bi] == 0)
				continue;
			/* Reconstruct approximate value and re-record. */
			g_hist.buckets[bi] += local_hist.buckets[bi];
		}
		g_hist.total   += local_hist.total;
		if (local_hist.min_ns < g_hist.min_ns)
			g_hist.min_ns = local_hist.min_ns;
		if (local_hist.max_ns > g_hist.max_ns)
			g_hist.max_ns = local_hist.max_ns;
	}
	pthread_mutex_unlock(&g_hist_mu);

	hist_fini(&local_hist);
	atomic_fetch_add_explicit(&g_clients_done, 1, memory_order_release);
	return NULL;
}

/* -------------------------------------------------------------------------
 * Argument parsing
 * Accepts:
 *   --clients=<N>
 *   --msgs=<M>
 *   --params=clients=<N>:msgs=<M>
 * ------------------------------------------------------------------------- */

static void
parse_args(int argc, char *argv[], long *out_clients, long *out_msgs)
{
	int i;
	*out_clients = 1000;
	*out_msgs    = 10;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (strncmp(a, "--clients=", 10) == 0) {
			*out_clients = atol(a + 10);
		} else if (strncmp(a, "--msgs=", 7) == 0) {
			*out_msgs = atol(a + 7);
		} else if (strncmp(a, "--params=", 9) == 0) {
			const char *p = a + 9;
			do {
				if (strncmp(p, "clients=", 8) == 0)
					*out_clients = atol(p + 8);
				else if (strncmp(p, "msgs=", 5) == 0)
					*out_msgs = atol(p + 5);
				p = strchr(p, ':');
				if (p != NULL) p++;
			} while (p != NULL);
		}
	}

	if (*out_clients <= 0) *out_clients = 1000;
	if (*out_msgs    <= 0) *out_msgs    = 10;
}

/* -------------------------------------------------------------------------
 * Free-port helper (probe-and-release, same as test_net.c)
 * ------------------------------------------------------------------------- */

static int
find_free_port(void)
{
	struct sockaddr_in s;
	socklen_t l = sizeof s;
	int probe, port;
	int one = 1;

	probe = socket(AF_INET, SOCK_STREAM, 0);
	if (probe < 0) return -1;
	(void)setsockopt(probe, SOL_SOCKET, SO_REUSEADDR,
	    &one, sizeof one);
	memset(&s, 0, sizeof s);
	s.sin_family      = AF_INET;
	s.sin_port        = 0;
	s.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(probe, (struct sockaddr *)&s, sizeof s) != 0) {
		close(probe);
		return -1;
	}
	if (getsockname(probe, (struct sockaddr *)&s, &l) != 0) {
		close(probe);
		return -1;
	}
	port = (int)ntohs(s.sin_port);
	close(probe);
	return port;
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int
main(int argc, char *argv[])
{
	long               n_clients, n_msgs;
	pthread_t          srv_tid;
	pthread_t         *cli_tids = NULL;
	pthread_attr_t     tattr;
	struct client_args ca;
	xtc_tcp_opts_t     opts    = XTC_TCP_OPTS_DEFAULT;
	uint64_t           t0, t1, elapsed_ns;
	struct rusage      ru;
	uint64_t           cpu_us, rss_kb;
	long               i;

	parse_args(argc, argv, &n_clients, &n_msgs);

	if (n_clients > MAX_CLIENTS) {
		fprintf(stderr, "w2/xtc: --clients capped at %d\n", MAX_CLIENTS);
		n_clients = MAX_CLIENTS;
	}

	/*
	 * Raise the file-descriptor limit to handle listen_fd +
	 * N client sockets + N server-side accepted sockets + headroom.
	 * Without this, the OS default (1024) causes fd exhaustion at
	 * ~500 clients, leading to accept() EMFILE failures and deadlock.
	 */
	{
		struct rlimit rl;
		rlim_t need = (rlim_t)(n_clients * 2 + 64);
		if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
			if (rl.rlim_cur < need) {
				rl.rlim_cur = need;
				if (rl.rlim_max != RLIM_INFINITY &&
				    rl.rlim_max < need)
					rl.rlim_max = need;
				(void)setrlimit(RLIMIT_NOFILE, &rl);
			}
		}
	}

	/* Initialise global histogram. */
	if (hist_init(&g_hist, HIST_SUB_BITS_DEFAULT) != 0) {
		fprintf(stderr, "w2/xtc: hist_init failed\n");
		return 1;
	}

	/* Initialise coordination primitives. */
	sem_init(&g_server_ready, 0, 0);
	atomic_store_explicit(&g_clients_done, 0, memory_order_relaxed);
	g_n_clients = (int)n_clients;

	/* Find a free port and open the listen socket. */
	g_port = find_free_port();
	if (g_port < 0) {
		fprintf(stderr, "w2/xtc: find_free_port failed\n");
		return 1;
	}

	if (xtc_net_listen(XTC_NET_INET, "127.0.0.1", g_port, &opts,
	                   &g_listen_fd) != XTC_OK) {
		fprintf(stderr, "w2/xtc: xtc_net_listen failed on port %d\n",
		    g_port);
		return 1;
	}
	xtc_net_setnonblock(g_listen_fd);

	/* Allocate client thread handles. */
	cli_tids = calloc((size_t)n_clients, sizeof(pthread_t));
	if (cli_tids == NULL) {
		fprintf(stderr, "w2/xtc: calloc cli_tids failed\n");
		return 1;
	}

	/*
	 * Reduce per-thread stack to 256 KB (default 8 MB) so 1000
	 * client threads do not exhaust virtual address space.
	 */
	pthread_attr_init(&tattr);
	pthread_attr_setstacksize(&tattr, 256 * 1024);

	ca.msgs = (int)n_msgs;

	/* ---- Start wall clock ---- */
	t0 = mono_ns();

	/* Spawn server thread. */
	if (pthread_create(&srv_tid, NULL, server_thread, NULL) != 0) {
		fprintf(stderr, "w2/xtc: server pthread_create failed\n");
		pthread_attr_destroy(&tattr);
		return 1;
	}

	/* Spawn client threads (they block on g_server_ready). */
	for (i = 0; i < n_clients; i++) {
		if (pthread_create(&cli_tids[i], &tattr, client_thread, &ca) != 0) {
			fprintf(stderr,
			    "w2/xtc: client pthread_create failed at i=%ld\n", i);
			n_clients = i; /* join only created threads */
			break;
		}
	}

	pthread_attr_destroy(&tattr);

	/* Wait for all client threads. */
	for (i = 0; i < n_clients; i++)
		pthread_join(cli_tids[i], NULL);

	/* Wait for server thread (it exits once clients_done == n_clients). */
	pthread_join(srv_tid, NULL);

	/* ---- Stop wall clock ---- */
	t1 = mono_ns();

	xtc_net_close(g_listen_fd);
	sem_destroy(&g_server_ready);
	free(cli_tids);

	/* ---- Resource usage ---- */
	if (getrusage(RUSAGE_SELF, &ru) != 0) {
		perror("getrusage");
		return 1;
	}

	elapsed_ns = t1 - t0;
	cpu_us = (uint64_t)(ru.ru_utime.tv_sec  + ru.ru_stime.tv_sec)
	         * UINT64_C(1000000)
	       + (uint64_t)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec);
	rss_kb = (uint64_t)ru.ru_maxrss; /* Linux: KiB */

	/* ---- Emit M17 result line ---- */
	printf("workload=W2 runtime=xtc params=clients=%ld:msgs=%ld"
	       " elapsed_ns=%llu"
	       " cpu_us=%llu"
	       " rss_kb=%llu"
	       " p50_ns=%llu"
	       " p95_ns=%llu"
	       " p99_ns=%llu"
	       " p999_ns=%llu\n",
	    n_clients, n_msgs,
	    (unsigned long long)elapsed_ns,
	    (unsigned long long)cpu_us,
	    (unsigned long long)rss_kb,
	    (unsigned long long)hist_percentile(&g_hist, 50.0),
	    (unsigned long long)hist_percentile(&g_hist, 95.0),
	    (unsigned long long)hist_percentile(&g_hist, 99.0),
	    (unsigned long long)hist_percentile(&g_hist, 99.9));

	hist_fini(&g_hist);
	return 0;
}
