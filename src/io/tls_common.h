/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/io/tls_common.h
 *	Shared bits across the TLS backends (tls_openssl.c, tls_mbedtls.c,
 *	tls_gnutls.c, tls_wolfssl.c).  These backends all implement the
 *	same internal xtc_tls seam over different libraries, and several
 *	pieces were byte-identical in every one.
 *
 *	Compile-time only (static inline / macros): the project keeps no
 *	runtime vtable on the TLS read/write hot path, so this header is
 *	included directly by each backend rather than dispatched through
 *	function pointers.
 *
 *	The backend's struct xtc_tls must begin and end with the common
 *	fields this header touches:
 *
 *	    struct xtc_tls {
 *	        xtc_tls_ctx_t *ctx;     // first
 *	        int            fd;      // second
 *	        ... backend handle ...
 *	        int            wants_read;   // these two last
 *	        int            wants_write;
 *	    };
 *
 *	Only wants_read / wants_write are referenced here, by name, so the
 *	middle (the library session handle) stays private to each backend.
 */
#ifndef XTC_TLS_COMMON_H
#define XTC_TLS_COMMON_H

#include "xtc_int.h"
#include "xtc_tls.h"

/* Clear the would-block direction on a connection.  Called at the top
 * of every read / write / handshake so a fresh attempt does not report
 * a stale direction; the backend then sets wants_read / wants_write
 * directly when its native op reports it would block.  (The struct
 * xtc_tls in each backend must already be complete when this header is
 * included, since this touches the trailing wants_* fields.) */
static inline void
xtc_tls_clear_wants(struct xtc_tls *tls)
{
	tls->wants_read = 0;
	tls->wants_write = 0;
}

/*
 * The wants_read / wants_write accessors are identical in every
 * backend (they just read the trailing flags), so define them once.
 * A backend includes this header after its struct xtc_tls is complete
 * and then writes XTC_TLS_DEFINE_WANTS_ACCESSORS to emit both.
 */
#define XTC_TLS_DEFINE_WANTS_ACCESSORS                                    \
	int                                                              \
	xtc_tls_wants_read(const xtc_tls_t *tls)                         \
	{                                                                \
		return tls != NULL ? tls->wants_read : 0;                \
	}                                                                \
	int                                                              \
	xtc_tls_wants_write(const xtc_tls_t *tls)                        \
	{                                                                \
		return tls != NULL ? tls->wants_write : 0;               \
	}

#endif /* XTC_TLS_COMMON_H */
