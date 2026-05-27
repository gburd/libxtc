/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/io/tls_openssl.c
 *	OpenSSL TLS backend for xtc_tls.
 *
 *	This file is always compiled into libxtc.a.  When configure
 *	sets XTC_TLS_BACKEND_OPENSSL=1 the full implementation is
 *	compiled.  When TLS is disabled (--with-tls=none) the #else
 *	branch provides minimal NOSYS stubs so the public symbols remain
 *	present and test_tls_basic links unconditionally.
 *
 *	Internal struct layout (private to this file, OpenSSL branch only):
 *
 *	  struct xtc_tls_ctx {
 *	      xtc_tls_role_t   role;
 *	      SSL_CTX         *ssl_ctx;
 *	      unsigned char   *alpn_protos;     // wire-form copy; NULL if unset
 *	      unsigned int     alpn_protos_len; // byte count
 *	  };
 *
 *	  struct xtc_tls {
 *	      xtc_tls_ctx_t  *ctx;
 *	      int             fd;
 *	      SSL            *ssl;
 *	      int             wants_read;
 *	      int             wants_write;
 *	  };
 *
 *	s_async note: the pthread_once call in xtc_tls_ctx_create is
 *	annotated XTC_BLOCKING_OK because OPENSSL_init_ssl is a one-shot
 *	startup initialiser that is amortised to zero cost after the first
 *	call.  All per-connection paths (handshake, read, write, shutdown)
 *	use non-blocking OpenSSL calls and must never block the event loop.
 */

#include "xtc_int.h"
#include "xtc_tls.h"

#if defined(XTC_TLS_BACKEND_OPENSSL)

#include <pthread.h>
#include <string.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/tls1.h>

/* -------------------------------------------------------------------------
 * Internal struct definitions.
 * ----------------------------------------------------------------------- */

struct xtc_tls_ctx {
	xtc_tls_role_t   role;
	SSL_CTX         *ssl_ctx;
	unsigned char   *alpn_protos;      /* wire-form copy; NULL if unset */
	unsigned int     alpn_protos_len;  /* byte count of alpn_protos */
};

struct xtc_tls {
	xtc_tls_ctx_t   *ctx;
	int              fd;
	SSL             *ssl;
	int              wants_read;
	int              wants_write;
};

/* -------------------------------------------------------------------------
 * One-time library initialisation.
 *
 * OpenSSL 1.1.0+ performs auto-init via constructors, but we call
 * OPENSSL_init_ssl explicitly once so that error strings are loaded
 * before any connection attempt is made.
 * ----------------------------------------------------------------------- */

static pthread_once_t s_init_once = PTHREAD_ONCE_INIT;

static void
openssl_global_init(void)  /* XTC_BLOCKING_OK: one-shot startup initialiser */
{
	(void)OPENSSL_init_ssl(
	    OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS,
	    NULL);
}

/* -------------------------------------------------------------------------
 * ALPN server-side select callback.
 *
 * Registered via SSL_CTX_set_alpn_select_cb when alpn_protos is set.
 * Picks the first protocol in the *server's* ordered list that the
 * client also advertises (server preference).
 * ----------------------------------------------------------------------- */

static int
alpn_select_cb(SSL *ssl,
               const unsigned char **out, unsigned char *outlen,
               const unsigned char *in,  unsigned int   inlen,
               void *arg)
{
	struct xtc_tls_ctx *c = (struct xtc_tls_ctx *)arg;
	(void)ssl;

	if (SSL_select_next_proto((unsigned char **)out, outlen,
	                          c->alpn_protos, c->alpn_protos_len,
	                          in, inlen) == OPENSSL_NPN_NEGOTIATED)
		return SSL_TLSEXT_ERR_OK;

	return SSL_TLSEXT_ERR_NOACK;
}

/* -------------------------------------------------------------------------
 * Version mapping.
 *
 * XTC_TLS_VER_12 == 0x0303 == TLS1_2_VERSION,
 * XTC_TLS_VER_13 == 0x0304 == TLS1_3_VERSION.
 * The constants happen to be identical; the switch makes intent clear.
 * ----------------------------------------------------------------------- */

static int
xtc_ver_to_openssl(int v)
{
	switch (v) {
	case XTC_TLS_VER_12:  return TLS1_2_VERSION;
	case XTC_TLS_VER_13:  return TLS1_3_VERSION;
	default:              return v;   /* pass through unknown versions */
	}
}

/* -------------------------------------------------------------------------
 * PUBLIC: int  xtc_tls_ctx_create __P((xtc_tls_role_t,
 * PUBLIC:                              const xtc_tls_opts_t *,
 * PUBLIC:                              xtc_tls_ctx_t **));
 * PUBLIC: void xtc_tls_ctx_destroy __P((xtc_tls_ctx_t *));
 * ----------------------------------------------------------------------- */

