/*-
 * Copyright (c) 2026, The XTC Project
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

#include <stddef.h>
#include <stdint.h>

#include "xtc_proc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PUBLIC: int  xtc_alloc_audit_enable __P((int));
 * PUBLIC: void xtc_alloc_audit_stats __P((size_t *, size_t *));
 * PUBLIC: void xtc_alloc_audit_proc_leaks __P((xtc_pid_t, size_t *, size_t *));
 */

/* Enable (on != 0) or disable the auditor.  Enabling wraps the
 * current allocator hook; disabling restores it.  Returns XTC_OK, or
 * XTC_E_NOMEM if the tracking table could not be allocated. */
int  xtc_alloc_audit_enable(int on);

/* Total live (unfreed) allocations and their byte sum across all
 * owners.  Either pointer may be NULL. */
void xtc_alloc_audit_stats(size_t *out_count, size_t *out_bytes);

/* Live allocations still attributed to process `pid` (allocations
 * made off a process are attributed to the none pid).  Either output
 * pointer may be NULL.  This is the per-proc leak check: call it from
 * the process's xtc_proc_at_exit hook or after it is reaped. */
void xtc_alloc_audit_proc_leaks(xtc_pid_t pid, size_t *out_count,
                                size_t *out_bytes);

#ifdef __cplusplus
}
#endif

#endif /* XTC_ALLOC_AUDIT_H */
