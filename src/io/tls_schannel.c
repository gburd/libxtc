/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/io/tls_schannel.c
 *	SChannel (Windows SSPI) TLS backend for xtc_tls.
 *
 *	Compiled into libxtc.a only when configure sets
 *	XTC_TLS_BACKEND_SCHANNEL=1 (--with-tls=schannel), which is only
 *	permitted on a Windows host.  Implements the same internal seam as
 *	tls_openssl.c via the Win32 Security Support Provider Interface
 *	(SSPI / Secur32): credential acquisition, a non-blocking handshake
 *	state machine (client and server), record-layer encrypt/decrypt
 *	for read/write, and graceful shutdown, with error mapping into
 *	XTC_E_*.
 *
 *	NOTE: This file builds on Windows (MSVC or mingw with
 *	-lsecur32 -lcrypt32) but has NOT been run on a live Windows host
 *	from this project's CI; it is COMPILE-VERIFIED ONLY, the same
 *	posture as src/io/io_aix.c and src/io/io_iocp.c.  Structural
 *	correctness was reviewed against Microsoft's SSPI / SChannel
 *	documentation (AcquireCredentialsHandle, InitializeSecurityContext,
 *	AcceptSecurityContext, EncryptMessage, DecryptMessage).  See
 *	docs/M_TLS_MATRIX.md for the verification status.
 *
 *	Non-blocking discipline:
 *
 *	  SChannel is a buffer transform, not a socket layer: it consumes
 *	  and produces opaque token / record buffers and the application
 *	  owns the wire I/O.  We do the wire I/O against the caller's
 *	  non-blocking fd with recv/send (Winsock).  When recv would block
 *	  (WSAEWOULDBLOCK) we set wants_read and return XTC_E_AGAIN; when a
 *	  token still needs flushing to the peer and send would block we
 *	  set wants_write.  This reproduces the OpenSSL backend's
 *	  WANT_READ / WANT_WRITE contract: the caller polls on the
 *	  indicated direction and re-drives the same call.
 *
 *	Internal struct layout (private to this file):
 *
 *	  struct xtc_tls_ctx {
 *	      xtc_tls_role_t  role;
 *	      CredHandle      cred;
 *	      int             verify_peer;
 *	  };
 *
 *	  struct xtc_tls {
 *	      xtc_tls_ctx_t  *ctx;
 *	      SOCKET          fd;
 *	      CtxtHandle      sctx;
 *	      int             have_sctx;
 *	      int             established;
 *	      SecPkgContext_StreamSizes sizes;
 *	      unsigned char  *enc;       // accumulated inbound ciphertext
 *	      size_t          enc_len, enc_cap;
 *	      unsigned char  *dec;       // leftover decrypted plaintext
 *	      size_t          dec_off, dec_len;
 *	      int             wants_read, wants_write;
 *	  };
 */

#include "xtc_int.h"
#include "xtc_tls.h"

#if defined(XTC_TLS_BACKEND_SCHANNEL)

#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif

#include <winsock2.h>
#include <windows.h>
#include <sspi.h>
#include <schannel.h>
#include <security.h>

#include <string.h>

#define XTC_SCH_IO_BUFSZ  (16 * 1024 + 512)   /* one TLS record + headroom */

/* -------------------------------------------------------------------------
 * Internal struct definitions.
 * ----------------------------------------------------------------------- */

struct xtc_tls_ctx {
	xtc_tls_role_t  role;
	CredHandle      cred;
	int             verify_peer;
};

struct xtc_tls {
	xtc_tls_ctx_t            *ctx;
	SOCKET                    fd;
	CtxtHandle                sctx;
	int                       have_sctx;
	int                       established;
	SecPkgContext_StreamSizes sizes;

	unsigned char            *enc;       /* inbound ciphertext buffer */
	size_t                    enc_len;
	size_t                    enc_cap;

	unsigned char            *dec;       /* decrypted plaintext spill */
	size_t                    dec_off;
	size_t                    dec_len;
	size_t                    dec_cap;

