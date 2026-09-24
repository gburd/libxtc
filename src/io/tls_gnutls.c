/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/io/tls_gnutls.c
 *	GnuTLS TLS backend for xtc_tls.
 *
 *	Compiled into libxtc.a only when configure sets
 *	XTC_TLS_BACKEND_GNUTLS=1 (--with-tls=gnutls).  Implements the
 *	same internal seam as tls_openssl.c: context create/free,
 *	non-blocking handshake (client and server), read, write,
 *	shutdown, and error mapping into XTC_E_*.
 *
 *	Non-blocking discipline:
 *
 *	  gnutls_init is given GNUTLS_NONBLOCK and the session's transport
 *	  is bound to the caller's non-blocking fd via
 *	  gnutls_transport_set_int.  gnutls_handshake / record_recv /
 *	  record_send then return GNUTLS_E_AGAIN (or GNUTLS_E_INTERRUPTED)
 *	  when the fd is not ready; we query gnutls_record_get_direction
 *	  to learn whether to wait for readability (0) or writability (1)
 *	  and surface XTC_E_AGAIN with wants_read / wants_write set, just
 *	  like the OpenSSL backend's WANT_READ / WANT_WRITE handling.
 *	  The caller re-drives after polling on the indicated direction.
 *
 *	Peer verification:
 *
 *	  The effective mode comes from resolve_verify (same rule as every
 *	  backend: verify_peer_mode, else legacy verify_peer, else the role
 *	  default -- CLIENT REQUIRE, SERVER NONE).  GnuTLS does not abort
 *	  the handshake on a bad certificate by default, so a verifying
 *	  session arms gnutls_session_set_verify_cert (in-handshake check,
 *	  alert to the peer) and re-checks with
 *	  gnutls_certificate_verify_peers3 when the handshake completes,
 *	  mapping a bad status to XTC_E_INTERNAL like the OpenSSL backend.
 *	  A SERVER requests the client certificate with
 *	  gnutls_certificate_server_set_request: REQUEST = ask, accept none,
 *	  reject an invalid one; REQUIRE = reject none too.
 *
 *	Internal struct layout (private to this file):
 *
 *	  struct xtc_tls_ctx {
 *	      xtc_tls_role_t                       role;
 *	      gnutls_certificate_credentials_t     cred;
 *	      char                                *priority;  // or NULL
 *	      gnutls_datum_t                      *alpn;       // or NULL
 *	      unsigned int                         alpn_count;
 *	      xtc_tls_verify_mode_t                verify;
 *	  };
 *
 *	  struct xtc_tls {
 *	      xtc_tls_ctx_t   *ctx;
 *	      int              fd;
 *	      gnutls_session_t session;
 *	      int              handshake_done;
 *	      int              wants_read;
 *	      int              wants_write;
 *	  };
 *
 *	s_async note: gnutls_global_init (called once via pthread_once) is
 *	XTC_BLOCKING_OK -- it is a one-shot startup initialiser amortised
 *	to zero cost after the first context creation.  All per-connection
 *	paths use the non-blocking transport and never block the loop.
 */

#include "xtc_int.h"
#include "xtc_tls.h"

#if defined(XTC_TLS_BACKEND_GNUTLS)

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <gnutls/gnutls.h>
#include <gnutls/x509.h>

/* -------------------------------------------------------------------------
 * Internal struct definitions.
 * ----------------------------------------------------------------------- */

struct xtc_tls_ctx {
	xtc_tls_role_t                    role;
	gnutls_certificate_credentials_t  cred;
	char                             *priority;     /* NULL = default */
	gnutls_datum_t                   *alpn;         /* NULL if unset */
	unsigned int                      alpn_count;
	xtc_tls_verify_mode_t             verify;       /* resolved; never DEFAULT */
};


struct xtc_tls {
	xtc_tls_ctx_t    *ctx;
	int               fd;
	gnutls_session_t  session;
	int               handshake_done;
	char             *hostname;      /* expected peer name; NULL = none */
	int               wants_read;
	int               wants_write;
};

