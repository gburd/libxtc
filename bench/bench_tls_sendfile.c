/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * bench/bench_tls_sendfile.c -- serve a file over TLS: what the copies
 * and the user-space crypto cost the SENDER.
 *
 *   bench_tls_sendfile MODE FILE MIB CERT KEY
 *     MODE   user      read(2) the file into a buffer, xtc_tls_write it
 *                      (user-space TLS: plaintext copied into the record
 *                      buffer, encrypted, copied to the socket)
 *            ktls      same read(2) + xtc_tls_write, kernel TLS on
 *                      (plaintext copied once, user -> kernel)
 *            sendfile  xtc_tls_sendfile, kernel TLS on: the file pages
 *                      never enter user space
 *     FILE   file to serve (created with MIB MiB of data if shorter)
 *     MIB    MiB to send per run (the file is re-sent from offset 0)
 *
 * Sender and receiver are two threads over loopback TCP; the receiver
 * is the same in every mode (user-space TLS, reads and discards).
 * Prints one line:
 *   mode mib secs MiB_per_s sender_cpu_s sender_cpu_s_per_GiB ktls
 * Run under taskset with two cores so the two threads do not share one.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_net.h"
#include "xtc_tls.h"

#define CHUNK (256u * 1024)

static int     g_sv[2];
static size_t  g_total;

