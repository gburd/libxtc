/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_probes.h
 *	Static (USDT) probe points: the C side of src/xtc_probes.d.
 *	Library-internal; not installed.  Every probe site in src/ is one
 *	XTC_PROBE_<NAME>(...) line.  Three builds, picked at configure
 *	time (--enable-dtrace, meson -Ddtrace):
 *
 *	XTC_HAVE_DTRACE   the build ran `dtrace -h` over xtc_probes.d into
 *	                  xtc_probes_dtrace.h (its XTC_ prefix rewritten
 *	                  to XTC_DT_ so a probe can never collide with a
 *	                  libxtc macro).  FreeBSD, illumos and macOS; also
 *	                  Linux with the systemtap `dtrace` script, which
 *	                  adds is-enabled semaphores.  Everywhere but macOS
 *	                  `dtrace -G` then post-processes the objects into
 *	                  xtc_probes.o (dist/xtc_probes.sh).
 *	XTC_PROBES_SDT    header-only <sys/sdt.h> from systemtap (Linux),
 *	                  no tool and no post-link step.  perf, bpftrace,
 *	                  bcc and SystemTap find the probes from the ELF
 *	                  notes.  There are no semaphores, so
 *	                  XTC_PROBE_ENABLED() is a constant 1.
 *	neither           every macro expands to nothing and no argument
 *	                  is evaluated: the probes-off build carries no
 *	                  instruction for them.
 *
 *	Windows (ETW) is out of scope: there every macro is empty.
 *
 *	Arguments are cheap by design (registers and plain loads, never a
 *	clock read or a call), because on the XTC_PROBES_SDT path they are
 *	materialized even when no tracer is attached.  Wrap anything more
 *	expensive in `if (XTC_PROBE_ENABLED(NAME))`.  See xtc_usdt(3).
 */

#ifndef XTC_PROBES_H
#define XTC_PROBES_H

#include "xtc_int.h"	/* xtc_config.h: XTC_HAVE_DTRACE / XTC_PROBES_SDT */

/* A libxtc pid packed into one 64-bit probe argument. */
#define XTC_PROBE_PID(p)						\
	(((uint64_t)(p).loop_id << 48) | ((uint64_t)(p).local_id << 32) | \
	 (uint64_t)(p).gen)

/* fiber-park / fiber-resume `reason` (arg1).  Stable: documented in
 * xtc_usdt(3) and decoded by tools/dtrace and tools/bpftrace. */
#define XTC_PROBE_PARK_MAILBOX   1   /* xtc_recv*; object = 0 */
#define XTC_PROBE_PARK_SLEEP     2   /* xtc_proc_sleep; object = 0 */
#define XTC_PROBE_PARK_FD        3   /* xtc_proc_wait_fd; object = fd */
#define XTC_PROBE_PARK_AIO       4   /* xtc_aio_*; object = xtc_aio_t * */
#define XTC_PROBE_PARK_BLOCKING  5   /* xtc_blocking_run; object = work */
#define XTC_PROBE_PARK_SYNC      6   /* amutex/arwlock/sem/notify/barrier/gate */
#define XTC_PROBE_PARK_LWLOCK    7   /* xtc_lwlock; object = the lock */
#define XTC_PROBE_PARK_LOCKMGR   8   /* xtc_lockmgr; object = the manager */
#define XTC_PROBE_PARK_FUTURE    9   /* xtc_future wait; object = the future */

/* fiber-wake `how` (arg2). */
#define XTC_PROBE_WAKE_LOCAL     0   /* claimed; queued on this thread's loop */
#define XTC_PROBE_WAKE_REMOTE    1   /* claimed; posted to the loop's inbox */
#define XTC_PROBE_WAKE_UNPARKED  2   /* not parked: latched for the park
				      * verdict, or already runnable */

/* lock-wait-start / lock-wait-done `kind` (arg1). */
#define XTC_PROBE_LOCK_AMUTEX    1
#define XTC_PROBE_LOCK_ARWLOCK   2
#define XTC_PROBE_LOCK_LWLOCK    3
#define XTC_PROBE_LOCK_LOCKMGR   4

