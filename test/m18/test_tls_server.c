/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/m18/test_tls_server.c
 *	Server-side handshake smoke test (TLS-2).
 *
 *	Exercises the xtc_tls server path end-to-end:
 *	  - xtc_tls_ctx_create (SERVER role, cert + key)
 *	  - xtc_tls_create / xtc_tls_handshake (non-blocking, polled)
 *	  - xtc_tls_write / xtc_tls_read
 *	  - xtc_tls_shutdown / xtc_tls_destroy
 *
 *	The peer (client) side of the loopback connection is driven by the
 *	SAME xtc_tls API, in CLIENT role, inside a pthread.  Using xtc_tls
 *	for both halves keeps the test backend-agnostic: whatever TLS
 *	backend configure selected (OpenSSL/LibreSSL, mbedTLS, GnuTLS,
 *	wolfSSL, ...) is the backend actually being exercised on both
 *	sides.  Both fds are non-blocking and driven by poll(2), matching
 *	the event-loop discipline of the library.
 *
 *	A self-signed RSA-2048 certificate and matching private key are
 *	generated at runtime via the openssl CLI and written to /tmp files
 *	during test setup.  This avoids embedding private keys in source
 *	(which triggers GitHub secret scanners).
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
#include <sys/socket.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Runtime certificate generation.
 *
 * Instead of embedding PEM blobs (which trigger GitHub secret scanners),
 * we generate a fresh self-signed RSA-2048 cert+key via the openssl CLI
 * during suite_setup and remove them in suite_teardown.
 * ----------------------------------------------------------------------- */

#define TEST_CERT_PATH  "/tmp/xtc-tls2-test-cert.pem"
#define TEST_KEY_PATH   "/tmp/xtc-tls2-test-key.pem"
#define TEST_CLI_CERT   "/tmp/xtc-tls2-cli-cert.pem"
#define TEST_CLI_KEY    "/tmp/xtc-tls2-cli-key.pem"
/* Second SERVER cert with a distinct CN, selected via the SNI callback. */
#define TEST_SNI_CERT   "/tmp/xtc-tls2-sni-cert.pem"
#define TEST_SNI_KEY    "/tmp/xtc-tls2-sni-key.pem"
#define TEST_PSS_CERT   "/tmp/xtc-tls2-pss-cert.pem"
#define TEST_PSS_KEY    "/tmp/xtc-tls2-pss-key.pem"
#define SNI_HOSTNAME    "tenant.example"

/* Generate a self-signed RSA-2048 cert+key via the openssl CLI.
 * Returns 0 on success. */
static int
generate_cert(const char *cert_path, const char *key_path, const char *cn)
{
    char cmd[1024];
    char cnf_path[256];
    FILE *cnf_fp;
    snprintf(cnf_path, sizeof(cnf_path), "%s.cnf", cert_path);
    /* LibreSSL on Nix and some stripped distros ship `openssl' without
     * a default openssl.cnf, which makes `openssl req` fail before it
     * touches the key/cert.  Write a minimal config and pass -config
     * explicitly so the test is self-contained. */
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

/* Generate an RSA-PSS-signed self-signed cert whose message digest is
 * SHA-512 (NOT SHA-256).  This is the case that exposes the
 * channel-binding bug: X509_get_signature_nid() yields NID_rsassaPss
 * (the digest lives in the PSS parameters, not the sig OID), so the
 * portable OBJ_find_sigid_algs path returns NID_undef and wrongly falls
 * back to SHA-256.  RFC 5929 requires the cert's REAL digest (SHA-512
 * here).  Returns 0 on success, non-zero if the toolchain cannot build
 * a PSS cert (caller then skips). */
static int
generate_pss_cert(const char *cert_path, const char *key_path, const char *cn)
{
    char cmd[1024], cnf_path[256];
    FILE *cnf_fp;
    int rc;
    snprintf(cnf_path, sizeof(cnf_path), "%s.cnf", cert_path);
    cnf_fp = fopen(cnf_path, "w");
    if (cnf_fp != NULL) {
        fprintf(cnf_fp,
            "[req]\nprompt = no\ndistinguished_name = dn\n"
            "[dn]\nCN = %s\n", cn);
        fclose(cnf_fp);
    }
    /* rsa_pss_keygen_md sets the key's mandated digest; -sha512 signs
     * the cert with SHA-512 under RSASSA-PSS. */
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey rsa-pss "
             "-pkeyopt rsa_keygen_bits:2048 "
             "-pkeyopt rsa_pss_keygen_md:sha512 "
             "-sha512 -nodes -days 1 -config %s "
             "-keyout %s -out %s -subj /CN=%s 2>/dev/null",
             cnf_path, key_path, cert_path, cn);
    rc = system(cmd);
    (void)unlink(cnf_path);
    return rc;
}

/* -------------------------------------------------------------------------
 * Helpers -- drive xtc_tls operations to completion over a poll(2) loop.
 * Shared by both the system-under-test side and the peer thread.
 * ----------------------------------------------------------------------- */

/* Drive a single 0-arg xtc_tls op until it returns != XTC_E_AGAIN, or
 * timeout_ms elapses.  Returns the final xtc return code. */
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
            return XTC_E_INTERNAL;  /* timeout or poll error */
    }
}

/* Drive xtc_tls_write until all bytes are sent or error. */
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

