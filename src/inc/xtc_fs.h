/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_fs.h
 *	Portable synchronous filesystem helpers.  A small, uniform surface
 *	over the platform's file API (POSIX fds / Win32) so callers do not
 *	re-roll open/read/write/stat/rename/dir-walk and the per-platform
 *	quirks (F_FULLFSYNC vs fdatasync vs _commit, mkstemp vs _mktemp_s,
 *	dirent vs FindFirstFile, $TMPDIR vs GetTempPath).
 *
 *	Files are int fds, so a handle from xtc_fs_open composes directly
 *	with the async xtc_aio_* ops and with raw read/write.  read/write
 *	retry on EINTR/EAGAIN and loop to completion; a short pread is end
 *	of file.  Functions return XTC_OK or a negative XTC_E_* code (see
 *	xtc_strerror); byte counts and metadata come back via out-params.
 *
 *	These are blocking calls -- on a fiber loop, prefer xtc_aio_* for
 *	the read/write hot path and use these for open/stat/rename/dir
 *	management.  See xtc_fs(3).
 */

#ifndef XTC_FS_H
#define XTC_FS_H

#include <stddef.h>
#include <stdint.h>

/* Flags for xtc_fs_open (mapped to O_* / _O_* per platform). */
#define XTC_FS_READ    0x01u   /* open for reading */
#define XTC_FS_WRITE   0x02u   /* open for writing */
#define XTC_FS_CREATE  0x04u   /* create the file if it does not exist */
#define XTC_FS_TRUNC   0x08u   /* truncate to zero length on open */
#define XTC_FS_APPEND  0x10u   /* each write appends at end of file */
#define XTC_FS_EXCL    0x20u   /* with CREATE: fail if the file exists */

/* Metadata returned by xtc_fs_stat. */
typedef struct xtc_fs_stat {
	int64_t size;        /* size in bytes */
	int64_t mtime_ns;    /* last-modified time, nanoseconds since the epoch */
	int     is_dir;      /* 1 if a directory, 0 otherwise */
} xtc_fs_stat_t;

/* Opaque directory iterator (xtc_fs_dir_open/next/close). */
typedef struct xtc_fs_dir xtc_fs_dir_t;

/* ---- file handles (int fd; composes with xtc_aio_* and read/write) ---- */
int xtc_fs_open(const char *path, uint32_t flags, int *out_fd);
int xtc_fs_close(int fd);
int xtc_fs_pread(int fd, void *buf, size_t n, int64_t off, size_t *out_done);
int xtc_fs_pwrite(int fd, const void *buf, size_t n, int64_t off, size_t *out_done);
int xtc_fs_fsync(int fd);          /* data + metadata durable */
int xtc_fs_fdatasync(int fd);      /* data durable (metadata best-effort) */
int xtc_fs_ftruncate(int fd, int64_t len);
int xtc_fs_fsize(int fd, int64_t *out_size);

/* ---- namespace operations ---- */
int xtc_fs_stat(const char *path, xtc_fs_stat_t *out);
int xtc_fs_exists(const char *path);            /* returns 1 (yes) or 0 (no) */
int xtc_fs_unlink(const char *path);
int xtc_fs_rename(const char *from, const char *to);   /* atomic replace */
int xtc_fs_mkdir(const char *path);
int xtc_fs_rmdir(const char *path);

/* ---- temporary files ---- */
int xtc_fs_tmpdir(char *buf, size_t cap);       /* $TMPDIR/$TMP/$TEMP, else /tmp */
int xtc_fs_mkstemp(char *tmpl, int *out_fd);    /* tmpl ends in "XXXXXX" */

/* ---- directory iteration ---- */
int  xtc_fs_dir_open(const char *path, xtc_fs_dir_t **out);
int  xtc_fs_dir_next(xtc_fs_dir_t *d, const char **out_name);  /* *out_name == NULL at end */
void xtc_fs_dir_close(xtc_fs_dir_t *d);

#endif /* XTC_FS_H */