#include "tls_common.h"   /* after struct xtc_tls: shared want-flag accessors */

/* -------------------------------------------------------------------------
 * One-time library initialisation.  GnuTLS 3.3+ auto-initialises via a
 * constructor, but we call gnutls_global_init explicitly once so the
 * behaviour is deterministic regardless of build flags.
 * ----------------------------------------------------------------------- */

static pthread_once_t s_init_once = PTHREAD_ONCE_INIT;

static void
gnutls_global_setup(void)  /* XTC_BLOCKING_OK: one-shot startup initialiser */
{
	(void)gnutls_global_init();
}

/* -------------------------------------------------------------------------
 * Map a GnuTLS record/handshake return code into XTC_E_AGAIN (with the
 * poll direction set from gnutls_record_get_direction) or a hard error.
 * ----------------------------------------------------------------------- */

static int
map_again(struct xtc_tls *t, int rc)
{
	if (rc == GNUTLS_E_AGAIN || rc == GNUTLS_E_INTERRUPTED) {
		/*
		 * gnutls_record_get_direction: 0 => waiting to read,
		 * 1 => waiting to write.
		 */
		if (gnutls_record_get_direction(t->session) == 0)
			t->wants_read = 1;
		else
			t->wants_write = 1;
		return XTC_E_AGAIN;
	}
	return XTC_E_INTERNAL;
}

/* -------------------------------------------------------------------------
 * Build a GnuTLS priority string from min/max version.  Returns XTC_OK
 * and sets *out (malloc'd, free with __os_free) or NULL when the
 * defaults suffice.
 * ----------------------------------------------------------------------- */

static int
build_priority(int min_version, int max_version, char **out)
{
	char  buf[128];
	int   n;
	char *copy;
	int   rc;

	*out = NULL;
	if (min_version == 0 && max_version == 0)
		return XTC_OK;   /* use library default priority */

	/*
	 * Start from NORMAL and disable the versions outside the range.
	 * GnuTLS priority tokens: -VERS-TLS1.2 / -VERS-TLS1.3 remove a
	 * version; we keep only what the [min,max] window allows.
	 */
	n = snprintf(buf, sizeof(buf), "NORMAL");
	if (min_version == XTC_TLS_VER_13)
		n += snprintf(buf + n, sizeof(buf) - (size_t)n, ":-VERS-TLS1.2");
	if (max_version == XTC_TLS_VER_12)
		n += snprintf(buf + n, sizeof(buf) - (size_t)n, ":-VERS-TLS1.3");
	if (n < 0 || (size_t)n >= sizeof(buf))
		return XTC_E_INVAL;

	if ((rc = __os_malloc((size_t)n + 1, (void **)&copy)) != XTC_OK)
		return rc;
	memcpy(copy, buf, (size_t)n + 1);
	*out = copy;
	return XTC_OK;
}

/* -------------------------------------------------------------------------
 * ALPN wire form -> array of gnutls_datum_t (name pointers reference a
 * single packed copy of the wire data, see ctx layout).
 * ----------------------------------------------------------------------- */

static int
alpn_decode(const char *wire, gnutls_datum_t **out, unsigned int *out_count)
{
	const unsigned char *p     = (const unsigned char *)wire;
	size_t               off   = 0;
	unsigned int         count = 0;
	gnutls_datum_t      *arr;
	int                  rc;
	unsigned int         i;

	*out = NULL;
	*out_count = 0;

	while (wire[off] != '\0') {
		size_t l = (unsigned char)wire[off];
		if (l == 0)
			return XTC_E_INVAL;
		off += 1 + l;
		count++;
	}
	if (count == 0)
		return XTC_OK;

	if ((rc = __os_calloc(count, sizeof(gnutls_datum_t),
	                      (void **)&arr)) != XTC_OK)
		return rc;

	off = 0;
	for (i = 0; i < count; i++) {
		size_t         l = (unsigned char)p[off];
		unsigned char *name;
		if ((rc = __os_malloc(l, (void **)&name)) != XTC_OK) {
			unsigned int j;
			for (j = 0; j < i; j++)
				__os_free(arr[j].data);
			__os_free(arr);
			return rc;
		}
		memcpy(name, p + off + 1, l);
		arr[i].data = name;
		arr[i].size = (unsigned int)l;
		off += 1 + l;
	}
	*out = arr;
	*out_count = count;
	return XTC_OK;
}