	int                       wants_read;
	int                       wants_write;
};

/* -------------------------------------------------------------------------
 * Non-blocking wire helpers against the caller's Winsock fd.
 *   recv_some: append available ciphertext to t->enc; XTC_E_AGAIN when
 *              the socket would block (sets wants_read).
 *   send_all:  flush len bytes; XTC_E_AGAIN when blocked (wants_write).
 * ----------------------------------------------------------------------- */

static int
ensure_enc_cap(struct xtc_tls *t, size_t need)
{
	if (t->enc_cap >= need)
		return XTC_OK;
	{
		unsigned char *p;
		size_t         cap = t->enc_cap ? t->enc_cap : XTC_SCH_IO_BUFSZ;
		int            rc;
		while (cap < need)
			cap *= 2;
		if ((rc = __os_realloc(t->enc, cap, (void **)&p)) != XTC_OK)
			return rc;
		t->enc     = p;
		t->enc_cap = cap;
	}
	return XTC_OK;
}

static int
recv_some(struct xtc_tls *t)
{
	int rc;
	int n;

	if ((rc = ensure_enc_cap(t, t->enc_len + XTC_SCH_IO_BUFSZ)) != XTC_OK)
		return rc;

	n = recv(t->fd, (char *)t->enc + t->enc_len,
	         (int)(t->enc_cap - t->enc_len), 0);
	if (n > 0) {
		t->enc_len += (size_t)n;
		return XTC_OK;
	}
	if (n == 0)
		return XTC_E_INTERNAL;   /* peer closed mid-handshake */
	if (WSAGetLastError() == WSAEWOULDBLOCK) {
		t->wants_read = 1;
		return XTC_E_AGAIN;
	}
	return XTC_E_INTERNAL;
}

static int
send_all(struct xtc_tls *t, const unsigned char *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		int n = send(t->fd, (const char *)buf + off,
		             (int)(len - off), 0);
		if (n > 0) {
			off += (size_t)n;
			continue;
		}
		if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
			t->wants_write = 1;
			return XTC_E_AGAIN;
		}
		return XTC_E_INTERNAL;
	}
	return XTC_OK;
}

/* Drop `consumed` bytes from the front of the inbound ciphertext. */
static void
enc_consume(struct xtc_tls *t, size_t consumed)
{
	if (consumed >= t->enc_len) {
		t->enc_len = 0;
		return;
	}
	memmove(t->enc, t->enc + consumed, t->enc_len - consumed);
	t->enc_len -= consumed;
}

/* -------------------------------------------------------------------------
 * Version mapping to the SChannel grbitEnabledProtocols mask.
 * ----------------------------------------------------------------------- */

