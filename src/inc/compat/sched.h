/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/inc/compat/sched.h
 *
 *	The sliver of <sched.h> libxtc uses (sched_yield), over Win32,
 *	for the MSVC build.  See compat/pthread.h for the rationale.
 */

#ifndef XTC_COMPAT_SCHED_H
#define XTC_COMPAT_SCHED_H

#if !defined(_MSC_VER)
#  error "compat/sched.h is the MSVC-only shim"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <time.h>        /* struct timespec for the nanosleep shim */

static __inline int
sched_yield(void)
{
	/* SwitchToThread yields to another ready thread on the same
	 * processor; if none is ready it returns 0 (no yield), which
	 * matches sched_yield's best-effort contract. */
	(void)SwitchToThread();
	return 0;
}

/* nanosleep: POSIX high-resolution sleep over Win32.  Sleep() has 1 ms
 * granularity, which is ample for the test-thread pacing that reaches
 * this shim (all callers pass tens of ms, or spin-sleep sub-ms in a
 * retry loop where coarse rounding is harmless).  Rounds up so a
 * requested sub-ms delay never becomes a 0 ms no-op.  rem is always
 * cleared (Sleep is not interruptible here). */
static __inline int
nanosleep(const struct timespec *req, struct timespec *rem)
{
	DWORD ms;
	if (req == NULL)
		return 0;
	ms = (DWORD)(req->tv_sec * 1000
	    + (req->tv_nsec + 999999) / 1000000);
	Sleep(ms);
	if (rem != NULL) { rem->tv_sec = 0; rem->tv_nsec = 0; }
	return 0;
}

#endif /* XTC_COMPAT_SCHED_H */