static void
nonblock(int fd)
{
	(void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static int
wait_for(xtc_tls_t *t, int fd)
{
	struct pollfd p = { fd, POLLIN, 0 };
	if (xtc_tls_wants_write(t))
		p.events = POLLOUT;
	return poll(&p, 1, 10000) > 0 ? 0 : -1;
}

static int
handshake(xtc_tls_t *t, int fd)
{
	for (;;) {
		int rc = xtc_tls_handshake(t);
		if (rc != XTC_E_AGAIN)
			return rc;
		if (wait_for(t, fd) != 0)
			return XTC_E_INTERNAL;
	}
}

static void *
receiver(void *arg)
{
	xtc_tls_opts_t o;
	xtc_tls_ctx_t *ctx = NULL;
	xtc_tls_t *t = NULL;
	char *buf = malloc(CHUNK);
	size_t got = 0;
	(void)arg;
	(void)pthread_setname_np(pthread_self(), "tls-recv");  /* for perf */
	memset(&o, 0, sizeof o);
	o.size = sizeof o;
	o.verify_peer_mode = XTC_TLS_VERIFY_NONE;
	if (buf == NULL || xtc_tls_ctx_create(XTC_TLS_CLIENT, &o, &ctx) != 0 ||
	    xtc_tls_create(ctx, g_sv[1], &t) != 0 ||
	    handshake(t, g_sv[1]) != 0)
		goto out;
	while (got < g_total) {
		size_t n = 0;
		int rc = xtc_tls_read(t, buf, CHUNK, &n);
		if (rc == XTC_OK && n > 0)
			got += n;
		else if (rc == XTC_E_AGAIN)
			(void)wait_for(t, g_sv[1]);
		else
			break;
	}
out:
	if (got != g_total)
		fprintf(stderr, "receiver: got %zu of %zu\n", got, g_total);
	if (t != NULL)
		xtc_tls_destroy(t);
	if (ctx != NULL)
		xtc_tls_ctx_destroy(ctx);
	free(buf);
	return NULL;
}

static int
tcp_pair(int sv[2])
{
	struct sockaddr_in sa;
	socklen_t sl = sizeof sa;
	int l = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (l < 0 || bind(l, (struct sockaddr *)&sa, sizeof sa) != 0 ||
	    listen(l, 1) != 0 ||
	    getsockname(l, (struct sockaddr *)&sa, &sl) != 0 ||
	    (sv[1] = socket(AF_INET, SOCK_STREAM, 0)) < 0 ||
	    connect(sv[1], (struct sockaddr *)&sa, sizeof sa) != 0 ||
	    (sv[0] = accept(l, NULL, NULL)) < 0)
		return -1;
	close(l);
	(void)setsockopt(sv[0], IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	nonblock(sv[0]);
	nonblock(sv[1]);
	return 0;
}

static double
thread_cpu(void)
{
	struct rusage ru;
	(void)getrusage(RUSAGE_THREAD, &ru);
	return (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
	    (double)ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

int
main(int argc, char **argv)
{
	xtc_tls_opts_t o;
	xtc_tls_ctx_t *ctx = NULL;
	xtc_tls_t *t = NULL;
	pthread_t th;
	struct stat st;
	char *buf;
	size_t fsize, sent = 0;
	int64_t t0, t1;
	double c0, c1, secs;
	int fd, mode, rc;

	if (argc != 6) {
		fprintf(stderr, "usage: %s user|ktls|sendfile FILE MIB CERT "
		    "KEY\n", argv[0]);
		return 2;
	}
	mode = strcmp(argv[1], "user") == 0 ? 0 :
	    strcmp(argv[1], "ktls") == 0 ? 1 :
	    strcmp(argv[1], "sendfile") == 0 ? 2 : -1;
	if (mode < 0)
		return 2;
	(void)signal(SIGPIPE, SIG_IGN);
	g_total = (size_t)atol(argv[3]) << 20;
	fsize = g_total < (64u << 20) ? g_total : (64u << 20);
	buf = malloc(CHUNK);
	fd = open(argv[2], O_RDWR | O_CREAT, 0600);
	if (buf == NULL || fd < 0 || fstat(fd, &st) != 0)
		return 1;
	if ((size_t)st.st_size < fsize) {
		size_t i, off;
		for (i = 0; i < CHUNK; i++)
			buf[i] = (char)(i * 31 + 7);
		for (off = 0; off < fsize; off += CHUNK)
			if (pwrite(fd, buf, CHUNK, (off_t)off) != (ssize_t)CHUNK)
				return 1;
	}
	memset(&o, 0, sizeof o);
	o.size = sizeof o;
	o.cert_file = argv[4];
	o.key_file = argv[5];
	if (tcp_pair(g_sv) != 0 ||
	    xtc_tls_ctx_create(XTC_TLS_SERVER, &o, &ctx) != 0 ||
	    xtc_tls_create(ctx, g_sv[0], &t) != 0)
		return 1;
	if (mode > 0 && xtc_tls_enable_ktls(t) != XTC_OK)
		return 3;
	if (pthread_create(&th, NULL, receiver, NULL) != 0 ||
	    handshake(t, g_sv[0]) != XTC_OK)
		return 1;
	if (mode > 0 && (xtc_tls_ktls_active(t) & XTC_TLS_KTLS_TX) == 0) {
		fprintf(stderr, "kernel TLS TX not active\n");
		return 3;
	}
	t0 = xtc_clock_mono();
	c0 = thread_cpu();
	while (sent < g_total) {
		size_t off = sent % fsize, want = CHUNK, n = 0;
		if (want > g_total - sent)
			want = g_total - sent;
		if (want > fsize - off)
			want = fsize - off;
		if (mode == 2) {
			rc = xtc_tls_sendfile(t, fd, (int64_t)off, want, &n);
		} else {
			if (pread(fd, buf, want, (off_t)off) != (ssize_t)want)
				return 1;
			for (;;) {
				size_t w = 0;
				rc = xtc_tls_write(t, buf + n, want - n, &w);
				if (rc == XTC_OK && (n += w) == want)
					break;
				if (rc == XTC_E_AGAIN)
					(void)wait_for(t, g_sv[0]);
				else if (rc != XTC_OK)
					break;
			}
		}
		if (rc == XTC_OK)
			sent += n;
		else if (rc == XTC_E_AGAIN)
			(void)wait_for(t, g_sv[0]);
		else
			return 1;
	}
	(void)pthread_join(th, NULL);
	c1 = thread_cpu();
	t1 = xtc_clock_mono();
	secs = (double)(t1 - t0) / 1e9;
	printf("%s %zu %.3f %.1f %.3f %.3f %d\n", argv[1], g_total >> 20,
	    secs, (double)(g_total >> 20) / secs, c1 - c0,
	    (c1 - c0) / ((double)g_total / (1u << 30)),
	    xtc_tls_ktls_active(t));
	xtc_tls_destroy(t);
	xtc_tls_ctx_destroy(ctx);
	xtc_net_close(g_sv[0]);
	close(g_sv[1]);
	close(fd);
	free(buf);
	return 0;
}