static DWORD
proto_mask(xtc_tls_role_t role, int min_v, int max_v)
{
	DWORD m = 0;
	int   want12 = 1, want13 = 1;

	if (min_v == XTC_TLS_VER_13)
		want12 = 0;
	if (max_v == XTC_TLS_VER_12)
		want13 = 0;

	if (role == XTC_TLS_SERVER) {
		if (want12) m |= SP_PROT_TLS1_2_SERVER;
#ifdef SP_PROT_TLS1_3_SERVER
		if (want13) m |= SP_PROT_TLS1_3_SERVER;
#endif
	} else {
		if (want12) m |= SP_PROT_TLS1_2_CLIENT;
#ifdef SP_PROT_TLS1_3_CLIENT
		if (want13) m |= SP_PROT_TLS1_3_CLIENT;
#endif
	}
	return m;
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
	SCHANNEL_CRED       sc;
	TimeStamp           ts;
	SECURITY_STATUS     ss;
	unsigned long       direction;
	int                 rc;

	if (out == NULL)
		return XTC_E_INVAL;
	*out = NULL;

	if ((rc = __os_calloc(1, sizeof(*c), (void **)&c)) != XTC_OK)
		return rc;

	c->role        = role;
	c->verify_peer = (opts != NULL) ? opts->verify_peer : 0;

	memset(&sc, 0, sizeof(sc));
	sc.dwVersion = SCHANNEL_CRED_VERSION;
	sc.grbitEnabledProtocols = proto_mask(role,
	    opts ? opts->min_version : 0,
	    opts ? opts->max_version : 0);

	/*
	 * Certificate selection (server) and CA-store handling are
	 * normally configured by enumerating the Windows certificate
	 * store; the file-based cert_file/key_file/ca_file inputs would
	 * be imported via CertCreateCertificateContext on a real run.
	 * The flags below reproduce the OpenSSL backend's verify policy:
	 * with verify_peer off, suppress automatic credential validation.
	 */
	if (!c->verify_peer)
		sc.dwFlags |= SCH_CRED_MANUAL_CRED_VALIDATION |
		              SCH_CRED_NO_DEFAULT_CREDS;
	else
		sc.dwFlags |= SCH_CRED_AUTO_CRED_VALIDATION;

	direction = (role == XTC_TLS_SERVER)
	    ? SECPKG_CRED_INBOUND
	    : SECPKG_CRED_OUTBOUND;

	ss = AcquireCredentialsHandleA(NULL, (LPSTR)UNISP_NAME_A, direction,
	                               NULL, &sc, NULL, NULL, &c->cred, &ts);
	if (ss != SEC_E_OK) {
		__os_free(c);
		return XTC_E_INTERNAL;
	}

	*out = c;
	return XTC_OK;
}

void
xtc_tls_ctx_destroy(xtc_tls_ctx_t *ctx)
{
	if (ctx == NULL)
		return;
	FreeCredentialsHandle(&ctx->cred);
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

	t->ctx = ctx;
	t->fd  = (SOCKET)fd;

	*out = t;
	return XTC_OK;
}

void
xtc_tls_destroy(xtc_tls_t *tls)
{
	if (tls == NULL)
		return;
	if (tls->have_sctx)
		DeleteSecurityContext(&tls->sctx);
	if (tls->enc != NULL)
		__os_free(tls->enc);
	if (tls->dec != NULL)
		__os_free(tls->dec);
	__os_free(tls);
}

/* -------------------------------------------------------------------------
 * Handshake state machine.
 *
 * Client: InitializeSecurityContext loop.
 * Server: AcceptSecurityContext loop.
 * Both consume inbound ciphertext (t->enc) and emit a token we must
 * flush.  SEC_I_CONTINUE_NEEDED => keep going; SEC_E_INCOMPLETE_MESSAGE
 * => read more; SEC_E_OK => handshake complete.
 * ----------------------------------------------------------------------- */