#if defined(XTC_HAVE_DTRACE) && !defined(_WIN32)

#include "xtc_probes_dtrace.h"

#if defined(XTC_PROBES_ANCHOR)
/*
 * `dtrace -G` puts the probe table (DOF) in xtc_probes.o, which nothing
 * references by name -- so a link against libxtc.a would leave it out
 * and the probes would silently not exist.  Every probe TU references
 * this anchor, defined in xtc_probes.o, which pulls it in.  Set by the
 * libxtc build only; an amalgamation links xtc.o directly and needs none.
 */
extern const char __xtc_probes_anchor __attribute__((visibility("hidden")));
static const char *const __xtc_probes_ref __attribute__((used)) =
    &__xtc_probes_anchor;
#endif

#define XTC_PROBE_ENABLED(name)		(XTC_DT_##name##_ENABLED())

#define XTC_PROBE_PROC_SPAWN(a, b)	XTC_DT_PROC_SPAWN(a, b)
#define XTC_PROBE_PROC_START(a, b)	XTC_DT_PROC_START(a, b)
#define XTC_PROBE_PROC_EXIT(a, b)	XTC_DT_PROC_EXIT(a, b)
#define XTC_PROBE_PROC_KILL(a, b)	XTC_DT_PROC_KILL(a, b)
#define XTC_PROBE_MSG_SEND(a, b, c)	XTC_DT_MSG_SEND(a, b, c)
#define XTC_PROBE_MSG_RECV(a, b, c)	XTC_DT_MSG_RECV(a, b, c)
#define XTC_PROBE_FIBER_PARK(a, b, c)	XTC_DT_FIBER_PARK(a, b, c)
#define XTC_PROBE_FIBER_RESUME(a, b, c)	XTC_DT_FIBER_RESUME(a, b, c)
#define XTC_PROBE_FIBER_WAKE(a, b, c)	XTC_DT_FIBER_WAKE(a, b, c)
#define XTC_PROBE_TASK_RUN_START(a, b, c) XTC_DT_TASK_RUN_START(a, b, c)
#define XTC_PROBE_TASK_RUN_DONE(a, b, c) XTC_DT_TASK_RUN_DONE(a, b, c)
#define XTC_PROBE_LOOP_POLL(a, b, c)	XTC_DT_LOOP_POLL(a, b, c)
#define XTC_PROBE_IO_DISPATCH(a, b, c, d) XTC_DT_IO_DISPATCH(a, b, c, d)
#define XTC_PROBE_SCHED_STEAL(a, b, c)	XTC_DT_SCHED_STEAL(a, b, c)
#define XTC_PROBE_TIMER_ARM(a, b, c)	XTC_DT_TIMER_ARM(a, b, c)
#define XTC_PROBE_TIMER_FIRE(a, b, c)	XTC_DT_TIMER_FIRE(a, b, c)
#define XTC_PROBE_TIMER_CANCEL(a)	XTC_DT_TIMER_CANCEL(a)
#define XTC_PROBE_AIO_SUBMIT(a, b, c, d) XTC_DT_AIO_SUBMIT(a, b, c, d)
#define XTC_PROBE_AIO_DONE(a, b, c)	XTC_DT_AIO_DONE(a, b, c)
#define XTC_PROBE_BLOCKING_ENQUEUE(a, b) XTC_DT_BLOCKING_ENQUEUE(a, b)
#define XTC_PROBE_BLOCKING_START(a)	XTC_DT_BLOCKING_START(a)
#define XTC_PROBE_BLOCKING_DONE(a, b)	XTC_DT_BLOCKING_DONE(a, b)
#define XTC_PROBE_LOCK_WAIT_START(a, b, c, d) XTC_DT_LOCK_WAIT_START(a, b, c, d)
#define XTC_PROBE_LOCK_WAIT_DONE(a, b, c, d) XTC_DT_LOCK_WAIT_DONE(a, b, c, d)
#define XTC_PROBE_SUP_RESTART(a, b, c)	XTC_DT_SUP_RESTART(a, b, c)
#define XTC_PROBE_PREEMPT_TICK(a)	XTC_DT_PREEMPT_TICK(a)
#define XTC_PROBE_ALLOC_FAIL(a)		XTC_DT_ALLOC_FAIL(a)
#define XTC_PROBE_SLAB_GROW(a, b)	XTC_DT_SLAB_GROW(a, b)
/* dtrace -h drops `const` from string arguments on some platforms. */
#define XTC_PROBE_SIM_NONDETERMINISM(a)					\
	XTC_DT_SIM_NONDETERMINISM((char *)(uintptr_t)(a))
