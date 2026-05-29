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

static __inline int
sched_yield(void)
{
	/* SwitchToThread yields to another ready thread on the same
	 * processor; if none is ready it returns 0 (no yield), which
	 * matches sched_yield's best-effort contract. */
	(void)SwitchToThread();
	return 0;
}

#endif /* XTC_COMPAT_SCHED_H */
