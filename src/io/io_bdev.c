/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/io/io_bdev.c
 *	Portable block-device I/O over the async xtc_aio path (xtc_bdev_*).
 *	See xtc_bdev.h.  Opens a raw block device (or a regular file used
 *	as one), probes its logical/physical sector sizes and capacity with
 *	the host's native query, and reads/writes with sector-aligned
 *	positioned I/O routed through xtc_aio so the calling fiber parks
 *	while the loop keeps running.
 */

#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#endif

#include "xtc_int.h"
#include "xtc_bdev.h"
#include "xtc_aio.h"
#include "xtc_fs.h"

#include <errno.h>
#include <string.h>

/*
 * PUBLIC: int xtc_bdev_open __P((const char *, int, xtc_bdev_t **));
 * PUBLIC: void xtc_bdev_close __P((xtc_bdev_t *));
 * PUBLIC: uint32_t xtc_bdev_logical_sector __P((const xtc_bdev_t *));
 * PUBLIC: uint32_t xtc_bdev_physical_sector __P((const xtc_bdev_t *));
 * PUBLIC: uint64_t xtc_bdev_capacity __P((const xtc_bdev_t *));
 * PUBLIC: ssize_t xtc_bdev_pread __P((xtc_bdev_t *, void *, size_t, uint64_t));
 * PUBLIC: ssize_t xtc_bdev_pwrite __P((xtc_bdev_t *, const void *, size_t, uint64_t));
 * PUBLIC: int xtc_bdev_flush __P((xtc_bdev_t *));
 */

/* No over-aligned member: max_align_t alignment (__os_calloc) is fine. */
struct xtc_bdev {
	int      fd;
	uint32_t logical;    /* logical sector size (I/O alignment unit) */
	uint32_t physical;   /* physical sector size (informational) */
	uint64_t capacity;   /* total size in bytes */
};

#define BDEV_DEFAULT_LOGICAL   512u
#define BDEV_DEFAULT_PHYSICAL  4096u

/* Map a negative errno (the xtc_aio convention) to a stable XTC_E_ code. */
static int
errno_to_xtc(int e)
{
	switch (e) {
	case EINVAL:	return XTC_E_INVAL;
	case ENOMEM:	return XTC_E_NOMEM;
	case ENOENT:	return XTC_E_NOTFOUND;
	default:	return XTC_E_IO;
	}
}

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(__linux__)
#include <sys/ioctl.h>
#include <linux/fs.h>            /* BLKSSZGET, BLKPBSZGET, BLKGETSIZE64 */
#endif
#if defined(__FreeBSD__) || defined(__APPLE__) || defined(__NetBSD__) || \
    defined(__OpenBSD__) || defined(__DragonFly__)
#include <sys/ioctl.h>
#if defined(__APPLE__)
#include <sys/disk.h>            /* DKIOCGETBLOCKSIZE, DKIOCGETBLOCKCOUNT */
#else
#include <sys/disk.h>            /* DIOCGSECTORSIZE, DIOCGMEDIASIZE */
#endif
#endif
#if defined(__sun) || defined(__illumos__)
#include <sys/ioctl.h>
#include <sys/dkio.h>            /* DKIOCGMEDIAINFO */
#endif

/*
 * Probe sector sizes and capacity for a POSIX fd.  A regular file (or a
 * device whose queries fail) falls back to 512 / 4096 / fstat size, and
 * *is_device is cleared so the caller does not request direct I/O.
 */
static void
probe_posix(int fd, uint32_t *logical, uint32_t *physical,
    uint64_t *capacity, int *is_device)
{
	struct stat st;

	*logical = BDEV_DEFAULT_LOGICAL;
	*physical = BDEV_DEFAULT_PHYSICAL;
	*capacity = 0;
	*is_device = 0;

	if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
		*capacity = (uint64_t)st.st_size;
		return;                       /* regular file: fallback path */
	}

