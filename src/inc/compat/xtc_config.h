/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/inc/compat/xtc_config.h
 *
 *	Hand-authored configuration header for the MSVC build, which
 *	does not run the autoconf configure that generates xtc_config.h
 *	on the autotools path.  Placed on the include path ahead of the
 *	build directory so `#include "xtc_config.h"` resolves here.
 *
 *	Windows selects the IOCP I/O backend and ships no ucontext, so
 *	the coroutine substrate is the Win32-fiber implementation in
 *	coro_winfiber.c (XTC_HAVE_UCONTEXT stays undefined).
 */

#ifndef XTC_CONFIG_H
#define XTC_CONFIG_H

#define XTC_VERSION_MAJOR  0
#define XTC_VERSION_MINOR  2
#define XTC_VERSION_PATCH  0
#define XTC_VERSION_STRING "0.2.0"

/* Completion-based I/O backend for Windows. */
#define XTC_IO_BACKEND_IOCP 1

/* No ucontext on Windows; coro_winfiber.c provides the substrate. */
/* (XTC_HAVE_UCONTEXT intentionally undefined.) */

/* No TLS backend wired into the MSVC build yet. */
/* (XTC_TLS_BACKEND_OPENSSL intentionally undefined.) */

#endif /* XTC_CONFIG_H */
