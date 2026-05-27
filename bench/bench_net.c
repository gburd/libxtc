/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * bench/bench_net.c
 *	TCP echo server driven by xtc_io + xtc_loop.  Demonstrates that
 *	the M2-M6 layered substrate can saturate a NIC: the server
 *	loop accepts connections, registers each socket with xtc_io,
 *	echoes bytes back, and reports per-connection latency.
 *
 *	Usage:
 *	  bench_net server <port>           run the server (default 9999)
 *	  bench_net client <addr> <port> <n_conns> <msgs_each> <bytes>
 *	                                    drive the server, report stats
 *
 *	The server is single-threaded but services thousands of
 *	connections concurrently through xtc_io readiness events.
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_io.h"
#include "xtc_int.h"
#include "os_time.h"

#define BUF_SIZE 65536

/* ---------- server ---------- */

struct conn {
	int fd;
	char buf[BUF_SIZE];
	size_t pending;
	uint64_t bytes_in;
	uint64_t bytes_out;
};

static int
__set_nb(int fd)
{
	int f = fcntl(fd, F_GETFL);
	if (f < 0) return -1;
	return fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

static int
__server_main(int port)
{
	int listen_fd, on = 1;
	struct sockaddr_in sa;
	xtc_io_t *io;
	int rc;

	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) { perror("socket"); return 1; }
	(void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(listen_fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		perror("bind"); return 1;
	}
	if (listen(listen_fd, 1024) < 0) { perror("listen"); return 1; }
	if (__set_nb(listen_fd) < 0) { perror("nonblock listen"); return 1; }

	if ((rc = xtc_io_init(&io)) != XTC_OK) {
		fprintf(stderr, "io_init: %d\n", rc); return 1;
	}
	if ((rc = xtc_io_reg_fd(io, listen_fd, XTC_IO_READABLE,
	    (void *)(intptr_t)-1)) != XTC_OK) {
		fprintf(stderr, "reg listen: %d\n", rc); return 1;
	}

	fprintf(stderr, "echo server: listening on %d (backend=%s)\n",
	    port, xtc_io_backend_name());

	for (;;) {
		xtc_io_event_t evs[64];
		int n_out, i;
		rc = xtc_io_poll(io, evs, 64, -1, &n_out);
		if (rc != XTC_OK) {
			fprintf(stderr, "poll: %d\n", rc);
			break;
		}
		for (i = 0; i < n_out; i++) {
			if (evs[i].flags & XTC_IO_WAKEUP) continue;
			if ((intptr_t)evs[i].tag == -1) {
				/* listen fd ready: accept all pending */
				for (;;) {
					struct sockaddr_in pa;
					socklen_t pl = sizeof pa;
					struct conn *c;
					int fd = accept(listen_fd,
					    (struct sockaddr *)&pa, &pl);
					if (fd < 0) {
						if (errno == EAGAIN ||
						    errno == EWOULDBLOCK) break;
						perror("accept"); break;
					}
					(void)__set_nb(fd);
					(void)setsockopt(fd, IPPROTO_TCP,
					    TCP_NODELAY, &on, sizeof on);
					c = calloc(1, sizeof *c);
					c->fd = fd;
					(void)xtc_io_reg_fd(io, fd,
					    XTC_IO_READABLE | XTC_IO_HUP, c);
				}
			} else {
				struct conn *c = evs[i].tag;
				if (evs[i].flags & XTC_IO_HUP) {
					(void)xtc_io_del_fd(io, c->fd);
					(void)close(c->fd);
					free(c);
					continue;
				}
				if (evs[i].flags & XTC_IO_READABLE) {
					ssize_t r = recv(c->fd, c->buf,
					    BUF_SIZE, 0);
					if (r <= 0) {
						(void)xtc_io_del_fd(io, c->fd);
						(void)close(c->fd);
						free(c);
						continue;
					}
					c->bytes_in += r;
					/* Best-effort echo: blocking send.
					 * Real production would queue and
					 * use XTC_IO_WRITABLE for backpressure;
					 * for the bench we keep it simple. */
					{
						ssize_t w = 0, t;
						while (w < r) {
							t = send(c->fd,
							    c->buf + w,
							    r - w, MSG_NOSIGNAL);
							if (t <= 0) break;
							w += t;
						}
						c->bytes_out += w;
					}
				}
			}
		}
	}
	(void)xtc_io_fini(io);
	(void)close(listen_fd);
	return 0;
}

/* ---------- client ---------- */

struct cli_thread {
	const char *addr;
	int         port;
	int         n_conns;
	int         msgs_each;
	int         bytes;
	uint64_t    total_bytes;
	int64_t     total_ns;
	int64_t     min_ns;
	int64_t     max_ns;
};

static void *
__cli_thread(void *arg)
{
	struct cli_thread *t = arg;
	int i;
	char *send_buf = malloc((size_t)t->bytes);
	char *recv_buf = malloc((size_t)t->bytes);
	memset(send_buf, 'x', (size_t)t->bytes);
	t->min_ns = INT64_MAX;
	t->max_ns = 0;
	t->total_bytes = 0;
	t->total_ns = 0;

	for (i = 0; i < t->n_conns; i++) {
		struct sockaddr_in sa;
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		int j;
		int on = 1;
		if (fd < 0) { perror("client socket"); continue; }
		memset(&sa, 0, sizeof sa);
		sa.sin_family = AF_INET;
		sa.sin_port = htons((uint16_t)t->port);
		(void)inet_pton(AF_INET, t->addr, &sa.sin_addr);
		if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
			perror("connect"); close(fd); continue;
		}
		(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
		for (j = 0; j < t->msgs_each; j++) {
			int64_t a, b;
			ssize_t off, r;
			(void)__os_clock_mono(&a);
			off = 0;
			while (off < t->bytes) {
				ssize_t w = send(fd, send_buf + off,
				    t->bytes - off, MSG_NOSIGNAL);
				if (w <= 0) goto done;
				off += w;
			}
			off = 0;
			while (off < t->bytes) {
				r = recv(fd, recv_buf + off, t->bytes - off, 0);
				if (r <= 0) goto done;
				off += r;
			}
			(void)__os_clock_mono(&b);
			{
				int64_t d = b - a;
				if (d < t->min_ns) t->min_ns = d;
				if (d > t->max_ns) t->max_ns = d;
				t->total_ns += d;
			}
			t->total_bytes += (uint64_t)t->bytes * 2;
		}
done:
		(void)close(fd);
	}
	free(send_buf);
	free(recv_buf);
	return NULL;
}

static int
__client_main(const char *addr, int port, int n_conns, int msgs_each, int bytes)
{
	int n_threads = 4;
	pthread_t th[16];
	struct cli_thread ctxs[16];
	int i;
	int64_t t0, t1;
	uint64_t total_bytes = 0, total_msgs = 0;
	int64_t total_ns = 0, min_ns = INT64_MAX, max_ns = 0;

	if (n_threads > n_conns) n_threads = n_conns;

	signal(SIGPIPE, SIG_IGN);
	(void)__os_clock_mono(&t0);
	for (i = 0; i < n_threads; i++) {
		ctxs[i].addr = addr; ctxs[i].port = port;
		ctxs[i].n_conns = n_conns / n_threads;
		ctxs[i].msgs_each = msgs_each;
		ctxs[i].bytes = bytes;
		pthread_create(&th[i], NULL, __cli_thread, &ctxs[i]);
	}
	for (i = 0; i < n_threads; i++) pthread_join(th[i], NULL);
	(void)__os_clock_mono(&t1);

	for (i = 0; i < n_threads; i++) {
		total_bytes += ctxs[i].total_bytes;
		total_msgs += (uint64_t)ctxs[i].n_conns * (uint64_t)ctxs[i].msgs_each;
		total_ns += ctxs[i].total_ns;
		if (ctxs[i].min_ns < min_ns) min_ns = ctxs[i].min_ns;
		if (ctxs[i].max_ns > max_ns) max_ns = ctxs[i].max_ns;
	}

	{
		double secs = (double)(t1 - t0) / 1e9;
		double mib = (double)total_bytes / (1024.0 * 1024.0);
		printf("connections=%d msgs/conn=%d bytes/msg=%d threads=%d\n",
		    n_conns, msgs_each, bytes, n_threads);
		printf("throughput  = %.1f MiB/s in %.3f s\n", mib / secs, secs);
		printf("messages    = %llu\n", (unsigned long long)total_msgs);
		printf("rps (rt)    = %.0f\n", (double)total_msgs / secs);
		if (total_msgs > 0)
			printf("avg rt lat  = %lld ns\n",
			    (long long)(total_ns / (int64_t)total_msgs));
		printf("min rt lat  = %lld ns\n", (long long)min_ns);
		printf("max rt lat  = %lld ns\n", (long long)max_ns);
	}
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage:\n"
		    "  %s server [port=9999]\n"
		    "  %s client <addr> <port> <n_conns> <msgs/conn> <bytes/msg>\n",
		    argv[0], argv[0]);
		return 1;
	}
	if (strcmp(argv[1], "server") == 0) {
		int port = argc > 2 ? atoi(argv[2]) : 9999;
		return __server_main(port);
	}
	if (strcmp(argv[1], "client") == 0) {
		if (argc < 7) { fprintf(stderr, "client needs 5 args\n"); return 1; }
		return __client_main(argv[2], atoi(argv[3]),
		    atoi(argv[4]), atoi(argv[5]), atoi(argv[6]));
	}
	fprintf(stderr, "unknown mode\n");
	return 1;
}