#if defined(__linux__) && defined(BLKSSZGET)
	{
		int lz = 0, pz = 0;
		uint64_t bytes = 0;
		if (ioctl(fd, BLKSSZGET, &lz) == 0 && lz > 0)
			*logical = (uint32_t)lz;
#if defined(BLKPBSZGET)
		if (ioctl(fd, BLKPBSZGET, &pz) == 0 && pz > 0)
			*physical = (uint32_t)pz;
		else
			*physical = *logical;
#else
		*physical = *logical;
#endif
#if defined(BLKGETSIZE64)
		if (ioctl(fd, BLKGETSIZE64, &bytes) == 0)
			*capacity = bytes;
#endif
		*is_device = 1;
		return;
	}
#elif defined(DIOCGSECTORSIZE) && defined(DIOCGMEDIASIZE)
	/* FreeBSD (and other BSDs exposing the GEOM ioctls). */
	{
		u_int sz = 0;
		off_t media = 0;
		if (ioctl(fd, DIOCGSECTORSIZE, &sz) == 0 && sz > 0) {
			*logical = (uint32_t)sz;
			*physical = (uint32_t)sz;
		}
		if (ioctl(fd, DIOCGMEDIASIZE, &media) == 0 && media > 0)
			*capacity = (uint64_t)media;
		*is_device = 1;
		return;
	}
#elif defined(__APPLE__) && defined(DKIOCGETBLOCKSIZE)
	{
		uint32_t bs = 0;
		uint64_t bc = 0;
		if (ioctl(fd, DKIOCGETBLOCKSIZE, &bs) == 0 && bs > 0) {
			*logical = bs;
			*physical = bs;
		}
		if (ioctl(fd, DKIOCGETBLOCKCOUNT, &bc) == 0)
			*capacity = bc * (uint64_t)(*logical);
		*is_device = 1;
		return;
	}
#elif (defined(__sun) || defined(__illumos__)) && defined(DKIOCGMEDIAINFO)
	{
		struct dk_minfo mi;
		memset(&mi, 0, sizeof mi);
		if (ioctl(fd, DKIOCGMEDIAINFO, &mi) == 0 &&
		    mi.dki_lbsize > 0) {
			*logical = (uint32_t)mi.dki_lbsize;
			*physical = (uint32_t)mi.dki_lbsize;
			*capacity = (uint64_t)mi.dki_capacity *
			    (uint64_t)mi.dki_lbsize;
			*is_device = 1;
			return;
		}
	}
#endif
	/* Unqueryable device: keep defaults, capacity via fstat if any. */
	*capacity = (uint64_t)st.st_size;
}

int
xtc_bdev_open(const char *path, int flags, xtc_bdev_t **out)
{
	xtc_bdev_t *b;
	int oflags, fd, is_device = 0, rc;

	if (path == NULL || out == NULL)
		return XTC_E_INVAL;
	if ((flags & (XTC_BDEV_READ | XTC_BDEV_WRITE)) == 0)
		return XTC_E_INVAL;

	oflags = (flags & XTC_BDEV_WRITE) ? O_RDWR : O_RDONLY;

	/* First open buffered so the geometry probe works even on a plain
	 * file; if it is a real device, reopen with O_DIRECT. */
	fd = open(path, oflags);      /* XTC_BLOCKING_OK: one-shot open */
	if (fd < 0)
		return errno_to_xtc(errno);

	if ((rc = __os_calloc(1, sizeof *b, (void **)&b)) != XTC_OK) {
		(void)close(fd);
		return rc;
	}

	probe_posix(fd, &b->logical, &b->physical, &b->capacity, &is_device);

#if defined(O_DIRECT)
	if (is_device) {
		int dfd = open(path, oflags | O_DIRECT);
		if (dfd >= 0) {
			(void)close(fd);
			fd = dfd;
		}
		/* If O_DIRECT open failed, keep the buffered fd. */
	}
#endif
	b->fd = fd;
	*out = b;
	return XTC_OK;
}

void
xtc_bdev_close(xtc_bdev_t *b)
{
	if (b == NULL)
		return;
	if (b->fd >= 0)
		(void)close(b->fd);
	__os_free(b);
}