int
xtc_tls_ctx_create(xtc_tls_role_t role,
                   const xtc_tls_opts_t *opts,
                   xtc_tls_ctx_t **out)
{
	struct xtc_tls_ctx *c;
	const SSL_METHOD   *method;
	int                 rc;

	if (out == NULL)
		return XTC_E_INVAL;
	*out = NULL;

	/*
	 * One-time library init.  pthread_once is XTC_BLOCKING_OK: it
	 * is amortised after the first ctx_create call.
	 */
	pthread_once(&s_init_once, openssl_global_init); /* XTC_BLOCKING_OK */

	if ((rc = __os_calloc(1, sizeof(*c), (void **)&c)) != XTC_OK)
		return rc;

	c->role            = role;
	c->ssl_ctx         = NULL;
	c->alpn_protos     = NULL;
	c->alpn_protos_len = 0;

	method = (role == XTC_TLS_SERVER)
	    ? TLS_server_method()
	    : TLS_client_method();

	c->ssl_ctx = SSL_CTX_new(method);
	if (c->ssl_ctx == NULL) {
		__os_free(c);
		return XTC_E_NOMEM;
	}

	if (opts == NULL)
		goto done;

	/* ---- Certificate (PEM chain file) ---- */
	if (opts->cert_file != NULL) {
		if (SSL_CTX_use_certificate_chain_file(c->ssl_ctx,
		                                       opts->cert_file) != 1) {
			SSL_CTX_free(c->ssl_ctx);
			__os_free(c);
			return XTC_E_INVAL;
		}
	}

	/* ---- Private key ---- */
	if (opts->key_file != NULL) {
		if (SSL_CTX_use_PrivateKey_file(c->ssl_ctx,
		                                opts->key_file,
		                                SSL_FILETYPE_PEM) != 1) {
			SSL_CTX_free(c->ssl_ctx);
			__os_free(c);
			return XTC_E_INVAL;
		}
		if (SSL_CTX_check_private_key(c->ssl_ctx) != 1) {
			SSL_CTX_free(c->ssl_ctx);
			__os_free(c);
			return XTC_E_INVAL;
		}
	}

	/* ---- Minimum TLS version ---- */
	if (opts->min_version != 0) {
		int v = xtc_ver_to_openssl(opts->min_version);
		if (SSL_CTX_set_min_proto_version(c->ssl_ctx, v) <= 0) {
			SSL_CTX_free(c->ssl_ctx);
			__os_free(c);
			return XTC_E_INVAL;
		}
	}

	/* ---- Maximum TLS version ---- */
	if (opts->max_version != 0) {
		int v = xtc_ver_to_openssl(opts->max_version);
		if (SSL_CTX_set_max_proto_version(c->ssl_ctx, v) <= 0) {
			SSL_CTX_free(c->ssl_ctx);
			__os_free(c);
			return XTC_E_INVAL;
		}
	}

	/* ---- CA bundle + peer verification ---- */
	if (opts->ca_file != NULL) {
		if (SSL_CTX_load_verify_locations(c->ssl_ctx,
		                                  opts->ca_file,
		                                  NULL) != 1) {
			SSL_CTX_free(c->ssl_ctx);
			__os_free(c);
			return XTC_E_INVAL;
		}
	}

	if (opts->verify_peer) {
		int mode = SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
		SSL_CTX_set_verify(c->ssl_ctx, mode, NULL);
	}

	/* ---- ALPN protocol list ---- */
	if (opts->alpn_protos != NULL && opts->alpn_protos[0] != '\0') {
		size_t        len  = strlen(opts->alpn_protos);
		unsigned char *copy;

		if ((rc = __os_malloc(len, (void **)&copy)) != XTC_OK) {
			SSL_CTX_free(c->ssl_ctx);
			__os_free(c);
			return rc;
		}
		memcpy(copy, opts->alpn_protos, len);
		c->alpn_protos     = copy;
		c->alpn_protos_len = (unsigned int)len;
		SSL_CTX_set_alpn_select_cb(c->ssl_ctx, alpn_select_cb, c);
	}

done:
	*out = c;
	return XTC_OK;
}

void
xtc_tls_ctx_destroy(xtc_tls_ctx_t *ctx)
{
	if (ctx == NULL)
		return;
	if (ctx->alpn_protos != NULL)
		__os_free(ctx->alpn_protos);
	if (ctx->ssl_ctx != NULL)
		SSL_CTX_free(ctx->ssl_ctx);
	__os_free(ctx);
}

/* -------------------------------------------------------------------------
 * PUBLIC: int  xtc_tls_create  __P((xtc_tls_ctx_t *, int, xtc_tls_t **));
 * PUBLIC: void xtc_tls_destroy __P((xtc_tls_t *));
 * ----------------------------------------------------------------------- */

