/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/io/tls_mbedtls.c
 *	mbedTLS TLS backend for xtc_tls.
 *
 *	Compiled into libxtc.a only when configure sets
 *	XTC_TLS_BACKEND_MBEDTLS=1 (--with-tls=mbedtls).  Implements the
 *	same internal seam as tls_openssl.c: context create/free,
 *	non-blocking handshake (client and server), read, write,
 *	shutdown, and error mapping into XTC_E_*.
 *
 *	Non-blocking discipline:
 *
 *	  mbedTLS performs all transport via the BIO callbacks installed
 *	  with mbedtls_ssl_set_bio.  We install bio_send / bio_recv that
 *	  wrap a non-blocking read(2)/write(2) on the caller's fd and
 *	  translate EAGAIN/EWOULDBLOCK into MBEDTLS_ERR_SSL_WANT_WRITE /
 *	  MBEDTLS_ERR_SSL_WANT_READ.  mbedtls_ssl_handshake / read / write
 *	  then return those WANT codes, which we surface as XTC_E_AGAIN
 *	  with wants_read / wants_write set, exactly like the OpenSSL
 *	  backend's SSL_ERROR_WANT_READ / WANT_WRITE handling.  The caller
 *	  re-drives the operation after polling on the indicated direction.
 *
 *	Internal struct layout (private to this file):
 *
 *	  struct xtc_tls_ctx {
 *	      xtc_tls_role_t            role;
 *	      mbedtls_ssl_config        conf;
 *	      mbedtls_x509_crt          cert;       // own cert chain
 *	      mbedtls_pk_context        pkey;       // own private key
 *	      mbedtls_x509_crt          ca;         // CA bundle for verify
 *	      mbedtls_ctr_drbg_context  drbg;
 *	      mbedtls_entropy_context   entropy;
 *	      int                       have_cert;  // own_cert installed
 *	      int                       have_ca;    // ca_chain installed
 *	      char                    **alpn;       // NULL-terminated, or NULL
 *	  };
 *
 *	  struct xtc_tls {
 *	      xtc_tls_ctx_t       *ctx;
 *	      int                  fd;
 *	      mbedtls_ssl_context  ssl;
 *	      int                  wants_read;
 *	      int                  wants_write;
 *	  };
 *
 *	ALPN: xtc_tls_opts.alpn_protos is the OpenSSL wire form
 *	("\x02h2\x08http/1.1").  mbedTLS instead wants a NULL-terminated
 *	array of NUL-terminated C strings.  ctx_create decodes the wire
 *	form into that array once; the array is freed in ctx_destroy.
 *
 *	s_async note: ctr_drbg seeding in ctx_create draws from the
 *	entropy source once per context.  It is annotated XTC_BLOCKING_OK
 *	because it runs only at context-setup time, never on a
 *	per-connection hot path.  All per-connection paths (handshake,
 *	read, write, shutdown) use non-blocking transport callbacks and
 *	never block the event loop.
 */

#include "xtc_int.h"
#include "xtc_tls.h"

#if defined(XTC_TLS_BACKEND_MBEDTLS)

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>

/* -------------------------------------------------------------------------
 * Internal struct definitions.
 * ----------------------------------------------------------------------- */

struct xtc_tls_ctx {
	xtc_tls_role_t            role;
	mbedtls_ssl_config        conf;
	mbedtls_x509_crt          cert;
	mbedtls_pk_context        pkey;
	mbedtls_x509_crt          ca;
	mbedtls_ctr_drbg_context  drbg;
	mbedtls_entropy_context   entropy;
	int                       have_cert;
	int                       have_ca;
	char                    **alpn;
};


struct xtc_tls {
	xtc_tls_ctx_t       *ctx;
	int                  fd;
	mbedtls_ssl_context  ssl;
	int                  wants_read;
	int                  wants_write;
};

#include "tls_common.h"   /* after struct xtc_tls: shared want-flag accessors */

/* -------------------------------------------------------------------------
 * Transport BIO callbacks.
 *
 * mbedTLS calls these for all wire I/O.  We implement them against the
 * caller's non-blocking fd.  On EAGAIN/EWOULDBLOCK we return the
 * matching WANT code so mbedtls_ssl_* propagate it back to us.
 * ----------------------------------------------------------------------- */

