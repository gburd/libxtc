/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/os_errno.h
 *	Canonical POSIX errno -> XTC_E_* translation for the OS layer,
 *	plus an embedder hook so a consumer that shims the underlying
 *	syscalls (a custom VFS, a hosted/sandboxed libc) can override the
 *	mapping.  See M1_CLAIMS.md, Tm-errno.
 *
 *	The rest of the runtime works exclusively in the XTC_E_* error
 *	space; this module is the ONE place a raw errno crosses into it.
 *	OS-layer wrappers call __os_errno_map(errno) at the syscall
 *	boundary instead of hand-rolling a per-file switch.  Hot-path
 *	control-flow checks on a single specific value (e.g. EAGAIN ->
 *	XTC_E_AGAIN in the readiness loop) stay inline by design -- this
 *	module is for GENERAL failure translation, not fast-path branches.
 */

#ifndef XTC_OS_ERRNO_H
#define XTC_OS_ERRNO_H

#include "xtc_export.h"

/*
 * Translate a POSIX errno value into a stable XTC_E_* code.  Passing 0
 * (no error) or an unrecognized value maps to XTC_E_IO -- the mapper is
 * only ever consulted on a KNOWN failure, so "no mapping" means "some
 * I/O-class error we do not distinguish", never success.  Callers that
 * need XTC_OK check the syscall return BEFORE calling this.
 *
 * If an embedder installed a hook, the hook is consulted first; a hook
 * return of 0 means "defer to the built-in table".
 *
 * PUBLIC: int __os_errno_map __P((int));
 */
XTC_API int __os_errno_map(int posix_errno);

/*
 * Embedder hook.  fn receives the raw errno and returns an XTC_E_*
 * code, or 0 to fall through to the built-in table.  Installing NULL
 * restores the default (built-in table only).  Thread-safe: the swap
 * is a single atomic pointer store, so it may be called at any time,
 * though embedders normally set it once at startup.
 *
 * PUBLIC: void __os_errno_set_hook __P((int (*)(int)));
 * PUBLIC: int  (*__os_errno_get_hook __P((void)))(int);
 */
XTC_API void __os_errno_set_hook(int (*fn)(int posix_errno));
XTC_API int (*__os_errno_get_hook(void))(int posix_errno);

#endif /* XTC_OS_ERRNO_H */