#define XTC_PROBE_SIM_BUGGIFY(a, b)					\
	XTC_DT_SIM_BUGGIFY((char *)(uintptr_t)(a), b)

#elif defined(XTC_PROBES_SDT) && !defined(_WIN32)

#include <sys/sdt.h>

#define XTC_PROBE_ENABLED(name)		(1)

#define XTC_PROBE_PROC_SPAWN(a, b)	DTRACE_PROBE2(xtc, proc__spawn, a, b)
#define XTC_PROBE_PROC_START(a, b)	DTRACE_PROBE2(xtc, proc__start, a, b)
#define XTC_PROBE_PROC_EXIT(a, b)	DTRACE_PROBE2(xtc, proc__exit, a, b)
#define XTC_PROBE_PROC_KILL(a, b)	DTRACE_PROBE2(xtc, proc__kill, a, b)
#define XTC_PROBE_MSG_SEND(a, b, c)	DTRACE_PROBE3(xtc, msg__send, a, b, c)
#define XTC_PROBE_MSG_RECV(a, b, c)	DTRACE_PROBE3(xtc, msg__recv, a, b, c)
#define XTC_PROBE_FIBER_PARK(a, b, c)	DTRACE_PROBE3(xtc, fiber__park, a, b, c)
#define XTC_PROBE_FIBER_RESUME(a, b, c)	DTRACE_PROBE3(xtc, fiber__resume, a, b, c)
#define XTC_PROBE_FIBER_WAKE(a, b, c)	DTRACE_PROBE3(xtc, fiber__wake, a, b, c)
#define XTC_PROBE_TASK_RUN_START(a, b, c) \
	DTRACE_PROBE3(xtc, task__run__start, a, b, c)
#define XTC_PROBE_TASK_RUN_DONE(a, b, c) \
	DTRACE_PROBE3(xtc, task__run__done, a, b, c)
#define XTC_PROBE_LOOP_POLL(a, b, c)	DTRACE_PROBE3(xtc, loop__poll, a, b, c)
#define XTC_PROBE_IO_DISPATCH(a, b, c, d) \
	DTRACE_PROBE4(xtc, io__dispatch, a, b, c, d)
#define XTC_PROBE_SCHED_STEAL(a, b, c)	DTRACE_PROBE3(xtc, sched__steal, a, b, c)
#define XTC_PROBE_TIMER_ARM(a, b, c)	DTRACE_PROBE3(xtc, timer__arm, a, b, c)
#define XTC_PROBE_TIMER_FIRE(a, b, c)	DTRACE_PROBE3(xtc, timer__fire, a, b, c)
#define XTC_PROBE_TIMER_CANCEL(a)	DTRACE_PROBE1(xtc, timer__cancel, a)
#define XTC_PROBE_AIO_SUBMIT(a, b, c, d) \
	DTRACE_PROBE4(xtc, aio__submit, a, b, c, d)
#define XTC_PROBE_AIO_DONE(a, b, c)	DTRACE_PROBE3(xtc, aio__done, a, b, c)
#define XTC_PROBE_BLOCKING_ENQUEUE(a, b) \
	DTRACE_PROBE2(xtc, blocking__enqueue, a, b)