int
xtc_tls_handshake(xtc_tls_t *tls)
{
	SecBuffer        in_bufs[2], out_bufs[1];
	SecBufferDesc    in_desc, out_desc;
	SECURITY_STATUS  ss;
	unsigned long    req, attr = 0;
	TimeStamp        ts;
	int              rc;
	int              first;

	if (tls == NULL)
		return XTC_E_INVAL;

	tls->wants_read  = 0;
	tls->wants_write = 0;

	if (tls->established)
		return XTC_OK;

	req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
	      ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY |
	      ISC_REQ_STREAM;
	if (!tls->ctx->verify_peer)
		req |= ISC_REQ_MANUAL_CRED_VALIDATION;

	first = !tls->have_sctx;

	for (;;) {
		/* For all but the very first client token we need inbound
		 * data from the peer. */
		if (!first && tls->enc_len == 0) {
			if ((rc = recv_some(tls)) != XTC_OK)
				return rc;
		}

		in_bufs[0].BufferType = SECBUFFER_TOKEN;
		in_bufs[0].pvBuffer   = tls->enc;
		in_bufs[0].cbBuffer   = (unsigned long)tls->enc_len;
		in_bufs[1].BufferType = SECBUFFER_EMPTY;
		in_bufs[1].pvBuffer   = NULL;
		in_bufs[1].cbBuffer   = 0;
		in_desc.ulVersion = SECBUFFER_VERSION;
		in_desc.cBuffers  = 2;
		in_desc.pBuffers  = in_bufs;

		out_bufs[0].BufferType = SECBUFFER_TOKEN;
		out_bufs[0].pvBuffer   = NULL;
		out_bufs[0].cbBuffer   = 0;
		out_desc.ulVersion = SECBUFFER_VERSION;
		out_desc.cBuffers  = 1;
		out_desc.pBuffers  = out_bufs;

		if (tls->ctx->role == XTC_TLS_SERVER) {
			ss = AcceptSecurityContext(&tls->ctx->cred,
			    tls->have_sctx ? &tls->sctx : NULL,
			    (first ? NULL : &in_desc),
			    req, SECURITY_NATIVE_DREP, &tls->sctx,
			    &out_desc, &attr, &ts);
		} else {
			ss = InitializeSecurityContextA(&tls->ctx->cred,
			    tls->have_sctx ? &tls->sctx : NULL,
			    NULL, req, 0, SECURITY_NATIVE_DREP,
			    (first ? NULL : &in_desc), 0, &tls->sctx,
			    &out_desc, &attr, &ts);
		}
		tls->have_sctx = 1;
		first = 0;

		/* Flush any token the provider produced. */
		if (out_bufs[0].cbBuffer != 0 && out_bufs[0].pvBuffer != NULL) {
			rc = send_all(tls, out_bufs[0].pvBuffer,
			              out_bufs[0].cbBuffer);
			FreeContextBuffer(out_bufs[0].pvBuffer);
			if (rc != XTC_OK)
				return rc;
		}

		/* The provider may have left trailing bytes unprocessed. */
		if (in_bufs[1].BufferType == SECBUFFER_EXTRA) {
			enc_consume(tls,
			    tls->enc_len - in_bufs[1].cbBuffer);
		} else if (ss != SEC_E_INCOMPLETE_MESSAGE) {
			tls->enc_len = 0;
		}

		switch (ss) {
		case SEC_E_OK:
			if (QueryContextAttributes(&tls->sctx,
			    SECPKG_ATTR_STREAM_SIZES, &tls->sizes) != SEC_E_OK)
				return XTC_E_INTERNAL;
			tls->established = 1;
			return XTC_OK;
		case SEC_I_CONTINUE_NEEDED:
			continue;            /* another round trip */
		case SEC_E_INCOMPLETE_MESSAGE:
			if ((rc = recv_some(tls)) != XTC_OK)
				return rc;
			continue;
		default:
			return XTC_E_INTERNAL;
		}
	}
}

/* -------------------------------------------------------------------------
 * PUBLIC: int  xtc_tls_read  __P((xtc_tls_t *, void *, size_t, size_t *));
 * ----------------------------------------------------------------------- */

