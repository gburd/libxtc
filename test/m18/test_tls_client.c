/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/m18/test_tls_client.c
 *	Client-side handshake smoke test (TLS-3).
 *
 *	Exercises the xtc_tls CLIENT path end-to-end:
 *	  - xtc_tls_ctx_create (CLIENT role, optional CA + verify_peer)
 *	  - xtc_tls_create / xtc_tls_handshake (non-blocking, polled)
 *	  - xtc_tls_write / xtc_tls_read
 *	  - xtc_tls_shutdown / xtc_tls_destroy
 *
 *	The peer (server) side of the loopback connection is driven by the
 *	SAME xtc_tls API, in SERVER role, inside a pthread.  Using xtc_tls
 *	for both halves keeps the test backend-agnostic: whatever TLS
 *	backend configure selected is what is exercised on both sides.
 *	Both fds are non-blocking and driven by poll(2).
 *
 *	A self-signed RSA-2048 certificate and matching private key are
 *	generated at runtime via the openssl CLI and written to /tmp files
 *	during test setup.  This avoids embedding private keys in source
 *	(which triggers GitHub secret scanners).
 *
 *	A second self-signed certificate (different CN, different key)
 *	is generated as WRONG_CA to exercise the verify_peer reject path.
 *
 *	When no TLS backend is compiled in (--with-tls=none) every test in
 *	this suite returns MUNIT_SKIP cleanly.
 */

#include "munit.h"
#include "xtc_int.h"
#include "xtc_tls.h"

/* =========================================================================
 * TLS-enabled branch -- full implementation (any backend).
 * ======================================================================= */
#if defined(XTC_TLS_ENABLED)

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * Per-process certificate paths.  They were fixed "/tmp/xtc-tls3-*.pem"
 * names, so two m18 runs at once (a parallel make check, CI matrix jobs on
 * one runner, or the several TLS backends qualified side by side) rewrote
 * each other's certificates mid-test: a handshake then failed against a
 * key that no longer matched (seen as rc -6 / -1 on 3 backends run
 * concurrently; each passed alone).  $TMPDIR is honored, and the pid
 * keeps concurrent runs apart.
 */
static char TEST_CERT_PATH_BUF[256];
#define TEST_CERT_PATH ((const char *)TEST_CERT_PATH_BUF)
static char TEST_KEY_PATH_BUF[256];
#define TEST_KEY_PATH ((const char *)TEST_KEY_PATH_BUF)
static char WRONG_CA_PATH_BUF[256];
#define WRONG_CA_PATH ((const char *)WRONG_CA_PATH_BUF)
static char NAME_CERT_PATH_BUF[256];
#define NAME_CERT_PATH ((const char *)NAME_CERT_PATH_BUF)
static char NAME_KEY_PATH_BUF[256];
#define NAME_KEY_PATH ((const char *)NAME_KEY_PATH_BUF)

static void
__m18_paths_init(void)
{
	const char *td = getenv("TMPDIR");
	long pid = (long)getpid();
	if (td == NULL || td[0] == '\0')
		td = "/tmp";
	(void)snprintf(TEST_CERT_PATH_BUF, sizeof TEST_CERT_PATH_BUF, "%s/xtc-tls3-%ld-test-cert.pem", td, pid);
	(void)snprintf(TEST_KEY_PATH_BUF, sizeof TEST_KEY_PATH_BUF, "%s/xtc-tls3-%ld-test-key.pem", td, pid);
	(void)snprintf(WRONG_CA_PATH_BUF, sizeof WRONG_CA_PATH_BUF, "%s/xtc-tls3-%ld-wrong-ca.pem", td, pid);
	(void)snprintf(NAME_CERT_PATH_BUF, sizeof NAME_CERT_PATH_BUF, "%s/xtc-tls3-%ld-name-cert.pem", td, pid);
	(void)snprintf(NAME_KEY_PATH_BUF, sizeof NAME_KEY_PATH_BUF, "%s/xtc-tls3-%ld-name-key.pem", td, pid);
}

