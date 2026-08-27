/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/inc/xtc_tls.h
 *	Transport Layer Security (TLS) API for xtc.
 *
 *	Provides TLS 1.2/1.3 over xtc_io sockets with the same async,
 *	single-threaded event-loop discipline as the rest of xtc.  The
 *	implementation is backend-pluggable at configure time via
 *	--with-tls=openssl|none|auto; the public API is identical
 *	regardless of the backend selected.
 *
 *	Two opaque types:
 *	  xtc_tls_ctx_t  -- per-process context: loaded cert + key + CA bundle.
 *	  xtc_tls_t      -- per-connection state machine.
 *
 *	Usage pattern:
 *
 *	  // 1.  Create a shared context (once per server/client role):
 *	  xtc_tls_opts_t opts = { .cert_file = "srv.crt", .key_file = "srv.key",
 *	                           .verify_peer = 0,
 *	                           .min_version = XTC_TLS_VER_12 };
 *	  xtc_tls_ctx_t *ctx;
 *	  xtc_tls_ctx_create(XTC_TLS_SERVER, &opts, &ctx);
 *
 *	  // 2.  Wrap an accepted fd:
 *	  xtc_tls_t *tls;
 *	  xtc_tls_create(ctx, fd, &tls);
 *
 *	  // 3.  Drive the non-blocking handshake inside the event loop:
 *	  for (;;) {
 *	      int rc = xtc_tls_handshake(tls);
 *	      if (rc == XTC_OK) break;
 *	      if (rc != XTC_E_AGAIN) { break; }  // hard error: tear down
 *	      uint32_t want = xtc_tls_wants_read(tls)
 *	          ? XTC_IO_READABLE : XTC_IO_WRITABLE;
 *	      xtc_io_mod_fd(io, fd, want, tls);
 *	      // yield, await event ...
 *	  }
 *
 *	  // 4.  Encrypted I/O:
 *	  size_t n;
 *	  xtc_tls_read(tls, buf, sizeof(buf), &n);
 *	  xtc_tls_write(tls, "hello", 5, &n);
 *
 *	  // 5.  Graceful shutdown + cleanup:
 *	  xtc_tls_shutdown(tls);
 *	  xtc_tls_destroy(tls);
 *	  xtc_tls_ctx_destroy(ctx);
 *
 *	When TLS support is not compiled in (--with-tls=none), every
 *	function returns XTC_E_NOSYS; callers may test for that at
 *	runtime to skip TLS-dependent code paths.
 */

#ifndef XTC_TLS_H
#define XTC_TLS_H

#include "xtc_export.h"

#include <stddef.h>
#include "xtc.h"

/*
 * XTC_TLS_ENABLED is defined (to 1) whenever a real TLS backend is
 * compiled in -- any one of OpenSSL/LibreSSL, mbedTLS, GnuTLS, wolfSSL,
 * or SChannel.  It is NOT defined for --with-tls=none (the NOSYS
 * stubs).  Callers and tests use it to gate code that needs a working
 * handshake without caring which backend provides it.  The selecting
 * XTC_TLS_BACKEND_* macro comes from xtc_config.h via configure.
 */
#if defined(XTC_TLS_BACKEND_OPENSSL)  || \
    defined(XTC_TLS_BACKEND_MBEDTLS)  || \
    defined(XTC_TLS_BACKEND_GNUTLS)   || \
    defined(XTC_TLS_BACKEND_WOLFSSL)  || \
    defined(XTC_TLS_BACKEND_SCHANNEL)
#define XTC_TLS_ENABLED 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Opaque types.
 * ----------------------------------------------------------------------- */

/*
 * xtc_tls_ctx_t -- per-process (or per-vhost) TLS context.
 *
 *   Holds the loaded certificate, private key, and CA bundle.  A
 *   single context may be shared by many concurrent connections of
 *   the same role; it is internally reference-safe via immutable
 *   configuration after creation.
 */
typedef struct xtc_tls_ctx  xtc_tls_ctx_t;

/*
 * xtc_tls_t -- per-connection TLS state machine.
 *
 *   Wraps an existing file descriptor.  The fd remains owned by the
 *   caller; xtc_tls_destroy does not close it.
 */