static void
alpn_free(gnutls_datum_t *arr, unsigned int count)
{
	unsigned int i;
	if (arr == NULL)
		return;
	for (i = 0; i < count; i++)
		__os_free(arr[i].data);
	__os_free(arr);
}

/*
 * Resolve the effective peer-verification mode.  verify_peer_mode wins
 * when set; otherwise the legacy verify_peer int (non-zero = REQUIRE);
 * otherwise the ROLE default: a CLIENT verifies the server (REQUIRE), a
 * SERVER does not ask for a client certificate (NONE).  Before 1.50 a
 * zeroed opts meant NONE for a client, and this backend ignored
 * verify_peer_mode entirely (PLAN 19.27.8).  Keep in sync with the
 * other tls_<backend>.c copies.
 */
static xtc_tls_verify_mode_t
resolve_verify(xtc_tls_role_t role, const xtc_tls_opts_t *opts)
{
	if (opts != NULL && opts->verify_peer_mode != XTC_TLS_VERIFY_DEFAULT)
		return opts->verify_peer_mode;
	if (opts != NULL && opts->verify_peer)
		return XTC_TLS_VERIFY_REQUIRE;
	return (role == XTC_TLS_CLIENT) ? XTC_TLS_VERIFY_REQUIRE
	                                : XTC_TLS_VERIFY_NONE;
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

	if (out == NULL)
		return XTC_E_INVAL;
	*out = NULL;

	pthread_once(&s_init_once, gnutls_global_setup); /* XTC_BLOCKING_OK */

	if ((rc = __os_calloc(1, sizeof(*c), (void **)&c)) != XTC_OK)
		return rc;

	c->role   = role;
	c->verify = resolve_verify(role, opts);

	if (gnutls_certificate_allocate_credentials(&c->cred) != 0) {
		__os_free(c);
		return XTC_E_NOMEM;
	}

	/* A verifying CLIENT with no ca_file trusts the platform store.  A
	 * failure (no store) leaves no anchors: every handshake then fails
	 * closed.  A SERVER never trusts the system store for clients. */
	if (role == XTC_TLS_CLIENT && c->verify != XTC_TLS_VERIFY_NONE &&
	    (opts == NULL || opts->ca_file == NULL))
		(void)gnutls_certificate_set_x509_system_trust(c->cred);

	if (opts == NULL)
		goto done;

	/* ---- Certificate + key (PEM) ---- */
	if (opts->cert_file != NULL && opts->key_file != NULL) {
		if (gnutls_certificate_set_x509_key_file(c->cred,
		        opts->cert_file, opts->key_file,
		        GNUTLS_X509_FMT_PEM) != GNUTLS_E_SUCCESS) {
			rc = XTC_E_INVAL;
			goto fail;
		}
	} else if (opts->cert_file != NULL || opts->key_file != NULL) {
		/* Cert without key (or vice-versa) is not usable. */
		rc = XTC_E_INVAL;
		goto fail;
	}

	/* ---- CA trust file ---- */
	if (opts->ca_file != NULL) {
		if (gnutls_certificate_set_x509_trust_file(c->cred,
		        opts->ca_file, GNUTLS_X509_FMT_PEM) < 0) {
			rc = XTC_E_INVAL;
			goto fail;
		}
	}

	/* ---- Priority string from min/max version ---- */
	if ((rc = build_priority(opts->min_version, opts->max_version,
	                         &c->priority)) != XTC_OK)
		goto fail;

	/* ---- ALPN ---- */
	if (opts->alpn_protos != NULL && opts->alpn_protos[0] != '\0') {
		if ((rc = alpn_decode(opts->alpn_protos, &c->alpn,
		                      &c->alpn_count)) != XTC_OK)
			goto fail;
	}

done:
	*out = c;
	return XTC_OK;

fail:
	if (c->cred != NULL)
		gnutls_certificate_free_credentials(c->cred);
	if (c->priority != NULL)
		__os_free(c->priority);
	alpn_free(c->alpn, c->alpn_count);
	__os_free(c);
	return rc;
}