/* -------------------------------------------------------------------------
 * Runtime certificate generation.
 *
 * TEST_CERT + TEST_KEY: the server presents this; the client verifies
 * against it (good-CA case).
 *
 * WRONG_CA: a different self-signed cert (different CN, different key)
 * used to trigger a peer-verification failure.
 * ----------------------------------------------------------------------- */

/* Server cert issued for NAME_CERT (CN and SAN), for the name check. */
#define NAME_CERT        "a.example"
#define NAME_OTHER       "b.example"

/* Generate a self-signed RSA-2048 cert+key via the openssl CLI.
 * Returns 0 on success. */
static int
generate_cert(const char *cert_path, const char *key_path, const char *cn)
{
    char cmd[1024];
    char cnf_path[256];
    FILE *cnf_fp;
    snprintf(cnf_path, sizeof(cnf_path), "%s.cnf", cert_path);
    cnf_fp = fopen(cnf_path, "w");
    if (cnf_fp != NULL) {
        fprintf(cnf_fp,
            "[req]\nprompt = no\ndistinguished_name = dn\n"
            "[dn]\nCN = %s\n", cn);
        fclose(cnf_fp);
    }
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey rsa:2048 -nodes -days 1 "
             "-config %s -keyout %s -out %s -subj /CN=%s 2>/dev/null",
             cnf_path, key_path, cert_path, cn);
    int rc = system(cmd);
    (void)unlink(cnf_path);
    return rc;
}

/* Generate a self-signed cert+key whose CN AND subjectAltName DNS entry
 * are both `name' (RFC 6125 matchers look at the SAN; some fall back to
 * the CN -- carrying both means every backend has the name to match).
 * CA:TRUE so the cert can be its own trust anchor via ca_file. */
static int
generate_name_cert(const char *cert_path, const char *key_path,
                   const char *name)
{
    char cmd[1024];
    char cnf_path[256];
    FILE *cnf_fp;
    int rc;
    snprintf(cnf_path, sizeof(cnf_path), "%s.cnf", cert_path);
    cnf_fp = fopen(cnf_path, "w");
    if (cnf_fp == NULL)
        return -1;
    fprintf(cnf_fp,
        "[req]\nprompt = no\ndistinguished_name = dn\n"
        "x509_extensions = v3\n"
        "[dn]\nCN = %s\n"
        "[v3]\nbasicConstraints = critical,CA:TRUE\n"
        "subjectKeyIdentifier = hash\n"
        "subjectAltName = DNS:%s\n", name, name);
    fclose(cnf_fp);
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey rsa:2048 -nodes -days 1 "
             "-config %s -keyout %s -out %s 2>/dev/null",
             cnf_path, key_path, cert_path);
    rc = system(cmd);
    (void)unlink(cnf_path);
    return rc;
}

/* Generate a self-signed cert (key discarded) for use as a wrong CA. */
static int
generate_wrong_ca(const char *cert_path, const char *cn)
{
    char cmd[1024];
    char cnf_path[256];
    FILE *cnf_fp;
    snprintf(cnf_path, sizeof(cnf_path), "%s.cnf", cert_path);
    cnf_fp = fopen(cnf_path, "w");
    if (cnf_fp != NULL) {
        fprintf(cnf_fp,
            "[req]\nprompt = no\ndistinguished_name = dn\n"
            "[dn]\nCN = %s\n", cn);
        fclose(cnf_fp);
    }
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey rsa:2048 -nodes -days 1 "
             "-config %s -keyout /dev/null -out %s -subj /CN=%s 2>/dev/null",
             cnf_path, cert_path, cn);
    int rc = system(cmd);
    (void)unlink(cnf_path);
    return rc;
}

/* -------------------------------------------------------------------------
 * Helpers -- drive xtc_tls operations to completion over a poll(2) loop.
 * ----------------------------------------------------------------------- */

