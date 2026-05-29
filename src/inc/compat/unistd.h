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

#include <io.h>          /* _read, _write, _close */
#include <BaseTsd.h>     /* SSIZE_T */

typedef SSIZE_T ssize_t;

static __inline ssize_t xtc__read(int fd, void *buf, size_t n)
{ return _read(fd, buf, (unsigned)n); }
static __inline ssize_t xtc__write(int fd, const void *buf, size_t n)
{ return _write(fd, buf, (unsigned)n); }

#define read(fd, buf, n)   xtc__read((fd), (buf), (n))
#define write(fd, buf, n)  xtc__write((fd), (buf), (n))
#define close(fd)          _close(fd)

#endif /* XTC_COMPAT_UNISTD_H */
