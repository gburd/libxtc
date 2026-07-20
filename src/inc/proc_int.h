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

#endif /* XTC_PROC_INT_H */