typedef struct xtc_tls      xtc_tls_t;

/* -------------------------------------------------------------------------
 * Role.
 * ----------------------------------------------------------------------- */

typedef enum xtc_tls_role {
	XTC_TLS_SERVER = 0,   /* accept connections, present certificate */
	XTC_TLS_CLIENT = 1    /* initiate connections, optionally verify server */
} xtc_tls_role_t;

/* -------------------------------------------------------------------------
 * TLS version constants.
 * ----------------------------------------------------------------------- */

#define XTC_TLS_VER_12  0x0303   /* TLS 1.2 (RFC 5246) */
#define XTC_TLS_VER_13  0x0304   /* TLS 1.3 (RFC 8446) */

/* -------------------------------------------------------------------------
 * Peer-verification mode (tri-state).
 *
 * The legacy opts.verify_peer int is a two-state on/off.  A TLS server
 * often needs the third, middle behavior: REQUEST a client certificate
 * but still complete the handshake if the client presents none (PG's
 * default -- certificate auth is then optional, decided per-hba-line
 * after the handshake).  opts.verify_peer_mode expresses all three.
 * See the compatibility note on opts.verify_peer below.
 * ----------------------------------------------------------------------- */

typedef enum xtc_tls_verify_mode {
	XTC_TLS_VERIFY_DEFAULT = 0,  /* defer to legacy opts.verify_peer */
	XTC_TLS_VERIFY_NONE,         /* do not request a peer certificate */
	XTC_TLS_VERIFY_REQUEST,      /* request; accept a handshake with none */
	XTC_TLS_VERIFY_REQUIRE       /* require a valid peer certificate */
} xtc_tls_verify_mode_t;

/* -------------------------------------------------------------------------
 * Passphrase callback for an encrypted private key.
 *
 * Mirrors OpenSSL's pem_password_cb: write up to size bytes of the
 * passphrase into buf and return the number written, or <= 0 to fail
 * (which fails key load).  userdata is opts.passphrase_userdata.  When
 * no callback is set and the key is encrypted, key load fails rather
 * than prompting interactively -- a server must never block on a tty.
 * ----------------------------------------------------------------------- */

typedef int (*xtc_tls_passphrase_cb_t)(char *buf, int size, void *userdata);

/* -------------------------------------------------------------------------
 * Options.
 * ----------------------------------------------------------------------- */

/*
 * xtc_tls_opts_t -- creation-time parameters for xtc_tls_ctx_create.
 *
 * All string pointers are borrowed for the duration of the
 * xtc_tls_ctx_create call only; the implementation copies any data
 * it needs before returning.
 *
 * Fields:
 *   cert_file    Path to the PEM certificate file.  Required for
 *                SERVER role; ignored (may be NULL) for CLIENT role
 *                unless mutual TLS is needed.
 *
 *   key_file     Path to the PEM private-key file matching cert_file.
 *                Required when cert_file is set.
 *
 *   ca_file      Path to a PEM CA bundle used for peer verification.
 *                If NULL the backend's system CA bundle is used.
 *
 *   verify_peer  Boolean.  1 = require a valid peer certificate;
 *                0 = do not verify.  For SERVER role, 1 enables
 *                mutual TLS (client must present a cert).
 *
 *   alpn_protos  ALPN protocol list in wire encoding:
 *                "\x02h2\x08http/1.1".  NULL disables ALPN
 *                negotiation.
 *
 *   min_version  Minimum TLS version to accept.  Use XTC_TLS_VER_12
 *                or XTC_TLS_VER_13.  0 means "backend default"
 *                (typically TLS 1.2).
 *
 *   max_version  Maximum TLS version to offer.  0 means "backend
 *                default" (typically TLS 1.3).
 *
 * Additions (all optional; a zeroed opts behaves exactly as before):
 *
 *   verify_peer_mode  Tri-state peer verification (see
 *                xtc_tls_verify_mode_t).  When XTC_TLS_VERIFY_DEFAULT
 *                (0), the legacy verify_peer int is used.  Any other
 *                value takes precedence over verify_peer.
 *
 *   cipher_list       TLS 1.2 cipher list (OpenSSL SSL_CTX_set_cipher_list
 *                syntax).  NULL = backend default.
 *   ciphersuites_13   TLS 1.3 ciphersuites.  NULL = backend default.
 *   groups            Key-exchange groups / curves, e.g.
 *                "X25519:prime256v1".  NULL = backend default.
 *
 *   crl_file          PEM CRL file for revocation checking.  NULL = none.
 *   crl_dir           Hashed CRL directory.  NULL = none.  When either
 *                crl_file or crl_dir is set, full-chain CRL checking is
 *                enabled.
 *
 *   prefer_server_ciphers  Non-zero: honor the server's cipher order
 *                over the client's (SSL_OP_CIPHER_SERVER_PREFERENCE).
 *
 *   passphrase_cb / passphrase_userdata  Supply the passphrase for an
 *                encrypted key_file without an interactive prompt.
 *
 * Server hardening applied by default (no knob; a DB-server-safe posture
 * the request asked to be a documented default rather than a field):
 * TLS renegotiation is disabled, TLS compression is disabled, session
 * tickets and the session cache are off, and the moving-write-buffer
 * mode required for correct non-blocking sends is enabled.
 */