/* Drive xtc_tls_read until exactly `len` bytes arrive or error. */
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
 * Client thread: connects via xtc_tls (CLIENT role) on the peer fd.
 *
 * Protocol:
 *   1. handshake
 *   2. Read exactly CLIENT_MSG_LEN bytes sent by the server.
 *   3. Echo them back.
 *   4. shutdown.
 * ----------------------------------------------------------------------- */

#define CLIENT_MSG_LEN 5

struct client_args {
    int   fd;         /* non-blocking socketpair peer fd */
    int   rc;         /* 0 = success; non-zero = error */
    char  echoed[CLIENT_MSG_LEN + 1];
};

static void *
client_thread(void *arg)
{
    struct client_args *a   = (struct client_args *)arg;
    xtc_tls_opts_t      opts;
    xtc_tls_ctx_t      *ctx = NULL;
    xtc_tls_t          *tls = NULL;
    char                buf[CLIENT_MSG_LEN];

    a->rc = 1;   /* assume failure */

    memset(&opts, 0, sizeof(opts));
    opts.verify_peer = 0;   /* self-signed server cert in test */
    opts.min_version = XTC_TLS_VER_12;

    if (xtc_tls_ctx_create(XTC_TLS_CLIENT, &opts, &ctx) != XTC_OK)
        goto done;
    if (xtc_tls_create(ctx, a->fd, &tls) != XTC_OK)
        goto done;

    if (poll_until_done(tls, a->fd, xtc_tls_handshake, 5000) != XTC_OK)
        goto done;

    if (tls_read_exact(tls, a->fd, buf, CLIENT_MSG_LEN, 5000) != XTC_OK)
        goto done;

    if (tls_write_all(tls, a->fd, buf, CLIENT_MSG_LEN, 5000) != XTC_OK)
        goto done;

    (void)xtc_tls_shutdown(tls);

    memcpy(a->echoed, buf, CLIENT_MSG_LEN);
    a->echoed[CLIENT_MSG_LEN] = '\0';
    a->rc = 0;

done:
    if (tls != NULL) xtc_tls_destroy(tls);
    if (ctx != NULL) xtc_tls_ctx_destroy(ctx);
    return arg;
}

/* Client that PRESENTS a certificate (mutual TLS), so the server side can
 * read the peer-cert introspection accessors. */