static int
bio_send(void *p, const unsigned char *buf, size_t len)
{
	struct xtc_tls *t = (struct xtc_tls *)p;
	ssize_t         n;

	n = write(t->fd, buf, len);
	if (n >= 0)
		return (int)n;
	if (errno == EAGAIN || errno == EWOULDBLOCK)
		return MBEDTLS_ERR_SSL_WANT_WRITE;
	if (errno == EINTR)
		return MBEDTLS_ERR_SSL_WANT_WRITE;
	return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int
bio_recv(void *p, unsigned char *buf, size_t len)
{
	struct xtc_tls *t = (struct xtc_tls *)p;
	ssize_t         n;

	n = read(t->fd, buf, len);
	if (n > 0)
		return (int)n;
	if (n == 0)
		return 0;   /* EOF: mbedTLS treats 0 from recv as conn close */
	if (errno == EAGAIN || errno == EWOULDBLOCK)
		return MBEDTLS_ERR_SSL_WANT_READ;
	if (errno == EINTR)
		return MBEDTLS_ERR_SSL_WANT_READ;
	return MBEDTLS_ERR_NET_RECV_FAILED;
}

/* -------------------------------------------------------------------------
 * Map a non-WANT mbedTLS return into XTC_E_AGAIN (with direction set) or
 * a hard error.  Returns XTC_OK only if rc indicates no error condition;
 * callers handle rc >= 0 themselves before calling this.
 * ----------------------------------------------------------------------- */

static int
map_want(struct xtc_tls *t, int rc)
{
	switch (rc) {
	case MBEDTLS_ERR_SSL_WANT_READ:
		t->wants_read = 1;
		return XTC_E_AGAIN;
	case MBEDTLS_ERR_SSL_WANT_WRITE:
		t->wants_write = 1;
		return XTC_E_AGAIN;
	default:
		return XTC_E_INTERNAL;
	}
}

/* -------------------------------------------------------------------------
 * ALPN wire-form -> NULL-terminated C-string array.
 *
 * Input is the OpenSSL wire encoding: a sequence of (len byte, len
 * bytes of protocol name).  We allocate an array of (count + 1)
 * char* and a NUL-terminated copy of each name.  Returns XTC_OK and
 * sets *out (which must be freed with alpn_free), or an error.
 * ----------------------------------------------------------------------- */

static int
alpn_decode(const char *wire, char ***out)
{
	const unsigned char *p   = (const unsigned char *)wire;
	size_t               off = 0;
	size_t               count = 0;
	char               **arr;
	int                  rc;
	size_t               i;

	*out = NULL;

	/* First pass: count protocols and validate framing. */
	while (wire[off] != '\0') {
		size_t l = (unsigned char)wire[off];
		if (l == 0)
			return XTC_E_INVAL;
		off += 1 + l;
		count++;
	}
	if (count == 0)
		return XTC_OK;   /* nothing to do */

	if ((rc = __os_calloc(count + 1, sizeof(char *),
	                      (void **)&arr)) != XTC_OK)
		return rc;

	off = 0;
	for (i = 0; i < count; i++) {
		size_t l = (unsigned char)p[off];
		char  *name;
		/* l is a single byte, so l + 1 cannot overflow SIZE_MAX. */
		if ((rc = __os_malloc(l + 1, (void **)&name)) != XTC_OK) {
			size_t j;
			for (j = 0; j < i; j++)
				__os_free(arr[j]);
			__os_free(arr);
			return rc;
		}
		memcpy(name, p + off + 1, l);
		name[l] = '\0';
		arr[i] = name;
		off += 1 + l;
	}
	arr[count] = NULL;
	*out = arr;
	return XTC_OK;
}

static void
alpn_free(char **arr)
{
	size_t i;
	if (arr == NULL)
		return;
	for (i = 0; arr[i] != NULL; i++)
		__os_free(arr[i]);
	__os_free(arr);
}

/* -------------------------------------------------------------------------
 * Version mapping.  XTC_TLS_VER_12 / _13 share the wire values mbedTLS
 * uses (0x0303 / 0x0304); the switch makes intent explicit and rejects
 * unknown versions.
 * ----------------------------------------------------------------------- */

static int
xtc_ver_to_mbedtls(int v, mbedtls_ssl_protocol_version *out)
{
	switch (v) {
	case XTC_TLS_VER_12:
		*out = MBEDTLS_SSL_VERSION_TLS1_2;
		return XTC_OK;
	case XTC_TLS_VER_13:
		*out = MBEDTLS_SSL_VERSION_TLS1_3;
		return XTC_OK;
	default:
		return XTC_E_INVAL;
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
	int                 rc;
	int                 endpoint;

	if (out == NULL)
		return XTC_E_INVAL;
	*out = NULL;

	if ((rc = __os_calloc(1, sizeof(*c), (void **)&c)) != XTC_OK)
		return rc;

	c->role = role;
	mbedtls_ssl_config_init(&c->conf);
	mbedtls_x509_crt_init(&c->cert);
	mbedtls_pk_init(&c->pkey);
	mbedtls_x509_crt_init(&c->ca);
	mbedtls_ctr_drbg_init(&c->drbg);
	mbedtls_entropy_init(&c->entropy);

	/*
	 * Seed the deterministic RBG from the entropy source.  This is
	 * the one setup-time operation that may touch the OS RNG; it is
	 * XTC_BLOCKING_OK because it happens only at context creation.
	 */
	rc = mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, /* XTC_BLOCKING_OK */
	                           &c->entropy,
	                           (const unsigned char *)"xtc_tls", 7);
	if (rc != 0) {
		rc = XTC_E_INTERNAL;
		goto fail;
	}

	endpoint = (role == XTC_TLS_SERVER)
	    ? MBEDTLS_SSL_IS_SERVER
	    : MBEDTLS_SSL_IS_CLIENT;

	if (mbedtls_ssl_config_defaults(&c->conf, endpoint,
	                                MBEDTLS_SSL_TRANSPORT_STREAM,
	                                MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
		rc = XTC_E_INTERNAL;
		goto fail;
	}

	mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);

	if (opts == NULL) {
		/*
		 * No options: default to no peer verification so an
		 * un-configured client/server still completes a handshake
		 * (matching the OpenSSL backend's "verify off unless asked").
		 */
		mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
		goto done;
	}

	/* ---- Certificate chain ---- */
	if (opts->cert_file != NULL) {
		if (mbedtls_x509_crt_parse_file(&c->cert,
		                                opts->cert_file) != 0) {
			rc = XTC_E_INVAL;
			goto fail;
		}
		c->have_cert = 1;
	}

	/* ---- Private key ---- */
	if (opts->key_file != NULL) {
		if (mbedtls_pk_parse_keyfile(&c->pkey, opts->key_file, NULL,
		                             mbedtls_ctr_drbg_random,
		                             &c->drbg) != 0) {
			rc = XTC_E_INVAL;
			goto fail;
		}
		if (mbedtls_ssl_conf_own_cert(&c->conf, &c->cert,
		                              &c->pkey) != 0) {
			rc = XTC_E_INVAL;
			goto fail;
		}
	}

	/* ---- Minimum / maximum TLS version ---- */
	if (opts->min_version != 0) {
		mbedtls_ssl_protocol_version v;
		if (xtc_ver_to_mbedtls(opts->min_version, &v) != XTC_OK) {
			rc = XTC_E_INVAL;
			goto fail;
		}
		mbedtls_ssl_conf_min_tls_version(&c->conf, v);
	}
	if (opts->max_version != 0) {
		mbedtls_ssl_protocol_version v;
		if (xtc_ver_to_mbedtls(opts->max_version, &v) != XTC_OK) {
			rc = XTC_E_INVAL;
			goto fail;
		}
		mbedtls_ssl_conf_max_tls_version(&c->conf, v);
	}

	/* ---- CA bundle + peer verification ---- */
	if (opts->ca_file != NULL) {
		if (mbedtls_x509_crt_parse_file(&c->ca,
		                                opts->ca_file) != 0) {
			rc = XTC_E_INVAL;
			goto fail;
		}
		mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
		c->have_ca = 1;
	}

	mbedtls_ssl_conf_authmode(&c->conf,
	    opts->verify_peer ? MBEDTLS_SSL_VERIFY_REQUIRED
	                      : MBEDTLS_SSL_VERIFY_NONE);

	/* ---- ALPN protocol list ---- */
	if (opts->alpn_protos != NULL && opts->alpn_protos[0] != '\0') {
		if ((rc = alpn_decode(opts->alpn_protos, &c->alpn)) != XTC_OK)
			goto fail;
		if (c->alpn != NULL &&
		    mbedtls_ssl_conf_alpn_protocols(&c->conf,
		        (const char **)c->alpn) != 0) {
			rc = XTC_E_INVAL;
			goto fail;
		}
	}

done:
	*out = c;
	return XTC_OK;

fail:
	mbedtls_entropy_free(&c->entropy);
	mbedtls_ctr_drbg_free(&c->drbg);
	mbedtls_x509_crt_free(&c->ca);
	mbedtls_pk_free(&c->pkey);
	mbedtls_x509_crt_free(&c->cert);
	mbedtls_ssl_config_free(&c->conf);
	alpn_free(c->alpn);
	__os_free(c);
	return rc;
}

void
xtc_tls_ctx_destroy(xtc_tls_ctx_t *ctx)
{
	if (ctx == NULL)
		return;
	alpn_free(ctx->alpn);
	mbedtls_entropy_free(&ctx->entropy);
	mbedtls_ctr_drbg_free(&ctx->drbg);
	mbedtls_x509_crt_free(&ctx->ca);
	mbedtls_pk_free(&ctx->pkey);
	mbedtls_x509_crt_free(&ctx->cert);
	mbedtls_ssl_config_free(&ctx->conf);
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
	mbedtls_ssl_init(&t->ssl);

	if (mbedtls_ssl_setup(&t->ssl, &ctx->conf) != 0) {
		mbedtls_ssl_free(&t->ssl);
		__os_free(t);
		return XTC_E_INTERNAL;
	}

	/* Transport BIO: non-blocking send/recv against t->fd. */
	mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);

	/*
	 * Verify the certificate chain but not the peer name.  mbedTLS
	 * matches the server certificate's CN/SAN against the hostname set
	 * here; passing NULL disables that name check while leaving chain
	 * verification (VERIFY_REQUIRED) in force, matching the OpenSSL
	 * backend, which verifies the chain and leaves hostname matching
	 * to the caller.  Must be called (even with NULL) on a client, or
	 * the handshake fails the verify step.
	 */
	if (ctx->role == XTC_TLS_CLIENT)
		(void)mbedtls_ssl_set_hostname(&t->ssl, NULL);

	*out = t;
	return XTC_OK;
}

