/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/m18/test_tls_server.c
 *	Server-side OpenSSL handshake smoke test (TLS-2).
 *
 *	Exercises the xtc_tls server path end-to-end:
 *	  - xtc_tls_ctx_create (SERVER role, cert + key)
 *	  - xtc_tls_create / xtc_tls_handshake (non-blocking, polled)
 *	  - xtc_tls_write / xtc_tls_read
 *	  - xtc_tls_shutdown / xtc_tls_destroy
 *
 *	The client side of the loopback connection is driven by raw
 *	OpenSSL calls inside a pthread so that both halves can run
 *	concurrently.
 *
 *	A self-signed RSA-2048 certificate and matching private key are
 *	embedded as string literals and written to /tmp files during test
 *	setup.  This avoids subprocess fragility.
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
#include <openssl/x509.h>

/* -------------------------------------------------------------------------
 * Embedded self-signed RSA-2048 certificate and key.
 *
 * Generated once with:
 *   openssl req -x509 -newkey rsa:2048 -nodes -days 3650
 *     -keyout /dev/stdout -out /dev/stdout -subj /CN=localhost
 *
 * Valid 2026-05-27 through 2036-05-24.
 * ----------------------------------------------------------------------- */

static const char TEST_CERT[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDCTCCAfGgAwIBAgIUWlUYGYaD4ssyA9bpZlUewxUigfIwDQYJKoZIhvcNAQEL\n"
    "BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDUyNzE1MzAzMFoXDTM2MDUy\n"
    "NDE1MzAzMFowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF\n"
    "AAOCAQ8AMIIBCgKCAQEA4Uyc4grpIGmXoa/fXAz02Unmv/ffInFKG1DC/OtBEBMI\n"
    "6LokD0N/61bPrVDwRlvcji+frhG+Q7NUZsoy4gk78Gqg4Ff1Lgg51PhKo6L1ws6T\n"
    "JZatSP4VIsjMnjkOT9WYOMJ1lmiotFDEJ44LXXJ77FTQp4Gb2aEb1dhQilCHHOQ5\n"
    "4mmguBZcntxqTBtyag0akpmgm5T50mWl3YNlRZjME1gvVa13bnA8UFb/dAMTwsti\n"
    "4GevpTB/10Ac9cwkQrNG4kko2H2u4xpP9qZrVpRsRy9MiLuoOfaFbV48jtO9rmp0\n"
    "Ptd9C9UasqEy0VswWZVqhIrNb0WzNlY986u/V4be5wIDAQABo1MwUTAdBgNVHQ4E\n"
    "FgQUbsBzAOzptAsYFVaMrTZomT2+7skwHwYDVR0jBBgwFoAUbsBzAOzptAsYFVaM\n"
    "rTZomT2+7skwDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAnHRq\n"
    "jvpsjP+szy8YiZhrDIMs3dkggAL+uQB9U/Zm+Dkkm98TYVc+FWII7h2gPCngTJcg\n"
    "UdAmi5NhKuA3den/ahDMEHYzxhV8DE6yP4FZXagHS0lBztMAt0IR970L4wWdpAR3\n"
    "rONrI4z3BwUphbk4fvnfftas3Ee8MTbU5PxzM9KoI3XteJrGd63mVdrBQjawGMgo\n"
    "c036GB1pSyEHZQWo7MwbLBaqH40Vq56pxgcO4EB22N21pddrJOk7ypO8vGkAjBjb\n"
    "VbiQZ0I1N6NA5DaP7lSLDXQAq2/F0uUIJeZWKm84RPd6Oetzj6ibtijtbSjBgSWP\n"
    "gKOejUIkuypd+Uq6/w==\n"
    "-----END CERTIFICATE-----\n";

static const char TEST_KEY[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvwIBADANBgkqhkiG9w0BAQEFAASCBKkwggSlAgEAAoIBAQDhTJziCukgaZeh\n"
    "r99cDPTZSea/998icUobUML860EQEwjouiQPQ3/rVs+tUPBGW9yOL5+uEb5Ds1Rm\n"
    "yjLiCTvwaqDgV/UuCDnU+EqjovXCzpMllq1I/hUiyMyeOQ5P1Zg4wnWWaKi0UMQn\n"
    "jgtdcnvsVNCngZvZoRvV2FCKUIcc5DniaaC4Flye3GpMG3JqDRqSmaCblPnSZaXd\n"
    "g2VFmMwTWC9VrXducDxQVv90AxPCy2LgZ6+lMH/XQBz1zCRCs0biSSjYfa7jGk/2\n"
    "pmtWlGxHL0yIu6g59oVtXjyO072uanQ+130L1RqyoTLRWzBZlWqEis1vRbM2Vj3z\n"
    "q79Xht7nAgMBAAECggEAGuiJkZMisNUBCotKLrZQGrucLFqwcxlQyTO7diBVjv6X\n"
    "ASS/iyB8C2GSQEL7JOFygCY+o2L0dHrbSRjmkYnvd2a6JtFFM+WWR/y3qBTzK/x8\n"
    "JUXPwHiy5t/7Vae+29jI63kzjhlF554AhNuO4+75JLV545JenimDJ5FEXUtXjSLu\n"
    "RRieq4CnA6h4Kg5yomA4S7D9BKEqOGxAbijmuREWdFmjU5jYcFX6nmv/L/qkCZBB\n"
    "QKLe53EOHSfRw+5cSxsbc/B+zvZipAuJkH6/YRdeKYgHiA0fb7SIu1BAXxToqGtI\n"
    "IhDDT2EfcvuSEfoisxn8q2ZYkmL8wi3kHjkdZJBb4QKBgQD3ZmvsknC5jano8oW8\n"
    "9k+kDtWS62eo7eAGBn5hcwq99tjUREcbeN6Ns1sC5gIP67iwAXY0e5yQSFTAGjQe\n"
    "ndRo0s3sf4ztPfDXpkwVdi0352QkpchJmToCkQY602CH2FbCg5Dl+c0S/9jTv0uI\n"
    "gc/KxFyBM45F/xreTOZcZi3CXwKBgQDpIYTq5IpzztI5k3RIVw/Q9dh7e8azX2pR\n"
    "2wgVDHNW0vMlK/65VCQQdMePlFUtep1jf0kPjL5WUDaktbzzWJ65SV9eHh41dB5+\n"
    "Wj3dFooorf9+fTbCs+GV21/B6YBul5EoaO/D9cf2/jwM6AQ1T1gWqukdZXEddmBu\n"
    "PAtH2doAeQKBgQDRYOxL7m/aiitXjBFlqCwk060rR3GWhaOIVeyVutBHj2dY1mQ7\n"
    "uuLXmAiZfmIWaVAIHWSV7FvHvH+FiWe81aSUBnzi/9wcWMTBLevMahTA8GNPpMLK\n"
    "jxSKYYSdOpHCxnQ+8Swrhmtp/f/azVY2tG5Q1DjZ2/E4CjwKEZkQcCWgDwKBgQDa\n"
    "Ho9iygM0CQyt6+U/DZ3xryMlnZAyIRKzlU/Bic2cLXBqlfgUY8H+V5SjJHBxRahe\n"
    "AChWUSOAVDpb7uHjeEXBLAH7aAhxkLw7EamR4lXPa8SBDxweHPjyIbc9EYAleM/K\n"
    "VCwIVzwJPqLmnGnbiunrA2tqIpArtabRXIJdbllGWQKBgQDUT2Jnr4mQ7NMsVGTX\n"
    "DOxH71QP3tsABpa7So67X7e6+japM8kCR85/QHJnIsNxC/auaPYbQfp45Qi93TPT\n"
    "L7sdWtKC3L9LzYImZfrUFreH9ToHaH9xXjs6HjvDV5qPOGBqALE4RWAdGEuX23QY\n"
    "uQIExdKBzY8FDv2RzpS481a6Tg==\n"
    "-----END PRIVATE KEY-----\n";

#define TEST_CERT_PATH  "/tmp/xtc-tls2-test-cert.pem"
#define TEST_KEY_PATH   "/tmp/xtc-tls2-test-key.pem"

/* -------------------------------------------------------------------------
 * Helpers.
 * ----------------------------------------------------------------------- */

/* Write a NUL-terminated string to a file, return 0 on success. */
static int
write_file(const char *path, const char *data)
{
    FILE *f = fopen(path, "w");   /* XTC_BLOCKING_OK: one-shot test setup */
    if (f == NULL)
        return -1;
    fputs(data, f);
    return fclose(f);
}

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
server_write_all(xtc_tls_t *tls, int fd,
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
server_read_exact(xtc_tls_t *tls, int fd,
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
 * Client thread: connects via raw OpenSSL (blocking) on the peer fd.
 *
 * Protocol:
 *   1. SSL_connect (blocking)
 *   2. Read exactly CLIENT_MSG_LEN bytes sent by the server.
 *   3. Echo them back.
 *   4. SSL_shutdown.
 * ----------------------------------------------------------------------- */

#define CLIENT_MSG_LEN 5

struct client_args {
    int         fd;         /* socketpair peer fd (blocking) */
    int         rc;         /* 0 = success; non-zero = error */
    char        echoed[CLIENT_MSG_LEN + 1];
};

static void *
client_thread(void *arg)
{
    struct client_args *a    = (struct client_args *)arg;
    SSL_CTX            *cctx = NULL;
    SSL                *ssl  = NULL;
    char                buf[CLIENT_MSG_LEN];
    int                 total;

    a->rc = 1;   /* assume failure */

    /* Create a client-side SSL context that skips server cert verification
     * (self-signed cert in test). */
    cctx = SSL_CTX_new(TLS_client_method());
    if (cctx == NULL)
        goto done;
    SSL_CTX_set_verify(cctx, SSL_VERIFY_NONE, NULL);

    ssl = SSL_new(cctx);
    if (ssl == NULL)
        goto done;
    if (SSL_set_fd(ssl, a->fd) != 1)
        goto done;

    /* Blocking SSL_connect. */
    if (SSL_connect(ssl) != 1)
        goto done;

    /* Read exactly CLIENT_MSG_LEN bytes from the server. */
    total = 0;
    while (total < CLIENT_MSG_LEN) {
        int r = SSL_read(ssl, buf + total, CLIENT_MSG_LEN - total);
        if (r <= 0)
            goto done;
        total += r;
    }

    /* Echo back. */
    total = 0;
    while (total < CLIENT_MSG_LEN) {
        int w = SSL_write(ssl, buf + total, CLIENT_MSG_LEN - total);
        if (w <= 0)
            goto done;
        total += w;
    }

    /* Graceful shutdown. */
    (void)SSL_shutdown(ssl);

    memcpy(a->echoed, buf, CLIENT_MSG_LEN);
    a->echoed[CLIENT_MSG_LEN] = '\0';
    a->rc = 0;

done:
    if (ssl  != NULL) { SSL_free(ssl); }
    if (cctx != NULL) { SSL_CTX_free(cctx); }
    return arg;
}

/* =========================================================================
 * Tests.
 * ======================================================================= */

/* -------------------------------------------------------------------------
 * test_server_ctx_create_destroy:
 *   Create a SERVER ctx with the embedded cert+key.
 *   Verify xtc_tls_ctx_create succeeds and ctx_destroy is clean.
 * ----------------------------------------------------------------------- */
static MunitResult
test_server_ctx_create_destroy(const MunitParameter params[], void *data)
{
    xtc_tls_opts_t  opts;
    xtc_tls_ctx_t  *ctx = NULL;
    int             rc;

    (void)params;
    (void)data;

    memset(&opts, 0, sizeof(opts));
    opts.cert_file   = TEST_CERT_PATH;
    opts.key_file    = TEST_KEY_PATH;
    opts.min_version = XTC_TLS_VER_12;
    opts.verify_peer = 0;

    rc = xtc_tls_ctx_create(XTC_TLS_SERVER, &opts, &ctx);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_ptr_not_null(ctx);

    xtc_tls_ctx_destroy(ctx);
    return MUNIT_OK;
}

/* -------------------------------------------------------------------------
 * test_server_handshake_roundtrip:
 *   Full loopback:
 *     - socketpair -> non-blocking server fd + blocking client fd
 *     - client thread: SSL_connect, read "hello", echo back
 *     - server: xtc_tls_handshake loop, write "hello", read echo
 *     - assert echo == "hello"
 * ----------------------------------------------------------------------- */
static MunitResult
test_server_handshake_roundtrip(const MunitParameter params[], void *data)
{
    xtc_tls_opts_t  opts;
    xtc_tls_ctx_t  *ctx   = NULL;
    xtc_tls_t      *tls   = NULL;
    struct client_args ca;
    pthread_t       tid;
    int             sv[2];
    int             rc;
    char            rbuf[CLIENT_MSG_LEN + 1];

    (void)params;
    (void)data;

    /* ---- ctx ---- */
    memset(&opts, 0, sizeof(opts));
    opts.cert_file   = TEST_CERT_PATH;
    opts.key_file    = TEST_KEY_PATH;
    opts.min_version = XTC_TLS_VER_12;
    opts.verify_peer = 0;

    rc = xtc_tls_ctx_create(XTC_TLS_SERVER, &opts, &ctx);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_ptr_not_null(ctx);

    /* ---- socketpair ---- */
    munit_assert_int(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);

    /* sv[0] = server (non-blocking), sv[1] = client (blocking) */
    {
        int flags = fcntl(sv[0], F_GETFL, 0);
        munit_assert_int(flags, !=, -1);
        munit_assert_int(fcntl(sv[0], F_SETFL, flags | O_NONBLOCK), ==, 0);
    }

    /* ---- wrap server fd ---- */
    rc = xtc_tls_create(ctx, sv[0], &tls);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_ptr_not_null(tls);

    /* ---- launch client thread ---- */
    memset(&ca, 0, sizeof(ca));
    ca.fd = sv[1];
    ca.rc = 1;
    munit_assert_int(pthread_create(&tid, NULL, client_thread, &ca), ==, 0);

    /* ---- server: drive handshake ---- */
    rc = poll_until_done(tls, sv[0], xtc_tls_handshake, 5000);
    munit_assert_int(rc, ==, XTC_OK);

    /* ---- server: write "hello" ---- */
    rc = server_write_all(tls, sv[0], "hello", CLIENT_MSG_LEN, 5000);
    munit_assert_int(rc, ==, XTC_OK);

    /* ---- server: read echo ---- */
    memset(rbuf, 0, sizeof(rbuf));
    rc = server_read_exact(tls, sv[0], rbuf, CLIENT_MSG_LEN, 5000);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_memory_equal(CLIENT_MSG_LEN, rbuf, "hello");

    /* ---- server: graceful shutdown ---- */
    /* Call once to send close_notify; do not loop -- peer may not have
     * sent theirs yet, and the test does not need the full bidir close. */
    (void)xtc_tls_shutdown(tls);

    /* ---- join client ---- */
    pthread_join(tid, NULL);
    munit_assert_int(ca.rc, ==, 0);
    munit_assert_memory_equal(CLIENT_MSG_LEN, ca.echoed, "hello");

    /* ---- cleanup ---- */
    xtc_tls_destroy(tls);
    xtc_tls_ctx_destroy(ctx);
    close(sv[0]);
    close(sv[1]);

    return MUNIT_OK;
}

/* -------------------------------------------------------------------------
 * test_server_alpn:
 *   Create a SERVER ctx with ALPN h2 preference.
 *   Verify ctx creation succeeds (ALPN path exercised).
 * ----------------------------------------------------------------------- */
static MunitResult
test_server_alpn(const MunitParameter params[], void *data)
{
    xtc_tls_opts_t  opts;
    xtc_tls_ctx_t  *ctx = NULL;
    int             rc;

    (void)params;
    (void)data;

    memset(&opts, 0, sizeof(opts));
    opts.cert_file   = TEST_CERT_PATH;
    opts.key_file    = TEST_KEY_PATH;
    opts.min_version = XTC_TLS_VER_12;
    opts.verify_peer = 0;
    /* Wire-form ALPN: h2 then http/1.1 */
    opts.alpn_protos = "\x02h2\x08http/1.1";

    rc = xtc_tls_ctx_create(XTC_TLS_SERVER, &opts, &ctx);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_ptr_not_null(ctx);

    xtc_tls_ctx_destroy(ctx);
    return MUNIT_OK;
}

/* -------------------------------------------------------------------------
 * test_server_tls_create_bad_args:
 *   xtc_tls_create guard rails after ctx_create succeeded.
 * ----------------------------------------------------------------------- */
static MunitResult
test_server_tls_create_bad_args(const MunitParameter params[], void *data)
{
    xtc_tls_opts_t  opts;
    xtc_tls_ctx_t  *ctx = NULL;
    xtc_tls_t      *tls = NULL;
    int             rc;

    (void)params;
    (void)data;

    memset(&opts, 0, sizeof(opts));
    opts.cert_file = TEST_CERT_PATH;
    opts.key_file  = TEST_KEY_PATH;

    rc = xtc_tls_ctx_create(XTC_TLS_SERVER, &opts, &ctx);
    munit_assert_int(rc, ==, XTC_OK);

    /* NULL ctx */
    rc = xtc_tls_create(NULL, 3, &tls);
    munit_assert_int(rc, ==, XTC_E_INVAL);

    /* negative fd */
    rc = xtc_tls_create(ctx, -1, &tls);
    munit_assert_int(rc, ==, XTC_E_INVAL);

    /* NULL out */
    rc = xtc_tls_create(ctx, 3, NULL);
    munit_assert_int(rc, ==, XTC_E_INVAL);

    xtc_tls_ctx_destroy(ctx);
    return MUNIT_OK;
}

/* -------------------------------------------------------------------------
 * test_server_bad_cert_path:
 *   xtc_tls_ctx_create with non-existent cert returns XTC_E_INVAL.
 * ----------------------------------------------------------------------- */
static MunitResult
test_server_bad_cert_path(const MunitParameter params[], void *data)
{
    xtc_tls_opts_t  opts;
    xtc_tls_ctx_t  *ctx = NULL;
    int             rc;

    (void)params;
    (void)data;

    memset(&opts, 0, sizeof(opts));
    opts.cert_file = "/tmp/xtc-nonexistent-cert-89ab.pem";
    opts.key_file  = "/tmp/xtc-nonexistent-key-89ab.pem";

    rc = xtc_tls_ctx_create(XTC_TLS_SERVER, &opts, &ctx);
    munit_assert_int(rc, ==, XTC_E_INVAL);
    munit_assert_ptr_null(ctx);
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

    if (write_file(TEST_CERT_PATH, TEST_CERT) != 0)
        return NULL;
    if (write_file(TEST_KEY_PATH, TEST_KEY) != 0) {
        unlink(TEST_CERT_PATH);
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
}

/* =========================================================================
 * Test array and suite.
 * ======================================================================= */

static MunitTest tests[] = {
    { "/ctx_create_destroy",   test_server_ctx_create_destroy,  suite_setup, suite_teardown,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/handshake_roundtrip",  test_server_handshake_roundtrip, suite_setup, suite_teardown,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/alpn_create",          test_server_alpn,                suite_setup, suite_teardown,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/tls_create_bad_args",  test_server_tls_create_bad_args, suite_setup, suite_teardown,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/bad_cert_path",        test_server_bad_cert_path,       NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
    "/m18/tls_server", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
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
    { "/tls_server_all", skip_test, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
    "/m18/tls_server", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
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
