/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/include/fd_probe_compat.h
 *	"Is this fd closed?" probe that is safe on Windows.
 *
 *	A test that proves the runtime CLOSED an fd naturally asserts
 *	`close(fd) == -1`.  On POSIX that is a plain EBADF.  On Windows
 *	with the MSVC CRT it is FATAL: _close() on an unused descriptor
 *	invokes the invalid-parameter handler, whose default action is
 *	__fastfail(FAST_FAIL_INVALID_ARG) -- the process dies with
 *	0xC0000409 (STATUS_STACK_BUFFER_OVERRUN) and no assertion ever
 *	runs.  Measured on Windows Server 2022 / MSVC 19.44: the bare
 *	double close aborts, and installing a no-op thread-local
 *	invalid-parameter handler around it makes it return -1/EBADF
 *	exactly as POSIX does.
 *
 *	xtc_test_fd_is_closed(fd) returns 1 if fd is not a live
 *	descriptor, 0 if it is still open.  It CONSUMES the descriptor on
 *	POSIX (it really calls close), which is the same thing the
 *	`close(fd) == -1` idiom did, so it is a drop-in replacement.
 */

#ifndef XTC_TEST_FD_PROBE_COMPAT_H
#define XTC_TEST_FD_PROBE_COMPAT_H

#if defined(_WIN32)
# include <io.h>
# include <stdlib.h>
# include <errno.h>

static inline void
xtc_test_bad_param_noop(const wchar_t *expr, const wchar_t *fn,
    const wchar_t *file, unsigned int line, uintptr_t reserved)
{
	(void)expr; (void)fn; (void)file; (void)line; (void)reserved;
}

static inline int
xtc_test_fd_is_closed(int fd)
{
	_invalid_parameter_handler old;
	int rc;
	/* A no-op handler: return normally instead of __fastfail, so the
	 * CRT hands back -1/EBADF like POSIX.  A REAL no-op function, not
	 * NULL -- passing NULL restores the default (fatal) handler. */
	old = _set_thread_local_invalid_parameter_handler(
	    xtc_test_bad_param_noop);
	rc = _close(fd);
	(void)_set_thread_local_invalid_parameter_handler(old);
	return rc == -1;
}

#else /* !_WIN32 */
# include <unistd.h>

static inline int
xtc_test_fd_is_closed(int fd)
{
	return close(fd) == -1;
}

#endif /* _WIN32 */

#endif /* XTC_TEST_FD_PROBE_COMPAT_H */
