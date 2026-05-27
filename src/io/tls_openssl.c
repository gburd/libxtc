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
 *	sets XTC_TLS_BACKEND_OPENSSL=1 the full (skeleton) implementation
 *	is compiled.  When TLS is disabled (--with-tls=none) the #else
 *	branch provides minimal NOSYS stubs so the public symbols remain
 *	present and test_tls_basic links unconditionally.
 *
 *	The skeleton (XTC_TLS_BACKEND_OPENSSL branch) allocates the
 *	internal structs correctly via __os_calloc / __os_free but returns
 *	XTC_E_NOSYS on every call.  TLS-2 will replace these bodies with
 *	real OpenSSL calls (SSL_CTX_new, SSL_new, SSL_do_handshake, etc.).
 *
 *	Internal struct layout (private to this file, OpenSSL branch only):
 *
 *	  struct xtc_tls_ctx {
 *	      xtc_tls_role_t  role;
 *	      void           *ssl_ctx;   // SSL_CTX *  (TLS-2)
 *	  };
 *
 *	  struct xtc_tls {
 *	      xtc_tls_ctx_t  *ctx;
 *	      int             fd;
 *	      void           *ssl;       // SSL *      (TLS-2)
 *	      int             wants_read;
 *	      int             wants_write;
 *	  };
 */

#include "xtc_int.h"
#include "xtc_tls.h"

#if defined(XTC_TLS_BACKEND_OPENSSL)

/* -------------------------------------------------------------------------
 * Internal struct definitions.
 * ----------------------------------------------------------------------- */

struct xtc_tls_ctx {
	xtc_tls_role_t  role;
	void           *ssl_ctx;	/* SSL_CTX * -- filled by TLS-2 */
};

struct xtc_tls {
	xtc_tls_ctx_t  *ctx;
	int             fd;
	void           *ssl;		/* SSL * -- filled by TLS-2 */
	int             wants_read;
	int             wants_write;
};

/* -------------------------------------------------------------------------
 * PUBLIC: int  xtc_tls_ctx_create __P((xtc_tls_role_t,
 * PUBLIC:                              const xtc_tls_opts_t *,
 * PUBLIC:                              xtc_tls_ctx_t **));
 * PUBLIC: void xtc_tls_ctx_destroy __P((xtc_tls_ctx_t *));
 * PUBLIC: int  xtc_tls_create  __P((xtc_tls_ctx_t *, int, xtc_tls_t **));
 * PUBLIC: void xtc_tls_destroy __P((xtc_tls_t *));
 * PUBLIC: int  xtc_tls_handshake  __P((xtc_tls_t *));
 * PUBLIC: int  xtc_tls_read  __P((xtc_tls_t *, void *, size_t, size_t *));
 * PUBLIC: int  xtc_tls_write __P((xtc_tls_t *, const void *, size_t, size_t *));
 * PUBLIC: int  xtc_tls_wants_read __P((const xtc_tls_t *));
 * PUBLIC: int  xtc_tls_wants_write __P((const xtc_tls_t *));
 * PUBLIC: int  xtc_tls_shutdown __P((xtc_tls_t *));
 */

int
xtc_tls_ctx_create(xtc_tls_role_t role,
                   const xtc_tls_opts_t *opts,
                   xtc_tls_ctx_t **out)
{
	struct xtc_tls_ctx *c;
	int rc;

	if (out == NULL)
		return XTC_E_INVAL;
	(void)opts;   /* validated in TLS-2 */

	if ((rc = __os_calloc(1, sizeof(*c), (void **)&c)) != XTC_OK)
		return rc;

	c->role    = role;
	c->ssl_ctx = NULL;   /* TLS-2 fills with SSL_CTX_new(...) */
	*out = c;

	/*
	 * TLS-2 will call SSL_CTX_new, load certificates, and configure
	 * ALPN + version constraints here.  For now we return NOSYS so
	 * callers know TLS is not yet operational, but the allocation and
	 * the destroy pairing below are already correct.
	 */
	__os_free(c);
	*out = NULL;
	return XTC_E_NOSYS;
}

void
xtc_tls_ctx_destroy(xtc_tls_ctx_t *ctx)
{
	if (ctx == NULL)
		return;
	/* TLS-2: SSL_CTX_free(ctx->ssl_ctx); */
	__os_free(ctx);
}

int
xtc_tls_create(xtc_tls_ctx_t *ctx, int fd, xtc_tls_t **out)
{
	struct xtc_tls *t;
	int rc;

	if (ctx == NULL || fd < 0 || out == NULL)
		return XTC_E_INVAL;

	if ((rc = __os_calloc(1, sizeof(*t), (void **)&t)) != XTC_OK)
		return rc;

	t->ctx         = ctx;
	t->fd          = fd;
	t->ssl         = NULL;   /* TLS-2 fills with SSL_new(ctx->ssl_ctx) */
	t->wants_read  = 0;
	t->wants_write = 0;
	*out = t;

	/* Allocate is correct; return NOSYS until TLS-2 fills in the backend. */
	__os_free(t);
	*out = NULL;
	return XTC_E_NOSYS;
}

void
xtc_tls_destroy(xtc_tls_t *tls)
{
	if (tls == NULL)
		return;
	/* TLS-2: SSL_free(tls->ssl); */
	__os_free(tls);
}

int
xtc_tls_handshake(xtc_tls_t *tls)
{
	if (tls == NULL)
		return XTC_E_INVAL;
	/* TLS-2: SSL_do_handshake / SSL_connect / SSL_accept */
	return XTC_E_NOSYS;
}

int
xtc_tls_read(xtc_tls_t *tls, void *buf, size_t buflen, size_t *out_n)
{
	if (tls == NULL || buf == NULL || out_n == NULL)
		return XTC_E_INVAL;
	(void)buflen;
	*out_n = 0;
	/* TLS-2: SSL_read */
	return XTC_E_NOSYS;
}

int
xtc_tls_write(xtc_tls_t *tls, const void *buf, size_t buflen, size_t *out_n)
{
	if (tls == NULL || buf == NULL || out_n == NULL)
		return XTC_E_INVAL;
	(void)buflen;
	*out_n = 0;
	/* TLS-2: SSL_write */
	return XTC_E_NOSYS;
}

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

int
xtc_tls_shutdown(xtc_tls_t *tls)
{
	if (tls == NULL)
		return XTC_E_INVAL;
	/* TLS-2: SSL_shutdown */
	return XTC_E_NOSYS;
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
