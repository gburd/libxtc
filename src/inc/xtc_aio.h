/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_aio.h
 *	Async file I/O for fibers.  A read/write/fsync that suspends the
 *	calling fiber and resumes it on completion, keeping the loop live
 *	for other work meanwhile.
 *
 *	This is a SINGLE, PORTABLE async-file-I/O API: write storage code
 *	once as though true AIO is always available, and it runs unchanged
 *	everywhere libxtc builds.  Where the host has a native completion
 *	engine (io_uring on Linux, IOCP on Windows) the call uses it; where
 *	it does not, the call transparently offloads to the blocking thread
 *	pool -- the SAME API and the same observable behavior (the fiber
 *	parks, the loop keeps running, the call returns the byte count or a
 *	negative errno).  The caller never branches on the platform.
 *
 *	Must be called from a fiber running on a loop; off a loop the op
 *	runs synchronously.  Returns the byte count transferred (>= 0) or
 *	a negative errno; xtc_aio_fsync returns 0 or a negative errno.
 *	See xtc_aio(3).
 */

#ifndef XTC_AIO_H
#define XTC_AIO_H

#include <stdint.h>

/*
 * PUBLIC: int xtc_aio_pread __P((int, void *, uint32_t, int64_t));
 * PUBLIC: int xtc_aio_pwrite __P((int, const void *, uint32_t, int64_t));
 * PUBLIC: int xtc_aio_fsync __P((int));
 * PUBLIC: int xtc_aio_fdatasync __P((int));
 */

int xtc_aio_pread(int fd, void *buf, uint32_t len, int64_t off);
int xtc_aio_pwrite(int fd, const void *buf, uint32_t len, int64_t off);
int xtc_aio_fsync(int fd);       /* full sync: data + metadata */
int xtc_aio_fdatasync(int fd);   /* data only (the page/WAL flush hot path) */

/*
 * Internal / test hook: force the blocking-pool offload path even on a
 * host with a native completion engine (io_uring / IOCP).  Lets the
 * portable fallback be exercised and proven identical to the native
 * path where the tests run.  Also reads XTC_AIO_FORCE_OFFLOAD=1 on
 * first use.  Not part of the stable API.
 */
void __xtc_aio_force_offload(int on);

#endif /* XTC_AIO_H */