void
xtc_tls_ctx_destroy(xtc_tls_ctx_t *ctx)
{
	if (ctx == NULL)
		return;
	alpn_free(ctx->alpn, ctx->alpn_count);
	if (ctx->priority != NULL)
		__os_free(ctx->priority);
	if (ctx->cred != NULL)
		gnutls_certificate_free_credentials(ctx->cred);
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
	unsigned int    flags;
	int             rc;

	if (ctx == NULL || fd < 0 || out == NULL)
		return XTC_E_INVAL;
	*out = NULL;

	if ((rc = __os_calloc(1, sizeof(*t), (void **)&t)) != XTC_OK)
		return rc;

	t->ctx            = ctx;
	t->fd             = fd;
	t->handshake_done = 0;
	xtc_tls_clear_wants(t);

	/* GNUTLS_NO_SIGNAL: send with MSG_NOSIGNAL, so a write to a peer
	 * that has gone returns an error instead of raising SIGPIPE and
	 * killing the host process (reproduced: exit 141 without it). */
	flags = GNUTLS_NONBLOCK |
	    ((ctx->role == XTC_TLS_SERVER) ? GNUTLS_SERVER : GNUTLS_CLIENT);
#if GNUTLS_VERSION_NUMBER >= 0x030402   /* enum value, not a macro */
	flags |= GNUTLS_NO_SIGNAL;
#endif

	if (gnutls_init(&t->session, flags) != GNUTLS_E_SUCCESS) {
		__os_free(t);
		return XTC_E_INTERNAL;
	}

	/* Priority: explicit window or library default. */
	if (ctx->priority != NULL)
		rc = gnutls_priority_set_direct(t->session, ctx->priority,
		                                NULL);
	else
		rc = gnutls_set_default_priority(t->session);
	if (rc != GNUTLS_E_SUCCESS)
		goto fail;

	if (gnutls_credentials_set(t->session, GNUTLS_CRD_CERTIFICATE,
	                           ctx->cred) != GNUTLS_E_SUCCESS)
		goto fail;

	/* ALPN offer / accept list. */
	if (ctx->alpn != NULL &&
	    gnutls_alpn_set_protocols(t->session, ctx->alpn,
	                              ctx->alpn_count, 0) != GNUTLS_E_SUCCESS)
		goto fail;

	/* Bind transport to the caller's fd; no handshake timeout (we are
	 * non-blocking and the caller drives the poll loop). */
	gnutls_transport_set_int(t->session, fd);
	gnutls_handshake_set_timeout(t->session, 0);

	/* CLIENT with verification: verify the server certificate INSIDE
	 * the handshake so a bad chain (or, once xtc_tls_set_hostname names
	 * the peer, a bad name) aborts it with an alert the server sees,
	 * rather than completing and being rejected afterwards.  SERVER:
	 * request the client certificate (GNUTLS_CERT_REQUIRE also fails a
	 * client that sends none); the chain is checked when the handshake
	 * completes (xtc_tls_handshake). */
	if (ctx->role == XTC_TLS_CLIENT && ctx->verify != XTC_TLS_VERIFY_NONE)
		gnutls_session_set_verify_cert(t->session, NULL, 0);
	if (ctx->role == XTC_TLS_SERVER && ctx->verify != XTC_TLS_VERIFY_NONE)
		gnutls_certificate_server_set_request(t->session,
		    ctx->verify == XTC_TLS_VERIFY_REQUEST ? GNUTLS_CERT_REQUEST
		                                          : GNUTLS_CERT_REQUIRE);

	*out = t;
	return XTC_OK;

fail:
	gnutls_deinit(t->session);
	__os_free(t);
	return XTC_E_INTERNAL;
}