typedef struct xtc_tls_opts {
	const char *cert_file;
	const char *key_file;
	const char *ca_file;
	int         verify_peer;   /* legacy on/off; see verify_peer_mode */
	const char *alpn_protos;
	int         min_version;
	int         max_version;

	/* additions (v1.24.0) -- a zeroed struct is byte-for-byte the old
	 * behavior, so this grows the struct without breaking callers that
	 * only set the original fields. */
	xtc_tls_verify_mode_t   verify_peer_mode;
	const char             *cipher_list;
	const char             *ciphersuites_13;
	const char             *groups;
	const char             *crl_file;
	const char             *crl_dir;
	int                     prefer_server_ciphers;
	xtc_tls_passphrase_cb_t passphrase_cb;
	void                   *passphrase_userdata;
} xtc_tls_opts_t;

/* -------------------------------------------------------------------------
 * Context lifecycle.
 * ----------------------------------------------------------------------- */

/*
 * PUBLIC: int  xtc_tls_ctx_create __P((xtc_tls_role_t,
 * PUBLIC:                              const xtc_tls_opts_t *,
 * PUBLIC:                              xtc_tls_ctx_t **));
 * PUBLIC: void xtc_tls_ctx_destroy __P((xtc_tls_ctx_t *));
 */

/*
 * xtc_tls_ctx_create --
 *	Allocate and initialise a TLS context.
 *
 *	role    SERVER or CLIENT.
 *	opts    Options struct.  May be NULL (all defaults).
 *	out     On XTC_OK, *out points to the new context.
 *
 *	Returns:
 *	  XTC_OK        on success
 *	  XTC_E_INVAL   if out is NULL or opts contains contradictory settings
 *	  XTC_E_NOMEM   on allocation failure
 *	  XTC_E_NOSYS   if TLS support was not compiled in
 */
XTC_API int  xtc_tls_ctx_create(xtc_tls_role_t role,
                                const xtc_tls_opts_t *opts,
                                xtc_tls_ctx_t **out);

/*
 * xtc_tls_ctx_destroy --
 *	Release all resources held by a TLS context.
 *	Must not be called while any xtc_tls_t created from it is live:
 *	live connections hold a raw pointer to the ctx (dereferenced e.g.
 *	by xtc_tls_get_server_cert_hash and xtc_tls_set_hostname), so
 *	destroying it underneath them is a use-after-free.  The ctx is NOT
 *	refcounted.  On config reload, build the NEW ctx and point new
 *	connections at it, but do NOT destroy the OLD ctx until all
 *	connections created from it have been xtc_tls_destroy()'d --
 *	retire it (e.g. to a list freed later) rather than freeing it in
 *	place.  ctx may be NULL (no-op).
 */
XTC_API void xtc_tls_ctx_destroy(xtc_tls_ctx_t *ctx);

/* -------------------------------------------------------------------------
 * Per-connection lifecycle.
 * ----------------------------------------------------------------------- */

