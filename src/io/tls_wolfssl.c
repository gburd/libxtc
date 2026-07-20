/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/io/tls_wolfssl.c
 *	wolfSSL TLS backend for xtc_tls.
 *
 *	Compiled into libxtc.a only when configure sets
 *	XTC_TLS_BACKEND_WOLFSSL=1 (--with-tls=wolfssl).  Implements the
 *	same internal seam as tls_openssl.c using wolfSSL's native API
 *	(the wolfSSL_* family, not the OpenSSL-compat shim): context
 *	create/free, non-blocking handshake (client and server), read,
 *	write, shutdown, and error mapping into XTC_E_*.
 *
 *	Non-blocking discipline:
 *
 *	  The caller's fd is non-blocking.  wolfSSL_set_fd binds it and
 *	  wolfSSL_set_using_nonblock tells wolfSSL not to expect a
 *	  blocking socket.  wolfSSL_negotiate / wolfSSL_read /
 *	  wolfSSL_write then return < 0 with wolfSSL_get_error reporting
 *	  WOLFSSL_ERROR_WANT_READ / WANT_WRITE when the fd stalls; we
 *	  surface XTC_E_AGAIN with wants_read / wants_write set, exactly
 *	  like the OpenSSL backend.  The caller re-drives after polling.
 *
 *	Internal struct layout (private to this file):
 *
 *	  struct xtc_tls_ctx {
 *	      xtc_tls_role_t   role;
 *	      WOLFSSL_CTX     *ctx;
 *	      char            *alpn;     // comma-joined names, or NULL
 *	  };
 *
 *	  struct xtc_tls {
 *	      xtc_tls_ctx_t  *ctx;
 *	      int             fd;
 *	      WOLFSSL        *ssl;
 *	      int             wants_read;
 *	      int             wants_write;
 *	  };
 *
 *	ALPN: xtc_tls_opts.alpn_protos is the OpenSSL wire form
 *	("\x02h2\x08http/1.1").  wolfSSL_UseALPN instead takes a
 *	comma-separated list of names ("h2,http/1.1"), set per-session.
 *	ctx_create decodes the wire form into that comma form once.
 *
 *	s_async note: wolfSSL_Init (called once via pthread_once) is
 *	XTC_BLOCKING_OK -- a one-shot startup initialiser amortised after
 *	the first context creation.  All per-connection paths are
 *	non-blocking and never block the loop.
 */

#include "xtc_int.h"
#include "xtc_tls.h"

#if defined(XTC_TLS_BACKEND_WOLFSSL)

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

/* -------------------------------------------------------------------------
 * Internal struct definitions.
 * ----------------------------------------------------------------------- */

struct xtc_tls_ctx {
	xtc_tls_role_t   role;
	WOLFSSL_CTX     *ctx;
	char            *alpn;   /* comma-joined protocol names; NULL if unset */
};


struct xtc_tls {
	xtc_tls_ctx_t  *ctx;
	int             fd;
	WOLFSSL        *ssl;
	int             wants_read;
	int             wants_write;
};

#include "tls_common.h"   /* after struct xtc_tls: shared want-flag accessors */

/* -------------------------------------------------------------------------
 * One-time library initialisation.
 * ----------------------------------------------------------------------- */

static pthread_once_t s_init_once = PTHREAD_ONCE_INIT;

static void
wolfssl_global_init(void)  /* XTC_BLOCKING_OK: one-shot startup initialiser */
{
	(void)wolfSSL_Init();
}

/* -------------------------------------------------------------------------
 * Map a wolfSSL get_error result into XTC_E_AGAIN (with direction set)
 * or a hard error.
 * ----------------------------------------------------------------------- */

static int
map_want(struct xtc_tls *t, int err)
{
	switch (err) {
	case WOLFSSL_ERROR_WANT_READ:
		t->wants_read = 1;
		return XTC_E_AGAIN;
	case WOLFSSL_ERROR_WANT_WRITE:
		t->wants_write = 1;
		return XTC_E_AGAIN;
	default:
		return XTC_E_INTERNAL;
	}
}

/* -------------------------------------------------------------------------
 * ALPN wire form -> comma-joined name string.  Returns XTC_OK and sets
 * *out (malloc'd, free with __os_free) or NULL when there is nothing to
 * set.
 * ----------------------------------------------------------------------- */

static int
alpn_to_csv(const char *wire, char **out)
{
	const unsigned char *p     = (const unsigned char *)wire;
	size_t               off   = 0;
	size_t               total = 0;
	size_t               count = 0;
	char                *buf;
	size_t               pos = 0;
	int                  rc;

	*out = NULL;

	while (wire[off] != '\0') {
		size_t l = (unsigned char)wire[off];
		if (l == 0)
			return XTC_E_INVAL;
		total += l;
		count++;
		off += 1 + l;
	}
	if (count == 0)
		return XTC_OK;

	/* names + (count-1) commas + NUL */
	if (total > SIZE_MAX - count)        /* overflow guard */
		return XTC_E_INVAL;
	if ((rc = __os_malloc(total + count, (void **)&buf)) != XTC_OK)
		return rc;

	off = 0;
	while (wire[off] != '\0') {
		size_t l = (unsigned char)p[off];
		if (pos != 0)
			buf[pos++] = ',';
		memcpy(buf + pos, p + off + 1, l);
		pos += l;
		off += 1 + l;
	}
	buf[pos] = '\0';
	*out = buf;
	return XTC_OK;
}