/* SNI context-selection callback + custom transport + client SNI host:
 * not supported on this backend (see the OpenSSL backend for the full
 * implementation).  Uniform API -> XTC_E_NOSYS, matching the accessor
 * stubs. */
int
xtc_tls_ctx_set_sni_cb(xtc_tls_ctx_t *ctx, xtc_tls_sni_cb_t cb, void *userdata)
{
	(void)ctx; (void)cb; (void)userdata;
	return XTC_E_NOSYS;
}

int
xtc_tls_create_transport(xtc_tls_ctx_t *ctx,
                         const xtc_tls_transport_t *transport,
                         xtc_tls_t **out)
{
	(void)ctx; (void)transport;
	if (out != NULL)
		*out = NULL;
	return XTC_E_NOSYS;
}

/*
 * Client SNI + RFC 6125 name check.  SNI goes out via
 * gnutls_server_name_set; the name check is done in the handshake by
 * gnutls_session_set_verify_cert (and again, belt and braces, by
 * gnutls_certificate_verify_peers3 in xtc_tls_handshake), which
 * matches the name against the certificate's SAN / CN.  Like OpenSSL,
 * the name is only enforced when the context verifies peers.
 */
int
xtc_tls_set_hostname(xtc_tls_t *tls, const char *name)
{
	char   *copy = NULL;
	size_t  len;
	int     rc;

	if (tls == NULL)
		return XTC_E_INVAL;
	if (tls->ctx->role != XTC_TLS_CLIENT)
		return XTC_OK;   /* no-op on the server side */
	len = (name != NULL) ? strlen(name) : 0;
	if (len != 0) {
		if (len > SIZE_MAX - 1)          /* len + 1 below must not wrap */
			return XTC_E_INVAL;
		if ((rc = __os_malloc(len + 1, (void **)&copy)) != XTC_OK)
			return rc;
		memcpy(copy, name, len + 1);
	}
	/* name_length 0 clears any server name previously set. */
	if (gnutls_server_name_set(tls->session, GNUTLS_NAME_DNS,
	        len != 0 ? name : "", len) != GNUTLS_E_SUCCESS) {
		if (copy != NULL)
			__os_free(copy);
		return XTC_E_INTERNAL;
	}
	/* Re-arm in-handshake verification with the name (the pointer must
	 * outlive the session, hence the owned copy; set before freeing the
	 * old one). */
	if (tls->ctx->verify != XTC_TLS_VERIFY_NONE)
		gnutls_session_set_verify_cert(tls->session, copy, 0);
	if (tls->hostname != NULL)
		__os_free(tls->hostname);
	tls->hostname = copy;
	return XTC_OK;
}

