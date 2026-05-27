/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
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
 *	  xtc_tls_ctx_t  — per-process context: loaded cert + key + CA bundle.
 *	  xtc_tls_t      — per-connection state machine.
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

#include <stddef.h>
#include "xtc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Opaque types.
 * ----------------------------------------------------------------------- */

/*
 * xtc_tls_ctx_t — per-process (or per-vhost) TLS context.
 *
 *   Holds the loaded certificate, private key, and CA bundle.  A
 *   single context may be shared by many concurrent connections of
 *   the same role; it is internally reference-safe via immutable
 *   configuration after creation.
 */
typedef struct xtc_tls_ctx  xtc_tls_ctx_t;

/*
 * xtc_tls_t — per-connection TLS state machine.
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
 * Options.
 * ----------------------------------------------------------------------- */

/*
 * xtc_tls_opts_t — creation-time parameters for xtc_tls_ctx_create.
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
 */
typedef struct xtc_tls_opts {
	const char *cert_file;
	const char *key_file;
	const char *ca_file;
	int         verify_peer;
	const char *alpn_protos;
	int         min_version;
	int         max_version;
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
int  xtc_tls_ctx_create(xtc_tls_role_t role,
                        const xtc_tls_opts_t *opts,
                        xtc_tls_ctx_t **out);

/*
 * xtc_tls_ctx_destroy --
 *	Release all resources held by a TLS context.
 *	Must not be called while any xtc_tls_t created from it is live.
 *	ctx may be NULL (no-op).
 */
void xtc_tls_ctx_destroy(xtc_tls_ctx_t *ctx);

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
int  xtc_tls_create(xtc_tls_ctx_t *ctx, int fd, xtc_tls_t **out);

/*
 * xtc_tls_destroy --
 *	Release per-connection TLS state.  Does not close the underlying fd,
 *	does not send close_notify — call xtc_tls_shutdown first if a clean
 *	shutdown is needed.
 *	tls may be NULL (no-op).
 */
void xtc_tls_destroy(xtc_tls_t *tls);

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
int  xtc_tls_handshake(xtc_tls_t *tls);

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
int  xtc_tls_read(xtc_tls_t *tls, void *buf, size_t buflen, size_t *out_n);

/*
 * xtc_tls_write --
 *	Encrypt and write up to buflen bytes from buf.
 *
 *	On XTC_OK, *out_n holds the number of bytes consumed (may be < buflen).
 *	On XTC_E_AGAIN the fd was not writable; *out_n is 0.
 *	On XTC_E_INVAL tls, buf, or out_n is NULL.
 *	On XTC_E_NOSYS TLS was not compiled in.
 */
int  xtc_tls_write(xtc_tls_t *tls, const void *buf, size_t buflen,
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
int  xtc_tls_wants_read(const xtc_tls_t *tls);

/*
 * xtc_tls_wants_write --
 *	Return non-zero if the most recent TLS operation stalled waiting
 *	for the underlying fd to become writable.  The caller should arm
 *	a POLLOUT/XTC_IO_WRITABLE watch and retry.
 */
int  xtc_tls_wants_write(const xtc_tls_t *tls);

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
int  xtc_tls_shutdown(xtc_tls_t *tls);

#ifdef __cplusplus
}
#endif

#endif /* XTC_TLS_H */