/*
 * PUBLIC: int  xtc_tls_create  __P((xtc_tls_ctx_t *, int, xtc_tls_t **));
 * PUBLIC: void xtc_tls_destroy __P((xtc_tls_t *));
 */

/*
 * xtc_tls_create --
 *	Wrap an existing file descriptor in a TLS state machine.
 *
 *	ctx     The context created by xtc_tls_ctx_create.
 *	fd      A non-blocking socket fd.  Ownership remains with the caller;
 *	        xtc_tls_destroy does not close fd.
 *	out     On XTC_OK, *out points to the new xtc_tls_t.
 *
 *	After this call the handshake has not yet run; call
 *	xtc_tls_handshake to drive it.
 *
 *	Returns:
 *	  XTC_OK        on success
 *	  XTC_E_INVAL   if ctx or out is NULL, or fd < 0
 *	  XTC_E_NOMEM   on allocation failure
 *	  XTC_E_NOSYS   if TLS support was not compiled in
 */
XTC_API int  xtc_tls_create(xtc_tls_ctx_t *ctx, int fd, xtc_tls_t **out);

/* -------------------------------------------------------------------------
 * SNI / ClientHello context-selection callback (multi-tenant servers).
 *
 * A server that presents different certificates for different requested
 * host names (SNI) registers a selection callback on the SERVER context.
 * During the handshake, after the ClientHello is parsed but before the
 * certificate is chosen, the callback fires with the requested
 * server_name (the SNI host, or NULL if the client sent none) and may
 * return a DIFFERENT, pre-created xtc_tls_ctx_t whose certificate / key /
 * CA / verify-mode is then used for the rest of THIS handshake -- exactly
 * OpenSSL's SSL_CTX_set_client_hello_cb + SSL_set_SSL_CTX shape.
 *
 * Contract:
 *   - Return a pointer to one of your pre-created SERVER xtc_tls_ctx_t to
 *     swap it in for this connection, or
 *   - Return NULL (or the same ctx) to keep the current context.
 *   - The returned context MUST outlive the connection and MUST have
 *     been created with role XTC_TLS_SERVER; returning a CLIENT context
 *     or a destroyed one is undefined.
 *   - server_name points at backend-owned memory valid only for the
 *     duration of the callback; copy it if you need it later.
 *   - The callback runs on the carrier driving the handshake (the same
 *     thread as xtc_tls_handshake); it must not block.
 *
 * userdata is passed through unchanged.  Registering on a CLIENT context
 * or on a backend that cannot swap the context mid-handshake returns
 * XTC_E_NOSYS.
 * ----------------------------------------------------------------------- */

typedef xtc_tls_ctx_t *(*xtc_tls_sni_cb_t)(xtc_tls_t *tls,
                                           const char *server_name,
                                           void *userdata);

/*
 * PUBLIC: int  xtc_tls_ctx_set_sni_cb __P((xtc_tls_ctx_t *,
 * PUBLIC:                              xtc_tls_sni_cb_t, void *));
 */
XTC_API int  xtc_tls_ctx_set_sni_cb(xtc_tls_ctx_t *ctx,
                                    xtc_tls_sni_cb_t cb, void *userdata);

/* -------------------------------------------------------------------------
 * Custom transport (caller owns recv/send instead of an fd).
 *
 * xtc_tls_create binds an fd and does its own recv/send.  A consumer that
 * must control the underlying transport -- to feed bytes it already read
 * off the socket before deciding to negotiate TLS (pushback), to weave
 * the read/write into its own interrupt / cancellation / fiber-yield
 * discipline, or to handle a platform signal window -- supplies a
 * transport instead, the same way OpenSSL's BIO lets an application own
 * the wire.  Only the TLS state machine lives in xtc_tls; every byte to
 * or from the network flows through the caller's callbacks.
 *
 * Callback contract (BIO-like):
 *   read_cb:  read up to len bytes into buf.  Return >0 = bytes read;
 *             0 = clean EOF (peer closed); XTC_E_AGAIN = would block
 *             (arm a readable watch and retry the TLS op); any other
 *             negative xtc error = hard failure.
 *   write_cb: write up to len bytes from buf.  Return >0 = bytes
 *             written; XTC_E_AGAIN = would block (arm a writable watch);
 *             other negative = hard failure.  A short write is fine.
 *   userdata: the transport's userdata, passed to both callbacks.
 *
 * The caller retains ownership of whatever the transport wraps;
 * xtc_tls_destroy does not touch it.  There is no fd, so
 * xtc_tls_wants_read/_write still report the direction the stall is in,
 * but the caller drives its own transport readiness.
 * ----------------------------------------------------------------------- */