void
xtc_tls_destroy(xtc_tls_t *tls)
{
	if (tls == NULL)
		return;
	if (tls->hostname != NULL)
		__os_free(tls->hostname);
	gnutls_deinit(tls->session);
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

	rc = gnutls_handshake(tls->session);
	if (rc == GNUTLS_E_SUCCESS) {
		tls->handshake_done = 1;
		/*
		 * GnuTLS does not fail the handshake on a bad peer cert by
		 * default; verify explicitly when requested and map a bad
		 * status to a hard error (matches the OpenSSL backend).
		 * verify_peers3 also checks the peer name when one was set
		 * with xtc_tls_set_hostname (NULL = chain only).  A SERVER in
		 * REQUEST mode accepts a client that presented no certificate
		 * (but not one that presented an invalid certificate).
		 */
		if (tls->ctx->verify != XTC_TLS_VERIFY_NONE) {
			unsigned int status = 0;
			unsigned int npeer  = 0;
			int vr;

			if (tls->ctx->role == XTC_TLS_SERVER &&
			    tls->ctx->verify == XTC_TLS_VERIFY_REQUEST &&
			    gnutls_certificate_get_peers(tls->session,
			        &npeer) == NULL)
				return XTC_OK;
			vr = gnutls_certificate_verify_peers3(tls->session,
			                                      tls->hostname,
			                                      &status);
			if (vr != GNUTLS_E_SUCCESS || status != 0)
				return XTC_E_INTERNAL;
		}
		return XTC_OK;
	}

	/* Non-fatal alerts during the handshake are retryable; treat any
	 * fatal error as a hard failure. */
	if (rc != GNUTLS_E_AGAIN && rc != GNUTLS_E_INTERRUPTED &&
	    gnutls_error_is_fatal(rc) == 0)
		return XTC_E_AGAIN;

	return map_again(tls, rc);
}

/* -------------------------------------------------------------------------
 * PUBLIC: int  xtc_tls_read  __P((xtc_tls_t *, void *, size_t, size_t *));
 * PUBLIC: int  xtc_tls_write __P((xtc_tls_t *, const void *, size_t, size_t *));
 * ----------------------------------------------------------------------- */

int
xtc_tls_read(xtc_tls_t *tls, void *buf, size_t buflen, size_t *out_n)
{
	ssize_t n;

	if (tls == NULL || buf == NULL || out_n == NULL)
		return XTC_E_INVAL;

	*out_n           = 0;
	xtc_tls_clear_wants(tls);

	n = gnutls_record_recv(tls->session, buf, buflen);
	if (n >= 0) {
		*out_n = (size_t)n;   /* n == 0 => clean peer close (EOF) */
		return XTC_OK;
	}
	return map_again(tls, (int)n);
}

int
xtc_tls_write(xtc_tls_t *tls, const void *buf, size_t buflen, size_t *out_n)
{
	ssize_t n;

	if (tls == NULL || buf == NULL || out_n == NULL)
		return XTC_E_INVAL;

	*out_n           = 0;
	xtc_tls_clear_wants(tls);

	n = gnutls_record_send(tls->session, buf, buflen);
	if (n >= 0) {
		*out_n = (size_t)n;
		return XTC_OK;
	}
	return map_again(tls, (int)n);
}

/* -------------------------------------------------------------------------
 * PUBLIC: int  xtc_tls_wants_read  __P((const xtc_tls_t *));
 * PUBLIC: int  xtc_tls_wants_write __P((const xtc_tls_t *));
 * ----------------------------------------------------------------------- */

XTC_TLS_DEFINE_WANTS_ACCESSORS

/* -------------------------------------------------------------------------
 * PUBLIC: int  xtc_tls_shutdown __P((xtc_tls_t *));
 *
 * gnutls_bye(GNUTLS_SHUT_WR) sends close_notify.  It returns
 * GNUTLS_E_SUCCESS once flushed, or GNUTLS_E_AGAIN if the alert could
 * not be written yet.
 * ----------------------------------------------------------------------- */

int
xtc_tls_shutdown(xtc_tls_t *tls)
{
	int rc;

	if (tls == NULL)
		return XTC_E_INVAL;

	xtc_tls_clear_wants(tls);

	rc = gnutls_bye(tls->session, GNUTLS_SHUT_WR);
	if (rc == GNUTLS_E_SUCCESS)
		return XTC_OK;

	return map_again(tls, rc);
}

XTC_TLS_DEFINE_INTROSPECT_STUBS   /* introspection not yet ported to GnuTLS */

#endif /* XTC_TLS_BACKEND_GNUTLS */
