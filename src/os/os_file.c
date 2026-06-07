/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/os_file.c
 *	Portable synchronous filesystem helpers (xtc_fs_*).  See xtc_fs.h.
 *	Two implementations: POSIX (fds, dirent, fdatasync) and Win32 (CRT
 *	fds + overlapped ReadFile/WriteFile for positioned I/O, _commit,
 *	FindFirstFile).  Files are int fds so a handle composes with the
 *	async xtc_aio_* ops.
 */

#include "xtc_int.h"
#include "xtc_fs.h"

#include <errno.h>
#include <stdio.h>      /* rename, snprintf */
#include <stdlib.h>
#include <string.h>

/*
 * PUBLIC: int xtc_fs_open __P((const char *, uint32_t, int *));
 * PUBLIC: int xtc_fs_close __P((int));
 * PUBLIC: int xtc_fs_pread __P((int, void *, size_t, int64_t, size_t *));
 * PUBLIC: int xtc_fs_pwrite __P((int, const void *, size_t, int64_t, size_t *));
 * PUBLIC: int xtc_fs_fsync __P((int));
 * PUBLIC: int xtc_fs_fdatasync __P((int));
 * PUBLIC: int xtc_fs_ftruncate __P((int, int64_t));
 * PUBLIC: int xtc_fs_fsize __P((int, int64_t *));
 * PUBLIC: int xtc_fs_stat __P((const char *, xtc_fs_stat_t *));
 * PUBLIC: int xtc_fs_exists __P((const char *));
 * PUBLIC: int xtc_fs_unlink __P((const char *));
 * PUBLIC: int xtc_fs_rename __P((const char *, const char *));
 * PUBLIC: int xtc_fs_mkdir __P((const char *));
 * PUBLIC: int xtc_fs_rmdir __P((const char *));
 * PUBLIC: int xtc_fs_tmpdir __P((char *, size_t));
 * PUBLIC: int xtc_fs_mkstemp __P((char *, int *));
 * PUBLIC: int xtc_fs_dir_open __P((const char *, xtc_fs_dir_t **));
 * PUBLIC: int xtc_fs_dir_next __P((xtc_fs_dir_t *, const char **));
 * PUBLIC: void xtc_fs_dir_close __P((xtc_fs_dir_t *));
 */

#if defined(_WIN32)
/* ===================== Windows (Win32 + CRT) ======================= */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

struct xtc_fs_dir {
	HANDLE          h;
	WIN32_FIND_DATAA fd;
	int             first;     /* 1 until the first FindNextFile */
	int             done;
};

static int
err_map(DWORD e)
{
	switch (e) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:	return XTC_E_NOTFOUND;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:		return XTC_E_NOMEM;
	case ERROR_INVALID_PARAMETER:	return XTC_E_INVAL;
	default:			return XTC_E_IO;
	}
}

int
xtc_fs_open(const char *path, uint32_t flags, int *out_fd)
{
	int oflags = _O_BINARY, fd;
	if (path == NULL || out_fd == NULL)
		return XTC_E_INVAL;
	if ((flags & XTC_FS_READ) && (flags & XTC_FS_WRITE))
		oflags |= _O_RDWR;
	else if (flags & XTC_FS_WRITE)
		oflags |= _O_WRONLY;
	else
		oflags |= _O_RDONLY;
	if (flags & XTC_FS_CREATE) oflags |= _O_CREAT;
	if (flags & XTC_FS_TRUNC)  oflags |= _O_TRUNC;
	if (flags & XTC_FS_APPEND) oflags |= _O_APPEND;
	if (flags & XTC_FS_EXCL)   oflags |= _O_EXCL;
	fd = _open(path, oflags, _S_IREAD | _S_IWRITE);
	if (fd < 0)
		return errno == ENOENT ? XTC_E_NOTFOUND :
		    errno == EEXIST ? XTC_E_INVAL : XTC_E_IO;
	*out_fd = fd;
	return XTC_OK;
}

int
xtc_fs_close(int fd)
{
	return _close(fd) == 0 ? XTC_OK : XTC_E_IO;
}

int
xtc_fs_pread(int fd, void *buf, size_t n, int64_t off, size_t *out_done)
{
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	size_t done = 0;
	if (out_done) *out_done = 0;
	if (h == INVALID_HANDLE_VALUE) return XTC_E_INVAL;
	while (done < n) {
		OVERLAPPED ov;
		DWORD got = 0, want = (n - done) > 0x40000000u ? 0x40000000u
		    : (DWORD)(n - done);
		uint64_t at = (uint64_t)off + done;
		memset(&ov, 0, sizeof ov);
		ov.Offset = (DWORD)(at & 0xFFFFFFFFu);
		ov.OffsetHigh = (DWORD)(at >> 32);
		if (!ReadFile(h, (char *)buf + done, want, &got, &ov)) {
			if (GetLastError() == ERROR_HANDLE_EOF) break;
			return err_map(GetLastError());
		}
		if (got == 0) break;            /* EOF */
		done += got;
	}
	if (out_done) *out_done = done;
	return XTC_OK;
}

