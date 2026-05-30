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
 * BDB convention: 0 == OK; negative values are stable XTC_E_* codes.
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
	XTC_E_ABORTED		= -10	/* operation cancelled via abort token */
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

#ifdef __cplusplus
}
#endif

#endif /* XTC_H */
