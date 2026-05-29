/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/m18/test_tls_client.c
 *	Client-side OpenSSL handshake smoke test (TLS-3).
 *
 *	Exercises the xtc_tls CLIENT path end-to-end:
 *	  - xtc_tls_ctx_create (CLIENT role, optional CA + verify_peer)
 *	  - xtc_tls_create / xtc_tls_handshake (non-blocking, polled)
 *	  - xtc_tls_write / xtc_tls_read
 *	  - xtc_tls_shutdown / xtc_tls_destroy
 *
 *	The server side of the loopback connection is driven by raw
 *	OpenSSL calls inside a pthread so that both halves can run
 *	concurrently; this keeps the test focused on the xtc client
 *	code paths.
 *
 *	A self-signed RSA-2048 certificate and matching private key are
 *	generated at runtime via the openssl CLI and written to /tmp files
 *	during test setup.  This avoids embedding private keys in source
 *	(which triggers GitHub secret scanners).
 *
 *	A second self-signed certificate (different CN, different key)
 *	is generated as WRONG_CA to exercise the verify_peer reject path.
 *
 *	SNI: deferred to TLS-4 (requires a new opts field in xtc_tls.h).
 *
 *	When XTC_TLS_BACKEND_OPENSSL is not defined (--with-tls=none)
 *	every test in this suite returns MUNIT_SKIP cleanly.
 */

#include "munit.h"
#include "xtc_int.h"
#include "xtc_tls.h"

/* =========================================================================
 * OpenSSL-backend branch -- full implementation.
 * ======================================================================= */
#if defined(XTC_TLS_BACKEND_OPENSSL)

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

/* -------------------------------------------------------------------------
 * Runtime certificate generation.
 *
 * Instead of embedding PEM blobs (which trigger GitHub secret scanners),
 * we generate fresh self-signed RSA-2048 certs via the openssl CLI
 * during suite_setup and remove them in suite_teardown.
 *
 * TEST_CERT + TEST_KEY: the server presents this; the client verifies
 * against it (good-CA case).
 *
 * WRONG_CA: a different self-signed cert (different CN, different key)
 * used to trigger a peer-verification failure.
 * ----------------------------------------------------------------------- */

#define TEST_CERT_PATH   "/tmp/xtc-tls3-test-cert.pem"
#define TEST_KEY_PATH    "/tmp/xtc-tls3-test-key.pem"
#define WRONG_CA_PATH    "/tmp/xtc-tls3-wrong-ca.pem"

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

/* Generate a self-signed cert (key discarded) for use as a wrong CA. */
static int
generate_wrong_ca(const char *cert_path, const char *cn)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey rsa:2048 -nodes -days 1 "
             "-keyout /dev/null -out %s -subj /CN=%s 2>/dev/null",
             cert_path, cn);
    return system(cmd);
}

/* -------------------------------------------------------------------------
 * Helpers.
 * ----------------------------------------------------------------------- */

/* Drive a single xtc_tls operation with poll(2) until it returns
 * something other than XTC_E_AGAIN, or until timeout_ms elapses.
 * Returns the final xtc return code. */
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
        pfd.events  = xtc_tls_wants_read(tls) ? POLLIN : POLLOUT;
        pfd.revents = 0;

        int prc = poll(&pfd, 1, timeout_ms);
        if (prc <= 0)
            return XTC_E_INTERNAL;  /* timeout or poll error */
    }
}