#else /* _WIN32 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>            /* IOCTL_DISK_GET_DRIVE_GEOMETRY_EX */
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>

int
xtc_bdev_open(const char *path, int flags, xtc_bdev_t **out)
{
	xtc_bdev_t *b;
	int oflags = _O_BINARY, fd, rc;

	if (path == NULL || out == NULL)
		return XTC_E_INVAL;
	if ((flags & (XTC_BDEV_READ | XTC_BDEV_WRITE)) == 0)
		return XTC_E_INVAL;

	oflags |= (flags & XTC_BDEV_WRITE) ? _O_RDWR : _O_RDONLY;
	fd = _open(path, oflags, _S_IREAD | _S_IWRITE);
	if (fd < 0)
		return errno == ENOENT ? XTC_E_NOTFOUND : XTC_E_IO;

	if ((rc = __os_calloc(1, sizeof *b, (void **)&b)) != XTC_OK) {
		(void)_close(fd);
		return rc;
	}

	/* ponytail: geometry probe is a defaults-returning stub on Windows;
	 * an IOCTL_DISK_GET_DRIVE_GEOMETRY_EX query on a \\.\PhysicalDriveN
	 * handle is the upgrade path when raw-device support lands there. */
	b->fd = fd;
	b->logical = BDEV_DEFAULT_LOGICAL;
	b->physical = BDEV_DEFAULT_PHYSICAL;
	{
		__int64 z = _filelengthi64(fd);
		b->capacity = z > 0 ? (uint64_t)z : 0;
	}
	*out = b;
	return XTC_OK;
}

void
xtc_bdev_close(xtc_bdev_t *b)
{
	if (b == NULL)
		return;
	if (b->fd >= 0)
		(void)_close(b->fd);
	__os_free(b);
}

#endif /* _WIN32 vs POSIX */

uint32_t
xtc_bdev_logical_sector(const xtc_bdev_t *b)
{
	return b != NULL ? b->logical : 0;
}

uint32_t
xtc_bdev_physical_sector(const xtc_bdev_t *b)
{
	return b != NULL ? b->physical : 0;
}

uint64_t
xtc_bdev_capacity(const xtc_bdev_t *b)
{
	return b != NULL ? b->capacity : 0;
}

/* offset and length must both be multiples of the logical sector. */
static int
aligned(const xtc_bdev_t *b, size_t n, uint64_t off)
{
	uint32_t s = b->logical;
	return s != 0 && (off % s) == 0 && (n % s) == 0;
}

ssize_t
xtc_bdev_pread(xtc_bdev_t *b, void *buf, size_t n, uint64_t off)
{
	int r;

	if (b == NULL || buf == NULL)
		return XTC_E_INVAL;
	if (!aligned(b, n, off))
		return XTC_E_INVAL;
	if (n == 0)
		return 0;
	if (n > 0xffffffffu || off > (uint64_t)INT64_MAX)
		return XTC_E_INVAL;

	r = xtc_aio_pread(b->fd, buf, (uint32_t)n, (int64_t)off);
	if (r < 0)
		return errno_to_xtc(-r);
	return (ssize_t)r;
}

ssize_t
xtc_bdev_pwrite(xtc_bdev_t *b, const void *buf, size_t n, uint64_t off)
{
	int r;

	if (b == NULL || buf == NULL)
		return XTC_E_INVAL;
	if (!aligned(b, n, off))
		return XTC_E_INVAL;
	if (n == 0)
		return 0;
	if (n > 0xffffffffu || off > (uint64_t)INT64_MAX)
		return XTC_E_INVAL;

	r = xtc_aio_pwrite(b->fd, buf, (uint32_t)n, (int64_t)off);
	if (r < 0)
		return errno_to_xtc(-r);
	return (ssize_t)r;
}

int
xtc_bdev_flush(xtc_bdev_t *b)
{
	int r;

	if (b == NULL)
		return XTC_E_INVAL;
	r = xtc_aio_fsync(b->fd);
	return r == 0 ? XTC_OK : errno_to_xtc(-r);
}