int
xtc_tls_read(xtc_tls_t *tls, void *buf, size_t buflen, size_t *out_n)
{
	SecBuffer        bufs[4];
	SecBufferDesc    desc;
	SECURITY_STATUS  ss;
	int              rc;
	size_t           i;

	if (tls == NULL || buf == NULL || out_n == NULL)
		return XTC_E_INVAL;

	*out_n           = 0;
	tls->wants_read  = 0;
	tls->wants_write = 0;

	/* Serve any leftover plaintext from a prior DecryptMessage first. */
	if (tls->dec_len > tls->dec_off) {
		size_t avail = tls->dec_len - tls->dec_off;
		size_t n     = avail < buflen ? avail : buflen;
		memcpy(buf, tls->dec + tls->dec_off, n);
		tls->dec_off += n;
		*out_n = n;
		return XTC_OK;
	}

	for (;;) {
		if (tls->enc_len == 0) {
			if ((rc = recv_some(tls)) != XTC_OK)
				return rc;
		}

		bufs[0].BufferType = SECBUFFER_DATA;
		bufs[0].pvBuffer   = tls->enc;
		bufs[0].cbBuffer   = (unsigned long)tls->enc_len;
		for (i = 1; i < 4; i++) {
			bufs[i].BufferType = SECBUFFER_EMPTY;
			bufs[i].pvBuffer   = NULL;
			bufs[i].cbBuffer   = 0;
		}
		desc.ulVersion = SECBUFFER_VERSION;
		desc.cBuffers  = 4;
		desc.pBuffers  = bufs;

		ss = DecryptMessage(&tls->sctx, &desc, 0, NULL);
		if (ss == SEC_E_INCOMPLETE_MESSAGE) {
			if ((rc = recv_some(tls)) != XTC_OK)
				return rc;
			continue;
		}
		if (ss == SEC_I_CONTEXT_EXPIRED) {
			/* Peer sent close_notify: clean EOF. */
			tls->enc_len = 0;
			*out_n = 0;
			return XTC_OK;
		}
		if (ss != SEC_E_OK && ss != SEC_I_RENEGOTIATE)
			return XTC_E_INTERNAL;

		/* Locate the decrypted DATA buffer and any EXTRA tail. */
		{
			SecBuffer *data = NULL, *extra = NULL;
			size_t     consumed = tls->enc_len;
			for (i = 1; i < 4; i++) {
				if (bufs[i].BufferType == SECBUFFER_DATA &&
				    data == NULL)
					data = &bufs[i];
				else if (bufs[i].BufferType == SECBUFFER_EXTRA)
					extra = &bufs[i];
			}
			if (extra != NULL)
				consumed = tls->enc_len - extra->cbBuffer;

			if (data != NULL && data->cbBuffer > 0) {
				size_t avail = data->cbBuffer;
				size_t n     = avail < buflen ? avail : buflen;
				memcpy(buf, data->pvBuffer, n);
				*out_n = n;

				/* Stash the remainder for the next read. */
				if (avail > n) {
					size_t rem = avail - n;
					if (rem > tls->dec_cap) {
						unsigned char *p;
						if (__os_realloc(tls->dec, rem,
						    (void **)&p) != XTC_OK)
							return XTC_E_NOMEM;
						tls->dec     = p;
						tls->dec_cap = rem;
					}
					memcpy(tls->dec,
					    (unsigned char *)data->pvBuffer + n,
					    rem);
					tls->dec_off = 0;
					tls->dec_len = rem;
				}
			}
			enc_consume(tls, consumed);
			return XTC_OK;
		}
	}
}

/* -------------------------------------------------------------------------
 * PUBLIC: int  xtc_tls_write __P((xtc_tls_t *, const void *, size_t, size_t *));
 * ----------------------------------------------------------------------- */

