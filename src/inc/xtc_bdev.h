/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_bdev.h
 *	Portable block-device I/O over the async xtc_aio path.  A thin,
 *	uniform surface for reading and writing a raw block device (or a
 *	regular file treated as one) with sector-aligned positioned I/O
 *	that suspends the calling fiber and resumes it on completion --
 *	the same "write once, runs as AIO everywhere" model as
 *	xtc_aio(3), on top of which this layer is built.
 *
 *	xtc_bdev_open probes the device for its logical and physical
 *	sector sizes and total capacity using the host's native query
 *	(BLKSSZGET / BLKGETSIZE64 on Linux, DIOCGSECTORSIZE on the BSDs
 *	and macOS, DKIOCGMEDIAINFO on illumos, the drive geometry IOCTL on
 *	Windows) and opens the descriptor with direct I/O (O_DIRECT) for a
 *	real device so transfers bypass the page cache.  A regular file or
 *	an unqueryable target falls back to logical=512, physical=4096, and
 *	the capacity reported by fstat, with buffered I/O.
 *
 *	xtc_bdev_pread / xtc_bdev_pwrite route through
 *	xtc_aio_pread / xtc_aio_pwrite: on a fiber loop the call parks the
 *	fiber and the loop keeps running other work.  The offset and the
 *	length must both be multiples of the logical sector size, else the
 *	call returns XTC_E_INVAL.  xtc_bdev_flush issues xtc_aio_fsync.
 *
 *	See xtc_bdev(3).
 */

#ifndef XTC_BDEV_H
#define XTC_BDEV_H

#include <stddef.h>
#include <stdint.h>

/* The pread/pwrite return values use the standard POSIX `ssize_t`.  We
 * provide it portably WITHOUT a namespace-polluting #define of the name
 * and WITHOUT redefining it where the platform already has it:
 *   - POSIX: <sys/types.h> defines the real ssize_t; we just use it.
 *   - MinGW/Cygwin on Windows: same -- their <sys/types.h> defines
 *     ssize_t and sets _SSIZE_T_DEFINED.
 *   - MSVC: the CRT has no ssize_t, so we typedef it from SSIZE_T,
 *     guarded by _SSIZE_T_DEFINED (the de-facto sentinel) so a consumer
 *     header that already defined it wins, and a later include of ours
 *     cannot double-define. */
#if defined(_WIN32) && defined(_MSC_VER)
#  ifndef _SSIZE_T_DEFINED
#    define _SSIZE_T_DEFINED
#    include <BaseTsd.h>          /* SSIZE_T */
typedef SSIZE_T ssize_t;
#  endif
#else
#  include <sys/types.h>         /* real ssize_t (POSIX, MinGW, Cygwin) */
#endif

/* Open flags for xtc_bdev_open (subset of xtc_fs flags that make sense
 * for a block device; mapped to O_* per platform). */
#define XTC_BDEV_READ    0x01u   /* open for reading */
#define XTC_BDEV_WRITE   0x02u   /* open for writing (implies read too) */

/* Opaque device handle. */
typedef struct xtc_bdev xtc_bdev_t;

/*
 * PUBLIC: int xtc_bdev_open __P((const char *, int, xtc_bdev_t **));
 *
 * Open path as a block device, probing its geometry.  flags is a mask of
 * XTC_BDEV_READ / XTC_BDEV_WRITE.  On success stores the handle in *out
 * and returns XTC_OK; otherwise a negative XTC_E_* code.
 */
int xtc_bdev_open(const char *path, int flags, xtc_bdev_t **out);

/*
 * PUBLIC: void xtc_bdev_close __P((xtc_bdev_t *));
 *
 * Close the device and free the handle.  NULL is a no-op.
 */
void xtc_bdev_close(xtc_bdev_t *b);

/*
 * PUBLIC: uint32_t xtc_bdev_logical_sector __P((const xtc_bdev_t *));
 * PUBLIC: uint32_t xtc_bdev_physical_sector __P((const xtc_bdev_t *));
 * PUBLIC: uint64_t xtc_bdev_capacity __P((const xtc_bdev_t *));
 *
 * Report the probed logical sector size, physical sector size, and total
 * capacity in bytes.  Offsets and lengths passed to pread/pwrite must be
 * multiples of the logical sector size.
 */
uint32_t xtc_bdev_logical_sector(const xtc_bdev_t *b);
uint32_t xtc_bdev_physical_sector(const xtc_bdev_t *b);
uint64_t xtc_bdev_capacity(const xtc_bdev_t *b);

/*
 * PUBLIC: ssize_t xtc_bdev_pread __P((xtc_bdev_t *, void *, size_t, uint64_t));
 * PUBLIC: ssize_t xtc_bdev_pwrite __P((xtc_bdev_t *, const void *, size_t, uint64_t));
 *
 * Read n bytes into buf / write n bytes from buf at absolute byte offset
 * off, routed through xtc_aio so the fiber parks and the loop stays live.
 * off and n must each be a multiple of the logical sector size, else the
 * call returns XTC_E_INVAL.  On success returns the byte count
 * transferred (>= 0); a short read at end of device returns the partial
 * count.  On failure returns a negative XTC_E_* code.
 */
ssize_t xtc_bdev_pread(xtc_bdev_t *b, void *buf, size_t n, uint64_t off);
ssize_t xtc_bdev_pwrite(xtc_bdev_t *b, const void *buf, size_t n, uint64_t off);

/*
 * PUBLIC: int xtc_bdev_flush __P((xtc_bdev_t *));
 *
 * Flush the device's data and metadata durably via xtc_aio_fsync.
 * Returns XTC_OK or a negative XTC_E_* code.
 */
int xtc_bdev_flush(xtc_bdev_t *b);

#endif /* XTC_BDEV_H */