typedef struct xtc_tls_transport {
	int  (*read_cb)(void *userdata, void *buf, size_t len);
	int  (*write_cb)(void *userdata, const void *buf, size_t len);
	void  *userdata;
} xtc_tls_transport_t;

/*
 * PUBLIC: int  xtc_tls_create_transport __P((xtc_tls_ctx_t *,
 * PUBLIC:                              const xtc_tls_transport_t *,
 * PUBLIC:                              xtc_tls_t **));
 */
XTC_API int  xtc_tls_create_transport(xtc_tls_ctx_t *ctx,
                                      const xtc_tls_transport_t *transport,
                                      xtc_tls_t **out);

/* -------------------------------------------------------------------------
 * Client-side SNI / hostname (the other half of SNI).
 *
 * A CLIENT connection calls this before the handshake to send the
 * requested host in the ClientHello's SNI extension (so a multi-tenant
 * server's selection callback can pick the right certificate) and to
 * enable RFC 6125 hostname verification against the server certificate
 * when the context verifies peers.  name is copied; NULL or "" clears
 * it.  Must be called before xtc_tls_handshake.  Ignored (harmless) on
 * a SERVER connection; XTC_E_NOSYS on a backend that cannot set it.
 * ----------------------------------------------------------------------- */

/*
 * PUBLIC: int  xtc_tls_set_hostname __P((xtc_tls_t *, const char *));
 */
XTC_API int  xtc_tls_set_hostname(xtc_tls_t *tls, const char *name);

/*
 * xtc_tls_destroy --
 *	Release per-connection TLS state.  Does not close the underlying fd,
 *	does not send close_notify -- call xtc_tls_shutdown first if a clean
 *	shutdown is needed.
 *	tls may be NULL (no-op).
 */
XTC_API void xtc_tls_destroy(xtc_tls_t *tls);

/* -------------------------------------------------------------------------
 * Handshake.
 * ----------------------------------------------------------------------- */

/*
 * PUBLIC: int  xtc_tls_handshake  __P((xtc_tls_t *));
 * PUBLIC: int  xtc_tls_wants_read __P((const xtc_tls_t *));
 * PUBLIC: int  xtc_tls_wants_write __P((const xtc_tls_t *));
 */

/*
 * xtc_tls_handshake --
 *	Drive the TLS handshake state machine one step.
 *
 *	Returns:
 *	  XTC_OK        handshake complete; the connection is ready for I/O
 *	  XTC_E_AGAIN   the fd is not yet ready; poll on
 *	                  xtc_tls_wants_read  ? XTC_IO_READABLE
 *	                                      : XTC_IO_WRITABLE
 *	                and call again when the event fires
 *	  XTC_E_INVAL   tls is NULL
 *	  XTC_E_NOSYS   TLS not compiled in
 *	  (other)       backend-specific hard error; connection must be torn down
 */
XTC_API int  xtc_tls_handshake(xtc_tls_t *tls);

/* -------------------------------------------------------------------------
 * Encrypted I/O.
 * ----------------------------------------------------------------------- */

/*
 * PUBLIC: int  xtc_tls_read  __P((xtc_tls_t *, void *, size_t, size_t *));
 * PUBLIC: int  xtc_tls_write __P((xtc_tls_t *, const void *, size_t, size_t *));
 */

/*
 * xtc_tls_read --
 *	Read up to buflen decrypted bytes into buf.
 *
 *	On XTC_OK, *out_n holds the number of bytes read (may be < buflen).
 *	On XTC_E_AGAIN the fd was not readable; *out_n is 0.
 *	On XTC_E_INVAL tls, buf, or out_n is NULL.
 *	On XTC_E_NOSYS TLS was not compiled in.
 */
