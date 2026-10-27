/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/m18/test_tls_ktls.c -- kernel TLS (zero-copy TLS) on fd mode.
 *
 *   api          contract on every backend: NULL -> XTC_E_INVAL /
 *                0; sendfile before kTLS -> XTC_E_NOSYS; enable after
 *                the handshake started -> XTC_E_INVAL (OpenSSL).
 *   roundtrip    a server that enabled kTLS and a plain client exchange
 *                data both ways over loopback TCP; when the kernel took
 *                TX the server's xtc_tls_write and xtc_tls_sendfile reach
 *                the client intact, and the client's data reaches the
 *                server (kernel RX).  SKIP (77 semantics via
 *                MUNIT_SKIP) when the kernel/OpenSSL declines kTLS.
 *                When the kernel itself has the TLS ULP (probed with
 *                setsockopt(TCP_ULP) directly), kernel TX is REQUIRED,
 *                and sendfile must not read(2) the file on this thread.
 *   declined     TLS 1.2 + AES-CBC (no kernel support): both sides asked
 *                for kTLS, the kernel declines, the connection still
 *                works in user space and sendfile reports XTC_E_NOSYS.
 *   no_sigpipe   a kTLS connection whose peer vanished: writes return an
 *                error, the process (default SIGPIPE disposition) lives,
 *                including through xtc_net_close of the socket.
 *
 * Certificates are generated at runtime and removed afterwards (see the
 * TLS test pattern in AGENTS.md).
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
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_net.h"
#include "xtc_tls.h"

static char g_cert[256], g_key[256], g_file[256];

static int
generate_cert(const char *cert_path, const char *key_path, const char *cn)
{
	char cmd[1024], cnf[300];
	FILE *fp;
	int rc;
	(void)snprintf(cnf, sizeof cnf, "%s.cnf", cert_path);
	if ((fp = fopen(cnf, "w")) != NULL) {
		fprintf(fp, "[req]\nprompt = no\ndistinguished_name = dn\n"
		    "[dn]\nCN = %s\n", cn);
		fclose(fp);
	}
	(void)snprintf(cmd, sizeof cmd,
	    "openssl req -x509 -newkey rsa:2048 -nodes -days 1 "
	    "-config %s -keyout %s -out %s -subj /CN=%s 2>/dev/null",
	    cnf, key_path, cert_path, cn);
	rc = system(cmd);
	(void)unlink(cnf);
	return rc;
}

static int
set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL);
	return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* A connected loopback TCP pair (kTLS needs TCP, not AF_UNIX). */