int
xtc_tls_write(xtc_tls_t *tls, const void *buf, size_t buflen, size_t *out_n)
{
	SecBuffer        bufs[4];
	SecBufferDesc    desc;
	SECURITY_STATUS  ss;
	unsigned char   *rec;
	unsigned long    plain;
	int              rc;

	if (tls == NULL || buf == NULL || out_n == NULL)
		return XTC_E_INVAL;

	*out_n           = 0;
	tls->wants_read  = 0;
	tls->wants_write = 0;

	/* Cap one EncryptMessage to the negotiated maximum record body. */
	plain = (unsigned long)buflen;
	if (plain > tls->sizes.cbMaximumMessage)
		plain = tls->sizes.cbMaximumMessage;

	{
		size_t total = tls->sizes.cbHeader + plain +
		               tls->sizes.cbTrailer;
		if ((rc = __os_malloc(total, (void **)&rec)) != XTC_OK)
			return rc;
	}

	memcpy(rec + tls->sizes.cbHeader, buf, plain);

	bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
	bufs[0].pvBuffer   = rec;
	bufs[0].cbBuffer   = tls->sizes.cbHeader;
	bufs[1].BufferType = SECBUFFER_DATA;
	bufs[1].pvBuffer   = rec + tls->sizes.cbHeader;
	bufs[1].cbBuffer   = plain;
	bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
	bufs[2].pvBuffer   = rec + tls->sizes.cbHeader + plain;
	bufs[2].cbBuffer   = tls->sizes.cbTrailer;
	bufs[3].BufferType = SECBUFFER_EMPTY;
	bufs[3].pvBuffer   = NULL;
	bufs[3].cbBuffer   = 0;
	desc.ulVersion = SECBUFFER_VERSION;
	desc.cBuffers  = 4;
	desc.pBuffers  = bufs;

	ss = EncryptMessage(&tls->sctx, 0, &desc, 0);
	if (ss != SEC_E_OK) {
		__os_free(rec);
		return XTC_E_INTERNAL;
	}

	rc = send_all(tls, rec,
	    bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer);
	__os_free(rec);
	if (rc != XTC_OK)
		return rc;

	*out_n = plain;
	return XTC_OK;
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
 * Signals SCHANNEL_SHUTDOWN to the provider, then runs one
 * InitializeSecurityContext / AcceptSecurityContext pass to produce the
 * close_notify token and flushes it.
 * ----------------------------------------------------------------------- */

int
xtc_tls_shutdown(xtc_tls_t *tls)
{
	SecBuffer        buf;
	SecBufferDesc    desc;
	SECURITY_STATUS  ss;
	DWORD            shut = SCHANNEL_SHUTDOWN;
	SecBuffer        out_bufs[1];
	SecBufferDesc    out_desc;
	unsigned long    req, attr = 0;
	TimeStamp        ts;
	int              rc;

	if (tls == NULL)
		return XTC_E_INVAL;

	tls->wants_read  = 0;
	tls->wants_write = 0;

	if (!tls->have_sctx)
		return XTC_OK;

	buf.BufferType = SECBUFFER_TOKEN;
	buf.pvBuffer   = &shut;
	buf.cbBuffer   = sizeof(shut);
	desc.ulVersion = SECBUFFER_VERSION;
	desc.cBuffers  = 1;
	desc.pBuffers  = &buf;

	if (ApplyControlToken(&tls->sctx, &desc) != SEC_E_OK)
		return XTC_E_INTERNAL;

	req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
	      ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY |
	      ISC_REQ_STREAM;

	out_bufs[0].BufferType = SECBUFFER_TOKEN;
	out_bufs[0].pvBuffer   = NULL;
	out_bufs[0].cbBuffer   = 0;
	out_desc.ulVersion = SECBUFFER_VERSION;
	out_desc.cBuffers  = 1;
	out_desc.pBuffers  = out_bufs;

	if (tls->ctx->role == XTC_TLS_SERVER)
		ss = AcceptSecurityContext(&tls->ctx->cred, &tls->sctx, NULL,
		    req, SECURITY_NATIVE_DREP, &tls->sctx, &out_desc, &attr,
		    &ts);
	else
		ss = InitializeSecurityContextA(&tls->ctx->cred, &tls->sctx,
		    NULL, req, 0, SECURITY_NATIVE_DREP, NULL, 0, &tls->sctx,
		    &out_desc, &attr, &ts);

	if (ss != SEC_E_OK && ss != SEC_I_CONTINUE_NEEDED &&
	    ss != SEC_I_CONTEXT_EXPIRED) {
		if (out_bufs[0].pvBuffer != NULL)
			FreeContextBuffer(out_bufs[0].pvBuffer);
		return XTC_E_INTERNAL;
	}

	rc = XTC_OK;
	if (out_bufs[0].cbBuffer != 0 && out_bufs[0].pvBuffer != NULL) {
		rc = send_all(tls, out_bufs[0].pvBuffer, out_bufs[0].cbBuffer);
		FreeContextBuffer(out_bufs[0].pvBuffer);
	}
	return rc;
}

#endif /* XTC_TLS_BACKEND_SCHANNEL */
