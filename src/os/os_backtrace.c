/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/os_backtrace.c
 *	Native stack backtrace, configure-selected backend.
 *
 *	execinfo (XTC_HAVE_EXECINFO): glibc, macOS, and the BSDs ship
 *	<execinfo.h> with backtrace()/backtrace_symbols_fd().  The _fd
 *	variant does not allocate and is async-signal-safe, so the emit
 *	path is usable from a crash handler.
 *
 *	Stub: musl (no execinfo) and platforms without it get a backtrace
 *	of length 0.  A libunwind backend (musl) and a DbgHelp backend
 *	(Windows) are future work; the dump facility degrades to "no C
 *	stack, but full proc/loop/mailbox state" rather than failing.
 */

#include "xtc_int.h"

#include "os_backtrace.h"

#if defined(XTC_HAVE_EXECINFO)

#include <execinfo.h>
#include <unistd.h>

/* PUBLIC: int __os_backtrace __P((void **, int)); */
int
__os_backtrace(void **frames, int max)
{
	if (frames == NULL || max <= 0)
		return 0;
	return backtrace(frames, max);
}

/* PUBLIC: void __os_backtrace_emit __P((int, void *const *, int)); */
void
__os_backtrace_emit(int fd, void *const *frames, int n)
{
	if (frames == NULL || n <= 0)
		return;
	/* backtrace_symbols_fd writes directly to fd with no malloc:
	 * async-signal-safe, unlike backtrace_symbols. */
	backtrace_symbols_fd((void *const *)frames, n, fd);
}

/* PUBLIC: int __os_backtrace_supported __P((void)); */
int
__os_backtrace_supported(void)
{
	return 1;
}

#else /* stub */

int
__os_backtrace(void **frames, int max)
{
	(void)frames;
	(void)max;
	return 0;
}

void
__os_backtrace_emit(int fd, void *const *frames, int n)
{
	(void)fd;
	(void)frames;
	(void)n;
}

int
__os_backtrace_supported(void)
{
	return 0;
}

#endif