/* -------------------------------------------------------------------------
 * Version mapping to wolfSSL's WOLFSSL_TLSV1_x enum.
 * ----------------------------------------------------------------------- */

static int
xtc_ver_to_wolfssl(int v, int *out)
{
	switch (v) {
	case XTC_TLS_VER_12:  *out = WOLFSSL_TLSV1_2; return XTC_OK;
	case XTC_TLS_VER_13:  *out = WOLFSSL_TLSV1_3; return XTC_OK;
	default:              return XTC_E_INVAL;
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
	WOLFSSL_METHOD     *method;
	int                 rc;

	if (out == NULL)
		return XTC_E_INVAL;
	*out = NULL;

	pthread_once(&s_init_once, wolfssl_global_init); /* XTC_BLOCKING_OK */

	if ((rc = __os_calloc(1, sizeof(*c), (void **)&c)) != XTC_OK)
		return rc;

	c->role = role;

	/* Negotiable TLS method: accepts TLS 1.2 and 1.3, narrowed below
	 * by SetMinVersion when min_version is requested. */
	method = (role == XTC_TLS_SERVER)
	    ? wolfSSLv23_server_method()
	    : wolfSSLv23_client_method();
	if (method == NULL) {
		__os_free(c);
		return XTC_E_INTERNAL;
	}

	c->ctx = wolfSSL_CTX_new(method);
	if (c->ctx == NULL) {
		__os_free(c);
		return XTC_E_NOMEM;
	}

	if (opts == NULL) {
		wolfSSL_CTX_set_verify(c->ctx, WOLFSSL_VERIFY_NONE, NULL);
		goto done;
	}

	/* ---- Certificate chain ---- */
	if (opts->cert_file != NULL) {
		if (wolfSSL_CTX_use_certificate_chain_file(c->ctx,
		        opts->cert_file) != WOLFSSL_SUCCESS) {
			rc = XTC_E_INVAL;
			goto fail;
		}
	}

	/* ---- Private key ---- */
	if (opts->key_file != NULL) {
		if (wolfSSL_CTX_use_PrivateKey_file(c->ctx, opts->key_file,
		        WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS) {
			rc = XTC_E_INVAL;
			goto fail;
		}
	}

	/* ---- Minimum TLS version ---- */
	if (opts->min_version != 0) {
		int wv;
		if (xtc_ver_to_wolfssl(opts->min_version, &wv) != XTC_OK) {
			rc = XTC_E_INVAL;
			goto fail;
		}
		if (wolfSSL_CTX_SetMinVersion(c->ctx, wv) != WOLFSSL_SUCCESS) {
			rc = XTC_E_INVAL;
			goto fail;
		}
	}

	/* ---- CA bundle ---- */
	if (opts->ca_file != NULL) {
		if (wolfSSL_CTX_load_verify_locations(c->ctx, opts->ca_file,
		        NULL) != WOLFSSL_SUCCESS) {
			rc = XTC_E_INVAL;
			goto fail;
		}
	}

	/* ---- Peer verification ---- */
	if (opts->verify_peer) {
		int mode = WOLFSSL_VERIFY_PEER;
		if (role == XTC_TLS_SERVER)
			mode |= WOLFSSL_VERIFY_FAIL_IF_NO_PEER_CERT;
		wolfSSL_CTX_set_verify(c->ctx, mode, NULL);
	} else {
		wolfSSL_CTX_set_verify(c->ctx, WOLFSSL_VERIFY_NONE, NULL);
	}

	/* ---- ALPN (decoded to comma form; applied per-session) ---- */
	if (opts->alpn_protos != NULL && opts->alpn_protos[0] != '\0') {
		if ((rc = alpn_to_csv(opts->alpn_protos, &c->alpn)) != XTC_OK)
			goto fail;
	}

done:
	*out = c;
	return XTC_OK;

fail:
	if (c->alpn != NULL)
		__os_free(c->alpn);
	wolfSSL_CTX_free(c->ctx);
	__os_free(c);
	return rc;
}

void
xtc_tls_ctx_destroy(xtc_tls_ctx_t *ctx)
{
	if (ctx == NULL)
		return;
	if (ctx->alpn != NULL)
		__os_free(ctx->alpn);
	if (ctx->ctx != NULL)
		wolfSSL_CTX_free(ctx->ctx);
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
	xtc_tls_clear_wants(t);

	t->ssl = wolfSSL_new(ctx->ctx);
	if (t->ssl == NULL) {
		__os_free(t);
		return XTC_E_NOMEM;
	}

	if (wolfSSL_set_fd(t->ssl, fd) != WOLFSSL_SUCCESS) {
		wolfSSL_free(t->ssl);
		__os_free(t);
		return XTC_E_INTERNAL;
	}

	/* The fd is non-blocking; tell wolfSSL so it returns WANT_* rather
	 * than spinning. */
	wolfSSL_set_using_nonblock(t->ssl, 1);

	/* ALPN: comma-form list, fail handshake on mismatch (matches the
	 * strictness of the OpenSSL server-select callback). */
	if (ctx->alpn != NULL) {
		if (wolfSSL_UseALPN(t->ssl, ctx->alpn,
		        (unsigned int)strlen(ctx->alpn),
		        WOLFSSL_ALPN_CONTINUE_ON_MISMATCH) != WOLFSSL_SUCCESS) {
			wolfSSL_free(t->ssl);
			__os_free(t);
			return XTC_E_INTERNAL;
		}
	}

	if (ctx->role == XTC_TLS_SERVER)
		wolfSSL_set_accept_state(t->ssl);
	else
		wolfSSL_set_connect_state(t->ssl);

	*out = t;
	return XTC_OK;
}

void
xtc_tls_destroy(xtc_tls_t *tls)
{
	if (tls == NULL)
		return;
	if (tls->ssl != NULL)
		wolfSSL_free(tls->ssl);
	__os_free(tls);
}

/* -------------------------------------------------------------------------
 * PUBLIC: int  xtc_tls_handshake __P((xtc_tls_t *));
 * ----------------------------------------------------------------------- */

int
xtc_tls_handshake(xtc_tls_t *tls)
{
	int rc, err;

	if (tls == NULL)
		return XTC_E_INVAL;

	xtc_tls_clear_wants(tls);

	rc = wolfSSL_negotiate(tls->ssl);
	if (rc == WOLFSSL_SUCCESS)
		return XTC_OK;

	err = wolfSSL_get_error(tls->ssl, rc);
	return map_want(tls, err);
}

/* -------------------------------------------------------------------------
 * PUBLIC: int  xtc_tls_read  __P((xtc_tls_t *, void *, size_t, size_t *));
 * PUBLIC: int  xtc_tls_write __P((xtc_tls_t *, const void *, size_t, size_t *));
 * ----------------------------------------------------------------------- */

int
xtc_tls_read(xtc_tls_t *tls, void *buf, size_t buflen, size_t *out_n)
{
	int rc, err;

	if (tls == NULL || buf == NULL || out_n == NULL)
		return XTC_E_INVAL;

	*out_n           = 0;
	xtc_tls_clear_wants(tls);

	rc = wolfSSL_read(tls->ssl, buf, (int)buflen);
	if (rc > 0) {
		*out_n = (size_t)rc;
		return XTC_OK;
	}
	if (rc == 0) {
		/* Clean peer close_notify: treat as EOF. */
		*out_n = 0;
		return XTC_OK;
	}

	err = wolfSSL_get_error(tls->ssl, rc);
	return map_want(tls, err);
}

int
xtc_tls_write(xtc_tls_t *tls, const void *buf, size_t buflen, size_t *out_n)
{
	int rc, err;

	if (tls == NULL || buf == NULL || out_n == NULL)
		return XTC_E_INVAL;

	*out_n           = 0;
	xtc_tls_clear_wants(tls);

	rc = wolfSSL_write(tls->ssl, buf, (int)buflen);
	if (rc > 0) {
		*out_n = (size_t)rc;
		return XTC_OK;
	}

	err = wolfSSL_get_error(tls->ssl, rc);
	return map_want(tls, err);
}

/* -------------------------------------------------------------------------
 * PUBLIC: int  xtc_tls_wants_read  __P((const xtc_tls_t *));
 * PUBLIC: int  xtc_tls_wants_write __P((const xtc_tls_t *));
 * ----------------------------------------------------------------------- */

XTC_TLS_DEFINE_WANTS_ACCESSORS

/* -------------------------------------------------------------------------
 * PUBLIC: int  xtc_tls_shutdown __P((xtc_tls_t *));
 *
 * wolfSSL_shutdown returns WOLFSSL_SUCCESS on a completed bidirectional
 * close, WOLFSSL_SHUTDOWN_NOT_DONE when only our close_notify has been
 * sent (the caller may close the fd now), or < 0 for WANT_* / errors.
 * ----------------------------------------------------------------------- */

int
xtc_tls_shutdown(xtc_tls_t *tls)
{
	int rc, err;

	if (tls == NULL)
		return XTC_E_INVAL;

	xtc_tls_clear_wants(tls);

	rc = wolfSSL_shutdown(tls->ssl);
	if (rc == WOLFSSL_SUCCESS || rc == WOLFSSL_SHUTDOWN_NOT_DONE)
		return XTC_OK;

	err = wolfSSL_get_error(tls->ssl, rc);
	return map_want(tls, err);
}

XTC_TLS_DEFINE_INTROSPECT_STUBS   /* introspection not yet ported to wolfSSL */

#endif /* XTC_TLS_BACKEND_WOLFSSL */