int
xtc_tls_create(xtc_tls_ctx_t *ctx, int fd, xtc_tls_t **out)
{
	struct xtc_tls *t;
	int             rc;

	if (ctx == NULL || fd < 0 || out == NULL)
		return XTC_E_INVAL;
	*out = NULL;

	if ((rc = __os_calloc(1, sizeof(*t), (void **)&t)) != XTC_OK)
		return rc;

	t->ctx         = ctx;
	t->fd          = fd;
	t->wants_read  = 0;
	t->wants_write = 0;

	t->ssl = SSL_new(ctx->ssl_ctx);
	if (t->ssl == NULL) {
		__os_free(t);
		return XTC_E_NOMEM;
	}

	if (SSL_set_fd(t->ssl, fd) != 1) {
		SSL_free(t->ssl);
		__os_free(t);
		return XTC_E_INTERNAL;
	}

	/*
	 * Prime the handshake direction.  For client role (TLS-3),
	 * SSL_set_connect_state will be implemented here.  For now only
	 * the server path is live.
	 */
	if (ctx->role == XTC_TLS_SERVER)
		SSL_set_accept_state(t->ssl);

	*out = t;
	return XTC_OK;
}

void
xtc_tls_destroy(xtc_tls_t *tls)
{
	if (tls == NULL)
		return;
	if (tls->ssl != NULL)
		SSL_free(tls->ssl);
	__os_free(tls);
}

/* -------------------------------------------------------------------------
 * PUBLIC: int  xtc_tls_handshake __P((xtc_tls_t *));
 *
 * Drive one step of the TLS handshake.  Returns XTC_OK when complete.
 * Returns XTC_E_AGAIN with wants_read or wants_write set when more I/O
 * is needed.  Returns XTC_E_INTERNAL on a hard error.
 * ----------------------------------------------------------------------- */

int
xtc_tls_handshake(xtc_tls_t *tls)
{
	int rc, err;

	if (tls == NULL)
		return XTC_E_INVAL;

	tls->wants_read  = 0;
	tls->wants_write = 0;

	rc = SSL_do_handshake(tls->ssl);
	if (rc == 1)
		return XTC_OK;   /* handshake complete */

	err = SSL_get_error(tls->ssl, rc);
	switch (err) {
	case SSL_ERROR_WANT_READ:
		tls->wants_read = 1;
		return XTC_E_AGAIN;
	case SSL_ERROR_WANT_WRITE:
		tls->wants_write = 1;
		return XTC_E_AGAIN;
	default:
		return XTC_E_INTERNAL;
	}
}

/* -------------------------------------------------------------------------
 * PUBLIC: int  xtc_tls_read  __P((xtc_tls_t *, void *, size_t, size_t *));
 * PUBLIC: int  xtc_tls_write __P((xtc_tls_t *, const void *, size_t, size_t *));
 * ----------------------------------------------------------------------- */

int
xtc_tls_read(xtc_tls_t *tls, void *buf, size_t buflen, size_t *out_n)
{
	size_t nread;
	int    rc, err;

	if (tls == NULL || buf == NULL || out_n == NULL)
		return XTC_E_INVAL;

	*out_n           = 0;
	tls->wants_read  = 0;
	tls->wants_write = 0;

	rc = SSL_read_ex(tls->ssl, buf, buflen, &nread);
	if (rc == 1) {
		*out_n = nread;
		return XTC_OK;
	}

	err = SSL_get_error(tls->ssl, rc);
	switch (err) {
	case SSL_ERROR_WANT_READ:
		tls->wants_read = 1;
		return XTC_E_AGAIN;
	case SSL_ERROR_WANT_WRITE:
		tls->wants_write = 1;
		return XTC_E_AGAIN;
	case SSL_ERROR_ZERO_RETURN:
		/* Clean TLS close_notify received: treat as EOF. */
		*out_n = 0;
		return XTC_OK;
	default:
		return XTC_E_INTERNAL;
	}
}

int
xtc_tls_write(xtc_tls_t *tls, const void *buf, size_t buflen, size_t *out_n)
{
	size_t nwritten;
	int    rc, err;

	if (tls == NULL || buf == NULL || out_n == NULL)
		return XTC_E_INVAL;

	*out_n           = 0;
	tls->wants_read  = 0;
	tls->wants_write = 0;

	rc = SSL_write_ex(tls->ssl, buf, buflen, &nwritten);
	if (rc == 1) {
		*out_n = nwritten;
		return XTC_OK;
	}

	err = SSL_get_error(tls->ssl, rc);
	switch (err) {
	case SSL_ERROR_WANT_READ:
		tls->wants_read = 1;
		return XTC_E_AGAIN;
	case SSL_ERROR_WANT_WRITE:
		tls->wants_write = 1;
		return XTC_E_AGAIN;
	default:
		return XTC_E_INTERNAL;
	}
}