XTC_API int  xtc_tls_read(xtc_tls_t *tls, void *buf, size_t buflen, size_t *out_n);

/*
 * xtc_tls_write --
 *	Encrypt and write up to buflen bytes from buf.
 *
 *	On XTC_OK, *out_n holds the number of bytes consumed (may be < buflen).
 *	On XTC_E_AGAIN the fd was not writable; *out_n is 0.
 *	On XTC_E_INVAL tls, buf, or out_n is NULL.
 *	On XTC_E_NOSYS TLS was not compiled in.
 */
XTC_API int  xtc_tls_write(xtc_tls_t *tls, const void *buf, size_t buflen,
                           size_t *out_n);

/* -------------------------------------------------------------------------
 * Readiness queries.
 * ----------------------------------------------------------------------- */

/*
 * xtc_tls_wants_read --
 *	Return non-zero if the most recent TLS operation stalled waiting
 *	for the underlying fd to become readable.  The caller should arm
 *	a POLLIN/XTC_IO_READABLE watch and retry.
 */
XTC_API int  xtc_tls_wants_read(const xtc_tls_t *tls);

/*
 * xtc_tls_wants_write --
 *	Return non-zero if the most recent TLS operation stalled waiting
 *	for the underlying fd to become writable.  The caller should arm
 *	a POLLOUT/XTC_IO_WRITABLE watch and retry.
 */
XTC_API int  xtc_tls_wants_write(const xtc_tls_t *tls);

/* -------------------------------------------------------------------------
 * Graceful shutdown.
 * ----------------------------------------------------------------------- */

/*
 * PUBLIC: int  xtc_tls_shutdown __P((xtc_tls_t *));
 */

/*
 * xtc_tls_shutdown --
 *	Initiate or continue a TLS close_notify shutdown.
 *
 *	Returns:
 *	  XTC_OK        shutdown complete; the underlying fd may be closed
 *	  XTC_E_AGAIN   not yet done; poll for readiness as with the handshake
 *	                and call again
 *	  XTC_E_INVAL   tls is NULL
 *	  XTC_E_NOSYS   TLS not compiled in
 */
XTC_API int  xtc_tls_shutdown(xtc_tls_t *tls);

/* -------------------------------------------------------------------------
 * Post-handshake introspection.
 *
 * Valid only after xtc_tls_handshake has returned XTC_OK.  These back a
 * consumer's connection logging / statistics, certificate-based auth,
 * and SCRAM channel binding.  On an unconnected handle, a backend that
 * does not implement the accessor, or --with-tls=none, the const char *
 * getters return NULL, the int getters return 0 or XTC_E_NOSYS, and the
 * buffer-filling getters return XTC_E_NOSYS.
 * ----------------------------------------------------------------------- */

/*
 * PUBLIC: const char *xtc_tls_get_version __P((const xtc_tls_t *));
 * PUBLIC: const char *xtc_tls_get_cipher __P((const xtc_tls_t *));
 * PUBLIC: int  xtc_tls_get_cipher_bits __P((const xtc_tls_t *));
 * PUBLIC: int  xtc_tls_get_alpn_selected __P((const xtc_tls_t *,
 * PUBLIC:                              const unsigned char **, unsigned int *));
 * PUBLIC: int  xtc_tls_has_peer_cert __P((const xtc_tls_t *));
 * PUBLIC: int  xtc_tls_get_verify_error __P((const xtc_tls_t *, long *,
 * PUBLIC:                              char *, size_t));
 * PUBLIC: int  xtc_tls_get_peer_subject_dn __P((const xtc_tls_t *, char *, size_t));
 * PUBLIC: int  xtc_tls_get_peer_common_name __P((const xtc_tls_t *, char *, size_t));
 * PUBLIC: int  xtc_tls_get_peer_issuer_dn __P((const xtc_tls_t *, char *, size_t));
 * PUBLIC: int  xtc_tls_get_peer_serial __P((const xtc_tls_t *, char *, size_t));
 * PUBLIC: int  xtc_tls_get_server_cert_hash __P((const xtc_tls_t *,
 * PUBLIC:                              unsigned char *, size_t, size_t *));
 */

