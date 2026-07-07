/*-
 * Copyright (c) 2026, The XTC Project
 *
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/inc/xtc.h
 *	The single public header for the xtc library.
 *	See M0_CLAIMS.md [C4]: including this header alone is sufficient
 *	to use every M0-public API.
 */

#ifndef XTC_H
#define XTC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>   /* int64_t for the public clock / atomic helpers */

/*
 * Compile-time version macros.
 * These values come from configure-time substitution into xtc_config.h
 * via the build system; if xtc_config.h is unavailable (rare; e.g. when
 * a consumer parses just this header), they fall back to known-bad
 * sentinels so a missing build is detected loudly.
 */
#if defined(__has_include)
# if __has_include("xtc_config.h")
#  include "xtc_config.h"
# endif
#endif

#ifndef XTC_VERSION_MAJOR
# define XTC_VERSION_MAJOR	0
#endif
#ifndef XTC_VERSION_MINOR
# define XTC_VERSION_MINOR	0
#endif
#ifndef XTC_VERSION_PATCH
# define XTC_VERSION_PATCH	0
#endif
#ifndef XTC_VERSION_STRING
# define XTC_VERSION_STRING	"0.0.0-unconfigured"
#endif

/*
 * Error codes.
 * 0 == OK; negative values are stable XTC_E_* codes.
 * Codes are added at the end of the enumeration in minor releases and
 * never renumbered.  See docs/abi-stability.md.
 */
typedef enum xtc_err {
	XTC_OK			=  0,	/* success */
	XTC_E_INVAL		= -1,	/* invalid argument */
	XTC_E_NOMEM		= -2,	/* out of memory */
	XTC_E_NOSYS		= -3,	/* not implemented on this platform */
	XTC_E_RANGE		= -4,	/* numeric out of range */
	XTC_E_AGAIN		= -5,	/* try again later */
	XTC_E_INTERNAL		= -6,	/* invariant violation; bug */
	XTC_E_RESOURCE		= -7,	/* resource cap reached (xtc_res) */
	XTC_E_DEADLK		= -8,	/* lock-manager: deadlock victim */
	XTC_E_VERSION		= -9,	/* version mismatch (shm) */
	XTC_E_ABORTED		= -10,	/* operation cancelled via abort token */
	XTC_E_NOTFOUND		= -11,	/* requested item does not exist */
	XTC_E_IO		= -12	/* I/O error (read/write/fsync failed) */
} xtc_err_t;

/*
 * xtc_version_string --
 *	Return the library version as a NUL-terminated SemVer 2.0 string.
 *	The pointer is to static storage; the caller must not free it.
 *	See M0_CLAIMS.md [C1].
 */
const char *xtc_version_string(void);

/*
 * xtc_version_components --
 *	Decompose the version into three integers.
 *	On success returns XTC_OK and writes *major, *minor, *patch.
 *	On NULL out-pointers returns XTC_E_INVAL.
 *	See M0_CLAIMS.md [C2].
 */
int xtc_version_components(int *major, int *minor, int *patch);

/*
 * xtc_strerror --
 *	Return a stable English description of an xtc error code.
 *	The pointer is to static storage; the caller must not free it.
 *	Returns a pointer to "unknown" for codes outside the known set
 *	rather than NULL, so callers can chain into log lines safely.
 *	See M0_CLAIMS.md [C6].
 */
const char *xtc_strerror(int xtc_err);

/*
 * xtc_free --
 *	Free a heap buffer that a libxtc call handed to the caller to own
 *	-- notably the message buffer from xtc_recv / xtc_recv_match /
 *	xtc_recv_correlate, the frame from xtc_net_recv_frame, the reply
 *	from xtc_osproc_call / xtc_svr_call, and any other buffer whose
 *	documentation says "free with xtc_free".  Those buffers are
 *	allocated by libxtc's own allocator, which is not necessarily the
 *	C library malloc/free, so they MUST be released through this
 *	function rather than plain free().  Passing NULL is a no-op.
 *
 *	This is the public name for the library allocator's deallocation
 *	entry point; it is safe to call from any thread.
 */
void xtc_free(void *p);

/*
 * xtc_malloc / xtc_calloc / xtc_realloc --
 *	Allocate through libxtc's own allocator (the same one xtc_free
 *	releases and that xtc_alloc_set_hook can override), so a consumer
 *	never needs the internal __os_* surface.  Return the pointer
 *	directly (NULL on failure), matching the C idiom; the result is
 *	released with xtc_free.  xtc_calloc zero-fills; xtc_realloc grows
 *	/ shrinks an xtc_malloc/xtc_calloc/xtc_realloc block (NULL p acts
 *	like xtc_malloc).  A zero size yields a unique freeable pointer,
 *	not NULL.  Safe from any thread.  These are the public complement
 *	to xtc_free -- library CONSUMERS use only the xtc_* API, never the
 *	internal __os_* wrappers.
 */
void *xtc_malloc(size_t size);
void *xtc_calloc(size_t n, size_t size);
void *xtc_realloc(void *p, size_t size);

/*
 * xtc_aligned_alloc / xtc_aligned_free --
 *	Allocate `size` bytes aligned to `align` (a power of two, e.g.
 *	XTC_CACHE_LINE) through libxtc's allocator; release ONLY with
 *	xtc_aligned_free (never xtc_free -- an aligned block may carry
 *	header/padding a plain free would mishandle).  Returns NULL on
 *	failure.  For a struct with an over-aligned member.
 */
void *xtc_aligned_alloc(size_t align, size_t size);
void  xtc_aligned_free(void *p);

/*
 * xtc_clock_mono / xtc_clock_real --
 *	Read the monotonic (never goes backward; for intervals/timeouts)
 *	or real (wall-clock; for timestamps) clock in NANOSECONDS.  Return
 *	the time directly (0 on the rare query failure).  These are the
 *	public clocks a consumer uses instead of raw clock_gettime.
 */
int64_t xtc_clock_mono(void);
int64_t xtc_clock_real(void);

/*
 * xtc_sleep_ns --
 *	Sleep the CALLING OS THREAD for at least `ns` nanoseconds.  This
 *	is a THREAD sleep (blocking); inside a fiber use xtc_proc_sleep
 *	instead, which parks the fiber and keeps the loop live.  Provided
 *	so a consumer never needs raw nanosleep.  Returns XTC_OK, or a
 *	negative XTC_E_* on interruption/error.
 */
int xtc_sleep_ns(int64_t ns);

/*
 * xtc_atomic_i64_load / xtc_atomic_i64_add --
 *	Relaxed atomic load, and atomic fetch-add (returns the PRIOR
 *	value), on a shared int64_t.  The minimal public atomic surface a
 *	consumer needs for a shared counter / token bucket without reaching
 *	for the internal __os_atomic_* macros or a compiler builtin.
 */
int64_t xtc_atomic_i64_load(const int64_t *p);
int64_t xtc_atomic_i64_add(int64_t *p, int64_t delta);

#ifdef __cplusplus
}
#endif

#endif /* XTC_H */
