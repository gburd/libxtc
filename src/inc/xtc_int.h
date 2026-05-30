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
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
/* Expose the Darwin/BSD extensions xtc uses: pthread_setname_np (1-arg),
 * sysconf(_SC_NPROCESSORS_ONLN), and friends.  _XOPEN_SOURCE alone
 * hides them on macOS. */
# define _DARWIN_C_SOURCE 1
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
 * Bit-scan helpers.  GCC and clang provide __builtin_ctzll /
 * __builtin_clzll directly; MSVC provides _BitScanForward64 /
 * _BitScanReverse64 with different signatures.  Wrap both so call
 * sites in the rest of the source need not branch on toolchain.
 */
#if defined(__GNUC__) || defined(__clang__)
#  define XTC_CTZLL(v) __builtin_ctzll(v)
#  define XTC_CLZLL(v) __builtin_clzll(v)
#elif defined(_MSC_VER)
#  include <intrin.h>
   static inline int xtc__ctzll(unsigned long long v) {
       unsigned long idx;
       _BitScanForward64(&idx, v);
       return (int)idx;
   }
   static inline int xtc__clzll(unsigned long long v) {
       unsigned long idx;
       _BitScanReverse64(&idx, v);
       return 63 - (int)idx;
   }
#  define XTC_CTZLL(v) xtc__ctzll(v)
#  define XTC_CLZLL(v) xtc__clzll(v)
#else
#  error "need bit-scan intrinsics for this toolchain"
#endif

/*
 * Cache-line padding helper.  Embeds explicit padding bytes to push
 * the next field onto a new cache line.
 */
#define XTC_CACHELINE_PAD  \
	char __pad_##__LINE__[XTC_CACHE_LINE]

/*
 * printf-format checking.  GCC and clang accept the format attribute;
 * MSVC has no equivalent that attaches to a prototype, so it expands
 * to nothing there (MSVC's /analyze provides its own checking).
 */
#if defined(__GNUC__) || defined(__clang__)
#  define XTC_PRINTF_FMT(fmt_idx, va_idx) \
      __attribute__((format(printf, fmt_idx, va_idx)))
#else
#  define XTC_PRINTF_FMT(fmt_idx, va_idx)
#endif

/*
 * Struct packing.  GCC and clang use a trailing
 * __attribute__((packed)); MSVC uses #pragma pack around the
 * declaration.  Wrap a packed struct as:
 *
 *     XTC_PACK_PUSH
 *     struct foo { ... } XTC_PACKED;
 *     XTC_PACK_POP
 *
 * The XTC_PACKED suffix carries the attribute on GCC/clang and is
 * empty on MSVC; the PUSH/POP carry the pragma on MSVC and are empty
 * on GCC/clang.  Using both makes one source form work everywhere.
 */
#if defined(_MSC_VER)
#  define XTC_PACK_PUSH  __pragma(pack(push, 1))
#  define XTC_PACK_POP   __pragma(pack(pop))
#  define XTC_PACKED
#else
#  define XTC_PACK_PUSH
#  define XTC_PACK_POP
#  define XTC_PACKED  __attribute__((packed))
#endif

#endif /* XTC_INT_H */
