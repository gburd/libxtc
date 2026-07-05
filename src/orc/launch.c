/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/orc/launch.c
 *	xtc_launch -- bounded-time / cancellable work (M_PREEMPTION Phase
 *	3, the libinger launch(f, timeout) model).  See xtc_launch.h.
 *
 *	Built on the substrate libxtc already has: a child xtc_proc runs
 *	fn(arg), the launcher monitors it and parks on a timed recv for the
 *	DOWN.  Finish-first delivers fn's result; deadline-first cancels
 *	the child (xtc_exit_pid, whose recovery/at-exit path releases the
 *	child's held resources) and reports XTC_E_TIMEDOUT.  A runaway
 *	CPU fn is time-sliced to a safe point by involuntary preemption
 *	(when enabled) so the launcher's deadline can fire; a cooperating
 *	fn is bounded at its next yield/recv/IO point regardless.
 */

#include "xtc_int.h"
#include "xtc_launch.h"
#include "xtc_proc.h"
#include "xtc_async.h"     /* xtc_yield */

#include "os_alloc.h"
#include "loop_int.h"    /* __xtc_current_loop: NULL loop == the caller's loop */

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

/* Shared box between the launcher and the launched child.  Heap-owned;
 * the launcher allocates it, the child fills result/finished, the
 * launcher frees it after the child is reaped (DOWN observed), so there
 * is no use-after-free even on the cancel path. */
struct launch_box {
	xtc_launch_fn      fn;
	void              *arg;
	_Atomic intptr_t   result;      /* fn's return, valid iff finished */
	_Atomic int        finished;    /* 1 once fn returned cleanly */
};

/* The child trampoline: run fn, publish its result, return normally
 * (exit reason 0).  If fn is cancelled (xtc_exit_pid) mid-run, this
 * never completes -- finished stays 0 and the launcher treats the
 * outcome as a timeout/cancel. */
static void
launch_child(void *arg)
{
	struct launch_box *box = arg;
	intptr_t r = box->fn(box->arg);
	atomic_store_explicit(&box->result, r, memory_order_release);
	atomic_store_explicit(&box->finished, 1, memory_order_release);
}

/*
 * PUBLIC: int xtc_launch __P((xtc_loop_t *, xtc_launch_fn, void *, int64_t, const xtc_launch_opts_t *, intptr_t *));
 */
int
xtc_launch(xtc_loop_t *loop, xtc_launch_fn fn, void *arg,
           int64_t timeout_ns, const xtc_launch_opts_t *opts,
           intptr_t *result)
{
	struct launch_box *box = NULL;
	xtc_proc_opts_t popts;
	xtc_pid_t child;
	uint64_t mref = 0;
	int rc;
	int timed_out = 0;
	int faulted = 0;

	if (fn == NULL)
		return XTC_E_INVAL;
	/* A NULL loop means "the loop the caller is running on" -- the
	 * natural default for launching from inside a fiber. */
	if (loop == NULL)
		loop = __xtc_current_loop;
	if (loop == NULL)
		return XTC_E_INVAL;   /* no loop given and not on one */

	if (__os_calloc(1, sizeof *box, (void **)&box) != XTC_OK ||
	    box == NULL)
		return XTC_E_NOMEM;
	box->fn = fn;
	box->arg = arg;
	atomic_init(&box->result, 0);
	atomic_init(&box->finished, 0);

	memset(&popts, 0, sizeof popts);
	popts.name = (opts != NULL && opts->name != NULL) ? opts->name
	    : "xtc-launch";
	if (opts != NULL && opts->mailbox_cap > 0)
		popts.mailbox_cap = opts->mailbox_cap;

	rc = xtc_proc_spawn(loop, launch_child, box, &popts, &child);
	if (rc != XTC_OK) {
		__os_free(box);
		return rc;
	}

	/* Monitor the child so its exit (clean finish OR our cancel) comes
	 * back as a DOWN we can wait for with a deadline. */
	rc = xtc_monitor(child, &mref);
	if (rc != XTC_OK) {
		/* Cannot monitor: fall back to a best-effort kill + return. */
		(void)xtc_exit_pid(child, 1);
		__os_free(box);
		return rc;
	}

	/* Wait for the DOWN, up to timeout_ns.  A negative timeout waits
	 * forever (a plain awaited launch). */
	for (;;) {
		void *m = NULL;
		size_t n = 0;
		xtc_pid_t dpid;
		int dreason = 0;

		rc = xtc_recv(&m, &n, timeout_ns);
		if (rc == XTC_E_AGAIN) {
			/* Deadline fired before the child exited: cancel it.
			 * The kill delivers a DOWN we drain on the next
			 * iteration (now with an unbounded wait, since the
			 * child is already dying -- the DOWN is imminent). */
			timed_out = 1;
			(void)xtc_exit_pid(child, XTC_LAUNCH_CANCEL_REASON);
			timeout_ns = -1;    /* wait for the cancel's DOWN */
			continue;
		}
		if (rc != XTC_OK) {
			/* recv error (e.g. called off a fiber with no loop to
			 * drive): best-effort kill + bail. */
			(void)xtc_exit_pid(child, 1);
			__os_free(box);
			return rc;
		}
		if (m == NULL)
			continue;

		/* Is this the child's DOWN?  Decode; ignore unrelated mail. */
		if (xtc_down_decode(m, n, &dpid, &dreason) == XTC_OK &&
		    xtc_pid_eq(dpid, child)) {
			__os_free(m);
			/* The child is reaped.  A nonzero reason that is NOT
			 * our cancel marker means the child faulted (its
			 * recovery path unwound it). */
			if (!atomic_load_explicit(&box->finished,
			    memory_order_acquire) && !timed_out &&
			    dreason != 0)
				faulted = 1;
			break;
		}
		/* Not our DOWN -- some other message arrived in our mailbox.
		 * Drop it (a launcher fiber owns no other protocol) and keep
		 * waiting; do not re-arm the original deadline (the child may
		 * be about to exit). */
		__os_free(m);
	}

	if (timed_out) {
		__os_free(box);
		return XTC_E_AGAIN;     /* deadline: cancelled, try-again/timeout */
	}
	if (faulted) {
		__os_free(box);
		return XTC_E_ABORTED;   /* fn faulted; contained + reaped */
	}
	if (result != NULL)
		*result = atomic_load_explicit(&box->result,
		    memory_order_acquire);
	__os_free(box);
	return XTC_OK;
}
