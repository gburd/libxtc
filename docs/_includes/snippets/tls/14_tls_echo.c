/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/tls/14_tls_echo.c -- a TLS server and a TLS client
 * that VERIFIES it, over loopback, as two processes on one loop.
 *
 * libxtc's TLS layer is a non-blocking state machine over a socket you
 * own: xtc_tls_handshake / _read / _write return XTC_E_AGAIN when the
 * socket is not ready, and xtc_tls_wants_read / _wants_write say which
 * way to wait.  Inside a process that wait is xtc_proc_wait_fd, so the
 * TLS code reads top to bottom like blocking code while the loop runs
 * everything else.  tls_wait() below is the whole adapter.
 *
 * Certificates: never commit one (not even a test key).  This program
 * makes a throwaway self-signed certificate for "localhost" with the
 * openssl CLI at startup and deletes it at exit.  The client trusts
 * exactly that certificate (ca_file) and checks the host name, which is
 * what a real client does with a private CA.  If the openssl CLI is not
 * installed, or libxtc was built without TLS, it says so and exits 0.
 *
 * Exits 0 when the client verified the server and got its line echoed.
 *
 * Gate: this file lives in snippets/tls/ because it must link the TLS
 * library the build chose; `make check` builds and runs it from the
 * check-examples target with the build's LIBS.
 */

/* !region full */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "xtc.h"
#include "xtc_io.h"
#include "xtc_loop.h"
#include "xtc_net.h"
#include "xtc_proc.h"
#include "xtc_tls.h"

#define MS (1000LL * 1000)

/* One TLS step returned XTC_E_AGAIN: park until the socket is ready in
 * the direction TLS asked for.  Any other rc is final. */
static int
tls_wait(xtc_tls_t *tls, int fd, int rc)
{
	uint32_t rev;

	if (rc != XTC_E_AGAIN)
		return rc;
	return xtc_proc_wait_fd(fd, xtc_tls_wants_write(tls) ?
	    XTC_IO_WRITABLE : XTC_IO_READABLE, 2000 * MS, &rev);
}

static int
tls_handshake(xtc_tls_t *tls, int fd)
{
	int rc;
	while ((rc = xtc_tls_handshake(tls)) == XTC_E_AGAIN)
		if ((rc = tls_wait(tls, fd, rc)) != XTC_OK)
			return rc;
	return rc;
}

static int
tls_write_all(xtc_tls_t *tls, int fd, const char *p, size_t len)
{
	size_t n;
	int rc;
	while (len > 0) {
		rc = xtc_tls_write(tls, p, len, &n);
		if (rc == XTC_OK) {
			p += n;
			len -= n;
		} else if ((rc = tls_wait(tls, fd, rc)) != XTC_OK)
			return rc;
	}
	return XTC_OK;
}

/* Read one line (up to cap - 1 bytes, NUL-terminated). */
static int
tls_read_line(xtc_tls_t *tls, int fd, char *buf, size_t cap)
{
	size_t got = 0, n;
	int rc;
	while (got == 0 || buf[got - 1] != '\n') {
		if (got == cap - 1)
			break;
		rc = xtc_tls_read(tls, buf + got, cap - 1 - got, &n);
		if (rc == XTC_OK && n == 0)
			return XTC_E_INTERNAL;         /* peer closed early */
		if (rc == XTC_OK)
			got += n;
		else if ((rc = tls_wait(tls, fd, rc)) != XTC_OK)
			return rc;
	}
	buf[got] = '\0';
	return XTC_OK;
}

struct shared {
	xtc_tls_ctx_t *srv_ctx, *cli_ctx;
	int            listen_fd, port;
	int            ok;                     /* client saw its echo */
};

/* Server: accept ONE connection, TLS-handshake, echo one line. */
static void
server(void *arg)
{
	struct shared *sh = arg;
	xtc_tls_t *tls = NULL;
	char       line[128];
	uint32_t   rev;
	int        fd;

	while ((fd = accept(sh->listen_fd, NULL, NULL)) < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			return;
		if (xtc_proc_wait_fd(sh->listen_fd, XTC_IO_READABLE, 2000 * MS,
		    &rev) != XTC_OK)
			return;                        /* no client in 2 s */
	}
	if (xtc_net_setnonblock(fd) == XTC_OK &&
	    xtc_tls_create(sh->srv_ctx, fd, &tls) == XTC_OK &&
	    tls_handshake(tls, fd) == XTC_OK &&
	    tls_read_line(tls, fd, line, sizeof line) == XTC_OK)
		(void)tls_write_all(tls, fd, line, strlen(line));
	if (tls != NULL) {
		(void)xtc_tls_shutdown(tls);           /* best-effort close_notify */
		xtc_tls_destroy(tls);                  /* does not close fd */
	}
	xtc_net_close(fd);
}

/* Client: connect, verify the server is "localhost" signed by our CA,
 * send a line, check the echo. */