void
xtc_tls_destroy(xtc_tls_t *tls)
{
	if (tls == NULL)
		return;
	mbedtls_ssl_free(&tls->ssl);
	__os_free(tls);
}

/* -------------------------------------------------------------------------
 * PUBLIC: int  xtc_tls_handshake __P((xtc_tls_t *));
 * ----------------------------------------------------------------------- */

int
xtc_tls_handshake(xtc_tls_t *tls)
{
	int rc;

	if (tls == NULL)
		return XTC_E_INVAL;

	xtc_tls_clear_wants(tls);

	rc = mbedtls_ssl_handshake(&tls->ssl);
	if (rc == 0)
		return XTC_OK;

	return map_want(tls, rc);
}

/* -------------------------------------------------------------------------
 * PUBLIC: int  xtc_tls_read  __P((xtc_tls_t *, void *, size_t, size_t *));
 * PUBLIC: int  xtc_tls_write __P((xtc_tls_t *, const void *, size_t, size_t *));
 * ----------------------------------------------------------------------- */

int
xtc_tls_read(xtc_tls_t *tls, void *buf, size_t buflen, size_t *out_n)
{
	int rc;

	if (tls == NULL || buf == NULL || out_n == NULL)
		return XTC_E_INVAL;

	*out_n           = 0;
	xtc_tls_clear_wants(tls);

	rc = mbedtls_ssl_read(&tls->ssl, (unsigned char *)buf, buflen);
	if (rc >= 0) {
		*out_n = (size_t)rc;
		return XTC_OK;
	}
	if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
		/* Clean peer close: treat as EOF (0 bytes). */
		*out_n = 0;
		return XTC_OK;
	}
	return map_want(tls, rc);
}

