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

#endif /* XTC_PROC_INT_H */