/* Negotiated protocol version string, e.g. "TLSv1.3".  NULL if none. */
XTC_API const char *xtc_tls_get_version(const xtc_tls_t *tls);

/* Negotiated cipher suite name.  NULL if none. */
XTC_API const char *xtc_tls_get_cipher(const xtc_tls_t *tls);

/* Symmetric key strength in bits of the negotiated cipher, or 0. */
XTC_API int  xtc_tls_get_cipher_bits(const xtc_tls_t *tls);

/*
 * ALPN protocol actually selected.  On XTC_OK *out points to the
 * selected protocol bytes (owned by the connection, valid until
 * destroy) and *len is its length.  XTC_E_NOTFOUND if none was
 * negotiated; XTC_E_INVAL on a NULL argument; XTC_E_NOSYS if
 * unsupported.
 */
XTC_API int  xtc_tls_get_alpn_selected(const xtc_tls_t *tls,
                const unsigned char **out, unsigned int *len);

/* Non-zero if the peer presented a certificate that passed verification. */
XTC_API int  xtc_tls_has_peer_cert(const xtc_tls_t *tls);

/*
 * Detail of the most recent peer-certificate verification result, for
 * diagnostics/log parity (e.g. PostgreSQL's verify_cb errdetail).  On
 * XTC_OK, *x509_err receives the backend verify-result code
 * (OpenSSL X509_V_*; X509_V_OK == 0 means verification succeeded) and,
 * if buf != NULL and len > 0, a human-readable reason string
 * (OpenSSL X509_verify_cert_error_string()) is written NUL-terminated
 * into buf (truncated to fit).  x509_err may be NULL if only the text
 * is wanted.  XTC_E_INVAL on a NULL tls; XTC_E_NOSYS if the backend
 * cannot report a verify result.  Note the code is per-connection and
 * only meaningful after the handshake.
 */
XTC_API int  xtc_tls_get_verify_error(const xtc_tls_t *tls, long *x509_err,
                char *buf, size_t len);

/*
 * Peer certificate subject / issuer distinguished name in RFC 2253
 * form (e.g. "CN=x,O=y"), and the subject commonName, written
 * NUL-terminated into buf.  (Note the RFC 2253 comma form; this is not
 * the legacy OpenSSL slash form "/CN=x/O=y" -- a consumer that needs
 * the slash form must reformat.)  A DN containing an embedded NUL is
 * rejected with XTC_E_INVAL (guards the CVE-2009-4034 truncation
 * class).  XTC_E_NOTFOUND if there is no peer certificate; XTC_E_RANGE
 * if buf is too small; XTC_E_NOSYS if unsupported.
 */
XTC_API int  xtc_tls_get_peer_subject_dn(const xtc_tls_t *tls, char *buf, size_t len);
XTC_API int  xtc_tls_get_peer_common_name(const xtc_tls_t *tls, char *buf, size_t len);
XTC_API int  xtc_tls_get_peer_issuer_dn(const xtc_tls_t *tls, char *buf, size_t len);

/* Peer certificate serial number as a decimal string. */
XTC_API int  xtc_tls_get_peer_serial(const xtc_tls_t *tls, char *buf, size_t len);

/*
 * Server-certificate hash for RFC 5929 tls-server-end-point channel
 * binding (the input SCRAM-SHA-256-PLUS needs).  Writes the hash into
 * buf; *out_len receives its length.  The digest follows RFC 5929: a
 * certificate signed with MD5 or SHA-1 is hashed with SHA-256,
 * otherwise the hash matching the certificate's signature algorithm is
 * used.  XTC_E_RANGE if buf is too small (SHA-512 needs 64 bytes),
 * XTC_E_NOTFOUND if there is no certificate, XTC_E_NOSYS if
 * unsupported.
 */
XTC_API int  xtc_tls_get_server_cert_hash(const xtc_tls_t *tls,
                unsigned char *buf, size_t buflen, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* XTC_TLS_H */
