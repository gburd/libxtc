/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/os_errno.h
 *	Canonical POSIX errno -> XTC_E_* translation for the OS layer
 *	(INTERNAL, __os_* surface), plus an internal override hook whose
 *	eventual audience is an embedder that shims the underlying
 *	syscalls (a custom VFS, a hosted/sandboxed libc); that hook gets a
 *	public xtc_* wrapper if/when a real embedder needs it (see the
 *	hook comment below).  See M1_CLAIMS.md, Tm-errno.
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
 * Translation-override hook (INTERNAL, __os_* surface).  fn receives the
 * raw errno and returns an XTC_E_* code, or 0 to fall through to the
 * built-in table.  Installing NULL restores the default (built-in table
 * only).  Thread-safe: the swap is a single atomic pointer store.
 *
 * Audience note: the eventual audience for overriding the errno mapping
 * is an EMBEDDER that shims the underlying syscalls (a custom VFS, a
 * hosted/sandboxed libc).  That is a consumer action, and consumers use
 * only the public xtc_* API (never __os_*), so when a real embedder
 * needs this it gets a thin public wrapper (xtc_errno_set_hook /
 * _get_hook) in an installed header -- exactly the pattern the
 * allocator hook (__os_alloc_set_hook) follows.  Until then this stays
 * __os_*-internal rather than shipping a public API with no caller
 * (YAGNI); __os_errno_map itself is unconditionally internal (library
 * OS wrappers call it directly).
 *
 * PUBLIC: void __os_errno_set_hook __P((int (*)(int)));
 * PUBLIC: int  (*__os_errno_get_hook __P((void)))(int);
 */
XTC_API void __os_errno_set_hook(int (*fn)(int posix_errno));
XTC_API int (*__os_errno_get_hook(void))(int posix_errno);

#endif /* XTC_OS_ERRNO_H */
