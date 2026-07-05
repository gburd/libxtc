/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_launch.h
 *	Bounded-time / cancellable work: xtc_launch (M_PREEMPTION Phase 3,
 *	the libinger launch(f, timeout) model, reimplemented natively).
 *
 *	Run fn(arg) on a child fiber with a one-shot DEADLINE.  If fn
 *	finishes within the deadline, its return value is delivered; if it
 *	exceeds the deadline it is CANCELLED at the deadline (its recovery
 *	/ at-exit cleanup runs, so locks, fds, and memory contexts it
 *	registered are released -- no leak) and xtc_launch returns
 *	XTC_E_TIMEDOUT.  This is the statement-timeout / bounded-untrusted-
 *	work primitive.
 *
 *	Precise-timeout on a RUNAWAY (a fn that never reaches a cooperative
 *	yield point) requires involuntary preemption to be enabled on the
 *	executor (xtc_exec_set_preempt + xtc_preempt_set_involuntary(1), on
 *	the arches where it is effective); otherwise a purely-uncooperative
 *	fn can only be cancelled once it next reaches a safe point.  A
 *	cooperating fn (one that yields, recvs, or does I/O) is bounded at
 *	its next such point regardless.
 *
 *	Composable: a launched fn may itself xtc_launch.  Must be called
 *	from a fiber (it parks the caller waiting on the child); off a
 *	fiber it drives the loop.
 */

#ifndef XTC_LAUNCH_H
#define XTC_LAUNCH_H

#include <stdint.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The launched function.  Runs on its own fiber; its intptr_t return is
 * delivered to the launcher on a clean finish. */
typedef intptr_t (*xtc_launch_fn)(void *arg);

/* The exit reason xtc_launch uses when it cancels a timed-out child, so
 * the child's DOWN is distinguishable from a fault. */
#define XTC_LAUNCH_CANCEL_REASON  0x4c43   /* 'LC' */

typedef struct xtc_launch_opts {
	const char *name;        /* child proc label (logs); may be NULL */
	size_t      mailbox_cap; /* child mailbox cap; 0 = default */
	int         loop;        /* exec loop index when launched on an exec
	                          * loop (via xtc_exec_loop); 0 default */
} xtc_launch_opts_t;

/*
 * Launch fn(arg) on `loop` with a `timeout_ns` deadline (< 0 = no
 * deadline, i.e. a plain awaited spawn).  A NULL `loop` means the loop
 * the caller is currently running on (the natural default when
 * launching from inside a fiber).  Blocks (parks) the caller until fn
 * finishes or the deadline fires.
 *
 * On a clean finish within the deadline: returns XTC_OK and, if result
 * != NULL, stores fn's return value.
 *
 * On deadline: cancels the child (xtc_exit_pid -> its recovery/at-exit
 * cleanup releases resources) and returns XTC_E_AGAIN (the runtime's
 * timeout/try-again code, as xtc_recv / xtc_svr_call use).
 *
 * On a fault inside fn (contained by the per-fiber recovery): returns
 * XTC_E_ABORTED.
 *
 * Other returns: XTC_E_INVAL (bad args), XTC_E_NOMEM / spawn failure.
 *
 * PUBLIC: int xtc_launch __P((xtc_loop_t *, xtc_launch_fn, void *, int64_t, const xtc_launch_opts_t *, intptr_t *));
 */
int xtc_launch(xtc_loop_t *loop, xtc_launch_fn fn, void *arg,
               int64_t timeout_ns, const xtc_launch_opts_t *opts,
               intptr_t *result);

#ifdef __cplusplus
}
#endif

#endif /* XTC_LAUNCH_H */