int
xtc_fs_pwrite(int fd, const void *buf, size_t n, int64_t off, size_t *out_done)
{
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	size_t done = 0;
	if (out_done) *out_done = 0;
	if (h == INVALID_HANDLE_VALUE) return XTC_E_INVAL;
	while (done < n) {
		OVERLAPPED ov;
		DWORD put = 0, want = (n - done) > 0x40000000u ? 0x40000000u
		    : (DWORD)(n - done);
		uint64_t at = (uint64_t)off + done;
		memset(&ov, 0, sizeof ov);
		ov.Offset = (DWORD)(at & 0xFFFFFFFFu);
		ov.OffsetHigh = (DWORD)(at >> 32);
		if (!WriteFile(h, (const char *)buf + done, want, &put, &ov))
			return err_map(GetLastError());
		if (put == 0) return XTC_E_IO;
		done += put;
	}
	if (out_done) *out_done = done;
	return XTC_OK;
}

int xtc_fs_fsync(int fd)     { return _commit(fd) == 0 ? XTC_OK : XTC_E_IO; }
int xtc_fs_fdatasync(int fd) { return _commit(fd) == 0 ? XTC_OK : XTC_E_IO; }

int
xtc_fs_ftruncate(int fd, int64_t len)
{
	return _chsize_s(fd, len) == 0 ? XTC_OK : XTC_E_IO;
}

int
xtc_fs_fsize(int fd, int64_t *out_size)
{
	__int64 z = _filelengthi64(fd);
	if (out_size == NULL) return XTC_E_INVAL;
	if (z < 0) return XTC_E_IO;
	*out_size = (int64_t)z;
	return XTC_OK;
}

int
xtc_fs_stat(const char *path, xtc_fs_stat_t *out)
{
	struct __stat64 st;
	if (path == NULL || out == NULL) return XTC_E_INVAL;
	if (_stat64(path, &st) != 0)
		return errno == ENOENT ? XTC_E_NOTFOUND : XTC_E_IO;
	out->size = (int64_t)st.st_size;
	out->mtime_ns = (int64_t)st.st_mtime * 1000000000LL;
	out->is_dir = (st.st_mode & _S_IFDIR) ? 1 : 0;
	return XTC_OK;
}

int xtc_fs_exists(const char *path) { return path && _access(path, 0) == 0; }
int xtc_fs_unlink(const char *path) { return _unlink(path) == 0 ? XTC_OK : (errno == ENOENT ? XTC_E_NOTFOUND : XTC_E_IO); }
int xtc_fs_mkdir(const char *path)  { return _mkdir(path) == 0 ? XTC_OK : XTC_E_IO; }
int xtc_fs_rmdir(const char *path)  { return _rmdir(path) == 0 ? XTC_OK : XTC_E_IO; }

int
xtc_fs_rename(const char *from, const char *to)
{
	/* MoveFileEx with REPLACE_EXISTING gives atomic replace semantics. */
	return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ?
	    XTC_OK : err_map(GetLastError());
}

int
xtc_fs_tmpdir(char *buf, size_t cap)
{
	DWORD n;
	if (buf == NULL || cap == 0) return XTC_E_INVAL;
	n = GetTempPathA((DWORD)cap, buf);    /* honors TMP/TEMP/USERPROFILE */
	if (n == 0 || n >= cap) return XTC_E_IO;
	if (n > 0 && (buf[n - 1] == '\\' || buf[n - 1] == '/')) buf[n - 1] = '\0';
	return XTC_OK;
}

int
xtc_fs_mkstemp(char *tmpl, int *out_fd)
{
	int fd;
	if (tmpl == NULL || out_fd == NULL) return XTC_E_INVAL;
	if (_mktemp_s(tmpl, strlen(tmpl) + 1) != 0) return XTC_E_IO;
	fd = _open(tmpl, _O_BINARY | _O_RDWR | _O_CREAT | _O_EXCL, _S_IREAD | _S_IWRITE);
	if (fd < 0) return XTC_E_IO;
	*out_fd = fd;
	return XTC_OK;
}

int
xtc_fs_dir_open(const char *path, xtc_fs_dir_t **out)
{
	xtc_fs_dir_t *d;
	char pat[1024];
	if (path == NULL || out == NULL) return XTC_E_INVAL;
	if (__os_calloc(1, sizeof *d, (void **)&d) != XTC_OK) return XTC_E_NOMEM;
	snprintf(pat, sizeof pat, "%s\\*", path);
	d->h = FindFirstFileA(pat, &d->fd);
	if (d->h == INVALID_HANDLE_VALUE) {
		DWORD e = GetLastError();
		__os_free(d);
		return e == ERROR_FILE_NOT_FOUND ? XTC_E_NOTFOUND : err_map(e);
	}
	d->first = 1;
	*out = d;
	return XTC_OK;
}

