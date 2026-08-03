/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/inc/compat/unistd.h
 *
 *	The sliver of <unistd.h> libxtc reaches on Windows (read, write,
 *	close, and the ssize_t type), mapped to the MSVC CRT, for the
 *	MSVC build.  Platform-specific I/O backends that use the rest of
 *	<unistd.h> (io_epoll/io_kqueue/io_solaris) compile to empty
 *	translation units on Windows because IOCP is selected, so their
 *	unistd usage is never reached.
 */

#ifndef XTC_COMPAT_UNISTD_H
#define XTC_COMPAT_UNISTD_H

#if !defined(_MSC_VER)
#  error "compat/unistd.h is the MSVC-only shim"
#endif

#include <io.h>          /* _read, _write, _close, _pipe */
#include <fcntl.h>       /* _O_BINARY for the pipe shim */
#include <stdio.h>       /* SEEK_SET / SEEK_CUR for the pread/pwrite shim */
#include <sys/types.h>   /* off_t (UCRT provides it -- do not redefine) */
#include <BaseTsd.h>     /* SSIZE_T */

typedef SSIZE_T ssize_t;

static __inline ssize_t xtc__read(int fd, void *buf, size_t n)
{ return _read(fd, buf, (unsigned)n); }
static __inline ssize_t xtc__write(int fd, const void *buf, size_t n)
{ return _write(fd, buf, (unsigned)n); }

/* pread/pwrite: positional I/O over Win32.  The CRT has no positional
 * read/write, so seek-then-read; NOT atomic w.r.t. the file's shared
 * position (unlike POSIX pread), which is fine for the single-threaded
 * per-fd test use that reaches this shim.  Restores the prior position
 * so an interleaved sequential caller is not disturbed. */
static __inline ssize_t xtc__pread(int fd, void *buf, size_t n, off_t off)
{
	__int64 prev = _lseeki64(fd, 0, SEEK_CUR);
	ssize_t r;
	if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
	r = _read(fd, buf, (unsigned)n);
	(void)_lseeki64(fd, prev, SEEK_SET);
	return r;
}
static __inline ssize_t xtc__pwrite(int fd, const void *buf, size_t n, off_t off)
{
	__int64 prev = _lseeki64(fd, 0, SEEK_CUR);
	ssize_t r;
	if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
	r = _write(fd, buf, (unsigned)n);
	(void)_lseeki64(fd, prev, SEEK_SET);
	return r;
}
/* POSIX pipe(fds) -> a binary CRT pipe; the blocking pool's wakeup
 * channel.  4 KiB is ample for the single wakeup byte it carries. */
static __inline int xtc__pipe(int fds[2])
{ return _pipe(fds, 4096, _O_BINARY); }

/* usleep: POSIX microsecond sleep over Win32.  Sleep()'s 1 ms floor
 * bounds the resolution; rounds up so a sub-ms request is not a no-op.
 * Test-thread pacing (the only reach here) tolerates the coarsening. */
#include <windows.h>
static __inline int xtc__usleep(unsigned usec)
{ Sleep((DWORD)((usec + 999) / 1000)); return 0; }

/* mkstemp: POSIX atomic temp-file create+open over Win32.  _mktemp_s
 * fills the trailing XXXXXX in place (mutating the caller's template,
 * as mkstemp does), then _open with _O_CREAT|_O_EXCL gives the same
 * create-if-not-exists semantics.  Returns an open fd or -1. */
#include <string.h>
#include <sys/stat.h>
#include <process.h>     /* _getpid */
static __inline int xtc__mkstemp(char *tmpl)
{
	if (_mktemp_s(tmpl, strlen(tmpl) + 1) != 0)
		return -1;
	return _open(tmpl, _O_RDWR | _O_CREAT | _O_EXCL | _O_BINARY,
	    _S_IREAD | _S_IWRITE);
}

#define read(fd, buf, n)   xtc__read((fd), (buf), (n))
#define write(fd, buf, n)  xtc__write((fd), (buf), (n))
#define pread(fd, buf, n, off)   xtc__pread((fd), (buf), (n), (off))
#define pwrite(fd, buf, n, off)  xtc__pwrite((fd), (buf), (n), (off))
#define close(fd)          _close(fd)
#define pipe(fds)          xtc__pipe(fds)
#define unlink(p)          _unlink((p))
#define fdopen(fd, mode)   _fdopen((fd), (mode))
#define usleep(usec)       xtc__usleep((usec))
#define mkstemp(tmpl)      xtc__mkstemp((tmpl))
#define getpid()           _getpid()

/* alarm(sec): POSIX SIGALRM-after-sec has no Win32 analogue, but every
 * caller that reaches this shim uses it purely as a hang-guard watchdog
 * ("abort the process if the test runs longer than sec seconds").
 * Emulate exactly that: a one-shot background thread that waits sec
 * seconds then aborts.  A healthy test exits normally long before the
 * guard fires (the thread just dies with the process).  alarm(0) is a
 * no-op cancel -- no shim caller re-arms or inspects the remaining
 * time, matching how the tests use it. */
static __inline unsigned long __stdcall xtc__alarm_thread(void *arg)
{
	unsigned sec = (unsigned)(uintptr_t)arg;
	Sleep(sec * 1000UL);
	_exit(3);   /* SIGALRM default action is terminate */
	return 0;
}
static __inline unsigned xtc__alarm(unsigned sec)
{
	if (sec != 0) {
		HANDLE h = CreateThread(NULL, 0,
		    (LPTHREAD_START_ROUTINE)xtc__alarm_thread,
		    (void *)(uintptr_t)sec, 0, NULL);
		if (h != NULL) CloseHandle(h);
	}
	return 0;
}
#define alarm(sec)         xtc__alarm((sec))

#endif /* XTC_COMPAT_UNISTD_H */