/* Drive xtc_tls_write until all bytes are sent or error. */
static int
client_write_all(xtc_tls_t *tls, int fd,
                 const void *buf, size_t len, int timeout_ms)
{
    size_t total = 0;
    while (total < len) {
        size_t n = 0;
        int rc = xtc_tls_write(tls,
                               (const char *)buf + total,
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

/* Drive xtc_tls_read until exactly `len` bytes arrive or error. */
static int
client_read_exact(xtc_tls_t *tls, int fd,
                  void *buf, size_t len, int timeout_ms)
{
    size_t total = 0;
    while (total < len) {
        size_t n = 0;
        int rc = xtc_tls_read(tls,
                              (char *)buf + total,
                              len - total, &n);
        if (rc == XTC_OK) {
            if (n == 0)
                return XTC_E_INTERNAL;  /* unexpected EOF */
            total += n;
        } else if (rc == XTC_E_AGAIN) {
            struct pollfd pfd;
            pfd.fd      = fd;
            pfd.events  = xtc_tls_wants_read(tls) ? POLLIN : POLLOUT;
            pfd.revents = 0;
            if (poll(&pfd, 1, timeout_ms) <= 0)
                return XTC_E_INTERNAL;
        } else {
            return rc;
        }
    }
    return XTC_OK;
}

/* -------------------------------------------------------------------------
 * Server thread: accepts via raw OpenSSL (blocking) on the peer fd.
 *
 * Protocol (for the normal case):
 *   1. SSL_accept (blocking)
 *   2. Read exactly SERVER_MSG_LEN bytes sent by the client.
 *   3. Echo them back.
 *   4. SSL_shutdown.
 *
 * For the bad-CA case the client sends an alert during the handshake.
 * SSL_accept then fails with a non-1 return; the thread sets rc=2 and
 * exits cleanly.
 * ----------------------------------------------------------------------- */

#define SERVER_MSG_LEN 5

struct server_args {
    int  fd;   /* blocking socketpair fd; owned by the calling test */
    int  rc;   /* 0 = full success; 2 = SSL_accept failed (bad-CA ok);
                * other non-zero = unexpected error */
};

static void *
server_thread(void *arg)
{
    struct server_args *a    = (struct server_args *)arg;
    SSL_CTX            *sctx = NULL;
    SSL                *ssl  = NULL;
    char                buf[SERVER_MSG_LEN];
    int                 total, r;

    a->rc = 1;   /* assume unexpected failure */

    sctx = SSL_CTX_new(TLS_server_method());
    if (sctx == NULL)
        goto done;

    if (SSL_CTX_use_certificate_chain_file(sctx, TEST_CERT_PATH) != 1)
        goto done;
    if (SSL_CTX_use_PrivateKey_file(sctx, TEST_KEY_PATH,
                                    SSL_FILETYPE_PEM) != 1)
        goto done;

    ssl = SSL_new(sctx);
    if (ssl == NULL)
        goto done;
    if (SSL_set_fd(ssl, a->fd) != 1)
        goto done;

    /* Blocking accept.  Returns != 1 if the client sends an alert
     * (e.g. certificate verify failed in the bad-CA test case). */
    if (SSL_accept(ssl) != 1) {
        a->rc = 2;   /* accept failed; caller decides if this is ok */
        goto done;
    }

    /* Read exactly SERVER_MSG_LEN bytes from client. */
    total = 0;
    while (total < SERVER_MSG_LEN) {
        r = SSL_read(ssl, buf + total, SERVER_MSG_LEN - total);
        if (r <= 0)
            goto done;
        total += r;
    }

    /* Echo back. */
    total = 0;
    while (total < SERVER_MSG_LEN) {
        r = SSL_write(ssl, buf + total, SERVER_MSG_LEN - total);
        if (r <= 0)
            goto done;
        total += r;
    }

    (void)SSL_shutdown(ssl);
    a->rc = 0;

done:
    if (ssl  != NULL) SSL_free(ssl);
    if (sctx != NULL) SSL_CTX_free(sctx);
    return arg;
}

/* =========================================================================
 * Tests.
 * ======================================================================= */

/* -------------------------------------------------------------------------
 * test_client_handshake_roundtrip:
 *
 *   Full loopback, no peer verification:
 *     - socketpair -> non-blocking client fd + blocking server fd
 *     - server thread: raw SSL_accept, read "hello" echo back
 *     - client: xtc_tls_ctx_create(CLIENT, ...) + xtc_tls_create
 *               + xtc_tls_handshake loop + xtc_tls_write + xtc_tls_read
 *     - assert round-trip equals "hello"
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

    /* ---- ctx ---- */
    memset(&opts, 0, sizeof(opts));
    opts.verify_peer = 0;   /* skip cert verification */
    opts.min_version = XTC_TLS_VER_12;

    rc = xtc_tls_ctx_create(XTC_TLS_CLIENT, &opts, &ctx);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_ptr_not_null(ctx);

    /* ---- socketpair ---- */
    munit_assert_int(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);

    /* sv[0] = server (blocking), sv[1] = client (non-blocking) */
    {
        int flags = fcntl(sv[1], F_GETFL, 0);
        munit_assert_int(flags, !=, -1);
        munit_assert_int(fcntl(sv[1], F_SETFL, flags | O_NONBLOCK), ==, 0);
    }

    /* ---- wrap client fd ---- */
    rc = xtc_tls_create(ctx, sv[1], &tls);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_ptr_not_null(tls);

    /* ---- launch server thread ---- */
    memset(&sa, 0, sizeof(sa));
    sa.fd = sv[0];
    sa.rc = 1;
    munit_assert_int(pthread_create(&tid, NULL, server_thread, &sa), ==, 0);

    /* ---- client: drive handshake ---- */
    rc = poll_until_done(tls, sv[1], xtc_tls_handshake, 5000);
    munit_assert_int(rc, ==, XTC_OK);

    /* ---- client: write "hello" ---- */
    rc = client_write_all(tls, sv[1], "hello", SERVER_MSG_LEN, 5000);
    munit_assert_int(rc, ==, XTC_OK);

    /* ---- client: read echo ---- */
    memset(rbuf, 0, sizeof(rbuf));
    rc = client_read_exact(tls, sv[1], rbuf, SERVER_MSG_LEN, 5000);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_memory_equal(SERVER_MSG_LEN, rbuf, "hello");

    /* ---- client: graceful shutdown ---- */
    (void)xtc_tls_shutdown(tls);

    /* ---- join server ---- */
    pthread_join(tid, NULL);
    munit_assert_int(sa.rc, ==, 0);

    /* ---- cleanup ---- */
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
 *     WRONG_CA does not sign TEST_CERT; OpenSSL sends a certificate
 *     alert.  xtc_tls_handshake must return XTC_E_INTERNAL.
 *     The server thread's SSL_accept will also fail (rc == 2); that
 *     is expected and accepted.
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
    {
        int flags = fcntl(sv[1], F_GETFL, 0);
        munit_assert_int(flags, !=, -1);
        munit_assert_int(fcntl(sv[1], F_SETFL, flags | O_NONBLOCK), ==, 0);
    }

    rc = xtc_tls_create(ctx, sv[1], &tls);
    munit_assert_int(rc, ==, XTC_OK);

    memset(&sa, 0, sizeof(sa));
    sa.fd = sv[0];
    sa.rc = 1;
    munit_assert_int(pthread_create(&tid, NULL, server_thread, &sa), ==, 0);

    rc = poll_until_done(tls, sv[1], xtc_tls_handshake, 5000);
    munit_assert_int(rc, ==, XTC_OK);

    rc = client_write_all(tls, sv[1], "hello", SERVER_MSG_LEN, 5000);
    munit_assert_int(rc, ==, XTC_OK);

    memset(rbuf, 0, sizeof(rbuf));
    rc = client_read_exact(tls, sv[1], rbuf, SERVER_MSG_LEN, 5000);
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
    {
        int flags = fcntl(sv[1], F_GETFL, 0);
        munit_assert_int(flags, !=, -1);
        munit_assert_int(fcntl(sv[1], F_SETFL, flags | O_NONBLOCK), ==, 0);
    }

    rc = xtc_tls_create(ctx, sv[1], &tls);
    munit_assert_int(rc, ==, XTC_OK);

    memset(&sa, 0, sizeof(sa));
    sa.fd = sv[0];
    sa.rc = 1;
    munit_assert_int(pthread_create(&tid, NULL, server_thread, &sa), ==, 0);

    /* Handshake must fail: certificate verify failed. */
    rc = poll_until_done(tls, sv[1], xtc_tls_handshake, 5000);
    munit_assert_int(rc, ==, XTC_E_INTERNAL);

    /* Join server; it may have failed in SSL_accept (rc==2) because
     * the client sent a certificate alert -- that is expected here. */
    pthread_join(tid, NULL);
    munit_assert_int(sa.rc, !=, 0);   /* server did NOT complete normally */

    xtc_tls_destroy(tls);
    xtc_tls_ctx_destroy(ctx);
    close(sv[0]);
    close(sv[1]);

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
    return (void *)(uintptr_t)1;   /* non-NULL: setup succeeded */
}

static void
suite_teardown(void *fixture)
{
    (void)fixture;
    unlink(TEST_CERT_PATH);
    unlink(TEST_KEY_PATH);
    unlink(WRONG_CA_PATH);
}

/* =========================================================================
 * Test array and suite.
 * ======================================================================= */

static MunitTest tests[] = {
    { "/handshake_roundtrip", test_client_handshake_roundtrip,
      suite_setup, suite_teardown, MUNIT_TEST_OPTION_NONE, NULL },
    { "/verify_peer",         test_client_verify_peer,
      suite_setup, suite_teardown, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
    "/m18/tls_client", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

#else  /* !XTC_TLS_BACKEND_OPENSSL -- skip stubs */

/*
 * TLS backend not compiled in.  Each test function returns MUNIT_SKIP
 * so the test binary exits with a clean "all skipped" result.
 */

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

#endif /* XTC_TLS_BACKEND_OPENSSL */

/* =========================================================================
 * main
 * ======================================================================= */

int
main(int argc, char *argv[])
{
    return munit_suite_main(&suite, NULL, argc, argv);
}
