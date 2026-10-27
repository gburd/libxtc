/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/xtc_probes.d
 *	The libxtc static probe set: ONE provider, `xtc`, serving DTrace
 *	(FreeBSD, illumos, macOS) and, through the same .note.stapsdt
 *	notes, perf, bpftrace, bcc, SystemTap and LTTng on Linux.
 *
 *	This file is the single definition.  The build turns it into
 *	the macro header (`dtrace -h`, then XTC_ -> XTC_PROBE_), and on
 *	FreeBSD / illumos / Linux-with-systemtap-dtrace into the DOF object
 *	(`dtrace -G`).  src/inc/xtc_probes.h hand-mirrors every probe for
 *	the header-only <sys/sdt.h> path and the probes-off build;
 *	test/dist/test_probes.sh fails if the two drift.
 *
 *	The probes are an INTERFACE: names, argument order and meaning are
 *	documented in xtc_usdt(3) and change only with a SemVer note.
 *	Conventions shared by every probe:
 *
 *	  pid     a libxtc pid as TWO arguments (2.0: the pid is 16 bytes):
 *	          pid = loop_id << 32 | local_id, then its 64-bit gen.
 *	          A pid's identity is the pair; 0, 0 is XTC_PID_NONE.
 *	  task    the xtc_task_t * of a fiber, as an integer: the join key
 *	          between park, wake, resume and the run probes (it is
 *	          stable for the fiber's life, and a fiber can migrate
 *	          between OS threads, so key D arrays on it, not on tid).
 *	  loop    the executor loop id (xtc_exec loop index), -1 for a
 *	          loop that is not part of an executor.
 *
 *	No probe argument reads a clock: use D's `timestamp` (or bpftrace
 *	`nsecs`).  That keeps every probe outside the DST determinism
 *	guard.  Numeric codes (park reasons, lock kinds) are listed in
 *	xtc_usdt(3) and src/inc/xtc_probes.h.
 */

provider xtc {
	/* Process lifecycle. */
	probe proc__spawn(uint64_t pid, uint64_t gen, uint64_t parent,
	    uint64_t parent_gen);
	probe proc__start(uint64_t pid, uint64_t gen, uintptr_t task);
	probe proc__exit(uint64_t pid, uint64_t gen, int reason);
	probe proc__kill(uint64_t target, uint64_t gen, int reason);

	/* Mailbox. */
	probe msg__send(uint64_t from, uint64_t from_gen, uint64_t to,
	    uint64_t to_gen, size_t bytes);
	probe msg__recv(uint64_t pid, uint64_t gen, uint64_t from,
	    uint64_t from_gen, size_t bytes);

	/* Fiber park / wake / resume. */
	probe fiber__park(uintptr_t task, int reason, uintptr_t object);
	probe fiber__wake(uintptr_t task, int loop, int how);
	probe fiber__resume(uintptr_t task, int reason, uintptr_t object);

	/* Scheduler. */
	probe task__run__start(uintptr_t task, int loop, int lane);
	probe task__run__done(uintptr_t task, int loop, int verdict);
	probe loop__poll(int loop, int64_t timeout_ns, int nevents);
	probe io__dispatch(int loop, int fd, uint32_t flags, uintptr_t task);
	probe sched__steal(int thief, int victim, uintptr_t task);

	/* Timers. */
	probe timer__arm(uintptr_t timer, int64_t deadline_ns, uintptr_t task);
	probe timer__fire(uintptr_t timer, int64_t deadline_ns, int64_t now_ns);
	probe timer__cancel(uintptr_t timer);

	/* Native file AIO (every completion backend). */
	probe aio__submit(uintptr_t aio, int op, int fd, uint32_t len);
	probe aio__done(uintptr_t aio, int op, int res);

	/* Blocking-pool hand-off. */
	probe blocking__enqueue(uintptr_t work, int qlen);
	probe blocking__start(uintptr_t work);
	probe blocking__done(uintptr_t work, int result);

	/* Blocking lock waits (amutex, arwlock, lwlock, lock manager): one
	 * start/done pair per sleep or park, as PostgreSQL's lwlock probes. */
	probe lock__wait__start(uintptr_t lock, int kind, int mode, uintptr_t waiter);
	probe lock__wait__done(uintptr_t lock, int kind, int mode, uintptr_t waiter);

	/* Supervision and preemption. */
	probe sup__restart(uintptr_t sup, uint64_t child, uint64_t child_gen,
	    int reason);
	probe preempt__tick(int involuntary);

	/* Allocation slow paths. */
	probe alloc__fail(size_t bytes);
	probe slab__grow(uintptr_t slab, size_t bytes);

	/* Deterministic simulation. */
	probe sim__nondeterminism(const char *what);
	probe sim__buggify(const char *site, int fired);
};

#pragma D attributes Evolving/Evolving/Common provider xtc provider
#pragma D attributes Private/Private/Unknown provider xtc module
#pragma D attributes Private/Private/Unknown provider xtc function
#pragma D attributes Evolving/Evolving/Common provider xtc name
#pragma D attributes Evolving/Evolving/Common provider xtc args
