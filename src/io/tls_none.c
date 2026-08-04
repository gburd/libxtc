/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/io/tls_none.c
 *	NOSYS TLS backend for xtc_tls.
 *
 *	Compiled into libxtc.a only when configure selects no TLS backend
 *	(--with-tls=none, or auto with no usable library found).  In that
 *	case XTC_TLS_BACKEND_NONE is defined and this file provides the
 *	public xtc_tls_* symbols so callers (and test_tls_basic) link
 *	unconditionally.  Every operation returns XTC_E_NOSYS; argument
 *	validation matches the real backends so callers still get
 *	XTC_E_INVAL on bad inputs even without TLS.
 *
 *	Exactly one tls_<backend>.c is compiled per build; the active
 *	backend's source defines the real implementation and this file is
 *	excluded (and vice-versa), so there is never a duplicate-symbol
 *	clash.
 */

#include "xtc_int.h"
#include "xtc_tls.h"
#define XTC_TLS_NO_STRUCT   /* opaque struct here; skip the struct-dependent inline */
#include "tls_common.h"

#if defined(XTC_TLS_BACKEND_NONE)

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

int
xtc_tls_set_hostname(xtc_tls_t *tls, const char *name)
{
	(void)tls; (void)name;
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

XTC_TLS_DEFINE_INTROSPECT_STUBS

#endif /* XTC_TLS_BACKEND_NONE */
