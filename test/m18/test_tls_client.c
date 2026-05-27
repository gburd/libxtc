/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
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
 *	embedded as string literals (same material as test_tls_server.c)
 *	and written to /tmp files during test setup.
 *
 *	A second self-signed certificate (different CN, different key)
 *	is embedded as WRONG_CA to exercise the verify_peer reject path.
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
 * OpenSSL-backend branch — full implementation.
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
 * Embedded self-signed RSA-2048 certificate and key.
 *
 * Identical to the material in test_tls_server.c.  The server presents
 * this certificate; the client verifies against it (good-CA case) or
 * against WRONG_CA (bad-CA case).
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

/* -------------------------------------------------------------------------
 * Wrong CA — a different self-signed RSA-2048 cert.
 *
 * Generated with:
 *   openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
 *     -keyout /dev/null -out /dev/stdout -subj /CN=wrong-ca
 *
 * Valid 2026-05-27 through 2036-05-24.
 * Used only to supply a CA bundle that does NOT contain TEST_CERT,
 * triggering a peer-verification failure.
 * ----------------------------------------------------------------------- */

static const char WRONG_CA[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDBzCCAe+gAwIBAgIUC3bdYTIZKQ0+mJGuh3VNt0srbp8wDQYJKoZIhvcNAQEL\n"
    "BQAwEzERMA8GA1UEAwwId3JvbmctY2EwHhcNMjYwNTI3MTU1NDI4WhcNMzYwNTI0\n"
    "MTU1NDI4WjATMREwDwYDVQQDDAh3cm9uZy1jYTCCASIwDQYJKoZIhvcNAQEBBQAD\n"
    "ggEPADCCAQoCggEBAMinxX6INzxvxJslAqNCjHo3UN/X632Kf0H6UkIJ0h3Ou8wD\n"
    "tf4QBtYN4oAUdpRlhbxm+VqjM0S6ZwAV/0+cdksNnzrxwdCTH6TPIEB5o6mhknfZ\n"
    "NkPcxPrDMNZb023jZF+o7fn2Mi/kgOMW0Cer0GDcR29VFNYCSXO9E7PWzehc1xaA\n"
    "gEEpv4xwaOlyKRU+IcET8vppLX6KyRqkjWEUPNK5lT+/MIlg576teVY8n+T0YiZV\n"
    "NUuBtn7Dtz+TbqY0MVVtkZyCFjd09YD4GjPXTeP17/HybymxyyuZjmD07Z8KhBT4\n"
    "onq//apOZnTdwNYH2m5GRWjGU1hIXKea3tvvwyUCAwEAAaNTMFEwHQYDVR0OBBYE\n"
    "FORc0Jjc4dJ4LVuscRBVfCdvUoBbMB8GA1UdIwQYMBaAFORc0Jjc4dJ4LVuscRBV\n"
    "fCdvUoBbMA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQELBQADggEBAGyuqSbv\n"
    "qPdwPB29gQlhfPqjrASny7VhMyjCo/KJycV7+dtE61fXtAi4TmzsmYod/eX5quax\n"
    "f2khK3ycLR+xG/5SkdfKxtaUQIBjaB0Srk7eZM023BYF/9uqb38zrrxWRDluq5tr\n"
    "KrRhHyhNauovv2XDi12IkUOI8bONqNTOqAmS3YXUU8lY4T5LIEkD8sAsQIcruGU5\n"
    "YFA/rJMjjwy33DN/F3Fe+5vRhM9MHg//QlAuHkNOHxsElRRUsl7DwXTvO9S9D/Zn\n"
    "koox3R/aLZBwggDY8t8sdUGOMIDRuxHtjGDCZ6AglNcJech9sSJuokJ5cjsDW+d4\n"
    "d/Rghn2IxYooWWA=\n"
    "-----END CERTIFICATE-----\n";

#define TEST_CERT_PATH   "/tmp/xtc-tls3-test-cert.pem"
#define TEST_KEY_PATH    "/tmp/xtc-tls3-test-key.pem"
#define WRONG_CA_PATH    "/tmp/xtc-tls3-wrong-ca.pem"

/* -------------------------------------------------------------------------
 * Helpers.
 * ----------------------------------------------------------------------- */

/* Write a NUL-terminated string to a file; return 0 on success. */
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
 *     - socketpair → non-blocking client fd + blocking server fd
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
 *   Sub-case A — Good CA:
 *     Client uses ca_file = TEST_CERT_PATH + verify_peer = 1.
 *     The server presents TEST_CERT (self-signed; it IS its own CA).
 *     Handshake must succeed; round-trip verifies data flow.
 *
 *   Sub-case B — Bad CA:
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
     * Sub-case A: good CA — handshake succeeds and data flows.
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
     * Sub-case B: bad CA — handshake must fail with XTC_E_INTERNAL.
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
     * the client sent a certificate alert — that is expected here. */
    pthread_join(tid, NULL);
    munit_assert_int(sa.rc, !=, 0);   /* server did NOT complete normally */

    xtc_tls_destroy(tls);
    xtc_tls_ctx_destroy(ctx);
    close(sv[0]);
    close(sv[1]);

    return MUNIT_OK;
}

/* =========================================================================
 * Suite setup / teardown — write/remove temp PEM files.
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
    if (write_file(WRONG_CA_PATH, WRONG_CA) != 0) {
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

#else  /* !XTC_TLS_BACKEND_OPENSSL — skip stubs */

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