#define XTC_PROBE_BLOCKING_START(a)	DTRACE_PROBE1(xtc, blocking__start, a)
#define XTC_PROBE_BLOCKING_DONE(a, b)	DTRACE_PROBE2(xtc, blocking__done, a, b)
#define XTC_PROBE_LOCK_WAIT_START(a, b, c, d) \
	DTRACE_PROBE4(xtc, lock__wait__start, a, b, c, d)
#define XTC_PROBE_LOCK_WAIT_DONE(a, b, c, d) \
	DTRACE_PROBE4(xtc, lock__wait__done, a, b, c, d)
#define XTC_PROBE_SUP_RESTART(a, b, c)	DTRACE_PROBE3(xtc, sup__restart, a, b, c)
#define XTC_PROBE_PREEMPT_TICK(a)	DTRACE_PROBE1(xtc, preempt__tick, a)
#define XTC_PROBE_ALLOC_FAIL(a)		DTRACE_PROBE1(xtc, alloc__fail, a)
#define XTC_PROBE_SLAB_GROW(a, b)	DTRACE_PROBE2(xtc, slab__grow, a, b)
#define XTC_PROBE_SIM_NONDETERMINISM(a) \
	DTRACE_PROBE1(xtc, sim__nondeterminism, a)
#define XTC_PROBE_SIM_BUGGIFY(a, b)	DTRACE_PROBE2(xtc, sim__buggify, a, b)

#else /* probes compiled out */

#define XTC_PROBE_ENABLED(name)		(0)

#define XTC_PROBE_PROC_SPAWN(a, b)	do { } while (0)
#define XTC_PROBE_PROC_START(a, b)	do { } while (0)
#define XTC_PROBE_PROC_EXIT(a, b)	do { } while (0)
#define XTC_PROBE_PROC_KILL(a, b)	do { } while (0)
#define XTC_PROBE_MSG_SEND(a, b, c)	do { } while (0)
#define XTC_PROBE_MSG_RECV(a, b, c)	do { } while (0)
#define XTC_PROBE_FIBER_PARK(a, b, c)	do { } while (0)
#define XTC_PROBE_FIBER_RESUME(a, b, c)	do { } while (0)
#define XTC_PROBE_FIBER_WAKE(a, b, c)	do { } while (0)
#define XTC_PROBE_TASK_RUN_START(a, b, c) do { } while (0)
#define XTC_PROBE_TASK_RUN_DONE(a, b, c) do { } while (0)
#define XTC_PROBE_LOOP_POLL(a, b, c)	do { } while (0)
#define XTC_PROBE_IO_DISPATCH(a, b, c, d) do { } while (0)
#define XTC_PROBE_SCHED_STEAL(a, b, c)	do { } while (0)
#define XTC_PROBE_TIMER_ARM(a, b, c)	do { } while (0)
#define XTC_PROBE_TIMER_FIRE(a, b, c)	do { } while (0)
#define XTC_PROBE_TIMER_CANCEL(a)	do { } while (0)
#define XTC_PROBE_AIO_SUBMIT(a, b, c, d) do { } while (0)
#define XTC_PROBE_AIO_DONE(a, b, c)	do { } while (0)
#define XTC_PROBE_BLOCKING_ENQUEUE(a, b) do { } while (0)
#define XTC_PROBE_BLOCKING_START(a)	do { } while (0)
#define XTC_PROBE_BLOCKING_DONE(a, b)	do { } while (0)
#define XTC_PROBE_LOCK_WAIT_START(a, b, c, d) do { } while (0)
#define XTC_PROBE_LOCK_WAIT_DONE(a, b, c, d) do { } while (0)
#define XTC_PROBE_SUP_RESTART(a, b, c)	do { } while (0)
#define XTC_PROBE_PREEMPT_TICK(a)	do { } while (0)
#define XTC_PROBE_ALLOC_FAIL(a)		do { } while (0)
#define XTC_PROBE_SLAB_GROW(a, b)	do { } while (0)
#define XTC_PROBE_SIM_NONDETERMINISM(a)	do { } while (0)
#define XTC_PROBE_SIM_BUGGIFY(a, b)	do { } while (0)

#endif

#endif /* XTC_PROBES_H */
