/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/dump.c
 *	Runtime-state dump, panic, and a fatal-signal handler.  See
 *	src/inc/xtc_dump.h for the contract and signal-safety notes.
 *
 *	Everything here writes through a fixed stack buffer with
 *	raw writes -- no malloc, no stdio buffering -- so the dump path is
 *	usable from a crash handler and never perturbs the heap it may be
 *	trying to diagnose.
 */

#include "xtc_int.h"
#include "xtc_dump.h"
#include "xtc_inspect.h"
#include "os_backtrace.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>

#if !defined(_WIN32)
#  include <unistd.h>
#else
#  include <io.h>
#  define STDERR_FILENO 2
#  define write _write
#endif

/* ---- raw fd writers (no stdio, no malloc) ---- */

static void
dump_write(int fd, const char *s, size_t n)
{
	while (n > 0) {
		ssize_t w = write(fd, s, n);  /* XTC_BLOCKING_OK: diagnostic dump to a caller-chosen fd (stderr/file); blocking is intended and acceptable on the crash path */
		if (w <= 0)
			break;
		s += (size_t)w;
		n -= (size_t)w;
	}
}

static void
dump_str(int fd, const char *s)
{
	dump_write(fd, s, strlen(s));
}

/* snprintf into a stack buffer, then one write. */
static void
dump_fmt(int fd, const char *fmt, ...)
{
	char buf[256];
	int n;
	va_list ap;
	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n >= sizeof buf)
		n = (int)sizeof buf - 1;
	dump_write(fd, buf, (size_t)n);
}

/* ---- label tables (match tools/gdb/xtc-gdb.py) ---- */

static const char *
run_state_name(int s)
{
	switch (s) {
	case XTC_PROC_SCHEDULED: return "scheduled";
	case XTC_PROC_RUNNING:   return "running";
	case XTC_PROC_PARKED:    return "parked";
	case XTC_PROC_DONE:      return "done";
	default:                 return "?";
	}
}

static const char *
park_name(int p)
{
	switch (p) {
	case XTC_PARK_NONE:    return "-";
	case XTC_PARK_FD:      return "fd";
	case XTC_PARK_TIMER:   return "timer";
	case XTC_PARK_MAILBOX: return "mailbox";
	default:               return "?";
	}
}

/* ---- inspect callbacks: format one line per loop / proc ---- */

static int
dump_loop_cb(const xtc_loop_info_t *li, void *user)
{
	int fd = *(int *)user;
	dump_fmt(fd,
	    "  loop %d  procs=%d alive=%d tasks_run=%llu steals=%llu\n",
	    li->loop_id, li->n_procs, li->n_alive,
	    (unsigned long long)li->tasks_run,
	    (unsigned long long)li->steals);
	return 0;
}

static int
dump_proc_cb(const xtc_proc_info_t *pi, void *user)
{
	int fd = *(int *)user;
	dump_fmt(fd,
	    "  <%u.%u.%u> %-9s park=%-7s mbox=%zu/%zu peak=%zu "
	    "recv=%llu drop=%llu%s%s\n",
	    (unsigned)pi->pid.loop_id, (unsigned)pi->pid.local_id,
	    (unsigned)pi->pid.gen,
	    run_state_name(pi->run_state),
	    pi->run_state == XTC_PROC_PARKED ? park_name(pi->park_reason) : "-",
	    pi->mbox_len, pi->mbox_cap, pi->mbox_peak,
	    (unsigned long long)pi->mbox_recv_total,
	    (unsigned long long)pi->mbox_drop_total,
	    pi->kill_pending ? " KILL" : "",
	    pi->alive ? "" : " DEAD");
	return 0;
}

/* ---- public: the dump ---- */

