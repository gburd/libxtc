/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_aio.h
 *	Async file I/O for fibers.  A read/write/fsync that suspends the
 *	calling fiber and resumes it on completion, using the platform's
 *	best mechanism: a native io_uring completion where available, and
 *	a blocking-pool offload (the thread-pool fallback) elsewhere -- a
 *	regular file is not pollable, so io_uring is the only POSIX
 *	backend that can do file I/O without a worker thread.  Either way
 *	the loop stays live while the I/O is outstanding.
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
 */

int xtc_aio_pread(int fd, void *buf, uint32_t len, int64_t off);
int xtc_aio_pwrite(int fd, const void *buf, uint32_t len, int64_t off);
int xtc_aio_fsync(int fd);

#endif /* XTC_AIO_H */
