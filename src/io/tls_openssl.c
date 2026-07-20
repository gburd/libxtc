/*-
 * Copyright (c) 2026, The XTC Project
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
#include <limits.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/tls1.h>

/* -------------------------------------------------------------------------
 * Portable read/write shim.
 *
 * OpenSSL 1.1.1+ and recent LibreSSL provide SSL_read_ex / SSL_write_ex,
 * which report the byte count through a size_t out-parameter and return
 * 1 on success.  BoringSSL (OPENSSL_IS_BORINGSSL) omits the _ex forms,
 * so there we wrap the classic SSL_read / SSL_write (int-returning,
 * positive == bytes transferred) behind the same 1-on-success, size_t
 * out-parameter contract.  Callers below are flavor-agnostic.
 * ----------------------------------------------------------------------- */
#if defined(OPENSSL_IS_BORINGSSL)
static int
xtc_ssl_read_ex(SSL *ssl, void *buf, size_t len, size_t *readbytes)
{
	int n;
	if (len > INT_MAX)
		len = INT_MAX;
	n = SSL_read(ssl, buf, (int)len);
	if (n > 0) { *readbytes = (size_t)n; return 1; }
	return n;   /* <= 0: caller resolves via SSL_get_error */
}
static int
xtc_ssl_write_ex(SSL *ssl, const void *buf, size_t len, size_t *written)
{
	int n;
	if (len > INT_MAX)
		len = INT_MAX;
	n = SSL_write(ssl, buf, (int)len);
	if (n > 0) { *written = (size_t)n; return 1; }
	return n;
}
#else
#define xtc_ssl_read_ex  SSL_read_ex
#define xtc_ssl_write_ex SSL_write_ex
#endif

/* -------------------------------------------------------------------------
 * Internal struct definitions.
 * ----------------------------------------------------------------------- */

struct xtc_tls_ctx {
	xtc_tls_role_t   role;
	SSL_CTX         *ssl_ctx;
	unsigned char   *alpn_protos;      /* wire-form copy; NULL if unset */
	unsigned int     alpn_protos_len;  /* byte count of alpn_protos */
	xtc_tls_passphrase_cb_t passphrase_cb;   /* NULL if unset */
	void            *passphrase_userdata;
};

struct xtc_tls {
	xtc_tls_ctx_t   *ctx;
	int              fd;
	SSL             *ssl;
	int              wants_read;
	int              wants_write;
};

#include "tls_common.h"   /* after struct xtc_tls: shared want-flag accessors */

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
 * Passphrase callback shim.
 *
 * OpenSSL's pem_password_cb hands us (buf, size, rwflag, userdata); we
 * route to the caller's xtc_tls_passphrase_cb_t.  userdata is the ctx,
 * from which we read the caller's cb + userdata (set via
 * SSL_CTX_set_default_passwd_cb_userdata).  With no caller cb we return
 * 0 (empty passphrase) rather than prompting -- a server must not block
 * on a tty.
 * ----------------------------------------------------------------------- */