/* PUBLIC: void xtc_dump __P((int)); */
void
xtc_dump(int fd)
{
	void *frames[64];
	int n, nloops, nprocs;

	dump_str(fd, "=== xtc runtime dump ===\n");

	/* 1. C backtrace of the calling (faulting) thread. */
	n = __os_backtrace(frames, (int)(sizeof frames / sizeof frames[0]));
	if (n > 0) {
		dump_fmt(fd, "thread backtrace (%d frames):\n", n);
		__os_backtrace_emit(fd, frames, n);
	} else {
		dump_str(fd, "thread backtrace: unavailable "
		    "(no backtrace backend; attach a debugger to a core)\n");
	}

	/* 2. Scheduler loops. */
	dump_str(fd, "loops:\n");
	nloops = xtc_inspect_loops(dump_loop_cb, &fd);
	if (nloops <= 0)
		dump_str(fd, "  (none registered)\n");

	/* 3. Live procs with mailbox state -- the message-passing view. */
	dump_str(fd, "procs:\n");
	nprocs = xtc_inspect_procs(dump_proc_cb, &fd);
	if (nprocs <= 0)
		dump_str(fd, "  (none)\n");

	dump_str(fd, "=== end dump ===\n");
}

/* ---- public: panic ---- */

/* PUBLIC: void xtc_panic __P((const char *, int, const char *, ...)); */
void
xtc_panic(const char *file, int line, const char *fmt, ...)
{
	char msg[256];
	va_list ap;

	va_start(ap, fmt);
	(void)vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);

	dump_fmt(STDERR_FILENO, "\n*** xtc panic: %s\n*** at %s:%d\n",
	    msg, file ? file : "?", line);
	xtc_dump(STDERR_FILENO);
	abort();
}

/* ---- public: fatal-signal handler ---- */

#if !defined(_WIN32)

static volatile sig_atomic_t in_handler;

static void
crash_handler(int sig)
{
	void *frames[64];
	int n;

	/* Re-entrancy guard: a fault inside the dump must not loop. */
	if (in_handler) {
		signal(sig, SIG_DFL);
		raise(sig);
		return;
	}
	in_handler = 1;

	/* Always emit the banner + backtrace: async-signal-safe. */
	dump_fmt(STDERR_FILENO, "\n*** xtc fatal signal %d ***\n", sig);
	n = __os_backtrace(frames, (int)(sizeof frames / sizeof frames[0]));
	if (n > 0) {
		dump_fmt(STDERR_FILENO, "thread backtrace (%d frames):\n", n);
		__os_backtrace_emit(STDERR_FILENO, frames, n);
	}

	/*
	 * The proc/loop walk takes locks and is therefore best-effort from
	 * a signal: if the fault happened while a loop lock was held it may
	 * block.  We attempt it anyway -- a hung handler still leaves the
	 * banner + backtrace above, and a core (default disposition below)
	 * remains available for deep inspection.
	 */
	{
		int fd = STDERR_FILENO;
		dump_str(STDERR_FILENO, "loops:\n");
		(void)xtc_inspect_loops(dump_loop_cb, &fd);
		dump_str(STDERR_FILENO, "procs:\n");
		(void)xtc_inspect_procs(dump_proc_cb, &fd);
	}

	/* Re-raise with the default disposition so a core is produced. */
	signal(sig, SIG_DFL);
	raise(sig);
}

/* PUBLIC: int xtc_crash_handler_install __P((void)); */
int
xtc_crash_handler_install(void)
{
	static const int sigs[] = { SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL };
	struct sigaction sa;
	size_t i;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = crash_handler;
	sigemptyset(&sa.sa_mask);
	/* SA_NODEFER so a fault inside the handler hits the re-entrancy
	 * guard rather than being silently blocked. */
	sa.sa_flags = SA_NODEFER;
	for (i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
		(void)sigaction(sigs[i], &sa, NULL);
	return XTC_OK;
}

#else /* _WIN32: no POSIX signals -- SEH would be the path, future work */

int
xtc_crash_handler_install(void)
{
	return XTC_E_NOSYS;
}

#endif
