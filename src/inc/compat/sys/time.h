/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/inc/compat/sys/time.h
 *
 *	The <sys/time.h> sliver libxtc reaches on Windows: struct
 *	timeval (which Winsock defines) plus a gettimeofday shim.  The
 *	files that include this header on Windows either compile to
 *	empty translation units (io_select / io_kqueue / io_solaris, as
 *	IOCP is selected) or do not call gettimeofday (lock_mgr), so
 *	this only needs to make the names resolve.
 */

#ifndef XTC_COMPAT_SYS_TIME_H
#define XTC_COMPAT_SYS_TIME_H

#if !defined(_MSC_VER)
#  error "compat/sys/time.h is the MSVC-only shim"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>    /* defines struct timeval */
#include <windows.h>
#include <stdint.h>

static __inline int
gettimeofday(struct timeval *tv, void *tz)
{
	FILETIME ft;
	ULARGE_INTEGER u;
	const int64_t EPOCH_DELTA_100NS = 116444736000000000LL;
	int64_t usec;
	(void)tz;
	if (tv == NULL) return -1;
	GetSystemTimePreciseAsFileTime(&ft);
	u.LowPart  = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	usec = (int64_t)((u.QuadPart - EPOCH_DELTA_100NS) / 10);
	tv->tv_sec  = (long)(usec / 1000000);
	tv->tv_usec = (long)(usec % 1000000);
	return 0;
}

#endif /* XTC_COMPAT_SYS_TIME_H */