static int
tcp_pair(int sv[2])
{
	struct sockaddr_in sa;
	socklen_t sl = sizeof sa;
	int l, one = 1;
	if ((l = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		return -1;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(l, (struct sockaddr *)&sa, sizeof sa) != 0 ||
	    listen(l, 1) != 0 ||
	    getsockname(l, (struct sockaddr *)&sa, &sl) != 0 ||
	    (sv[1] = socket(AF_INET, SOCK_STREAM, 0)) < 0 ||
	    connect(sv[1], (struct sockaddr *)&sa, sizeof sa) != 0 ||
	    (sv[0] = accept(l, NULL, NULL)) < 0) {
		close(l);
		return -1;
	}
	close(l);
	(void)setsockopt(sv[0], IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	(void)setsockopt(sv[1], IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	return set_nonblock(sv[0]) == 0 && set_nonblock(sv[1]) == 0 ? 0 : -1;
}

/*
 * Does this kernel do TLS in the socket layer?  Attach the "tls" ULP to a
 * connected TCP socket directly (no OpenSSL involved).  When it does, a
 * kTLS-capable libxtc build MUST end up with kernel TX on an AES-GCM
 * connection: a decline is then a failure, not a skip.
 */
static int
kernel_has_ktls(void)
{
	int sv[2], ok;
#if defined(TCP_ULP)
	if (tcp_pair(sv) != 0)
		return 0;
	ok = setsockopt(sv[0], IPPROTO_TCP, TCP_ULP, "tls", 4) == 0;
	close(sv[0]);
	close(sv[1]);
	return ok;
#else
	(void)sv; (void)ok;
	return 0;
#endif
}

/* This thread's read-syscall count (Linux /proc/thread-self/io syscr);
 * -1 when unavailable. */
static long long
thread_syscr(void)
{
	char line[128];
	long long v = -1;
	FILE *fp = fopen("/proc/thread-self/io", "r");
	if (fp == NULL)
		return -1;
	while (fgets(line, sizeof line, fp) != NULL)
		if (sscanf(line, "syscr: %lld", &v) == 1)
			break;
	fclose(fp);
	return v;
}

static int
drive(xtc_tls_t *tls, int fd, int (*fn)(xtc_tls_t *))
{
	for (;;) {
		struct pollfd p;
		int rc = fn(tls);
		if (rc != XTC_E_AGAIN)
			return rc;
		p.fd = fd;
		p.events = xtc_tls_wants_write(tls) ? POLLOUT : POLLIN;
		p.revents = 0;
		if (poll(&p, 1, 5000) <= 0)
			return XTC_E_INTERNAL;
	}
}

static int
write_all(xtc_tls_t *tls, int fd, const void *buf, size_t len)
{
	size_t done = 0;
	while (done < len) {
		size_t n = 0;
		int rc = xtc_tls_write(tls, (const char *)buf + done,
		    len - done, &n);
		if (rc == XTC_OK) {
			done += n;
		} else if (rc == XTC_E_AGAIN) {
			struct pollfd p = { fd, POLLOUT, 0 };
			if (xtc_tls_wants_read(tls))
				p.events = POLLIN;
			if (poll(&p, 1, 5000) <= 0)
				return XTC_E_INTERNAL;
		} else {
			return rc;
		}
	}
	return XTC_OK;
}

static int
read_all(xtc_tls_t *tls, int fd, void *buf, size_t len)
{
	size_t done = 0;
	while (done < len) {
		size_t n = 0;
		int rc = xtc_tls_read(tls, (char *)buf + done, len - done, &n);
		if (rc == XTC_OK && n > 0) {
			done += n;
		} else if (rc == XTC_OK) {
			return XTC_E_IO;                 /* EOF */
		} else if (rc == XTC_E_AGAIN) {
			struct pollfd p = { fd, POLLIN, 0 };
			if (xtc_tls_wants_write(tls))
				p.events = POLLOUT;
			if (poll(&p, 1, 5000) <= 0)
				return XTC_E_INTERNAL;
		} else {
			return rc;
		}
	}
	return XTC_OK;
}

/* *calls counts xtc_tls_sendfile invocations (one sendfile(2) each). */
static int
sendfile_all(xtc_tls_t *tls, int sock, int fd, int64_t off, size_t len,
    long long *calls)
{
	size_t done = 0;
	while (done < len) {
		size_t n = 0;
		int rc = xtc_tls_sendfile(tls, fd, off + (int64_t)done,
		    len - done, &n);
		++*calls;
		if (rc == XTC_OK) {
			done += n;
		} else if (rc == XTC_E_AGAIN) {
			struct pollfd p = { sock, POLLOUT, 0 };
			if (poll(&p, 1, 5000) <= 0)
				return XTC_E_INTERNAL;
		} else {
			return rc;
		}
	}
	return XTC_OK;
}

/* ---- api ------------------------------------------------------------ */

static MunitResult
test_api(const MunitParameter p[], void *d)
{
	xtc_tls_opts_t o;
	xtc_tls_ctx_t *ctx = NULL;
	xtc_tls_t *tls = NULL;
	size_t n = 7;
	int sv[2], rc;
	(void)p; (void)d;
	munit_assert_int(xtc_tls_enable_ktls(NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_tls_ktls_active(NULL), ==, 0);
	munit_assert_int(xtc_tls_sendfile(NULL, 0, 0, 1, &n), !=, XTC_OK);
	memset(&o, 0, sizeof o);
	o.size = sizeof o;
	o.verify_peer_mode = XTC_TLS_VERIFY_NONE;
	rc = xtc_tls_ctx_create(XTC_TLS_CLIENT, &o, &ctx);
	if (rc == XTC_E_NOSYS)
		return MUNIT_SKIP;                       /* --with-tls=none */
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_int(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
	munit_assert_int(set_nonblock(sv[0]), ==, 0);  /* handshake: AGAIN */
	munit_assert_int(xtc_tls_create(ctx, sv[0], &tls), ==, XTC_OK);
	n = 7;
	munit_assert_int(xtc_tls_sendfile(tls, 0, 0, 1, &n), ==, XTC_E_NOSYS);
	munit_assert_size(n, ==, 0);
	rc = xtc_tls_enable_ktls(tls);
	munit_assert_true(rc == XTC_OK || rc == XTC_E_NOSYS);
	if (rc == XTC_OK) {
		/* Start the handshake (it cannot finish: nobody answers). */
		(void)xtc_tls_handshake(tls);
		xtc_tls_destroy(tls);
		munit_assert_int(xtc_tls_create(ctx, sv[0], &tls), ==, XTC_OK);
		(void)xtc_tls_handshake(tls);
		munit_assert_int(xtc_tls_enable_ktls(tls), ==, XTC_E_INVAL);
	}
	munit_assert_int(xtc_tls_ktls_active(tls), ==, 0);
	xtc_tls_destroy(tls);
	xtc_tls_ctx_destroy(ctx);
	close(sv[0]);
	close(sv[1]);
	return MUNIT_OK;
}

/* ---- roundtrip ------------------------------------------------------ */

#define RT_MSG   (256u * 1024)
#define RT_FILE  (1024u * 1024)

struct rt_args {
	int      fd;
	int      rc;
	uint8_t *got_msg, *got_file;
};

static uint8_t g_msg[RT_MSG], g_up[RT_MSG];

static void *
rt_client(void *arg)
{
	struct rt_args *a = arg;
	xtc_tls_opts_t o;
	xtc_tls_ctx_t *ctx = NULL;
	xtc_tls_t *tls = NULL;
	a->rc = -1;
	memset(&o, 0, sizeof o);
	o.size = sizeof o;
	o.verify_peer_mode = XTC_TLS_VERIFY_NONE;   /* self-signed test cert */
	if (xtc_tls_ctx_create(XTC_TLS_CLIENT, &o, &ctx) != XTC_OK ||
	    xtc_tls_create(ctx, a->fd, &tls) != XTC_OK)
		goto out;
	if (drive(tls, a->fd, xtc_tls_handshake) != XTC_OK)
		goto out;
	if (read_all(tls, a->fd, a->got_msg, RT_MSG) != XTC_OK ||
	    read_all(tls, a->fd, a->got_file, RT_FILE) != XTC_OK ||
	    write_all(tls, a->fd, g_up, RT_MSG) != XTC_OK)
		goto out;
	a->rc = 0;
out:
	if (tls != NULL)
		xtc_tls_destroy(tls);
	if (ctx != NULL)
		xtc_tls_ctx_destroy(ctx);
	return NULL;
}

static MunitResult
test_roundtrip(const MunitParameter p[], void *d)
{
	xtc_tls_opts_t o;
	xtc_tls_ctx_t *ctx = NULL;
	xtc_tls_t *tls = NULL;
	struct rt_args a;
	pthread_t th;
	uint8_t *file, *up;
	size_t i;
	int sv[2], ffd, active, rc;
	(void)p; (void)d;

	memset(&o, 0, sizeof o);
	o.size = sizeof o;
	o.cert_file = g_cert;
	o.key_file = g_key;
	rc = xtc_tls_ctx_create(XTC_TLS_SERVER, &o, &ctx);
	if (rc == XTC_E_NOSYS)
		return MUNIT_SKIP;
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_int(tcp_pair(sv), ==, 0);
	munit_assert_int(xtc_tls_create(ctx, sv[0], &tls), ==, XTC_OK);
	rc = xtc_tls_enable_ktls(tls);
	if (rc == XTC_E_NOSYS) {
		xtc_tls_destroy(tls);
		xtc_tls_ctx_destroy(ctx);
		close(sv[0]);
		close(sv[1]);
		return MUNIT_SKIP;                       /* backend: no kTLS */
	}
	munit_assert_int(rc, ==, XTC_OK);

	file = malloc(RT_FILE);
	up = malloc(RT_MSG);
	a.got_msg = malloc(RT_MSG);
	a.got_file = malloc(RT_FILE);
	munit_assert_not_null(file);
	munit_assert_not_null(up);
	munit_assert_not_null(a.got_msg);
	munit_assert_not_null(a.got_file);
	for (i = 0; i < RT_MSG; i++) {
		g_msg[i] = (uint8_t)(i * 31 + 7);
		g_up[i] = (uint8_t)(i * 17 + 3);
	}
	for (i = 0; i < RT_FILE; i++)
		file[i] = (uint8_t)(i * 13 + 1);
	ffd = open(g_file, O_RDWR | O_CREAT | O_TRUNC, 0600);
	munit_assert_int(ffd, >=, 0);
	munit_assert_int((int)write(ffd, file, RT_FILE), ==, (int)RT_FILE);

	a.fd = sv[1];
	munit_assert_int(pthread_create(&th, NULL, rt_client, &a), ==, 0);
	munit_assert_int(drive(tls, sv[0], xtc_tls_handshake), ==, XTC_OK);
	active = xtc_tls_ktls_active(tls);
	if (kernel_has_ktls())
		munit_assert_int(active & XTC_TLS_KTLS_TX, !=, 0);
	if ((active & XTC_TLS_KTLS_TX) != 0) {
		long long r0, r1, probe, calls = 0;
		munit_assert_int(write_all(tls, sv[0], g_msg, RT_MSG), ==,
		    XTC_OK);
		probe = thread_syscr();
		r0 = thread_syscr();
		probe = r0 - probe;          /* the probe's own read(2)s */
		munit_assert_int(sendfile_all(tls, sv[0], ffd, 0, RT_FILE,
		    &calls), ==, XTC_OK);
		r1 = thread_syscr();
		/* Zero copy: the file never passed through a user buffer --
		 * the only read-class syscalls on this thread were the
		 * sendfile(2)s themselves (each counts one), no read(2). */
		if (r0 >= 0 && r1 >= 0)
			munit_assert_llong(r1 - r0 - probe, <=, calls);
		fprintf(stderr, "sendfile: %u bytes in %lld calls, %lld "
		    "read-class syscalls\n", RT_FILE, calls, r1 - r0 - probe);
	} else {
		/* The kernel declined (no tls.ko, unsupported cipher): the
		 * connection must still work in user space. */
		size_t n = 9;
		munit_assert_int(xtc_tls_sendfile(tls, ffd, 0, RT_FILE, &n),
		    ==, XTC_E_NOSYS);
		munit_assert_int(write_all(tls, sv[0], g_msg, RT_MSG), ==,
		    XTC_OK);
		munit_assert_int(write_all(tls, sv[0], file, RT_FILE), ==,
		    XTC_OK);
	}
	munit_assert_int(read_all(tls, sv[0], up, RT_MSG), ==, XTC_OK);
	munit_assert_int(pthread_join(th, NULL), ==, 0);
	munit_assert_int(a.rc, ==, 0);
	munit_assert_memory_equal(RT_MSG, a.got_msg, g_msg);
	munit_assert_memory_equal(RT_FILE, a.got_file, file);
	munit_assert_memory_equal(RT_MSG, up, g_up);
	xtc_tls_destroy(tls);
	xtc_tls_ctx_destroy(ctx);
	xtc_net_close(sv[0]);
	close(sv[1]);
	close(ffd);
	(void)unlink(g_file);
	free(file);
	free(up);
	free(a.got_msg);
	free(a.got_file);
	if ((active & XTC_TLS_KTLS_TX) == 0) {
		fprintf(stderr, "kTLS declined by this kernel/OpenSSL: "
		    "user-space path verified only\n");
		return MUNIT_SKIP;
	}
	fprintf(stderr, "kTLS active: tx=%d rx=%d\n",
	    (active & XTC_TLS_KTLS_TX) != 0, (active & XTC_TLS_KTLS_RX) != 0);
	return MUNIT_OK;
}

/* ---- declined: a cipher the kernel cannot do ------------------------ */

static void *
dc_client(void *arg)
{
	struct rt_args *a = arg;
	xtc_tls_opts_t o;
	xtc_tls_ctx_t *ctx = NULL;
	xtc_tls_t *tls = NULL;
	a->rc = -1;
	memset(&o, 0, sizeof o);
	o.size = sizeof o;
	o.verify_peer_mode = XTC_TLS_VERIFY_NONE;
	o.max_version = XTC_TLS_VER_12;
	o.cipher_list = "ECDHE-RSA-AES128-SHA256:AES128-SHA256";
	if (xtc_tls_ctx_create(XTC_TLS_CLIENT, &o, &ctx) != XTC_OK ||
	    xtc_tls_create(ctx, a->fd, &tls) != XTC_OK ||
	    xtc_tls_enable_ktls(tls) != XTC_OK)  /* both sides ask */
		goto out;
	if (drive(tls, a->fd, xtc_tls_handshake) != XTC_OK ||
	    xtc_tls_ktls_active(tls) != 0 ||
	    read_all(tls, a->fd, a->got_msg, RT_MSG) != XTC_OK ||
	    write_all(tls, a->fd, g_up, RT_MSG) != XTC_OK)
		goto out;
	a->rc = 0;
out:
	if (tls != NULL)
		xtc_tls_destroy(tls);
	if (ctx != NULL)
		xtc_tls_ctx_destroy(ctx);
	return NULL;
}

/* An AES-CBC TLS 1.2 session: the kernel's TLS layer has no CBC, so
 * both sides' kTLS requests are declined at the handshake; the
 * connection must work as plain user-space TLS and sendfile must say
 * XTC_E_NOSYS (the caller's cue to read + xtc_tls_write). */
static MunitResult
test_declined(const MunitParameter p[], void *d)
{
	xtc_tls_opts_t o;
	xtc_tls_ctx_t *ctx = NULL;
	xtc_tls_t *tls = NULL;
	struct rt_args a;
	pthread_t th;
	uint8_t *up;
	size_t i, n = 5;
	int sv[2], rc;
	(void)p; (void)d;

	memset(&o, 0, sizeof o);
	o.size = sizeof o;
	o.cert_file = g_cert;
	o.key_file = g_key;
	o.max_version = XTC_TLS_VER_12;
	o.cipher_list = "ECDHE-RSA-AES128-SHA256:AES128-SHA256";
	rc = xtc_tls_ctx_create(XTC_TLS_SERVER, &o, &ctx);
	if (rc == XTC_E_NOSYS)
		return MUNIT_SKIP;
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_int(tcp_pair(sv), ==, 0);
	munit_assert_int(xtc_tls_create(ctx, sv[0], &tls), ==, XTC_OK);
	if (xtc_tls_enable_ktls(tls) == XTC_E_NOSYS) {
		xtc_tls_destroy(tls);
		xtc_tls_ctx_destroy(ctx);
		close(sv[0]);
		close(sv[1]);
		return MUNIT_SKIP;
	}
	up = malloc(RT_MSG);
	a.got_msg = malloc(RT_MSG);
	munit_assert_not_null(up);
	munit_assert_not_null(a.got_msg);
	for (i = 0; i < RT_MSG; i++) {
		g_msg[i] = (uint8_t)(i * 29 + 5);
		g_up[i] = (uint8_t)(i * 11 + 9);
	}
	a.fd = sv[1];
	munit_assert_int(pthread_create(&th, NULL, dc_client, &a), ==, 0);
	munit_assert_int(drive(tls, sv[0], xtc_tls_handshake), ==, XTC_OK);
	munit_assert_int(xtc_tls_ktls_active(tls), ==, 0);
	munit_assert_int(xtc_tls_sendfile(tls, 0, 0, 1, &n), ==, XTC_E_NOSYS);
	munit_assert_size(n, ==, 0);
	munit_assert_int(write_all(tls, sv[0], g_msg, RT_MSG), ==, XTC_OK);
	munit_assert_int(read_all(tls, sv[0], up, RT_MSG), ==, XTC_OK);
	munit_assert_int(pthread_join(th, NULL), ==, 0);
	munit_assert_int(a.rc, ==, 0);
	munit_assert_memory_equal(RT_MSG, a.got_msg, g_msg);
	munit_assert_memory_equal(RT_MSG, up, g_up);
	xtc_tls_destroy(tls);
	xtc_tls_ctx_destroy(ctx);
	xtc_net_close(sv[0]);
	close(sv[1]);
	free(up);
	free(a.got_msg);
	return MUNIT_OK;
}

/* ---- no_sigpipe ----------------------------------------------------- */

static void *
vanish_client(void *arg)
{
	struct rt_args *a = arg;
	xtc_tls_opts_t o;
	xtc_tls_ctx_t *ctx = NULL;
	xtc_tls_t *tls = NULL;
	memset(&o, 0, sizeof o);
	o.size = sizeof o;
	o.verify_peer_mode = XTC_TLS_VERIFY_NONE;
	if (xtc_tls_ctx_create(XTC_TLS_CLIENT, &o, &ctx) == XTC_OK &&
	    xtc_tls_create(ctx, a->fd, &tls) == XTC_OK)
		(void)drive(tls, a->fd, xtc_tls_handshake);
	(void)close(a->fd);                           /* vanish abruptly */
	if (tls != NULL)
		xtc_tls_destroy(tls);
	if (ctx != NULL)
		xtc_tls_ctx_destroy(ctx);
	return NULL;
}

/* Exit 0 = writes failed and no signal killed us; 2 = setup; 3 = a
 * write never failed; 4 = kTLS not active (inconclusive). */
static int
vanish_child(void)
{
	xtc_tls_opts_t o;
	xtc_tls_ctx_t *ctx = NULL;
	xtc_tls_t *tls = NULL;
	struct rt_args a;
	pthread_t th;
	char buf[16384];
	size_t n;
	int sv[2], i, rc = XTC_OK, ffd;

	(void)signal(SIGPIPE, SIG_DFL);           /* an embedder that did nothing */
	memset(&o, 0, sizeof o);
	o.size = sizeof o;
	o.cert_file = g_cert;
	o.key_file = g_key;
	if (xtc_tls_ctx_create(XTC_TLS_SERVER, &o, &ctx) != XTC_OK ||
	    tcp_pair(sv) != 0 || xtc_tls_create(ctx, sv[0], &tls) != XTC_OK ||
	    xtc_tls_enable_ktls(tls) != XTC_OK)
		return 2;
	a.fd = sv[1];
	if (pthread_create(&th, NULL, vanish_client, &a) != 0)
		return 2;
	(void)drive(tls, sv[0], xtc_tls_handshake);
	(void)pthread_join(th, NULL);
	if ((xtc_tls_ktls_active(tls) & XTC_TLS_KTLS_TX) == 0)
		return 4;
	usleep(50 * 1000);                         /* let the RST land */
	memset(buf, 'x', sizeof buf);
	for (i = 0; i < 64; i++) {
		rc = xtc_tls_write(tls, buf, sizeof buf, &n);
		if (rc != XTC_OK && rc != XTC_E_AGAIN)
			break;
		usleep(1000);
	}
	if (rc == XTC_OK || rc == XTC_E_AGAIN)
		return 3;
	/* sendfile path too. */
	ffd = open(g_cert, O_RDONLY);
	if (ffd >= 0) {
		for (i = 0; i < 8; i++)
			if (xtc_tls_sendfile(tls, ffd, 0, 64, &n) != XTC_OK)
				break;
		close(ffd);
	}
	xtc_tls_destroy(tls);
	xtc_tls_ctx_destroy(ctx);
	xtc_net_close(sv[0]);       /* flushes the dead record: no SIGPIPE */
	return 0;
}

static MunitResult
test_no_sigpipe(const MunitParameter p[], void *d)
{
	pid_t pid;
	int st = 0;
	xtc_tls_ctx_t *ctx = NULL;
	xtc_tls_opts_t o;
	(void)p; (void)d;
	memset(&o, 0, sizeof o);
	o.size = sizeof o;
	if (xtc_tls_ctx_create(XTC_TLS_CLIENT, &o, &ctx) != XTC_OK)
		return MUNIT_SKIP;
	xtc_tls_ctx_destroy(ctx);
	pid = fork();
	munit_assert_int(pid, >=, 0);
	if (pid == 0)
		_exit(vanish_child());
	munit_assert_int(waitpid(pid, &st, 0), ==, pid);
	if (WIFSIGNALED(st))
		munit_errorf("kTLS write to a dead peer killed the process "
		    "with signal %d", WTERMSIG(st));
	if (WEXITSTATUS(st) == 2 || WEXITSTATUS(st) == 4)
		return MUNIT_SKIP;                       /* no kTLS here */
	munit_assert_int(WEXITSTATUS(st), ==, 0);
	return MUNIT_OK;
}

static void *
suite_setup(const MunitParameter params[], void *user)
{
	const char *td = getenv("TMPDIR");
	long pid = (long)getpid();
	(void)params; (void)user;
	if (td == NULL || td[0] == '\0')
		td = "/tmp";
	(void)snprintf(g_cert, sizeof g_cert, "%s/xtc-ktls-%ld-cert.pem", td, pid);
	(void)snprintf(g_key, sizeof g_key, "%s/xtc-ktls-%ld-key.pem", td, pid);
	(void)snprintf(g_file, sizeof g_file, "%s/xtc-ktls-%ld-file.bin", td, pid);
	(void)signal(SIGPIPE, SIG_IGN);              /* the runner itself */
	return generate_cert(g_cert, g_key, "localhost") == 0
	    ? (void *)(uintptr_t)1 : NULL;
}

static MunitTest tests[] = {
	{ "/api", test_api, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/roundtrip", test_roundtrip, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/declined", test_declined, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/no_sigpipe", test_no_sigpipe, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = {
	"/m18/tls_ktls", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	int rc;
	(void)suite_setup(NULL, NULL);
	rc = munit_suite_main(&suite, NULL, argc, argv);
	(void)unlink(g_cert);
	(void)unlink(g_key);
	(void)unlink(g_file);
	return rc;
}
