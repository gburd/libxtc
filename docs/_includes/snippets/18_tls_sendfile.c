/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/18_tls_sendfile.c -- serve a file over TLS with
 * kernel TLS: the file's pages go from the page cache to the socket,
 * encrypted by the kernel, without ever entering user space.  When the
 * kernel cannot take the connection (no tls module, a cipher it does not
 * do, a backend without kernel TLS) the same code falls back to
 * pread + xtc_tls_write.
 *
 * The server asks for kernel TLS with xtc_tls_enable_ktls BEFORE the
 * handshake; after it, xtc_tls_ktls_active says whether the kernel took
 * the transmit side, and xtc_tls_sendfile is the zero-copy send.  Close
 * a kernel-TLS socket with xtc_net_close (it keeps a dead peer from
 * raising SIGPIPE during close).
 *
 * The certificate is a throwaway self-signed one made with the openssl
 * CLI and deleted at exit (never commit a key).  No openssl CLI, or
 * libxtc built without TLS: says so and exits 0.  Plain threads and
 * poll(2) keep the example short; inside a libxtc process use
 * xtc_proc_wait_fd as in the TLS echo example.
 */

/* !region full */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_net.h"
#include "xtc_tls.h"

#define FILE_SZ (1024 * 1024)

static int g_cli_fd;
static size_t g_got;

static void
wait_io(xtc_tls_t *t, int fd)
{
	struct pollfd p = { fd, xtc_tls_wants_write(t) ? POLLOUT : POLLIN, 0 };
	(void)poll(&p, 1, 5000);
}

/* A plain user-space TLS client that reads everything. */
static void *
client(void *arg)
{
	xtc_tls_opts_t o = XTC_TLS_OPTS_INIT;
	xtc_tls_ctx_t *ctx = NULL;
	xtc_tls_t *t = NULL;
	char buf[65536];
	int rc;
	(void)arg;
	o.verify_peer_mode = XTC_TLS_VERIFY_NONE;   /* self-signed demo cert */
	if (xtc_tls_ctx_create(XTC_TLS_CLIENT, &o, &ctx) != XTC_OK ||
	    xtc_tls_create(ctx, g_cli_fd, &t) != XTC_OK)
		return NULL;
	while ((rc = xtc_tls_handshake(t)) == XTC_E_AGAIN)
		wait_io(t, g_cli_fd);
	while (rc == XTC_OK && g_got < FILE_SZ) {
		size_t n = 0;
		rc = xtc_tls_read(t, buf, sizeof buf, &n);
		if (rc == XTC_OK) {
			g_got += n;
		} else if (rc == XTC_E_AGAIN) {
			wait_io(t, g_cli_fd);
			rc = XTC_OK;
		}
	}
	xtc_tls_destroy(t);
	xtc_tls_ctx_destroy(ctx);
	return NULL;
}

/* Send len bytes of file fd over tls: zero copy when the kernel holds
 * the transmit side, read + write otherwise. */
static int
serve_file(xtc_tls_t *tls, int sock, int fd, size_t len)
{
	char buf[16384];
	size_t off = 0;
	while (off < len) {
		size_t n = 0;
		int rc = xtc_tls_sendfile(tls, fd, (int64_t)off, len - off, &n);
		if (rc == XTC_E_NOSYS) {             /* no kernel TX: fall back */
			ssize_t r = pread(fd, buf, sizeof buf, (off_t)off);
			if (r <= 0)
				return XTC_E_IO;
			rc = xtc_tls_write(tls, buf, (size_t)r, &n);
		}
		if (rc == XTC_OK)
			off += n;
		else if (rc == XTC_E_AGAIN)
			wait_io(tls, sock);
		else
			return rc;
	}
	return XTC_OK;
}

int
main(void)
{
	char cert[] = "/tmp/xtc-snip18-cert-XXXXXX", key[64], path[64];
	xtc_tls_opts_t o = XTC_TLS_OPTS_INIT;
	xtc_tls_ctx_t *ctx = NULL;
	xtc_tls_t *tls = NULL;
	struct sockaddr_in sa;
	socklen_t sl = sizeof sa;
	pthread_t th;
	char cmd[512], *data;
	int lfd, sv0, fd, rc, kt;

	if (mkstemp(cert) < 0)
		return 1;
	(void)snprintf(key, sizeof key, "%s.key", cert);
	(void)snprintf(path, sizeof path, "%s.dat", cert);
	(void)snprintf(cmd, sizeof cmd, "openssl req -x509 -newkey rsa:2048 "
	    "-nodes -days 1 -subj /CN=localhost -keyout %s -out %s "
	    ">/dev/null 2>&1", key, cert);
	o.cert_file = cert;
	o.key_file = key;
	if (system(cmd) != 0 ||
	    xtc_tls_ctx_create(XTC_TLS_SERVER, &o, &ctx) != XTC_OK) {
		printf("no openssl CLI or no TLS in this build: skipped\n");
		(void)unlink(cert);
		(void)unlink(key);
		return 0;
	}

	/* The file to serve. */
	data = malloc(FILE_SZ);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (data == NULL || fd < 0)
		return 1;
	memset(data, 'k', FILE_SZ);
	if (write(fd, data, FILE_SZ) != FILE_SZ)
		return 1;

	/* A loopback TCP connection (kernel TLS needs TCP). */
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	lfd = socket(AF_INET, SOCK_STREAM, 0);
	g_cli_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) != 0 ||
	    listen(lfd, 1) != 0 ||
	    getsockname(lfd, (struct sockaddr *)&sa, &sl) != 0 ||
	    connect(g_cli_fd, (struct sockaddr *)&sa, sizeof sa) != 0 ||
	    (sv0 = accept(lfd, NULL, NULL)) < 0)
		return 1;
	close(lfd);
	(void)xtc_net_setnonblock(sv0);
	(void)xtc_net_setnonblock(g_cli_fd);

	/* Server: ask for kernel TLS, then handshake. */
	if (xtc_tls_create(ctx, sv0, &tls) != XTC_OK)
		return 1;
	rc = xtc_tls_enable_ktls(tls);           /* XTC_E_NOSYS: not here */
	(void)rc;
	if (pthread_create(&th, NULL, client, NULL) != 0)
		return 1;
	while ((rc = xtc_tls_handshake(tls)) == XTC_E_AGAIN)
		wait_io(tls, sv0);
	kt = xtc_tls_ktls_active(tls) & XTC_TLS_KTLS_TX;
	if (rc == XTC_OK)
		rc = serve_file(tls, sv0, fd, FILE_SZ);
	(void)pthread_join(th, NULL);

	printf("served %zu bytes %s\n", g_got, kt ?
	    "with xtc_tls_sendfile (kernel TLS, zero copy)" :
	    "with pread + xtc_tls_write (user-space TLS)");
	xtc_tls_destroy(tls);
	xtc_tls_ctx_destroy(ctx);
	xtc_net_close(sv0);                      /* not close(2): see above */
	close(g_cli_fd);
	close(fd);
	free(data);
	(void)unlink(path);
	(void)unlink(cert);
	(void)unlink(key);
	return rc == XTC_OK && g_got == FILE_SZ ? 0 : 1;
}
/* !endregion full */
