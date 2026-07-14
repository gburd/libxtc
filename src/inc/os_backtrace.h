/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/os_backtrace.h
 *	Cross-platform native stack backtrace seam.  Captures the C call
 *	stack of the CALLING OS thread and emits human-readable frames.
 *	The implementation is selected at configure time, in priority order:
 *
 *	  - execinfo  (glibc/macOS/BSD): symbolized frames, async-signal-safe.
 *	  - libunwind (musl and other execinfo-less libc's): frame addresses
 *	    via the unwinder, symbolized best-effort with dladdr when present
 *	    (else addresses only).  Frame walking + address emission are
 *	    async-signal-safe; the dladdr name lookup is best-effort.
 *	  - DbgHelp   (Windows): symbolized frames -- COMPILED BUT NOT
 *	    RUNTIME-VERIFIED on the porting host (no Windows machine in the
 *	    CI matrix); reviewed against the DbgHelp API, like src/io/io_aix.c.
 *	  - stub      (no backend available): an empty backtrace, never a
 *	    build error.
 *
 *	__os_backtrace_emit uses only async-signal-safe primitives on the
 *	execinfo (backtrace_symbols_fd) and libunwind (write(2)) paths, so it
 *	is safe to call from a fatal-signal handler there.  The Windows path
 *	serializes the non-signal-safe DbgHelp Sym* family under a lock and is
 *	intended for the panic/abort path.  See src/ptc/dump.c and
 *	docs/guide/debugging.md.
 */

#ifndef XTC_OS_BACKTRACE_H
#define XTC_OS_BACKTRACE_H

#include "xtc_export.h"

/*
 * PUBLIC: int  __os_backtrace __P((void **, int));
 * PUBLIC: void __os_backtrace_emit __P((int, void *const *, int));
 * PUBLIC: int  __os_backtrace_supported __P((void));
 */

/* Capture up to `max` return addresses of the calling thread into
 * `frames`.  Returns the number captured (0 if unsupported). */
XTC_API int __os_backtrace(void **frames, int max);

/* Symbolize `n` frames (as returned by __os_backtrace) and write them,
 * one per line, to `fd`.  Best-effort and async-signal-safe; writes
 * nothing when unsupported. */
XTC_API void __os_backtrace_emit(int fd, void *const *frames, int n);

/* 1 if a real backtrace backend is compiled in, 0 if the stub. */
XTC_API int __os_backtrace_supported(void);

#endif /* XTC_OS_BACKTRACE_H */