int
xtc_tls_write(xtc_tls_t *tls, const void *buf, size_t buflen, size_t *out_n)
{
	int rc;

	if (tls == NULL || buf == NULL || out_n == NULL)
		return XTC_E_INVAL;

	*out_n           = 0;
	xtc_tls_clear_wants(tls);

	rc = mbedtls_ssl_write(&tls->ssl, (const unsigned char *)buf, buflen);
	if (rc >= 0) {
		*out_n = (size_t)rc;
		return XTC_OK;
	}
	return map_want(tls, rc);
}

/* -------------------------------------------------------------------------
 * PUBLIC: int  xtc_tls_wants_read  __P((const xtc_tls_t *));
 * PUBLIC: int  xtc_tls_wants_write __P((const xtc_tls_t *));
 * ----------------------------------------------------------------------- */

XTC_TLS_DEFINE_WANTS_ACCESSORS

/* -------------------------------------------------------------------------
 * PUBLIC: int  xtc_tls_shutdown __P((xtc_tls_t *));
 *
 * mbedtls_ssl_close_notify sends the close_notify alert.  It returns 0
 * on success, or a WANT code if the alert could not be flushed yet.
 * ----------------------------------------------------------------------- */

int
xtc_tls_shutdown(xtc_tls_t *tls)
{
	int rc;

	if (tls == NULL)
		return XTC_E_INVAL;

	xtc_tls_clear_wants(tls);

	rc = mbedtls_ssl_close_notify(&tls->ssl);
	if (rc == 0)
		return XTC_OK;

	return map_want(tls, rc);
}

#endif /* XTC_TLS_BACKEND_MBEDTLS */