int
xtc_fs_dir_next(xtc_fs_dir_t *d, const char **out_name)
{
	if (d == NULL || out_name == NULL) return XTC_E_INVAL;
	*out_name = NULL;
	for (;;) {
		if (d->done) return XTC_OK;
		if (!d->first && !FindNextFileA(d->h, &d->fd)) { d->done = 1; return XTC_OK; }
		d->first = 0;
		if (strcmp(d->fd.cFileName, ".") == 0 ||
		    strcmp(d->fd.cFileName, "..") == 0)
			continue;
		*out_name = d->fd.cFileName;
		return XTC_OK;
	}
}

void
xtc_fs_dir_close(xtc_fs_dir_t *d)
{
	if (d == NULL) return;
	if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
	__os_free(d);
}

#else
/* ========================== POSIX ================================= */

#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#define FS_RETRY(expr, rc) do {                                  \
	for (;;) {                                               \
		(rc) = (expr);                                   \
		if ((rc) >= 0 || (errno != EINTR && errno != EAGAIN)) break; \
	}                                                        \
} while (0)

struct xtc_fs_dir { DIR *dp; };

static int
err_map(int e)
{
	switch (e) {
	case ENOENT:	return XTC_E_NOTFOUND;
	case ENOMEM:	return XTC_E_NOMEM;
	case EINVAL:	return XTC_E_INVAL;
	case EEXIST:	return XTC_E_INVAL;
	default:	return XTC_E_IO;
	}
}

int
xtc_fs_open(const char *path, uint32_t flags, int *out_fd)
{
	int oflags = 0, fd;
	if (path == NULL || out_fd == NULL)
		return XTC_E_INVAL;
	if ((flags & XTC_FS_READ) && (flags & XTC_FS_WRITE))
		oflags |= O_RDWR;
	else if (flags & XTC_FS_WRITE)
		oflags |= O_WRONLY;
	else
		oflags |= O_RDONLY;
	if (flags & XTC_FS_CREATE) oflags |= O_CREAT;
	if (flags & XTC_FS_TRUNC)  oflags |= O_TRUNC;
	if (flags & XTC_FS_APPEND) oflags |= O_APPEND;
	if (flags & XTC_FS_EXCL)   oflags |= O_EXCL;
	FS_RETRY(open(path, oflags, 0644), fd);    /* XTC_BLOCKING_OK: blocking fs helper */
	if (fd < 0)
		return err_map(errno);
	*out_fd = fd;
	return XTC_OK;
}

int
xtc_fs_close(int fd)
{
	int rc;
	FS_RETRY(close(fd), rc);                   /* XTC_BLOCKING_OK */
	return rc == 0 ? XTC_OK : err_map(errno);
}

int
xtc_fs_pread(int fd, void *buf, size_t n, int64_t off, size_t *out_done)
{
	size_t done = 0;
	if (out_done) *out_done = 0;
	while (done < n) {
		ssize_t r = pread(fd, (char *)buf + done, n - done,  /* XTC_BLOCKING_OK */
		    (off_t)off + (off_t)done);
		if (r < 0) {
			if (errno == EINTR || errno == EAGAIN) continue;
			return err_map(errno);
		}
		if (r == 0) break;                 /* end of file */
		done += (size_t)r;
	}
	if (out_done) *out_done = done;
	return XTC_OK;
}

int
xtc_fs_pwrite(int fd, const void *buf, size_t n, int64_t off, size_t *out_done)
{
	size_t done = 0;
	if (out_done) *out_done = 0;
	while (done < n) {
		ssize_t w = pwrite(fd, (const char *)buf + done, n - done,  /* XTC_BLOCKING_OK */
		    (off_t)off + (off_t)done);
		if (w < 0) {
			if (errno == EINTR || errno == EAGAIN) continue;
			return err_map(errno);
		}
		if (w == 0) return XTC_E_IO;
		done += (size_t)w;
	}
	if (out_done) *out_done = done;
	return XTC_OK;
}

int
xtc_fs_fsync(int fd)
{
	int rc;
#if defined(F_FULLFSYNC)
	rc = fcntl(fd, F_FULLFSYNC, 0);   /* macOS: true platter flush */
	if (rc != 0)
		FS_RETRY(fsync(fd), rc);  /* XTC_BLOCKING_OK */
#else
	FS_RETRY(fsync(fd), rc);          /* XTC_BLOCKING_OK */
#endif
	return rc == 0 ? XTC_OK : err_map(errno);
}

