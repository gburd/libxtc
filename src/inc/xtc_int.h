/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_int.h
 *	Internal umbrella header.  Pulls in the headers an internal
 *	source file needs in 90% of cases.
 */

#ifndef XTC_INT_H
#define XTC_INT_H

/*
 * Feature-test macros -- must come before any system header.  We aim
 * for POSIX.1-2008 with BSD/illumos extensions enabled, since we
 * touch mmap flags, ucontext, pthread_setname_np, and a handful of
 * other extensions.
 */
#ifndef _XOPEN_SOURCE
# define _XOPEN_SOURCE 700
#endif
#ifndef _DEFAULT_SOURCE
# define _DEFAULT_SOURCE 1
#endif
#if defined(__FreeBSD__) || defined(__DragonFly__) || \
    defined(__NetBSD__) || defined(__OpenBSD__)
# ifndef __BSD_VISIBLE
#  define __BSD_VISIBLE 1
# endif
#endif
#if defined(__sun) || defined(__illumos__)
# ifndef __EXTENSIONS__
#  define __EXTENSIONS__ 1
# endif
#endif

#if defined(__has_include)
# if __has_include("xtc_config.h")
#  include "xtc_config.h"
# endif
#endif

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "xtc.h"
#include "os_atomic.h"
#include "os_alloc.h"
#include "os_thread.h"
#include "os_time.h"
#include "os_cpu.h"

/*
 * Cache-line size.  Default 64 bytes; some ARM cores (Apple M1/M2,
 * Cortex-X1) use 128-byte lines.  Override via -DXTC_CACHE_LINE=128
 * on those platforms.
 */
#ifndef XTC_CACHE_LINE
#  define XTC_CACHE_LINE  64
#endif

/*
 * Branch prediction hints.  Use XTC_LIKELY for conditions that are
 * almost always true on the hot path, XTC_UNLIKELY for error checks
 * and slow-path fallbacks.
 */
#if defined(__GNUC__) || defined(__clang__)
#  define XTC_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define XTC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define XTC_LIKELY(x)   (x)
#  define XTC_UNLIKELY(x) (x)
#endif

/*
 * Cache-line padding helper.  Embeds explicit padding bytes to push
 * the next field onto a new cache line.
 */
#define XTC_CACHELINE_PAD  \
	char __pad_##__LINE__[XTC_CACHE_LINE]

#endif /* XTC_INT_H */
