/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/os_sharp.h
 *	POSIX/libc "sharp edges" abstraction: thread-safe environment
 *	access, a per-thread seedable PRNG, and BSD-semantics bounded
 *	string copy/cat.  These smooth three well-known threaded-C
 *	footguns (getenv/setenv races, the process-global non-reentrant
 *	rand(), and strncpy's missing NUL terminator).  The public
 *	complement is xtc_env_get / xtc_env_set / xtc_rand_u64 /
 *	xtc_rand_seed / xtc_strlcpy / xtc_strlcat.
 */

#ifndef XTC_OS_SHARP_H
#define XTC_OS_SHARP_H

#include "xtc_export.h"

#include <stddef.h>
#include <stdint.h>

/*
 * --- Environment (thread-safe against concurrent get/set) ---
 *
 * getenv(3) and setenv(3) are not safe against each other: setenv may
 * reallocate the environ block a concurrent getenv is reading.  These
 * serialize every get/set on one process-wide mutex, and get COPIES
 * the value into a caller buffer so the returned data cannot be
 * invalidated by a later setenv from another thread.
 *
 * __os_env_get copies the value of `name` into `buf` (always
 * NUL-terminated when bufsize > 0, truncating if needed).  Returns
 * XTC_OK when the variable exists, XTC_E_NOTFOUND when it does not
 * (buf is set to the empty string), XTC_E_INVAL on a NULL argument.
 *
 * __os_env_set sets/overwrites `name` to `value`; when overwrite == 0
 * an existing variable is left unchanged.  Returns XTC_OK, or
 * XTC_E_INVAL / XTC_E_NOMEM.
 *
 * PUBLIC: int __os_env_get __P((const char *, char *, size_t));
 * PUBLIC: int __os_env_set __P((const char *, const char *, int));
 */
XTC_API int __os_env_get(const char *name, char *buf, size_t bufsize);
XTC_API int __os_env_set(const char *name, const char *value, int overwrite);

/*
 * --- Per-thread seedable PRNG ---
 *
 * rand(3)/random(3) share process-global state and are not thread-safe.
 * This is a per-thread splitmix64 stream: each thread has its own state
 * so there is no cross-thread contention or shared-state race, and
 * __os_rand_seed makes a thread's stream reproducible.  The first use
 * on a thread that has not been seeded auto-seeds from the monotonic
 * clock XORed with the thread-local state address, so distinct threads
 * get distinct streams by default.
 *
 * NOTE: this is NOT wired into the DST deterministic-simulation clock
 * (src/evt/sim.c); a future task can add a sim hook here so a
 * replayed run draws a reproducible sequence.  For now it is simply a
 * clean, thread-safe, explicitly seedable entropy source.
 *
 * PUBLIC: void __os_rand_seed __P((uint64_t));
 * PUBLIC: uint64_t __os_rand_u64 __P((void));
 */
XTC_API void     __os_rand_seed(uint64_t seed);
XTC_API uint64_t __os_rand_u64(void);

/*
 * --- Bounded string copy/cat (BSD strlcpy/strlcat semantics) ---
 *
 * strncpy(3) does not NUL-terminate when the source is at least as long
 * as the buffer; strncat(3) uses a confusing count.  These follow the
 * OpenBSD contract: always NUL-terminate when dstsize > 0, and return
 * the total length the function TRIED to create -- for strlcpy that is
 * strlen(src); for strlcat that is the initial strlen(dst) plus
 * strlen(src).  A return value >= dstsize means the result was
 * truncated.  Pure; no allocation.
 *
 * PUBLIC: size_t __os_strlcpy __P((char *, const char *, size_t));
 * PUBLIC: size_t __os_strlcat __P((char *, const char *, size_t));
 */
XTC_API size_t __os_strlcpy(char *dst, const char *src, size_t dstsize);
XTC_API size_t __os_strlcat(char *dst, const char *src, size_t dstsize);

#endif /* XTC_OS_SHARP_H */
