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
 * One table probe, no registry walk (through 2.1 PIDs were executor-relative
 * and a global info scan could not establish this join). */
int __xtc_proc_loop_contains(xtc_loop_t *, xtc_pid_t);

/* 1 iff pid names a live proc (its body has not ended), from any thread;
 * a snapshot.  0 for XTC_PID_NONE or a proc gone or in its exit hooks. */
int __xtc_proc_alive(xtc_pid_t pid);

/* The calling proc's OWNER loop: the one it was spawned on, whose table
 * holds its slot (p->loop).  Unchanged by work-stealing migration, so it
 * is NOT necessarily the loop the fiber is running on.  NULL off a proc.
 * *out_pid (if non-NULL) gets the same proc's pid.  (pid, owner) is an
 * unambiguous identity; record it on the proc's own fiber. */
xtc_loop_t *__xtc_proc_self_loop(xtc_pid_t *out_pid);

/* Startup acknowledgement (2.2, a supervisor's wait_children): a child
 * spawned with __xtc_proc_spawn_monitor_ack sends the spawner one sys
 * message {'K', monitor ref} when it calls __xtc_proc_start_ack (a server
 * after init, a nested supervisor once its children are up); a child that
 * dies first sends only its DOWN.  __xtc_proc_start_ack_wanted: 1 iff the
 * calling proc's spawner waits for that. */
int __xtc_proc_spawn_monitor_ack(xtc_loop_t *loop, xtc_proc_fn fn, void *arg,
    const xtc_proc_opts_t *opts, xtc_pid_t *out_pid, uint64_t *out_ref);
int __xtc_proc_start_ack_wanted(void);
void __xtc_proc_start_ack(void);

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

/* Test seam: set the generation of a FREE slot on `loop` (see proc.c). */
int __xtc_proc_slot_gen_set(xtc_loop_t *loop, uint32_t local_id,
    uint64_t gen);

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

/* Unbind any xtc_cfg session bound by task `t` (cfg.c).  Called on the
 * proc exit path after the at-exit hooks, before the task can be
 * recycled for another proc. */
void      __xtc_cfg_task_exit(xtc_task_t *t);

/* Release the pdict entries of proc `pid`, running their destructors
 * (pdict.c).  Called on the proc exit path after the at-exit hooks, and
 * by xtc_loop_destroy for a proc it tears down. */
void __xtc_pdict_proc_exit(xtc_pid_t pid);

/* xtc_exit_pid makes a target parked anywhere runnable (except inside
 * xtc_coro_join), so a park site re-checks promptly and can deliver the kill. */
#define XTC_HAVE_PROC_KILL_WAKE 1

#endif /* XTC_PROC_INT_H */