static void
client(void *arg)
{
	struct shared *sh = arg;
	const char msg[] = "hello over TLS\n";
	xtc_tls_t *tls = NULL;
	char       line[128];
	uint32_t   rev;
	int        fd, rc;

	if (xtc_net_dial(XTC_NET_INET, "127.0.0.1", sh->port, NULL, &fd)
	    != XTC_OK)
		return;
	if (xtc_proc_wait_fd(fd, XTC_IO_WRITABLE, 2000 * MS, &rev) != XTC_OK)
		goto out;
	if ((rc = xtc_tls_create(sh->cli_ctx, fd, &tls)) != XTC_OK ||
	    (rc = xtc_tls_set_hostname(tls, "localhost")) != XTC_OK ||
	    (rc = tls_handshake(tls, fd)) != XTC_OK) {
		fprintf(stderr, "client handshake: %s\n", xtc_strerror(rc));
		goto out;
	}
	printf("client: verified \"localhost\" (%s)\n",
	    xtc_tls_get_version(tls) ? xtc_tls_get_version(tls) : "?");
	if (tls_write_all(tls, fd, msg, strlen(msg)) == XTC_OK &&
	    tls_read_line(tls, fd, line, sizeof line) == XTC_OK &&
	    strcmp(line, msg) == 0) {
		printf("client: echoed: %s", line);
		sh->ok = 1;
	}
out:
	if (tls != NULL) {
		(void)xtc_tls_shutdown(tls);
		xtc_tls_destroy(tls);
	}
	xtc_net_close(fd);
}
/* !endregion full */

/* !region setup */
/* Throwaway self-signed cert for "localhost" (CN and subjectAltName).
 * CA:TRUE so the client can use the cert itself as its trust anchor. */
static int
make_cert(const char *dir, char *cert, char *key, size_t cap)
{
	char cnf[512], cmd[2048];
	FILE *f;
	int rc;

	snprintf(cert, cap, "%s/cert.pem", dir);
	snprintf(key, cap, "%s/key.pem", dir);
	snprintf(cnf, sizeof cnf, "%s/req.cnf", dir);
	if ((f = fopen(cnf, "w")) == NULL)
		return -1;
	fprintf(f, "[req]\nprompt = no\ndistinguished_name = dn\n"
	    "x509_extensions = v3\n[dn]\nCN = localhost\n"
	    "[v3]\nbasicConstraints = critical,CA:TRUE\n"
	    "subjectAltName = DNS:localhost\n");
	fclose(f);
	snprintf(cmd, sizeof cmd, "openssl req -x509 -newkey rsa:2048 -nodes "
	    "-days 1 -config %s -keyout %s -out %s >/dev/null 2>&1",
	    cnf, key, cert);
	rc = system(cmd);
	(void)unlink(cnf);
	return rc == 0 ? 0 : -1;
}
/* !endregion setup */

int
main(void)
{
	xtc_tls_opts_t sopts, copts;
	xtc_tcp_opts_t topts = XTC_TCP_OPTS_DEFAULT;
	struct shared  sh;
	char           dir[] = "/tmp/xtc-tls-snippet.XXXXXX";
	char           cert[600] = "", key[600] = "";
	xtc_loop_t    *loop = NULL;
	xtc_pid_t      spid, cpid;
	int            rc, status = 1;

	(void)signal(SIGPIPE, SIG_IGN);
	memset(&sh, 0, sizeof sh);
	sh.listen_fd = -1;
	if (mkdtemp(dir) == NULL)
		return 1;
	if (system("openssl version >/dev/null 2>&1") != 0 ||
	    make_cert(dir, cert, key, sizeof cert) != 0) {
		printf("skipped: the openssl CLI is needed to make a test "
		    "certificate\n");
		status = 0;
		goto out;
	}

	/* Server presents the cert; client trusts only it and (by default,
	 * since 1.50) REQUIRES a valid certificate from the server. */
	memset(&sopts, 0, sizeof sopts);
	sopts.cert_file = cert;
	sopts.key_file = key;
	memset(&copts, 0, sizeof copts);
	copts.ca_file = cert;
	copts.min_version = XTC_TLS_VER_12;
	rc = xtc_tls_ctx_create(XTC_TLS_SERVER, &sopts, &sh.srv_ctx);
	if (rc == XTC_E_NOSYS) {
		printf("skipped: libxtc was built without TLS\n");
		status = 0;
		goto out;
	}
	if (rc != XTC_OK ||
	    (rc = xtc_tls_ctx_create(XTC_TLS_CLIENT, &copts, &sh.cli_ctx))
	    != XTC_OK) {
		fprintf(stderr, "xtc_tls_ctx_create: %s\n", xtc_strerror(rc));
		goto out;
	}

	for (sh.port = 47341; sh.port < 47361; sh.port++)
		if (xtc_net_listen(XTC_NET_INET, "127.0.0.1", sh.port, &topts,
		    &sh.listen_fd) == XTC_OK)
			break;
	if (sh.listen_fd < 0 || xtc_loop_init(&loop) != XTC_OK)
		goto out;
	if (xtc_proc_spawn(loop, server, &sh, NULL, &spid) != XTC_OK ||
	    xtc_proc_spawn(loop, client, &sh, NULL, &cpid) != XTC_OK)
		goto out;
	if ((rc = xtc_loop_run(loop)) != XTC_OK)
		fprintf(stderr, "xtc_loop_run: %s\n", xtc_strerror(rc));
	else if (sh.ok)
		status = 0;
out:
	if (loop != NULL)
		(void)xtc_loop_fini(loop);
	if (sh.listen_fd >= 0)
		xtc_net_close(sh.listen_fd);
	xtc_tls_ctx_destroy(sh.cli_ctx);      /* after every connection */
	xtc_tls_ctx_destroy(sh.srv_ctx);
	if (cert[0] != '\0')
		(void)unlink(cert);
	if (key[0] != '\0')
		(void)unlink(key);
	(void)rmdir(dir);
	printf("%s\n", status == 0 ? "ok" : "FAILED");
	return status;
}