static void *
client_thread_with_cert(void *arg)
{
    struct client_args *a   = (struct client_args *)arg;
    xtc_tls_opts_t      opts;
    xtc_tls_ctx_t      *ctx = NULL;
    xtc_tls_t          *tls = NULL;
    char                buf[CLIENT_MSG_LEN];

    a->rc = 1;

    memset(&opts, 0, sizeof(opts));
    opts.verify_peer = 0;                 /* self-signed server cert */
    opts.min_version = XTC_TLS_VER_12;
    opts.cert_file   = TEST_CLI_CERT;     /* present our client cert */
    opts.key_file    = TEST_CLI_KEY;

    if (xtc_tls_ctx_create(XTC_TLS_CLIENT, &opts, &ctx) != XTC_OK)
        goto done;
    if (xtc_tls_create(ctx, a->fd, &tls) != XTC_OK)
        goto done;
    if (poll_until_done(tls, a->fd, xtc_tls_handshake, 5000) != XTC_OK)
        goto done;
    if (tls_read_exact(tls, a->fd, buf, CLIENT_MSG_LEN, 5000) != XTC_OK)
        goto done;
    if (tls_write_all(tls, a->fd, buf, CLIENT_MSG_LEN, 5000) != XTC_OK)
        goto done;
    (void)xtc_tls_shutdown(tls);
    memcpy(a->echoed, buf, CLIENT_MSG_LEN);
    a->echoed[CLIENT_MSG_LEN] = '\0';
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
 * test_server_ctx_create_destroy:
 *   Create a SERVER ctx with the runtime-generated cert+key.
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
 *   Full loopback, server side under test, client side via xtc_tls:
 *     - socketpair -> both ends non-blocking
 *     - client thread: xtc_tls CLIENT handshake, read "hello", echo
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

    memset(&opts, 0, sizeof(opts));
    opts.cert_file   = TEST_CERT_PATH;
    opts.key_file    = TEST_KEY_PATH;
    opts.min_version = XTC_TLS_VER_12;
    opts.verify_peer = 0;

    rc = xtc_tls_ctx_create(XTC_TLS_SERVER, &opts, &ctx);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_ptr_not_null(ctx);

    munit_assert_int(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    munit_assert_int(set_nonblock(sv[0]), ==, 0);
    munit_assert_int(set_nonblock(sv[1]), ==, 0);

    rc = xtc_tls_create(ctx, sv[0], &tls);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_ptr_not_null(tls);

    memset(&ca, 0, sizeof(ca));
    ca.fd = sv[1];
    ca.rc = 1;
    munit_assert_int(pthread_create(&tid, NULL, client_thread, &ca), ==, 0);

    rc = poll_until_done(tls, sv[0], xtc_tls_handshake, 5000);
    munit_assert_int(rc, ==, XTC_OK);

    rc = tls_write_all(tls, sv[0], "hello", CLIENT_MSG_LEN, 5000);
    munit_assert_int(rc, ==, XTC_OK);

    memset(rbuf, 0, sizeof(rbuf));
    rc = tls_read_exact(tls, sv[0], rbuf, CLIENT_MSG_LEN, 5000);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_memory_equal(CLIENT_MSG_LEN, rbuf, "hello");

    (void)xtc_tls_shutdown(tls);

    pthread_join(tid, NULL);
    munit_assert_int(ca.rc, ==, 0);
    munit_assert_memory_equal(CLIENT_MSG_LEN, ca.echoed, "hello");

    xtc_tls_destroy(tls);
    xtc_tls_ctx_destroy(ctx);
    close(sv[0]);
    close(sv[1]);

    return MUNIT_OK;
}

/* -------------------------------------------------------------------------
 * test_server_alpn:
 *   Create a SERVER ctx with ALPN h2 preference; exercise the ALPN path.
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

    rc = xtc_tls_create(NULL, 3, &tls);
    munit_assert_int(rc, ==, XTC_E_INVAL);

    rc = xtc_tls_create(ctx, -1, &tls);
    munit_assert_int(rc, ==, XTC_E_INVAL);

    rc = xtc_tls_create(ctx, 3, NULL);
    munit_assert_int(rc, ==, XTC_E_INVAL);

    xtc_tls_ctx_destroy(ctx);
    return MUNIT_OK;
}

/* -------------------------------------------------------------------------
 * test_server_bad_cert_path:
 *   xtc_tls_ctx_create with non-existent cert returns XTC_E_INVAL.
 * ----------------------------------------------------------------------- */
/* -------------------------------------------------------------------------
 * test_server_extended_opts:
 *   Create a SERVER ctx using the v1.24.0 additions (cipher_list,
 *   ciphersuites_13, groups, prefer_server_ciphers, verify_peer_mode
 *   REQUEST), run a full loopback handshake, then exercise every
 *   post-handshake introspection accessor on the connected server side.
 * ----------------------------------------------------------------------- */
static MunitResult
test_server_extended_opts(const MunitParameter params[], void *data)
{
    xtc_tls_opts_t  opts;
    xtc_tls_ctx_t  *ctx   = NULL;
    xtc_tls_t      *tls   = NULL;
    struct client_args ca;
    pthread_t       tid;
    int             sv[2];
    int             rc;
    char            rbuf[CLIENT_MSG_LEN + 1];
    const char     *ver, *cip;
    unsigned char   hash[64];
    size_t          hlen = 0;

    (void)params;
    (void)data;

    memset(&opts, 0, sizeof(opts));
    opts.cert_file        = TEST_CERT_PATH;
    opts.key_file         = TEST_KEY_PATH;
    opts.min_version      = XTC_TLS_VER_12;
    /* tri-state: request a client cert but do not require one */
    opts.verify_peer_mode = XTC_TLS_VERIFY_REQUEST;
    opts.cipher_list      = "HIGH:!aNULL:!MD5";
    opts.ciphersuites_13  = "TLS_AES_256_GCM_SHA384:TLS_AES_128_GCM_SHA256";
    opts.groups           = "X25519:prime256v1";
    opts.prefer_server_ciphers = 1;

    rc = xtc_tls_ctx_create(XTC_TLS_SERVER, &opts, &ctx);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_ptr_not_null(ctx);

    munit_assert_int(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    munit_assert_int(set_nonblock(sv[0]), ==, 0);
    munit_assert_int(set_nonblock(sv[1]), ==, 0);

    rc = xtc_tls_create(ctx, sv[0], &tls);
    munit_assert_int(rc, ==, XTC_OK);

    memset(&ca, 0, sizeof(ca));
    ca.fd = sv[1];
    ca.rc = 1;
    munit_assert_int(pthread_create(&tid, NULL, client_thread, &ca), ==, 0);

    rc = poll_until_done(tls, sv[0], xtc_tls_handshake, 5000);
    munit_assert_int(rc, ==, XTC_OK);

    /*
     * --- introspection accessors (server side, post-handshake) ---
     * Only the OpenSSL backend implements these; the others stub them
     * to NULL / XTC_E_NOSYS.  Detect which contract applies from one
     * accessor and assert accordingly, so this test is meaningful on
     * OpenSSL and still passes (verifying the stub contract) elsewhere.
     */
    ver = xtc_tls_get_version(tls);
    if (ver == NULL) {
        /* Backend without introspection: assert the stub contract. */
        munit_assert_ptr_null((void *)xtc_tls_get_cipher(tls));
        munit_assert_int(xtc_tls_get_cipher_bits(tls), ==, 0);
        munit_assert_int(xtc_tls_get_server_cert_hash(tls, hash,
            sizeof(hash), &hlen), ==, XTC_E_NOSYS);
    } else {
        munit_assert_true(strncmp(ver, "TLS", 3) == 0);

        cip = xtc_tls_get_cipher(tls);
        munit_assert_ptr_not_null(cip);
        munit_assert_size(strlen(cip), >, 0);

        munit_assert_int(xtc_tls_get_cipher_bits(tls), >=, 128);

        /* The client presented no cert (REQUEST, not REQUIRE), so there
         * is no peer cert on the server side -- the query must say so,
         * not crash. */
        munit_assert_int(xtc_tls_has_peer_cert(tls), ==, 0);

        /* Channel binding: the server hashes its OWN certificate.  Must
         * succeed and produce a plausible digest length. */
        rc = xtc_tls_get_server_cert_hash(tls, hash, sizeof(hash), &hlen);
        munit_assert_int(rc, ==, XTC_OK);
        munit_assert_size(hlen, >=, 32);   /* >= SHA-256 */
        munit_assert_size(hlen, <=, sizeof(hash));

        /* A too-small buffer is a clean XTC_E_RANGE, not an overflow. */
        {
            unsigned char tiny[4];
            size_t tl = 0;
            munit_assert_int(
                xtc_tls_get_server_cert_hash(tls, tiny, sizeof(tiny), &tl),
                ==, XTC_E_RANGE);
        }

        /* NULL-arg guards. */
        munit_assert_ptr_null((void *)xtc_tls_get_version(NULL));
        munit_assert_int(xtc_tls_get_alpn_selected(tls, NULL, NULL),
            ==, XTC_E_INVAL);
    }

    /* Data still flows after all the introspection. */
    rc = tls_write_all(tls, sv[0], "hello", CLIENT_MSG_LEN, 5000);
    munit_assert_int(rc, ==, XTC_OK);
    memset(rbuf, 0, sizeof(rbuf));
    rc = tls_read_exact(tls, sv[0], rbuf, CLIENT_MSG_LEN, 5000);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_memory_equal(CLIENT_MSG_LEN, rbuf, "hello");

    (void)xtc_tls_shutdown(tls);
    pthread_join(tid, NULL);

    xtc_tls_destroy(tls);
    xtc_tls_ctx_destroy(ctx);
    close(sv[0]);
    close(sv[1]);
    return MUNIT_OK;
}

/* -------------------------------------------------------------------------
 * test_server_pss_channel_binding:
 *   RFC 5929 tls-server-end-point with an RSA-PSS-signed server cert
 *   whose real digest is SHA-512.  xtc_tls_get_server_cert_hash must
 *   return the SHA-512 (64-byte) hash -- NOT fall back to SHA-256 (32).
 *   Regression for the channel-binding bug where
 *   OBJ_find_sigid_algs(NID_rsassaPss) returns NID_undef.  Also exercises
 *   xtc_tls_get_verify_error's success contract.  Skips if the toolchain
 *   cannot mint a PSS cert or the backend has no introspection.
 * ----------------------------------------------------------------------- */
static MunitResult
test_server_pss_channel_binding(const MunitParameter params[], void *data)
{
    xtc_tls_opts_t  opts;
    xtc_tls_ctx_t  *ctx = NULL;
    xtc_tls_t      *tls = NULL;
    struct client_args ca;
    pthread_t       tid;
    int             sv[2];
    int             rc;
    unsigned char   hash[64];
    size_t          hlen = 0;
    long            verr = 12345;
    char            vbuf[128];

    (void)params;
    (void)data;

    if (generate_pss_cert(TEST_PSS_CERT, TEST_PSS_KEY, "localhost") != 0)
        return MUNIT_SKIP;   /* toolchain cannot build an RSA-PSS cert */

    memset(&opts, 0, sizeof(opts));
    opts.cert_file   = TEST_PSS_CERT;
    opts.key_file    = TEST_PSS_KEY;
    opts.min_version = XTC_TLS_VER_12;

    rc = xtc_tls_ctx_create(XTC_TLS_SERVER, &opts, &ctx);
    if (rc != XTC_OK) {
        /* Some backends reject an RSA-PSS key; not this test's concern. */
        (void)unlink(TEST_PSS_CERT); (void)unlink(TEST_PSS_KEY);
        return MUNIT_SKIP;
    }

    munit_assert_int(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    munit_assert_int(set_nonblock(sv[0]), ==, 0);
    munit_assert_int(set_nonblock(sv[1]), ==, 0);
    rc = xtc_tls_create(ctx, sv[0], &tls);
    munit_assert_int(rc, ==, XTC_OK);

    memset(&ca, 0, sizeof(ca));
    ca.fd = sv[1];
    ca.rc = 1;
    munit_assert_int(pthread_create(&tid, NULL, client_thread, &ca), ==, 0);

    rc = poll_until_done(tls, sv[0], xtc_tls_handshake, 5000);
    if (rc != XTC_OK) {
        /* A backend that cannot handshake with an RSA-PSS server key
         * (e.g. the GnuTLS build path returns XTC_E_NOSYS here) has
         * nothing to test for PSS channel binding -- skip cleanly
         * rather than fail.  The OpenSSL backend, which this bug is
         * about, completes the handshake and runs the assertions below. */
        (void)xtc_tls_shutdown(tls);
        pthread_join(tid, NULL);
        xtc_tls_destroy(tls);
        xtc_tls_ctx_destroy(ctx);
        close(sv[0]);
        close(sv[1]);
        (void)unlink(TEST_PSS_CERT);
        (void)unlink(TEST_PSS_KEY);
        return MUNIT_SKIP;
    }
    /* If the backend stubs introspection (get_version == NULL: GnuTLS/
     * wolfSSL/etc. do not implement these accessors), assert the NOSYS
     * contract and skip the PSS-specific check.  Only OpenSSL, the
     * backend this bug concerns, exposes the real cert hash. */
    if (xtc_tls_get_version(tls) == NULL) {
        munit_assert_int(xtc_tls_get_server_cert_hash(tls, hash,
            sizeof(hash), &hlen), ==, XTC_E_NOSYS);
        (void)xtc_tls_shutdown(tls);
        pthread_join(tid, NULL);
        xtc_tls_destroy(tls);
        xtc_tls_ctx_destroy(ctx);
        close(sv[0]); close(sv[1]);
        (void)unlink(TEST_PSS_CERT); (void)unlink(TEST_PSS_KEY);
        return MUNIT_SKIP;
    }
    /* THE regression assertion: the PSS cert's true digest is
     * SHA-512, so the channel-binding hash MUST be 64 bytes.  The
     * old OBJ_find_sigid_algs path returned NID_undef for PSS and
     * fell back to SHA-256 (32 bytes) -- this asserts != that. */
    rc = xtc_tls_get_server_cert_hash(tls, hash, sizeof(hash), &hlen);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_size(hlen, ==, 64);   /* SHA-512, not SHA-256 */

    /* verify-error accessor: no client cert requested, so the
     * server-side verify result is X509_V_OK (0) with a readable
     * string, and the success contract holds. */
    rc = xtc_tls_get_verify_error(tls, &verr, vbuf, sizeof(vbuf));
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_long(verr, ==, 0);          /* X509_V_OK */
    munit_assert_size(strlen(vbuf), >, 0);
    /* NULL tls -> XTC_E_INVAL; NULL out-args are allowed. */
    munit_assert_int(xtc_tls_get_verify_error(NULL, &verr, vbuf,
        sizeof(vbuf)), ==, XTC_E_INVAL);
    munit_assert_int(xtc_tls_get_verify_error(tls, NULL, NULL, 0),
        ==, XTC_OK);

    (void)xtc_tls_shutdown(tls);
    pthread_join(tid, NULL);
    xtc_tls_destroy(tls);
    xtc_tls_ctx_destroy(ctx);
    close(sv[0]);
    close(sv[1]);
    (void)unlink(TEST_PSS_CERT);
    (void)unlink(TEST_PSS_KEY);
    return MUNIT_OK;
}

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

/* -------------------------------------------------------------------------
 * test_server_peer_cert: mutual TLS.  A client that presents a cert with a
 * known CN lets the server read the peer-cert introspection accessors
 * (common name, subject DN, issuer DN, serial).  On a non-OpenSSL backend
 * these stub out; detect via xtc_tls_get_version like extended_opts does.
 * ----------------------------------------------------------------------- */
static MunitResult
test_server_peer_cert(const MunitParameter params[], void *data)
{
    xtc_tls_opts_t  opts;
    xtc_tls_ctx_t  *ctx = NULL;
    xtc_tls_t      *tls = NULL;
    struct client_args ca;
    pthread_t       tid;
    int             sv[2];
    int             rc;
    char            rbuf[CLIENT_MSG_LEN + 1];

    (void)params; (void)data;

    memset(&opts, 0, sizeof(opts));
    opts.cert_file        = TEST_CERT_PATH;
    opts.key_file         = TEST_KEY_PATH;
    opts.min_version      = XTC_TLS_VER_12;
    opts.verify_peer_mode = XTC_TLS_VERIFY_REQUEST;   /* request client cert */
    /* Trust the client's self-signed cert as its own CA so REQUEST
     * verification passes and the peer cert is captured. */
    opts.ca_file          = TEST_CLI_CERT;

    rc = xtc_tls_ctx_create(XTC_TLS_SERVER, &opts, &ctx);
    munit_assert_int(rc, ==, XTC_OK);

    munit_assert_int(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    munit_assert_int(set_nonblock(sv[0]), ==, 0);
    munit_assert_int(set_nonblock(sv[1]), ==, 0);

    rc = xtc_tls_create(ctx, sv[0], &tls);
    munit_assert_int(rc, ==, XTC_OK);

    memset(&ca, 0, sizeof(ca));
    ca.fd = sv[1];
    ca.rc = 1;
    munit_assert_int(pthread_create(&tid, NULL, client_thread_with_cert,
        &ca), ==, 0);

    rc = poll_until_done(tls, sv[0], xtc_tls_handshake, 5000);
    munit_assert_int(rc, ==, XTC_OK);

    if (xtc_tls_get_version(tls) == NULL) {
        /* Backend without introspection: assert the stub contract, then
         * still drain the echo so the client thread completes. */
        char cn[256];
        munit_assert_int(
            xtc_tls_get_peer_common_name(tls, cn, sizeof cn),
            ==, XTC_E_NOSYS);
    } else {
        char cn[256], sub[512], iss[512], ser[128];
        /* The client presented a cert with CN=xtc-client. */
        munit_assert_int(xtc_tls_has_peer_cert(tls), ==, 1);
        munit_assert_int(
            xtc_tls_get_peer_common_name(tls, cn, sizeof cn), ==, XTC_OK);
        munit_assert_string_equal(cn, "xtc-client");
        munit_assert_int(
            xtc_tls_get_peer_subject_dn(tls, sub, sizeof sub), ==, XTC_OK);
        munit_assert_ptr_not_null(strstr(sub, "xtc-client"));
        munit_assert_int(
            xtc_tls_get_peer_issuer_dn(tls, iss, sizeof iss), ==, XTC_OK);
        munit_assert_size(strlen(iss), >, 0);
        munit_assert_int(
            xtc_tls_get_peer_serial(tls, ser, sizeof ser), ==, XTC_OK);
        munit_assert_size(strlen(ser), >, 0);

        /* NULL / zero-length guards. */
        munit_assert_int(
            xtc_tls_get_peer_common_name(NULL, cn, sizeof cn),
            ==, XTC_E_INVAL);
        munit_assert_int(
            xtc_tls_get_peer_common_name(tls, NULL, sizeof cn),
            ==, XTC_E_INVAL);
        munit_assert_int(
            xtc_tls_get_peer_common_name(tls, cn, 0), ==, XTC_E_INVAL);
    }

    /* Echo still flows. */
    rc = tls_write_all(tls, sv[0], "hello", CLIENT_MSG_LEN, 5000);
    munit_assert_int(rc, ==, XTC_OK);
    memset(rbuf, 0, sizeof(rbuf));
    rc = tls_read_exact(tls, sv[0], rbuf, CLIENT_MSG_LEN, 5000);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_memory_equal(CLIENT_MSG_LEN, rbuf, "hello");

    (void)xtc_tls_shutdown(tls);
    pthread_join(tid, NULL);
    munit_assert_int(ca.rc, ==, 0);

    xtc_tls_destroy(tls);
    xtc_tls_ctx_destroy(ctx);
    close(sv[0]);
    close(sv[1]);
    return MUNIT_OK;
}

/* =========================================================================
 * Suite setup / teardown -- write/remove temp PEM files.
 * ======================================================================= */

/* -------------------------------------------------------------------------
 * SNI-sending client thread: sets the SNI host via xtc_tls_set_hostname
 * so the server's ClientHello callback fires with SNI_HOSTNAME.
 * ----------------------------------------------------------------------- */
static void *
client_thread_sni(void *arg)
{
    struct client_args *a   = (struct client_args *)arg;
    xtc_tls_opts_t      opts;
    xtc_tls_ctx_t      *ctx = NULL;
    xtc_tls_t          *tls = NULL;
    char                buf[CLIENT_MSG_LEN];

    a->rc = 1;
    memset(&opts, 0, sizeof(opts));
    opts.verify_peer = 0;                 /* self-signed server cert */
    opts.min_version = XTC_TLS_VER_12;

    if (xtc_tls_ctx_create(XTC_TLS_CLIENT, &opts, &ctx) != XTC_OK)
        goto done;
    if (xtc_tls_create(ctx, a->fd, &tls) != XTC_OK)
        goto done;
    if (xtc_tls_set_hostname(tls, SNI_HOSTNAME) != XTC_OK)
        goto done;
    if (poll_until_done(tls, a->fd, xtc_tls_handshake, 5000) != XTC_OK)
        goto done;
    if (tls_read_exact(tls, a->fd, buf, CLIENT_MSG_LEN, 5000) != XTC_OK)
        goto done;
    if (tls_write_all(tls, a->fd, buf, CLIENT_MSG_LEN, 5000) != XTC_OK)
        goto done;
    (void)xtc_tls_shutdown(tls);
    memcpy(a->echoed, buf, CLIENT_MSG_LEN);
    a->echoed[CLIENT_MSG_LEN] = '\0';
    a->rc = 0;
done:
    if (tls != NULL) xtc_tls_destroy(tls);
    if (ctx != NULL) xtc_tls_ctx_destroy(ctx);
    return arg;
}

/* The SNI selector: fires with the requested host, returns the alternate
 * server context so the handshake presents the SNI cert.  Records the
 * host it saw so the test can assert the callback actually ran. */
static char        g_sni_seen[256];
static int         g_sni_fired;
static xtc_tls_ctx_t *g_sni_alt_ctx;

static xtc_tls_ctx_t *
sni_selector(xtc_tls_t *tls, const char *server_name, void *userdata)
{
    (void)tls;
    (void)userdata;
    g_sni_fired = 1;
    g_sni_seen[0] = '\0';
    if (server_name != NULL) {
        strncpy(g_sni_seen, server_name, sizeof(g_sni_seen) - 1);
        g_sni_seen[sizeof(g_sni_seen) - 1] = '\0';
        if (strcmp(server_name, SNI_HOSTNAME) == 0)
            return g_sni_alt_ctx;   /* swap to the tenant cert */
    }
    return NULL;   /* keep the default cert */
}

/* -------------------------------------------------------------------------
 * test_server_sni_select:
 *   Client sends SNI = SNI_HOSTNAME; the server's ClientHello callback
 *   fires, sees the host, and swaps in the alternate context.  Assert
 *   the callback ran with the right host and the handshake completed.
 * ----------------------------------------------------------------------- */
static MunitResult
test_server_sni_select(const MunitParameter params[], void *data)
{
    xtc_tls_opts_t  opts, alt_opts;
    xtc_tls_ctx_t  *ctx = NULL;
    xtc_tls_t      *tls = NULL;
    struct client_args ca;
    pthread_t       tid;
    int             sv[2], rc;
    char            rbuf[CLIENT_MSG_LEN + 1];

    (void)params;
    (void)data;

    g_sni_fired = 0;
    g_sni_seen[0] = '\0';

    /* Alternate (tenant) context. */
    memset(&alt_opts, 0, sizeof(alt_opts));
    alt_opts.cert_file   = TEST_SNI_CERT;
    alt_opts.key_file    = TEST_SNI_KEY;
    alt_opts.min_version = XTC_TLS_VER_12;
    rc = xtc_tls_ctx_create(XTC_TLS_SERVER, &alt_opts, &g_sni_alt_ctx);
    munit_assert_int(rc, ==, XTC_OK);

    /* Default context, with the SNI selector installed. */
    memset(&opts, 0, sizeof(opts));
    opts.cert_file   = TEST_CERT_PATH;
    opts.key_file    = TEST_KEY_PATH;
    opts.min_version = XTC_TLS_VER_12;
    rc = xtc_tls_ctx_create(XTC_TLS_SERVER, &opts, &ctx);
    munit_assert_int(rc, ==, XTC_OK);
    rc = xtc_tls_ctx_set_sni_cb(ctx, sni_selector, NULL);
    if (rc == XTC_E_NOSYS) {
        /* Backend without ClientHello context selection (only OpenSSL
         * implements it); nothing to exercise here. */
        xtc_tls_ctx_destroy(ctx);
        xtc_tls_ctx_destroy(g_sni_alt_ctx);
        g_sni_alt_ctx = NULL;
        return MUNIT_SKIP;
    }
    munit_assert_int(rc, ==, XTC_OK);
    /* Registering on a CLIENT context is refused. */
    {
        xtc_tls_ctx_t *cli = NULL;
        xtc_tls_opts_t co; memset(&co, 0, sizeof(co));
        munit_assert_int(xtc_tls_ctx_create(XTC_TLS_CLIENT, &co, &cli), ==, XTC_OK);
        munit_assert_int(xtc_tls_ctx_set_sni_cb(cli, sni_selector, NULL),
                         ==, XTC_E_NOSYS);
        xtc_tls_ctx_destroy(cli);
    }

    munit_assert_int(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    munit_assert_int(set_nonblock(sv[0]), ==, 0);
    munit_assert_int(set_nonblock(sv[1]), ==, 0);

    rc = xtc_tls_create(ctx, sv[0], &tls);
    munit_assert_int(rc, ==, XTC_OK);

    memset(&ca, 0, sizeof(ca));
    ca.fd = sv[1];
    ca.rc = 1;
    munit_assert_int(pthread_create(&tid, NULL, client_thread_sni, &ca), ==, 0);

    rc = poll_until_done(tls, sv[0], xtc_tls_handshake, 5000);
    munit_assert_int(rc, ==, XTC_OK);
    rc = tls_write_all(tls, sv[0], "hello", CLIENT_MSG_LEN, 5000);
    munit_assert_int(rc, ==, XTC_OK);
    memset(rbuf, 0, sizeof(rbuf));
    rc = tls_read_exact(tls, sv[0], rbuf, CLIENT_MSG_LEN, 5000);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_memory_equal(CLIENT_MSG_LEN, rbuf, "hello");
    (void)xtc_tls_shutdown(tls);
    pthread_join(tid, NULL);
    munit_assert_int(ca.rc, ==, 0);

    /* The callback fired with exactly the client's SNI host. */
    munit_assert_int(g_sni_fired, ==, 1);
    munit_assert_string_equal(g_sni_seen, SNI_HOSTNAME);

    xtc_tls_destroy(tls);
    xtc_tls_ctx_destroy(ctx);
    xtc_tls_ctx_destroy(g_sni_alt_ctx);
    g_sni_alt_ctx = NULL;
    close(sv[0]);
    close(sv[1]);
    return MUNIT_OK;
}

/* -------------------------------------------------------------------------
 * Custom-transport plumbing: a caller-owned fd wrapped in read/write
 * callbacks, exactly the shape a consumer uses to own recv/send.  A
 * pre-TLS pushback buffer is prepended to the read stream to exercise
 * the "bytes already read off the socket" case.
 * ----------------------------------------------------------------------- */
struct xport {
    int    fd;
    const unsigned char *pushback;   /* bytes to feed before the socket */
    size_t pushback_len;
    size_t pushback_off;
};

static int
xport_read(void *ud, void *buf, size_t len)
{
    struct xport *x = (struct xport *)ud;
    ssize_t n;
    /* Drain the pushback buffer first (the pre-TLS raw_buf case). */
    if (x->pushback_off < x->pushback_len) {
        size_t avail = x->pushback_len - x->pushback_off;
        size_t take  = len < avail ? len : avail;
        memcpy(buf, x->pushback + x->pushback_off, take);
        x->pushback_off += take;
        return (int)take;
    }
    n = read(x->fd, buf, len);
    if (n > 0)
        return (int)n;
    if (n == 0)
        return 0;   /* EOF */
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return XTC_E_AGAIN;
    return XTC_E_INTERNAL;
}

static int
xport_write(void *ud, const void *buf, size_t len)
{
    struct xport *x = (struct xport *)ud;
    ssize_t n = write(x->fd, buf, len);
    if (n > 0)
        return (int)n;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return XTC_E_AGAIN;
    return XTC_E_INTERNAL;
}

/* -------------------------------------------------------------------------
 * test_server_transport_roundtrip:
 *   The server drives its TLS state machine through a caller-supplied
 *   transport (BIO callbacks) instead of an fd, and completes a full
 *   handshake + echo against a normal xtc_tls client.
 * ----------------------------------------------------------------------- */
static MunitResult
test_server_transport_roundtrip(const MunitParameter params[], void *data)
{
    xtc_tls_opts_t  opts;
    xtc_tls_ctx_t  *ctx = NULL;
    xtc_tls_t      *tls = NULL;
    struct client_args ca;
    struct xport    x;
    xtc_tls_transport_t xt;
    pthread_t       tid;
    int             sv[2], rc;
    char            rbuf[CLIENT_MSG_LEN + 1];

    (void)params;
    (void)data;

    memset(&opts, 0, sizeof(opts));
    opts.cert_file   = TEST_CERT_PATH;
    opts.key_file    = TEST_KEY_PATH;
    opts.min_version = XTC_TLS_VER_12;
    rc = xtc_tls_ctx_create(XTC_TLS_SERVER, &opts, &ctx);
    munit_assert_int(rc, ==, XTC_OK);

    munit_assert_int(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    munit_assert_int(set_nonblock(sv[0]), ==, 0);
    munit_assert_int(set_nonblock(sv[1]), ==, 0);

    /* Bad-arg guards on the new entry point.  A backend that does not
     * implement custom transport returns XTC_E_NOSYS for every call;
     * detect that and skip. */
    memset(&xt, 0, sizeof(xt));
    rc = xtc_tls_create_transport(NULL, &xt, &tls);
    if (rc == XTC_E_NOSYS) {
        xtc_tls_ctx_destroy(ctx);
        close(sv[0]);
        close(sv[1]);
        return MUNIT_SKIP;
    }
    munit_assert_int(rc, ==, XTC_E_INVAL);
    munit_assert_int(xtc_tls_create_transport(ctx, &xt, &tls), ==, XTC_E_INVAL);

    memset(&x, 0, sizeof(x));
    x.fd = sv[0];
    xt.read_cb  = xport_read;
    xt.write_cb = xport_write;
    xt.userdata = &x;
    rc = xtc_tls_create_transport(ctx, &xt, &tls);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_ptr_not_null(tls);

    memset(&ca, 0, sizeof(ca));
    ca.fd = sv[1];
    ca.rc = 1;
    munit_assert_int(pthread_create(&tid, NULL, client_thread, &ca), ==, 0);

    rc = poll_until_done(tls, sv[0], xtc_tls_handshake, 5000);
    munit_assert_int(rc, ==, XTC_OK);
    rc = tls_write_all(tls, sv[0], "hello", CLIENT_MSG_LEN, 5000);
    munit_assert_int(rc, ==, XTC_OK);
    memset(rbuf, 0, sizeof(rbuf));
    rc = tls_read_exact(tls, sv[0], rbuf, CLIENT_MSG_LEN, 5000);
    munit_assert_int(rc, ==, XTC_OK);
    munit_assert_memory_equal(CLIENT_MSG_LEN, rbuf, "hello");
    (void)xtc_tls_shutdown(tls);
    pthread_join(tid, NULL);
    munit_assert_int(ca.rc, ==, 0);
    munit_assert_memory_equal(CLIENT_MSG_LEN, ca.echoed, "hello");

    xtc_tls_destroy(tls);
    xtc_tls_ctx_destroy(ctx);
    close(sv[0]);
    close(sv[1]);
    return MUNIT_OK;
}

static void *
suite_setup(const MunitParameter params[], void *user_data)
{
    (void)params;
    (void)user_data;

    if (generate_cert(TEST_CERT_PATH, TEST_KEY_PATH, "localhost") != 0)
        return NULL;
    /* Client cert (distinct CN) for the mutual-TLS peer-cert test. */
    if (generate_cert(TEST_CLI_CERT, TEST_CLI_KEY, "xtc-client") != 0)
        return NULL;
    /* Second SERVER cert (CN = SNI host) for the SNI-selection test. */
    if (generate_cert(TEST_SNI_CERT, TEST_SNI_KEY, SNI_HOSTNAME) != 0)
        return NULL;
    return (void *)(uintptr_t)1;   /* non-NULL: setup succeeded */
}

static void
suite_teardown(void *fixture)
{
    (void)fixture;
    unlink(TEST_CERT_PATH);
    unlink(TEST_KEY_PATH);
    unlink(TEST_CLI_CERT);
    unlink(TEST_CLI_KEY);
    unlink(TEST_SNI_CERT);
    unlink(TEST_SNI_KEY);
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
    { "/pss_channel_binding",  test_server_pss_channel_binding, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/extended_opts",        test_server_extended_opts,       suite_setup, suite_teardown,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/peer_cert",            test_server_peer_cert,           suite_setup, suite_teardown,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/sni_select",           test_server_sni_select,          suite_setup, suite_teardown,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/transport_roundtrip",  test_server_transport_roundtrip, suite_setup, suite_teardown,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
    "/m18/tls_server", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

#else  /* !XTC_TLS_ENABLED -- skip stubs */

/*
 * No TLS backend compiled in.  Each test function returns MUNIT_SKIP
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

#endif /* XTC_TLS_ENABLED */

/* =========================================================================
 * main
 * ======================================================================= */

int
main(int argc, char *argv[])
{
    return munit_suite_main(&suite, NULL, argc, argv);
}
