/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/aio_int.h
 *	Internal xtc_aio test/debug hook.  __xtc_aio_force_offload forces
 *	the blocking-pool offload path even on a host with a native
 *	completion engine (io_uring / IOCP), so the portable fallback can
 *	be exercised and proven identical to the native path.  Also reads
 *	XTC_AIO_FORCE_OFFLOAD=1 on first use.  Library-internal (the __
 *	prefix) and not part of the stable API -- split out of xtc_aio.h
 *	so no __-prefixed symbol leaks into an installed public header.
 */

#ifndef XTC_AIO_INT_H
#define XTC_AIO_INT_H

void __xtc_aio_force_offload(int on);

#endif /* XTC_AIO_INT_H */