static int
poll_until_done(xtc_tls_t *tls, int fd,
                int (*fn)(xtc_tls_t *), int timeout_ms)
{
    for (;;) {
        int rc = fn(tls);
        if (rc != XTC_E_AGAIN)
            return rc;

        struct pollfd pfd;
        pfd.fd      = fd;
        pfd.events  = xtc_tls_wants_write(tls) ? POLLOUT : POLLIN;
        pfd.revents = 0;

        if (poll(&pfd, 1, timeout_ms) <= 0)
            return XTC_E_INTERNAL;
    }
}

static int
tls_write_all(xtc_tls_t *tls, int fd,
              const void *buf, size_t len, int timeout_ms)
{
    size_t total = 0;
    while (total < len) {
        size_t n = 0;
        int rc = xtc_tls_write(tls, (const char *)buf + total,
                               len - total, &n);
        if (rc == XTC_OK) {
            total += n;
        } else if (rc == XTC_E_AGAIN) {
            struct pollfd pfd;
            pfd.fd      = fd;
            pfd.events  = xtc_tls_wants_write(tls) ? POLLOUT : POLLIN;
            pfd.revents = 0;
            if (poll(&pfd, 1, timeout_ms) <= 0)
                return XTC_E_INTERNAL;
        } else {
            return rc;
        }
    }
    return XTC_OK;
}

static int
tls_read_exact(xtc_tls_t *tls, int fd,
               void *buf, size_t len, int timeout_ms)
{
    size_t total = 0;
    while (total < len) {
        size_t n = 0;
        int rc = xtc_tls_read(tls, (char *)buf + total,
                              len - total, &n);
        if (rc == XTC_OK) {
            if (n == 0)
                return XTC_E_INTERNAL;  /* unexpected EOF */
            total += n;
        } else if (rc == XTC_E_AGAIN) {
            struct pollfd pfd;
            pfd.fd      = fd;
            pfd.events  = xtc_tls_wants_write(tls) ? POLLOUT : POLLIN;
            pfd.revents = 0;
            if (poll(&pfd, 1, timeout_ms) <= 0)
                return XTC_E_INTERNAL;
        } else {
            return rc;
        }
    }
    return XTC_OK;
}

static int
set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* -------------------------------------------------------------------------
 * Server thread: accepts via xtc_tls (SERVER role) on the peer fd.
 *
 * Protocol (normal case):
 *   1. handshake
 *   2. Read exactly SERVER_MSG_LEN bytes from the client.
 *   3. Echo them back.
 *   4. shutdown.
 *
 * For the bad-CA case the client aborts during the handshake; the
 * server's handshake then fails.  The thread sets rc=2 and exits
 * cleanly.
 * ----------------------------------------------------------------------- */

#define SERVER_MSG_LEN 5

struct server_args {
    int  fd;   /* non-blocking socketpair fd; owned by the calling test */
    const char *cert;   /* NULL = TEST_CERT_PATH */
    const char *key;    /* NULL = TEST_KEY_PATH */
    int  rc;   /* 0 = full success; 2 = handshake failed (bad-CA ok);
                * other non-zero = unexpected error */
};

static void *
server_thread(void *arg)
{
    struct server_args *a   = (struct server_args *)arg;
    xtc_tls_opts_t      opts;
    xtc_tls_ctx_t      *ctx = NULL;
    xtc_tls_t          *tls = NULL;
    char                buf[SERVER_MSG_LEN];

    a->rc = 1;   /* assume unexpected failure */

    memset(&opts, 0, sizeof(opts));
    opts.cert_file   = a->cert != NULL ? a->cert : TEST_CERT_PATH;
    opts.key_file    = a->key  != NULL ? a->key  : TEST_KEY_PATH;
    opts.min_version = XTC_TLS_VER_12;
    opts.verify_peer = 0;

    if (xtc_tls_ctx_create(XTC_TLS_SERVER, &opts, &ctx) != XTC_OK)
        goto done;
    if (xtc_tls_create(ctx, a->fd, &tls) != XTC_OK)
        goto done;

    if (poll_until_done(tls, a->fd, xtc_tls_handshake, 5000) != XTC_OK) {
        a->rc = 2;   /* handshake failed; caller decides if this is ok */
        goto done;
    }

    if (tls_read_exact(tls, a->fd, buf, SERVER_MSG_LEN, 5000) != XTC_OK)
        goto done;
    if (tls_write_all(tls, a->fd, buf, SERVER_MSG_LEN, 5000) != XTC_OK)
        goto done;

    (void)xtc_tls_shutdown(tls);
    a->rc = 0;

done:
    if (tls != NULL) xtc_tls_destroy(tls);
    if (ctx != NULL) xtc_tls_ctx_destroy(ctx);
    return arg;
}

