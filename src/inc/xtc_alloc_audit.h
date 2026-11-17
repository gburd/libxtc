/*-
 * Copyright (c) 2026, The XTC Project
 * @stability stable
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_alloc_audit.h
 *	Debug allocation auditor.  When enabled it wraps the allocator
 *	vtable, recording every live allocation together with the
 *	process (xtc_self) that made it, so a test can assert that a
 *	process freed everything it allocated before it died -- per-proc
 *	leak detection.  Off by default; it serializes alloc/free on a
 *	global mutex, so it is a debug/test tool, not for production.
 */

#ifndef XTC_ALLOC_AUDIT_H
#define XTC_ALLOC_AUDIT_H

#include "xtc_export.h"

#include <stddef.h>
#include <stdint.h>

#include "xtc_proc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PUBLIC: void xtc_alloc_audit_stats __P((size_t *, size_t *));
 * PUBLIC: void xtc_alloc_audit_proc_leaks __P((xtc_pid_t, size_t *, size_t *));
 */

/* The auditor is turned on and off with xtc_runtime_set_alloc_audit
 * (xtc_runtime.h): enabling wraps the current allocator hook, disabling
 * restores it. */

/* Total live (unfreed) allocations and their byte sum across all
 * owners.  Either pointer may be NULL. */
XTC_API void xtc_alloc_audit_stats(size_t *out_count, size_t *out_bytes);

/* Live allocations still attributed to process `pid` (allocations
 * made off a process are attributed to the none pid).  Either output
 * pointer may be NULL.  This is the per-proc leak check: call it from
 * the process's xtc_proc_at_exit hook or after it is reaped. */
XTC_API void xtc_alloc_audit_proc_leaks(xtc_pid_t pid, size_t *out_count,
                                        size_t *out_bytes);

/*
 * Deprecated names (docs/deprecations.md): removed in 3.0.
 */
XTC_DEPRECATED("use xtc_runtime_set_alloc_audit")
XTC_API int xtc_alloc_audit_enable(int on);

#ifdef __cplusplus
}
#endif

#endif /* XTC_ALLOC_AUDIT_H */
