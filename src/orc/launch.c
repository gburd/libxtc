/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/orc/launch.c
 *	xtc_launch -- bounded-time / cancellable work (M_PREEMPTION Phase
 *	3, the libinger launch(f, timeout) model).  See xtc_launch.h.
 *
 *	Built on the substrate libxtc already has: a child xtc_proc runs
 *	fn(arg), the launcher monitors it and parks on a timed receive for
 *	the DOWN.  Finish-first delivers fn's result; deadline-first cancels
 *	the child (xtc_exit_pid, whose recovery/at-exit path releases the
 *	child's held resources) and reports XTC_E_AGAIN.  A runaway CPU fn
 *	is time-sliced to a safe point by involuntary preemption (when
 *	enabled) so the launcher's deadline can fire; a cooperating fn is
 *	bounded at its next yield/recv/IO point regardless.
 *
 *	What the launcher owes its caller and its child (2.1.2):
 *	  - it takes ONLY its child's DOWN from the caller's mailbox (a
 *	    selective receive on the monitor ref); the caller's other mail
 *	    stays queued.  2.1.1 read and freed every message.
 *	  - the deadline is absolute, computed once.  2.1.1 re-armed the
 *	    relative timeout on every unrelated message, so steady mail
 *	    kept a timed-out launch waiting forever.
 *	  - the child does not outlive the launcher: a scope finalizer on
 *	    the launcher kills it if the launcher exits (is killed, faults)
 *	    while it waits.  2.1.1 only monitored the child, which ran on
 *	    as an orphan.  A link is not used: a link kills the LAUNCHER
 *	    when the child exits abnormally, which the timeout and fault
 *	    returns below exist to report instead.
 *	  - the box the child writes its result into is reference-counted
 *	    (launcher + child), so neither frees it under the other.  2.1.1
 *	    freed it on its error paths right after an asynchronous kill,
 *	    while the child could still run.
 */

#include "xtc_int.h"
#include "xtc_launch.h"
#include "xtc_proc.h"
#include "xtc_async.h"     /* xtc_yield */
#include "xtc_dst_inject.h"

#include "os_alloc.h"
#include "loop_int.h"    /* __xtc_current_loop: NULL loop == the caller's loop */
#include "proc_int.h"    /* __xtc_exit_pid_on: owner-qualified kill */

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

/* Shared between the launcher and the launched child.  Freed by
 * whichever of the two drops the last reference. */
struct launch_box {
	xtc_launch_fn      fn;
	void              *arg;
	_Atomic intptr_t   result;      /* fn's return, valid iff finished */
	_Atomic int        finished;    /* 1 once fn returned cleanly */
	_Atomic int refs;               /* launcher + child */
	xtc_loop_t *loop;               /* launcher only: the child's owner */
	xtc_pid_t child;                /* launcher only: NONE once reaped */
};

static void
launch_box_put(struct launch_box *box)
{
	if (atomic_fetch_sub_explicit(&box->refs, 1, memory_order_acq_rel) == 1)
		__os_free(box);
}

/* The child's at-exit hook: its reference, on every exit path. */
static void
launch_child_exited(void *arg)
{
	launch_box_put(arg);
}

/* The child trampoline: run fn, publish its result, return normally
 * (exit reason 0).  If fn is cancelled (xtc_exit_pid) mid-run, this
 * never completes -- finished stays 0 and the launcher treats the
 * outcome as a timeout/cancel. */
static void
launch_child(void *arg)
{
	struct launch_box *box = arg;
	intptr_t r;
	/* A new proc's at-exit table is empty, so this cannot fail; if it
	 * did, give the reference back and do not run fn. */
	if (xtc_proc_at_exit(launch_child_exited, box) != XTC_OK) {
		launch_box_put(box);
		return;
	}
	r = box->fn(box->arg);
	atomic_store_explicit(&box->result, r, memory_order_release);
	atomic_store_explicit(&box->finished, 1, memory_order_release);
}

/*
 * The launcher's reference, as a scope finalizer: it runs when the
 * launcher closes the scope, or on the launcher's unwind if the launcher
 * is killed or faults while it waits -- and then it kills the child,
 * which has not been reaped.  Heap state only: on an unwind the
 * launcher's stack frame is gone.
 */
static void
launch_guard_fire(void *arg)
{
	struct launch_box *box = arg;
	if (!xtc_pid_is_none(box->child))
		(void)__xtc_exit_pid_on(box->loop, box->child,
		    XTC_LAUNCH_CANCEL_REASON);
	launch_box_put(box);
}

/* The child's DOWN: a 'D' carrying our monitor ref (refs are global and
 * never 0, so this names exactly our monitor). */
static int
launch_match_down(const void *data, size_t size, void *u)
{
	xtc_down_info_t di = XTC_DOWN_INFO_INIT;
	if (xtc_down_decode_ex(data, size, &di) != XTC_OK)
		return 0;
	return *(const uint8_t *)data == 'D' && di.ref == *(uint64_t *)u;
}

#if XTC_DST_BUG(XTC_DST_BUG_LAUNCH_EATMAIL)
/* The 2.1.1 receive: takes (and the loop below frees) any message. */
static int
launch_match_any(const void *data, size_t size, void *u)
{
	(void)data;
	(void)size;
	(void)u;
	return 1;
}
#endif

/*
 * PUBLIC: int xtc_launch __P((xtc_loop_t *, xtc_launch_fn, void *, int64_t, const xtc_launch_opts_t *, intptr_t *));
 */
int
xtc_launch(xtc_loop_t *loop, xtc_launch_fn fn, void *arg,
           int64_t timeout_ns, const xtc_launch_opts_t *opts,
           intptr_t *result)
{
	struct launch_box *box = NULL;
	xtc_scope_t *scope;
	xtc_proc_opts_t popts;
	xtc_launch_opts_t o = XTC_LAUNCH_OPTS_INIT;
	xtc_pid_t child;
	uint64_t mref = 0;
	int64_t deadline = -1, now = 0, wait;
	int rc, timed_out = 0, finished;
	intptr_t r;
	xtc_down_info_t di = XTC_DOWN_INFO_INIT;
	void *m = NULL;
	size_t n = 0;

	if (fn == NULL)
		return XTC_E_INVAL;
	if (opts != NULL &&
	    (rc = __xtc_sized_in(&o, NULL, sizeof o, opts, 0)) != XTC_OK)
		return rc;
	/* A NULL loop means "the loop the caller is running on" -- the
	 * natural default for launching from inside a fiber. */
	if (loop == NULL)
		loop = __xtc_current_loop;
	if (loop == NULL)
		return XTC_E_INVAL;   /* no loop given and not on one */
	if (xtc_pid_is_none(xtc_self()))
		return XTC_E_INVAL; /* parks the caller: needs a proc */

	if (__os_calloc(1, sizeof *box, (void **)&box) != XTC_OK)
		return XTC_E_NOMEM;
	box->fn = fn;
	box->arg = arg;
	atomic_init(&box->result, 0);
	atomic_init(&box->finished, 0);
	atomic_init(&box->refs, 2); /* launcher + child */

	/* The guard's scope is opened before the spawn, so a failure here
	 * leaves nothing to undo but the box. */
	if ((scope = xtc_scope_open()) == NULL) {
		__os_free(box);
		return XTC_E_RESOURCE;
	}

	memset(&popts, 0, sizeof popts);
	popts.size = sizeof popts;
	popts.name = o.name != NULL ? o.name : "xtc-launch";
	popts.mailbox_cap = o.mailbox_cap;

	/* Spawn and monitor atomically: a child that finishes at once still
	 * reports its real exit, not a NOPROC DOWN. */
	rc = xtc_proc_spawn_monitor(loop, launch_child, box, &popts, &child,
	    &mref);
	if (rc != XTC_OK) {
		xtc_scope_close(scope);
		__os_free(box);
		return rc;
	}
	box->loop = loop;
	box->child = child;
	/* Cannot fail: the scope is new (one finalizer of 32). */
	(void)xtc_scope_defer(scope, launch_guard_fire, box);

	if (timeout_ns >= 0) {
		(void)__os_clock_mono(&deadline);
		deadline += timeout_ns;
	}
	for (;;) {
		wait = -1;
		if (deadline >= 0 && !timed_out) {
			(void)__os_clock_mono(&now);
			wait = deadline > now ? deadline - now : 0;
		}
#if XTC_DST_BUG(XTC_DST_BUG_LAUNCH_EATMAIL)
		rc = xtc_recv_match(launch_match_any, NULL, &m, &n, wait);
#else
		rc = xtc_recv_match(launch_match_down, &mref, &m, &n, wait);
#endif
		if (rc == XTC_E_AGAIN && !timed_out) {
			/* Deadline fired before the child exited: cancel it,
			 * then wait (unbounded) for the cancel's DOWN: the
			 * child is already dying and the box is not freed
			 * before it is. */
			timed_out = 1;
			(void)__xtc_exit_pid_on(loop, child,
			    XTC_LAUNCH_CANCEL_REASON);
			continue;
		}
		if (rc != XTC_OK) {
			/* A receive error (NOMEM copying the DOWN out): the
			 * guard cancels the child, which still holds its own
			 * reference to the box. */
			xtc_scope_close(scope);
			return rc;
		}
		if (xtc_down_decode_ex(m, n, &di) == XTC_OK &&
		    *(const uint8_t *)m == 'D' && di.ref == mref)
			break;
		__os_free(m); /* LAUNCH_EATMAIL plant only: drops caller mail */
		m = NULL;
	}
	__os_free(m);

	/* The child is reaped: read the box, then drop our reference (the
	 * guard, with nothing left to kill). */
	finished = atomic_load_explicit(&box->finished, memory_order_acquire);
	r = atomic_load_explicit(&box->result, memory_order_acquire);
	box->child = XTC_PID_NONE;
	xtc_scope_close(scope);

	if (timed_out)
		return XTC_E_AGAIN;     /* deadline: cancelled, try-again/timeout */
	/* A nonzero reason without a finish means the child faulted (or
	 * exited with an error): its recovery path unwound it. */
	if (!finished && di.reason != 0)
		return XTC_E_ABORTED; /* fn faulted; contained + reaped */
	if (result != NULL)
		*result = r;
	return XTC_OK;
}