/* -------------------------------------------------------------------------
 * PUBLIC: int  xtc_tls_wants_read  __P((const xtc_tls_t *));
 * PUBLIC: int  xtc_tls_wants_write __P((const xtc_tls_t *));
 * ----------------------------------------------------------------------- */

int
xtc_tls_wants_read(const xtc_tls_t *tls)
{
	if (tls == NULL)
		return 0;
	return tls->wants_read;
}

int
xtc_tls_wants_write(const xtc_tls_t *tls)
{
	if (tls == NULL)
		return 0;
	return tls->wants_write;
}

/* -------------------------------------------------------------------------
 * PUBLIC: int  xtc_tls_shutdown __P((xtc_tls_t *));
 *
 * Initiate or continue the bidirectional TLS close_notify shutdown.
 *
 *   rc == 1  bidirectional shutdown complete → XTC_OK
 *   rc == 0  our close_notify sent, peer's not yet received → XTC_OK
 *            (caller may call again to receive peer close_notify, but
 *             the fd can also be closed now without protocol error)
 *   rc < 0   WANT_READ / WANT_WRITE → XTC_E_AGAIN
 *            anything else          → XTC_E_INTERNAL
 * ----------------------------------------------------------------------- */

int
xtc_tls_shutdown(xtc_tls_t *tls)
{
	int rc, err;

	if (tls == NULL)
		return XTC_E_INVAL;

	tls->wants_read  = 0;
	tls->wants_write = 0;

	rc = SSL_shutdown(tls->ssl);
	if (rc >= 0)
		return XTC_OK;   /* rc==1: bidirectional; rc==0: half-done */

	err = SSL_get_error(tls->ssl, rc);
	switch (err) {
	case SSL_ERROR_WANT_READ:
		tls->wants_read = 1;
		return XTC_E_AGAIN;
	case SSL_ERROR_WANT_WRITE:
		tls->wants_write = 1;
		return XTC_E_AGAIN;
	default:
		return XTC_E_INTERNAL;
	}
}

#else  /* !XTC_TLS_BACKEND_OPENSSL -- always-present NOSYS stubs */

/*
 * When configured with --with-tls=none the public symbols must still
 * be present in libxtc.a so that test_tls_basic (and any future caller)
 * can link unconditionally.  Every operation returns XTC_E_NOSYS.
 * Argument validation follows the same rules as the real backend so
 * callers get XTC_E_INVAL on bad inputs even without TLS.
 */

int
xtc_tls_ctx_create(xtc_tls_role_t role,
                   const xtc_tls_opts_t *opts,
                   xtc_tls_ctx_t **out)
{
	(void)role; (void)opts;
	if (out == NULL)
		return XTC_E_INVAL;
	*out = NULL;
	return XTC_E_NOSYS;
}

void
xtc_tls_ctx_destroy(xtc_tls_ctx_t *ctx)
{
	(void)ctx;
}

int
xtc_tls_create(xtc_tls_ctx_t *ctx, int fd, xtc_tls_t **out)
{
	if (ctx == NULL || fd < 0 || out == NULL)
		return XTC_E_INVAL;
	*out = NULL;
	return XTC_E_NOSYS;
}

void
xtc_tls_destroy(xtc_tls_t *tls)
{
	(void)tls;
}

int
xtc_tls_handshake(xtc_tls_t *tls)
{
	if (tls == NULL)
		return XTC_E_INVAL;
	return XTC_E_NOSYS;
}

int
xtc_tls_read(xtc_tls_t *tls, void *buf, size_t buflen, size_t *out_n)
{
	if (tls == NULL || buf == NULL || out_n == NULL)
		return XTC_E_INVAL;
	(void)buflen;
	*out_n = 0;
	return XTC_E_NOSYS;
}

int
xtc_tls_write(xtc_tls_t *tls, const void *buf, size_t buflen, size_t *out_n)
{
	if (tls == NULL || buf == NULL || out_n == NULL)
		return XTC_E_INVAL;
	(void)buflen;
	*out_n = 0;
	return XTC_E_NOSYS;
}

int
xtc_tls_wants_read(const xtc_tls_t *tls)
{
	(void)tls;
	return 0;
}

int
xtc_tls_wants_write(const xtc_tls_t *tls)
{
	(void)tls;
	return 0;
}

int
xtc_tls_shutdown(xtc_tls_t *tls)
{
	if (tls == NULL)
		return XTC_E_INVAL;
	return XTC_E_NOSYS;
}

#endif /* XTC_TLS_BACKEND_OPENSSL */
