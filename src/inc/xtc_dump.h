/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_dump.h
 *	Crash diagnostics: a runtime-state dump (the programmatic form of
 *	the debugger's xtc-procs / xtc-loops / xtc-mailbox), plus panic and
 *	assert macros that emit it before aborting, and an optional fatal-
 *	signal handler that emits it on SIGSEGV / SIGBUS / SIGABRT.
 *
 *	xtc_dump writes, to a file descriptor:
 *	  - the C backtrace of the CALLING (faulting) OS thread, when a
 *	    backtrace backend is compiled in (see os_backtrace.h);
 *	  - every scheduler loop with its run-queue / steal stats;
 *	  - every live proc with its run state, park reason, and mailbox
 *	    depth / peak / recv / drop counters.
 *	The field labels match the debugger scripts (tools/gdb,
 *	tools/lldb), so an in-process dump and a gdb session read alike.
 *
 *	A parked fiber's OWN C stack is NOT unwound: it lives in a saved
 *	coroutine context, not on a live OS thread.  Like an Erlang crash
 *	dump or a Go panic, the dump reports each proc's state and mailbox,
 *	not N reconstructed C stacks.  For deep per-fiber stacks attach a
 *	debugger to a core (the dump and the debugger share field names).
 *
 *	Signal-safety: xtc_dump takes the per-loop inspection locks (via
 *	xtc_inspect), so it is fully reliable from an explicit XTC_PANIC /
 *	XTC_ASSERT in normal context.  From the fatal-signal handler the
 *	backtrace (async-signal-safe) is always emitted; the proc/loop walk
 *	is attempted best-effort and may be skipped if a lock was held at
 *	the moment of the fault.  See docs/guide/debugging.md.
 */

#ifndef XTC_DUMP_H
#define XTC_DUMP_H

#include "xtc_export.h"

#include "xtc.h"

#if defined(__GNUC__) || defined(__clang__)
#  define XTC_NORETURN __attribute__((noreturn))
#else
#  define XTC_NORETURN
#endif

/*
 * PUBLIC: void xtc_dump __P((int));
 * PUBLIC: void xtc_panic __P((const char *, int, const char *, ...));
 * PUBLIC: int  xtc_crash_handler_install __P((void));
 */

/* Write a best-effort runtime-state dump to file descriptor `fd`
 * (commonly STDERR_FILENO).  Never allocates; safe to call at any
 * time.  Does not abort -- callers decide what to do next. */
XTC_API void xtc_dump(int fd);

/* Emit `fmt`/... as a panic banner, then xtc_dump(STDERR_FILENO), then
 * abort().  Use the XTC_PANIC macro rather than calling directly. */
XTC_API void xtc_panic(const char *file, int line, const char *fmt, ...) XTC_NORETURN;

/* Install a fatal-signal handler (SIGSEGV, SIGBUS, SIGABRT, SIGFPE,
 * SIGILL) that emits a backtrace + best-effort runtime dump to stderr,
 * then re-raises the signal with the default disposition (so a core is
 * still produced).  Returns XTC_OK, or XTC_E_NOSYS where unsupported.
 * Idempotent.  Does NOT install SIGCHLD/SIGTERM/etc. -- only faults. */
XTC_API int xtc_crash_handler_install(void);

/* Log `fmt`/..., dump runtime state, and abort. */
#define XTC_PANIC(...) xtc_panic(__FILE__, __LINE__, __VA_ARGS__)

/* Abort with a dump unless `cond` holds. */
#define XTC_ASSERT(cond) \
	do { if (!(cond)) \
		xtc_panic(__FILE__, __LINE__, "assertion failed: %s", #cond); \
	} while (0)

/* Like XTC_ASSERT but with a custom printf-style message. */
#define XTC_ASSERT_F(cond, ...) \
	do { if (!(cond)) xtc_panic(__FILE__, __LINE__, __VA_ARGS__); } while (0)

#endif /* XTC_DUMP_H */