/* =========================================================================
 * Tests.
 * ======================================================================= */

/* -------------------------------------------------------------------------
 * test_client_handshake_roundtrip:
 *   Full loopback, no peer verification (explicit XTC_TLS_VERIFY_NONE:
 *   since 1.50 a zeroed opts verifies the server).  Client side under test.
 * ----------------------------------------------------------------------- */
static MunitResult
test_client_handshake_roundtrip(const MunitParameter params[], void *data)
{
    xtc_tls_opts_t  opts;
    xtc_tls_ctx_t  *ctx   = NULL;
    xtc_tls_t      *tls   = NULL;
    struct server_args sa;
    pthread_t       tid;
    int             sv[2];
    int             rc;
    char            rbuf[SERVER_MSG_LEN + 1];

    (void)params;
    (void)data;

    memset(&opts, 0, sizeof(opts));
    opts.verify_peer_mode = XTC_TLS_VERIFY_NONE;
    opts.min_version = XTC_TLS_VER_12;

    rc = xtc_tls_ctx_create(XTC_TLS_CLIENT, &opts, &ctx);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_ptr_not_null(ctx);

    munit_assert_int(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    munit_assert_int(set_nonblock(sv[0]), ==, 0);
    munit_assert_int(set_nonblock(sv[1]), ==, 0);

    rc = xtc_tls_create(ctx, sv[1], &tls);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_ptr_not_null(tls);

    memset(&sa, 0, sizeof(sa));
    sa.fd = sv[0];
    sa.rc = 1;
    munit_assert_int(pthread_create(&tid, NULL, server_thread, &sa), ==, 0);

    rc = poll_until_done(tls, sv[1], xtc_tls_handshake, 5000);
    munit_assert_int(rc, ==, XTC_OK);

    rc = tls_write_all(tls, sv[1], "hello", SERVER_MSG_LEN, 5000);
    munit_assert_int(rc, ==, XTC_OK);

    memset(rbuf, 0, sizeof(rbuf));
    rc = tls_read_exact(tls, sv[1], rbuf, SERVER_MSG_LEN, 5000);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_memory_equal(SERVER_MSG_LEN, rbuf, "hello");

    (void)xtc_tls_shutdown(tls);

    pthread_join(tid, NULL);
    munit_assert_int(sa.rc, ==, 0);

    xtc_tls_destroy(tls);
    xtc_tls_ctx_destroy(ctx);
    close(sv[0]);
    close(sv[1]);

    return MUNIT_OK;
}

/* -------------------------------------------------------------------------
 * test_client_verify_peer:
 *
 *   Sub-case A -- Good CA:
 *     Client uses ca_file = TEST_CERT_PATH + verify_peer = 1.
 *     The server presents TEST_CERT (self-signed; it IS its own CA).
 *     Handshake must succeed; round-trip verifies data flow.
 *
 *   Sub-case B -- Bad CA:
 *     Client uses ca_file = WRONG_CA_PATH + verify_peer = 1.
 *     WRONG_CA does not sign TEST_CERT; the client must reject the
 *     server certificate.  xtc_tls_handshake must return XTC_E_INTERNAL.
 *     The server thread's handshake also fails (rc != 0); expected.
 * ----------------------------------------------------------------------- */
static MunitResult
test_client_verify_peer(const MunitParameter params[], void *data)
{
    xtc_tls_opts_t  opts;
    xtc_tls_ctx_t  *ctx   = NULL;
    xtc_tls_t      *tls   = NULL;
    struct server_args sa;
    pthread_t       tid;
    int             sv[2];
    int             rc;
    char            rbuf[SERVER_MSG_LEN + 1];

    (void)params;
    (void)data;

    /* ==================================================================
     * Sub-case A: good CA -- handshake succeeds and data flows.
     * ================================================================== */

    memset(&opts, 0, sizeof(opts));
    opts.ca_file     = TEST_CERT_PATH;
    opts.verify_peer = 1;
    opts.min_version = XTC_TLS_VER_12;

    rc = xtc_tls_ctx_create(XTC_TLS_CLIENT, &opts, &ctx);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_ptr_not_null(ctx);

    munit_assert_int(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    munit_assert_int(set_nonblock(sv[0]), ==, 0);
    munit_assert_int(set_nonblock(sv[1]), ==, 0);

    rc = xtc_tls_create(ctx, sv[1], &tls);
    munit_assert_int(rc, ==, XTC_OK);

    memset(&sa, 0, sizeof(sa));
    sa.fd = sv[0];
    sa.rc = 1;
    munit_assert_int(pthread_create(&tid, NULL, server_thread, &sa), ==, 0);

    rc = poll_until_done(tls, sv[1], xtc_tls_handshake, 5000);
    munit_assert_int(rc, ==, XTC_OK);

    rc = tls_write_all(tls, sv[1], "hello", SERVER_MSG_LEN, 5000);
    munit_assert_int(rc, ==, XTC_OK);

    memset(rbuf, 0, sizeof(rbuf));
    rc = tls_read_exact(tls, sv[1], rbuf, SERVER_MSG_LEN, 5000);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_memory_equal(SERVER_MSG_LEN, rbuf, "hello");

    (void)xtc_tls_shutdown(tls);
    pthread_join(tid, NULL);
    munit_assert_int(sa.rc, ==, 0);   /* server must have completed ok */

    xtc_tls_destroy(tls);
    xtc_tls_ctx_destroy(ctx);
    close(sv[0]);
    close(sv[1]);

    /* ==================================================================
     * Sub-case B: bad CA -- handshake must fail with XTC_E_INTERNAL.
     * ================================================================== */

    memset(&opts, 0, sizeof(opts));
    opts.ca_file     = WRONG_CA_PATH;
    opts.verify_peer = 1;
    opts.min_version = XTC_TLS_VER_12;

    rc = xtc_tls_ctx_create(XTC_TLS_CLIENT, &opts, &ctx);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_ptr_not_null(ctx);

    munit_assert_int(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    munit_assert_int(set_nonblock(sv[0]), ==, 0);
    munit_assert_int(set_nonblock(sv[1]), ==, 0);

    rc = xtc_tls_create(ctx, sv[1], &tls);
    munit_assert_int(rc, ==, XTC_OK);

    memset(&sa, 0, sizeof(sa));
    sa.fd = sv[0];
    sa.rc = 1;
    munit_assert_int(pthread_create(&tid, NULL, server_thread, &sa), ==, 0);

    /* Handshake must fail: certificate verify failed. */
    rc = poll_until_done(tls, sv[1], xtc_tls_handshake, 5000);
    munit_assert_int(rc, ==, XTC_E_INTERNAL);

    /* The server's handshake also did not complete normally. */
    pthread_join(tid, NULL);
    munit_assert_int(sa.rc, !=, 0);

    xtc_tls_destroy(tls);
    xtc_tls_ctx_destroy(ctx);
    close(sv[0]);
    close(sv[1]);

    return MUNIT_OK;
}

/* -------------------------------------------------------------------------
 * One client connection under `copts' to a server presenting cert/key
 * (NULL = TEST_CERT).  When `expect' is non-NULL the client names the
 * peer with xtc_tls_set_hostname (after first setting `prior', if
 * non-NULL, so a replace/clear can be tested).  Returns the client's
 * handshake rc; *host_rc gets the last xtc_tls_set_hostname rc and
 * *srv_rc the server thread's rc.  On success the echo round trip must
 * also work.
 * ----------------------------------------------------------------------- */
static int
connect_opts(const xtc_tls_opts_t *copts, const char *cert, const char *key,
             const char *prior, const char *expect,
             int *host_rc, int *srv_rc)
{
    xtc_tls_ctx_t  *ctx = NULL;
    xtc_tls_t      *tls = NULL;
    struct server_args sa;
    pthread_t       tid;
    int             sv[2];
    int             rc;
    char            rbuf[SERVER_MSG_LEN + 1];

    munit_assert_int(xtc_tls_ctx_create(XTC_TLS_CLIENT, copts, &ctx),
                     ==, XTC_OK);

    munit_assert_int(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    munit_assert_int(set_nonblock(sv[0]), ==, 0);
    munit_assert_int(set_nonblock(sv[1]), ==, 0);
    munit_assert_int(xtc_tls_create(ctx, sv[1], &tls), ==, XTC_OK);

    *host_rc = XTC_OK;
    if (prior != NULL)
        munit_assert_int(xtc_tls_set_hostname(tls, prior), ==, XTC_OK);
    if (expect != NULL)
        *host_rc = xtc_tls_set_hostname(tls, expect);

    memset(&sa, 0, sizeof(sa));
    sa.fd   = sv[0];
    sa.rc   = 1;
    sa.cert = cert;
    sa.key  = key;
    munit_assert_int(pthread_create(&tid, NULL, server_thread, &sa), ==, 0);

    rc = poll_until_done(tls, sv[1], xtc_tls_handshake, 5000);
    if (rc == XTC_OK) {
        munit_assert_int(tls_write_all(tls, sv[1], "hello",
                         SERVER_MSG_LEN, 5000), ==, XTC_OK);
        memset(rbuf, 0, sizeof(rbuf));
        munit_assert_int(tls_read_exact(tls, sv[1], rbuf,
                         SERVER_MSG_LEN, 5000), ==, XTC_OK);
        munit_assert_memory_equal(SERVER_MSG_LEN, rbuf, "hello");
        (void)xtc_tls_shutdown(tls);
    }
    pthread_join(tid, NULL);
    *srv_rc = sa.rc;

    xtc_tls_destroy(tls);
    xtc_tls_ctx_destroy(ctx);
    close(sv[0]);
    close(sv[1]);
    return rc;
}

/* A client trusting NAME_CERT (verification ON) against a server that
 * presents it, expecting the peer to be `expect'. */
static int
connect_expecting(const char *prior, const char *expect,
                  int *host_rc, int *srv_rc)
{
    xtc_tls_opts_t opts;

    memset(&opts, 0, sizeof(opts));
    opts.ca_file     = NAME_CERT_PATH;
    opts.verify_peer = 1;
    opts.min_version = XTC_TLS_VER_12;
    return connect_opts(&opts, NAME_CERT_PATH, NAME_KEY_PATH, prior, expect,
                        host_rc, srv_rc);
}

/* -------------------------------------------------------------------------
 * test_client_hostname_check (PLAN 19.27.7):
 *   The server's certificate is valid and TRUSTED, but issued for
 *   NAME_CERT.  A client that expects NAME_OTHER must REJECT it; the same
 *   client expecting NAME_CERT must accept it (the positive control that
 *   keeps the negative case from passing vacuously, e.g. on a handshake
 *   that fails for some unrelated reason).  This is a correctness
 *   requirement on EVERY backend: xtc_tls_set_hostname returning
 *   XTC_E_NOSYS is a FAILURE here, never a skip -- a skip is exactly how
 *   the missing GnuTLS / wolfSSL / mbedTLS name check stayed hidden.
 * ----------------------------------------------------------------------- */
static MunitResult
test_client_hostname_check(const MunitParameter params[], void *data)
{
    int rc, host_rc, srv_rc;

    (void)params;
    (void)data;

    /* Positive control: the right name, trusted chain -> success. */
    rc = connect_expecting(NULL, NAME_CERT, &host_rc, &srv_rc);
    munit_assert_int(host_rc, ==, XTC_OK);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_int(srv_rc, ==, 0);

    /* The defect: a trusted cert for the WRONG name must be rejected. */
    rc = connect_expecting(NULL, NAME_OTHER, &host_rc, &srv_rc);
    munit_assert_int(rc, ==, XTC_E_INTERNAL);   /* handshake REJECTED */
    munit_assert_int(host_rc, ==, XTC_OK);      /* NOSYS is a failure */
    munit_assert_int(srv_rc, !=, 0);

    /* Clearing the name falls back to chain-only verification. */
    rc = connect_expecting(NULL, "", &host_rc, &srv_rc);
    munit_assert_int(host_rc, ==, XTC_OK);
    munit_assert_int(rc, ==, XTC_OK);

    /* ... and clearing must UNDO an earlier wrong name, not just stop
     * sending SNI (the OpenSSL clear path left SSL_set1_host armed
     * before 1.50, so this handshake still failed the name check). */
    rc = connect_expecting(NAME_OTHER, "", &host_rc, &srv_rc);
    munit_assert_int(host_rc, ==, XTC_OK);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_int(srv_rc, ==, 0);
    return MUNIT_OK;
}

/* -------------------------------------------------------------------------
 * test_client_default_verifies (PLAN 19.27.8):
 *   Since 1.50 a CLIENT whose opts set no verify field at all verifies
 *   the server: before, a zeroed opts resolved to XTC_TLS_VERIFY_NONE and
 *   accepted ANY certificate.  The server here presents a self-signed
 *   cert the client does not trust, so the handshake must FAIL -- with
 *   ca_file NULL (the platform trust store, which does not hold it) and
 *   with a ca_file that did not issue it.  The same client with an
 *   explicit XTC_TLS_VERIFY_NONE must still succeed (the documented
 *   opt-out), and with ca_file = the server cert it must succeed (the
 *   positive control: the rejections are about trust, nothing else).
 *
 *   mbedTLS has no platform trust store: a verifying client without
 *   ca_file is refused at ctx_create with XTC_E_INVAL instead.
 * ----------------------------------------------------------------------- */
static MunitResult
test_client_default_verifies(const MunitParameter params[], void *data)
{
    xtc_tls_opts_t  opts;
#if defined(XTC_TLS_BACKEND_MBEDTLS)
    xtc_tls_ctx_t  *ctx = NULL;
#endif
    int             rc, host_rc, srv_rc;

    (void)params;
    (void)data;

    /* Zeroed opts, ca_file NULL: the untrusted server is REJECTED. */
    memset(&opts, 0, sizeof(opts));
#if defined(XTC_TLS_BACKEND_MBEDTLS)
    munit_assert_int(xtc_tls_ctx_create(XTC_TLS_CLIENT, &opts, &ctx),
                     ==, XTC_E_INVAL);
    munit_assert_ptr_null(ctx);
    munit_assert_int(xtc_tls_ctx_create(XTC_TLS_CLIENT, NULL, &ctx),
                     ==, XTC_E_INVAL);
    munit_assert_ptr_null(ctx);
#else
    rc = connect_opts(&opts, NULL, NULL, NULL, NULL, &host_rc, &srv_rc);
    munit_assert_int(rc, ==, XTC_E_INTERNAL);
    munit_assert_int(srv_rc, !=, 0);
#endif

    /* Zeroed verify fields, a ca_file that did not issue the server
     * cert: REJECTED (on every backend, mbedTLS included). */
    memset(&opts, 0, sizeof(opts));
    opts.ca_file = WRONG_CA_PATH;
    rc = connect_opts(&opts, NULL, NULL, NULL, NULL, &host_rc, &srv_rc);
    munit_assert_int(rc, ==, XTC_E_INTERNAL);
    munit_assert_int(srv_rc, !=, 0);

    /* Positive control: zeroed verify fields, trusted ca_file -> OK. */
    memset(&opts, 0, sizeof(opts));
    opts.ca_file = TEST_CERT_PATH;
    rc = connect_opts(&opts, NULL, NULL, NULL, NULL, &host_rc, &srv_rc);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_int(srv_rc, ==, 0);

    /* The opt-out: explicit NONE accepts the untrusted server, and wins
     * over the legacy verify_peer int. */
    memset(&opts, 0, sizeof(opts));
    opts.verify_peer_mode = XTC_TLS_VERIFY_NONE;
    rc = connect_opts(&opts, NULL, NULL, NULL, NULL, &host_rc, &srv_rc);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_int(srv_rc, ==, 0);
    opts.verify_peer = 1;
    opts.ca_file     = WRONG_CA_PATH;
    rc = connect_opts(&opts, NULL, NULL, NULL, NULL, &host_rc, &srv_rc);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_int(srv_rc, ==, 0);

    /* Explicit REQUIRE is honored too (before 1.50 GnuTLS, wolfSSL and
     * mbedTLS ignored verify_peer_mode and read only verify_peer). */
    memset(&opts, 0, sizeof(opts));
    opts.verify_peer_mode = XTC_TLS_VERIFY_REQUIRE;
    opts.ca_file          = WRONG_CA_PATH;
    rc = connect_opts(&opts, NULL, NULL, NULL, NULL, &host_rc, &srv_rc);
    munit_assert_int(rc, ==, XTC_E_INTERNAL);
    munit_assert_int(srv_rc, !=, 0);
    return MUNIT_OK;
}

/* =========================================================================
 * Suite setup / teardown -- write/remove temp PEM files.
 * ======================================================================= */

static void *
suite_setup(const MunitParameter params[], void *user_data)
{
    (void)params;
    (void)user_data;

    if (generate_cert(TEST_CERT_PATH, TEST_KEY_PATH, "localhost") != 0)
        return NULL;
    if (generate_wrong_ca(WRONG_CA_PATH, "wrong-ca") != 0) {
        unlink(TEST_CERT_PATH);
        unlink(TEST_KEY_PATH);
        return NULL;
    }
    if (generate_name_cert(NAME_CERT_PATH, NAME_KEY_PATH, NAME_CERT) != 0) {
        unlink(TEST_CERT_PATH);
        unlink(TEST_KEY_PATH);
        unlink(WRONG_CA_PATH);
        return NULL;
    }
    return (void *)(uintptr_t)1;   /* non-NULL: setup succeeded */
}

static void
suite_teardown(void *fixture)
{
    (void)fixture;
    unlink(TEST_CERT_PATH);
    unlink(TEST_KEY_PATH);
    unlink(WRONG_CA_PATH);
    unlink(NAME_CERT_PATH);
    unlink(NAME_KEY_PATH);
}

/* =========================================================================
 * Test array and suite.
 * ======================================================================= */

static MunitTest tests[] = {
    { "/handshake_roundtrip", test_client_handshake_roundtrip,
      suite_setup, suite_teardown, MUNIT_TEST_OPTION_NONE, NULL },
    { "/verify_peer",         test_client_verify_peer,
      suite_setup, suite_teardown, MUNIT_TEST_OPTION_NONE, NULL },
    { "/hostname_check",      test_client_hostname_check,
      suite_setup, suite_teardown, MUNIT_TEST_OPTION_NONE, NULL },
    { "/default_verifies",    test_client_default_verifies,
      suite_setup, suite_teardown, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
    "/m18/tls_client", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

#else  /* !XTC_TLS_ENABLED -- skip stubs */

static MunitResult
skip_test(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    return MUNIT_SKIP;
}

static MunitTest tests[] = {
    { "/tls_client_all", skip_test, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
    "/m18/tls_client", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

#endif /* XTC_TLS_ENABLED */

/* =========================================================================
 * main
 * ======================================================================= */

int
main(int argc, char *argv[])
{
    __m18_paths_init();
    return munit_suite_main(&suite, NULL, argc, argv);
}