static int
passphrase_shim(char *buf, int size, int rwflag, void *userdata)
{
	struct xtc_tls_ctx *c = (struct xtc_tls_ctx *)userdata;
	(void)rwflag;
	if (c == NULL || c->passphrase_cb == NULL)
		return 0;
	return c->passphrase_cb(buf, size, c->passphrase_userdata);
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

	/*
	 * ---- Server-safe hardening (applied unconditionally; documented
	 * defaults, not knobs) ----
	 *   - No TLS compression (CRIME).
	 *   - No renegotiation (a DB server never needs it; disabling it
	 *     removes a DoS / attack surface).  SSL_OP_NO_RENEGOTIATION
	 *     exists on OpenSSL 1.1.0h+; guard it so older headers build.
	 *   - Moving-write-buffer mode: required for correct non-blocking
	 *     SSL_write, where a retry may present the buffer at a new
	 *     address.
	 *   - Session tickets and the session cache off: a server that does
	 *     not resume keeps no such state.
	 */
	SSL_CTX_set_options(c->ssl_ctx, SSL_OP_NO_COMPRESSION);
#ifdef SSL_OP_NO_RENEGOTIATION
	SSL_CTX_set_options(c->ssl_ctx, SSL_OP_NO_RENEGOTIATION);
#endif
	SSL_CTX_set_mode(c->ssl_ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
	SSL_CTX_set_session_cache_mode(c->ssl_ctx, SSL_SESS_CACHE_OFF);
#ifdef SSL_OP_NO_TICKET
	SSL_CTX_set_options(c->ssl_ctx, SSL_OP_NO_TICKET);
#endif

	/* ---- Passphrase callback for an encrypted key (must be set before
	 * the key load below). ---- */
	if (opts->passphrase_cb != NULL) {
		c->passphrase_cb       = opts->passphrase_cb;
		c->passphrase_userdata = opts->passphrase_userdata;
		SSL_CTX_set_default_passwd_cb(c->ssl_ctx, passphrase_shim);
		SSL_CTX_set_default_passwd_cb_userdata(c->ssl_ctx, c);
	}

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
		/*
		 * A server that verifies clients sends the trusted-CA names
		 * in its CertificateRequest so the client can pick a matching
		 * cert (derived from ca_file automatically -- no separate
		 * opts field needed).
		 */
		if (role == XTC_TLS_SERVER) {
			STACK_OF(X509_NAME) *cal =
			    SSL_load_client_CA_file(opts->ca_file);
			if (cal != NULL)
				SSL_CTX_set_client_CA_list(c->ssl_ctx, cal);
		}
	}

	/*
	 * Peer verification.  verify_peer_mode (tri-state) takes precedence
	 * when set to a non-DEFAULT value; otherwise the legacy verify_peer
	 * int decides (0 = none, non-zero = require).
	 */
	{
		xtc_tls_verify_mode_t vm = opts->verify_peer_mode;
		if (vm == XTC_TLS_VERIFY_DEFAULT)
			vm = opts->verify_peer ? XTC_TLS_VERIFY_REQUIRE
			                       : XTC_TLS_VERIFY_NONE;
		if (vm != XTC_TLS_VERIFY_NONE) {
			/*
			 * CLIENT: SSL_VERIFY_PEER makes the client verify the
			 * server certificate.  SERVER: SSL_VERIFY_PEER requests
			 * a client cert; REQUIRE adds FAIL_IF_NO_PEER_CERT so a
			 * client that presents none is rejected.  REQUEST omits
			 * it, so the handshake completes without a client cert
			 * (certificate auth then optional, decided later).
			 */
			int mode = SSL_VERIFY_PEER;
			if (role == XTC_TLS_SERVER &&
			    vm == XTC_TLS_VERIFY_REQUIRE)
				mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
			SSL_CTX_set_verify(c->ssl_ctx, mode, NULL);
		}
	}

	/* ---- Cipher / group selection ---- */
	if (opts->cipher_list != NULL &&
	    SSL_CTX_set_cipher_list(c->ssl_ctx, opts->cipher_list) != 1)
		goto cfg_fail;
#ifdef TLS1_3_VERSION
	if (opts->ciphersuites_13 != NULL &&
	    SSL_CTX_set_ciphersuites(c->ssl_ctx, opts->ciphersuites_13) != 1)
		goto cfg_fail;
#endif
	if (opts->groups != NULL &&
	    SSL_CTX_set1_groups_list(c->ssl_ctx, opts->groups) != 1)
		goto cfg_fail;

	/* ---- CRL-based revocation checking ---- */
	if (opts->crl_file != NULL || opts->crl_dir != NULL) {
		X509_STORE *store = SSL_CTX_get_cert_store(c->ssl_ctx);
		if (store == NULL ||
		    X509_STORE_load_locations(store, opts->crl_file,
		                              opts->crl_dir) != 1)
			goto cfg_fail;
		X509_STORE_set_flags(store,
		    X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
	}

	/* ---- Server cipher preference ---- */
	if (opts->prefer_server_ciphers)
		SSL_CTX_set_options(c->ssl_ctx,
		    SSL_OP_CIPHER_SERVER_PREFERENCE);

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

		if (role == XTC_TLS_SERVER) {
			/* Server: pick protocol from client's advertised list. */
			SSL_CTX_set_alpn_select_cb(c->ssl_ctx, alpn_select_cb, c);
		} else {
			/*
			 * Client: offer the wire-form list to the server.
			 * SSL_CTX_set_alpn_protos returns 0 on success
			 * (atypical OpenSSL convention).
			 */
			if (SSL_CTX_set_alpn_protos(c->ssl_ctx, copy,
			                            (unsigned int)len) != 0) {
				SSL_CTX_free(c->ssl_ctx);
				__os_free(c);
				return XTC_E_INVAL;
			}
		}
	}

done:
	*out = c;
	return XTC_OK;

cfg_fail:
	if (c->alpn_protos != NULL)
		__os_free(c->alpn_protos);
	SSL_CTX_free(c->ssl_ctx);
	__os_free(c);
	return XTC_E_INVAL;
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
	xtc_tls_clear_wants(t);

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
	 * Prime the handshake direction.  SSL_do_handshake consults the
	 * state set here to know whether to act as client or server.
	 */
	if (ctx->role == XTC_TLS_SERVER)
		SSL_set_accept_state(t->ssl);
	else
		SSL_set_connect_state(t->ssl);

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

	xtc_tls_clear_wants(tls);

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
	xtc_tls_clear_wants(tls);

	rc = xtc_ssl_read_ex(tls->ssl, buf, buflen, &nread);
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
	xtc_tls_clear_wants(tls);

	rc = xtc_ssl_write_ex(tls->ssl, buf, buflen, &nwritten);
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

XTC_TLS_DEFINE_WANTS_ACCESSORS

/* -------------------------------------------------------------------------
 * PUBLIC: int  xtc_tls_shutdown __P((xtc_tls_t *));
 *
 * Initiate or continue the bidirectional TLS close_notify shutdown.
 *
 *   rc == 1  bidirectional shutdown complete -> XTC_OK
 *   rc == 0  our close_notify sent, peer's not yet received -> XTC_OK
 *            (caller may call again to receive peer close_notify, but
 *             the fd can also be closed now without protocol error)
 *   rc < 0   WANT_READ / WANT_WRITE -> XTC_E_AGAIN
 *            anything else          -> XTC_E_INTERNAL
 * ----------------------------------------------------------------------- */

int
xtc_tls_shutdown(xtc_tls_t *tls)
{
	int rc, err;

	if (tls == NULL)
		return XTC_E_INVAL;

	xtc_tls_clear_wants(tls);

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

/* -------------------------------------------------------------------------
 * Post-handshake introspection.
 *
 * PUBLIC: const char *xtc_tls_get_version __P((const xtc_tls_t *));
 * PUBLIC: const char *xtc_tls_get_cipher __P((const xtc_tls_t *));
 * PUBLIC: int  xtc_tls_get_cipher_bits __P((const xtc_tls_t *));
 * PUBLIC: int  xtc_tls_get_alpn_selected __P((const xtc_tls_t *,
 * PUBLIC:                              const unsigned char **, unsigned int *));
 * PUBLIC: int  xtc_tls_has_peer_cert __P((const xtc_tls_t *));
 * PUBLIC: int  xtc_tls_get_peer_subject_dn __P((const xtc_tls_t *, char *, size_t));
 * PUBLIC: int  xtc_tls_get_peer_common_name __P((const xtc_tls_t *, char *, size_t));
 * PUBLIC: int  xtc_tls_get_peer_issuer_dn __P((const xtc_tls_t *, char *, size_t));
 * PUBLIC: int  xtc_tls_get_peer_serial __P((const xtc_tls_t *, char *, size_t));
 * PUBLIC: int  xtc_tls_get_server_cert_hash __P((const xtc_tls_t *,
 * PUBLIC:                              unsigned char *, size_t, size_t *));
 * ----------------------------------------------------------------------- */

const char *
xtc_tls_get_version(const xtc_tls_t *tls)
{
	return (tls != NULL && tls->ssl != NULL)
	    ? SSL_get_version(tls->ssl) : NULL;
}

const char *
xtc_tls_get_cipher(const xtc_tls_t *tls)
{
	const SSL_CIPHER *ci;
	if (tls == NULL || tls->ssl == NULL)
		return NULL;
	ci = SSL_get_current_cipher(tls->ssl);
	return ci != NULL ? SSL_CIPHER_get_name(ci) : NULL;
}

int
xtc_tls_get_cipher_bits(const xtc_tls_t *tls)
{
	const SSL_CIPHER *ci;
	int bits = 0;
	if (tls == NULL || tls->ssl == NULL)
		return 0;
	ci = SSL_get_current_cipher(tls->ssl);
	if (ci != NULL)
		(void)SSL_CIPHER_get_bits(ci, &bits);
	return bits;
}

int
xtc_tls_get_alpn_selected(const xtc_tls_t *tls,
    const unsigned char **out, unsigned int *len)
{
	if (tls == NULL || tls->ssl == NULL || out == NULL || len == NULL)
		return XTC_E_INVAL;
	*out = NULL;
	*len = 0;
	SSL_get0_alpn_selected(tls->ssl, out, len);
	return (*out != NULL && *len > 0) ? XTC_OK : XTC_E_NOTFOUND;
}

int
xtc_tls_has_peer_cert(const xtc_tls_t *tls)
{
	X509 *cert;
	if (tls == NULL || tls->ssl == NULL)
		return 0;
	if (SSL_get_verify_result(tls->ssl) != X509_V_OK)
		return 0;
	cert = SSL_get_peer_certificate(tls->ssl);
	if (cert == NULL)
		return 0;
	X509_free(cert);
	return 1;
}

/*
 * Render an X509_NAME into buf as an RFC 2253 string, rejecting an
 * embedded NUL (the CVE-2009-4034 truncation class).  Returns XTC_OK,
 * XTC_E_RANGE (buf too small), XTC_E_INVAL (embedded NUL / bad args),
 * or XTC_E_INTERNAL.
 */
static int
name_to_rfc2253(X509_NAME *nm, char *buf, size_t len)
{
	BIO *bio;
	long n;
	char *data = NULL;
	int rc = XTC_OK;

	if (nm == NULL || buf == NULL || len == 0)
		return XTC_E_INVAL;
	bio = BIO_new(BIO_s_mem());
	if (bio == NULL)
		return XTC_E_NOMEM;
	if (X509_NAME_print_ex(bio, nm, 0, XN_FLAG_RFC2253) < 0) {
		BIO_free(bio);
		return XTC_E_INTERNAL;
	}
	n = BIO_get_mem_data(bio, &data);
	if (n < 0 || data == NULL) {
		BIO_free(bio);
		return XTC_E_INTERNAL;
	}
	if (memchr(data, '\0', (size_t)n) != NULL)   /* embedded NUL */
		rc = XTC_E_INVAL;
	else if ((size_t)n + 1 > len)
		rc = XTC_E_RANGE;
	else {
		memcpy(buf, data, (size_t)n);
		buf[n] = '\0';
	}
	BIO_free(bio);
	return rc;
}

static int
peer_name_dn(const xtc_tls_t *tls, int issuer, char *buf, size_t len)
{
	X509 *cert;
	X509_NAME *nm;
	int rc;
	if (tls == NULL || tls->ssl == NULL || buf == NULL || len == 0)
		return XTC_E_INVAL;
	cert = SSL_get_peer_certificate(tls->ssl);
	if (cert == NULL)
		return XTC_E_NOTFOUND;
	nm = issuer ? X509_get_issuer_name(cert)
	            : X509_get_subject_name(cert);
	rc = name_to_rfc2253(nm, buf, len);
	X509_free(cert);
	return rc;
}

int
xtc_tls_get_peer_subject_dn(const xtc_tls_t *tls, char *buf, size_t len)
{
	return peer_name_dn(tls, 0, buf, len);
}

int
xtc_tls_get_peer_issuer_dn(const xtc_tls_t *tls, char *buf, size_t len)
{
	return peer_name_dn(tls, 1, buf, len);
}

int
xtc_tls_get_peer_common_name(const xtc_tls_t *tls, char *buf, size_t len)
{
	X509 *cert;
	X509_NAME *nm;
	int idx, dlen, rc = XTC_OK;
	X509_NAME_ENTRY *e;
	ASN1_STRING *as;
	unsigned char *utf8 = NULL;

	if (tls == NULL || tls->ssl == NULL || buf == NULL || len == 0)
		return XTC_E_INVAL;
	cert = SSL_get_peer_certificate(tls->ssl);
	if (cert == NULL)
		return XTC_E_NOTFOUND;
	nm = X509_get_subject_name(cert);
	idx = X509_NAME_get_index_by_NID(nm, NID_commonName, -1);
	if (idx < 0) { X509_free(cert); return XTC_E_NOTFOUND; }
	e = X509_NAME_get_entry(nm, idx);
	as = X509_NAME_ENTRY_get_data(e);
	if (as == NULL) { X509_free(cert); return XTC_E_NOTFOUND; }
	dlen = ASN1_STRING_to_UTF8(&utf8, as);
	if (dlen < 0 || utf8 == NULL) { X509_free(cert); return XTC_E_INTERNAL; }
	if (memchr(utf8, '\0', (size_t)dlen) != NULL)   /* embedded NUL */
		rc = XTC_E_INVAL;
	else if ((size_t)dlen + 1 > len)
		rc = XTC_E_RANGE;
	else {
		memcpy(buf, utf8, (size_t)dlen);
		buf[dlen] = '\0';
	}
	OPENSSL_free(utf8);
	X509_free(cert);
	return rc;
}

int
xtc_tls_get_peer_serial(const xtc_tls_t *tls, char *buf, size_t len)
{
	X509 *cert;
	ASN1_INTEGER *ai;
	BIGNUM *bn;
	char *dec;
	int rc = XTC_OK;

	if (tls == NULL || tls->ssl == NULL || buf == NULL || len == 0)
		return XTC_E_INVAL;
	cert = SSL_get_peer_certificate(tls->ssl);
	if (cert == NULL)
		return XTC_E_NOTFOUND;
	ai = X509_get_serialNumber(cert);
	bn = (ai != NULL) ? ASN1_INTEGER_to_BN(ai, NULL) : NULL;
	if (bn == NULL) { X509_free(cert); return XTC_E_INTERNAL; }
	dec = BN_bn2dec(bn);
	if (dec == NULL) { BN_free(bn); X509_free(cert); return XTC_E_INTERNAL; }
	if (strlen(dec) + 1 > len)
		rc = XTC_E_RANGE;
	else
		memcpy(buf, dec, strlen(dec) + 1);
	OPENSSL_free(dec);
	BN_free(bn);
	X509_free(cert);
	return rc;
}

int
xtc_tls_get_server_cert_hash(const xtc_tls_t *tls,
    unsigned char *buf, size_t buflen, size_t *out_len)
{
	X509 *cert;
	const EVP_MD *md;
	int sig_nid = 0, md_nid = 0;
	unsigned int dlen = 0;

	if (tls == NULL || tls->ssl == NULL || buf == NULL || out_len == NULL)
		return XTC_E_INVAL;
	/*
	 * The LOCAL certificate: tls-server-end-point binds to the cert
	 * the server presented, which on the server side is our own cert
	 * and on the client side is the peer's.
	 */
	if (tls->ctx != NULL && tls->ctx->role == XTC_TLS_SERVER) {
		cert = SSL_get_certificate(tls->ssl);
		if (cert != NULL)
			X509_up_ref(cert);
	} else {
		cert = SSL_get_peer_certificate(tls->ssl);
	}
	if (cert == NULL)
		return XTC_E_NOTFOUND;

	/*
	 * RFC 5929 tls-server-end-point: hash with the certificate's
	 * signature-algorithm digest, EXCEPT MD5 or SHA-1 are upgraded to
	 * SHA-256.  Fall back to SHA-256 if the signature digest is unknown.
	 */
	if (X509_get_signature_info(cert, &md_nid, NULL, NULL, NULL) != 1)
		md_nid = NID_undef;
	sig_nid = md_nid;
	if (sig_nid == NID_undef || sig_nid == NID_md5 || sig_nid == NID_sha1)
		md = EVP_sha256();
	else
		md = EVP_get_digestbynid(sig_nid);
	if (md == NULL)
		md = EVP_sha256();

	if (buflen < (size_t)EVP_MD_size(md)) {
		X509_free(cert);
		return XTC_E_RANGE;
	}
	if (X509_digest(cert, md, buf, &dlen) != 1) {
		X509_free(cert);
		return XTC_E_INTERNAL;
	}
	*out_len = dlen;
	X509_free(cert);
	return XTC_OK;
}

#endif /* XTC_TLS_BACKEND_OPENSSL */
