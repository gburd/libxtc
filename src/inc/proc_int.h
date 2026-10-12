/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/proc_int.h
 *	Internal process primitives that are not part of the consumer
 *	API.  Split out of xtc_proc.h so no __-prefixed symbol leaks into
 *	an installed public header.
 *
 *	(The recovery-frame __xtc_recovery_* / __xtc_proc_recovery_slot
 *	decls stay in xtc_proc.h: the PUBLIC macro xtc_proc_recovery_arm()
 *	expands to them, so they must be visible in the consumer's TU.
 *	These context save/restore helpers are called only from within
 *	the library, so they belong here.)
 */

#ifndef XTC_PROC_INT_H
#define XTC_PROC_INT_H

#include "xtc_proc.h"

/* Owner/quiescent lookup on a live loop, including procs still in exit hooks.
 * PIDs are executor-relative; a global info scan cannot establish this join. */
int __xtc_proc_loop_contains(xtc_loop_t *, xtc_pid_t);

/* The calling proc's OWNER loop: the one it was spawned on, whose table
 * holds its slot (p->loop).  Unchanged by work-stealing migration, so it
 * is NOT necessarily the loop the fiber is running on.  NULL off a proc.
 * *out_pid (if non-NULL) gets the same proc's pid.  (pid, owner) is an
 * unambiguous identity; record it on the proc's own fiber. */
xtc_loop_t *__xtc_proc_self_loop(xtc_pid_t *out_pid);

/* xtc_exit_pid_deadline resolved in `owner`'s table only, so an equal pid
 * on another loop is never signalled.  owner must be non-NULL and alive
 * (not finalized) for the call; XTC_E_INVAL if NULL. */
int __xtc_exit_pid_deadline_on(xtc_loop_t *owner, xtc_pid_t target,
    int reason, int64_t timeout_ns, int *out_status);

/* xtc_exit_pid / xtc_send resolved in `owner`'s table only (same owner
 * contract as above; XTC_E_INVAL if owner is NULL). */
int __xtc_exit_pid_on(xtc_loop_t *owner, xtc_pid_t target, int reason);
int __xtc_send_on(xtc_loop_t *owner, xtc_pid_t to, const void *data,
    size_t size);

/*
 * Save / restore the current-proc context across a yield done by a
 * lower-level primitive (e.g. xtc_amutex parking the fiber), so the
 * proc still sees itself on resume.  Opaque to the caller.
 */
void     *__xtc_proc_ctx_save(void);
void      __xtc_proc_ctx_restore(void *ctx);

/*
 * Exit the calling proc with `reason` and an explicit DOWN `kind`
 * (xtc_down_kind_t) rather than the CLEAN/EXIT xtc_exit_self infers from
 * the code.  For a proc that reports someone else's fate as its own --
 * the xproc shadow standing in for an OS child killed by a signal.
 * Does not return on success; XTC_E_INVAL off a proc or for a bad kind.
 */
int       __xtc_exit_self_kind(int reason, int kind);

/*
 * A3 async causal trace hook: record one suspend/resume boundary on the
 * CALLING proc's per-fiber ring.  `kind` is an enum xtc_causal_kind and
 * `site` a static string label (e.g. __func__).  A no-op fast path (one
 * relaxed load + branch) when the causal trace is disabled or off a
 * proc, so a default build writes nothing on the park/resume path.
 * Single-writer / core-private: called only on the proc's own fiber.
 */
void      __xtc_trace_causal(int kind, const char *site);

/*
 * Deliver a kill pending on the calling proc, at a park site's safe
 * point (after it has dequeued its waiter, returned any grant, cancelled
 * its park timer and dropped its locks).  No-op off a proc, when no kill
 * is pending, or when the kill is masked (it is latched, as at any other
 * delivery point); otherwise unwinds via xtc_exit_self.  The resume hook
 * inside xtc_yield does NOT deliver after a park yield, so a park site
 * that never calls this simply sees the kill at the proc's next delivery
 * point.
 */
void      __xtc_proc_kill_poll(void);

/* xtc_exit_pid makes a target parked anywhere runnable (except inside
 * xtc_await), so a park site re-checks promptly and can deliver the kill. */
#define XTC_HAVE_PROC_KILL_WAKE 1

#endif /* XTC_PROC_INT_H */