int
xtc_fs_fdatasync(int fd)
{
	int rc;
#if defined(__APPLE__)
	FS_RETRY(fsync(fd), rc);          /* macOS has no fdatasync */ /* XTC_BLOCKING_OK */
#else
	FS_RETRY(fdatasync(fd), rc);      /* XTC_BLOCKING_OK */
#endif
	return rc == 0 ? XTC_OK : err_map(errno);
}

int
xtc_fs_ftruncate(int fd, int64_t len)
{
	int rc;
	FS_RETRY(ftruncate(fd, (off_t)len), rc);   /* XTC_BLOCKING_OK */
	return rc == 0 ? XTC_OK : err_map(errno);
}

int
xtc_fs_fsize(int fd, int64_t *out_size)
{
	struct stat st;
	if (out_size == NULL) return XTC_E_INVAL;
	if (fstat(fd, &st) != 0) return err_map(errno);
	*out_size = (int64_t)st.st_size;
	return XTC_OK;
}

int
xtc_fs_stat(const char *path, xtc_fs_stat_t *out)
{
	struct stat st;
	if (path == NULL || out == NULL) return XTC_E_INVAL;
	if (stat(path, &st) != 0) return err_map(errno);
	out->size = (int64_t)st.st_size;
#if defined(__APPLE__)
	out->mtime_ns = (int64_t)st.st_mtimespec.tv_sec * 1000000000LL +
	    st.st_mtimespec.tv_nsec;
#elif defined(st_mtime)
	out->mtime_ns = (int64_t)st.st_mtim.tv_sec * 1000000000LL +
	    st.st_mtim.tv_nsec;
#else
	out->mtime_ns = (int64_t)st.st_mtime * 1000000000LL;
#endif
	out->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
	return XTC_OK;
}

int
xtc_fs_exists(const char *path)
{
	return path != NULL && access(path, F_OK) == 0;
}

int
xtc_fs_unlink(const char *path)
{
	if (unlink(path) == 0) return XTC_OK;
	return err_map(errno);
}

int
xtc_fs_rename(const char *from, const char *to)
{
	return rename(from, to) == 0 ? XTC_OK : err_map(errno);   /* atomic on POSIX */
}

int
xtc_fs_mkdir(const char *path)
{
	return mkdir(path, 0755) == 0 ? XTC_OK : err_map(errno);
}

int
xtc_fs_rmdir(const char *path)
{
	return rmdir(path) == 0 ? XTC_OK : err_map(errno);
}

int
xtc_fs_tmpdir(char *buf, size_t cap)
{
	const char *d;
	size_t n;
	if (buf == NULL || cap == 0) return XTC_E_INVAL;
	if ((d = getenv("TMPDIR")) == NULL &&
	    (d = getenv("TMP")) == NULL &&
	    (d = getenv("TEMP")) == NULL)
		d = "/tmp";
	n = strlen(d);
	while (n > 1 && d[n - 1] == '/') n--;     /* trim trailing slash */
	if (n + 1 > cap) return XTC_E_RANGE;
	memcpy(buf, d, n);
	buf[n] = '\0';
	return XTC_OK;
}

int
xtc_fs_mkstemp(char *tmpl, int *out_fd)
{
	int fd;
	if (tmpl == NULL || out_fd == NULL) return XTC_E_INVAL;
	fd = mkstemp(tmpl);                        /* XTC_BLOCKING_OK */
	if (fd < 0) return err_map(errno);
	*out_fd = fd;
	return XTC_OK;
}

int
xtc_fs_dir_open(const char *path, xtc_fs_dir_t **out)
{
	xtc_fs_dir_t *d;
	if (path == NULL || out == NULL) return XTC_E_INVAL;
	if (__os_calloc(1, sizeof *d, (void **)&d) != XTC_OK) return XTC_E_NOMEM;
	d->dp = opendir(path);
	if (d->dp == NULL) {
		int e = errno;
		__os_free(d);
		return err_map(e);
	}
	*out = d;
	return XTC_OK;
}

int
xtc_fs_dir_next(xtc_fs_dir_t *d, const char **out_name)
{
	struct dirent *de;
	if (d == NULL || out_name == NULL) return XTC_E_INVAL;
	*out_name = NULL;
	for (;;) {
		errno = 0;
		de = readdir(d->dp);
		if (de == NULL)
			return errno == 0 ? XTC_OK : err_map(errno);
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		*out_name = de->d_name;
		return XTC_OK;
	}
}

void
xtc_fs_dir_close(xtc_fs_dir_t *d)
{
	if (d == NULL) return;
	if (d->dp != NULL) (void)closedir(d->dp);
	__os_free(d);
}

#endif /* _WIN32 vs POSIX */
