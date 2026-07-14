/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_pkey.h
 *	Optional memory-protection-key (PKU) tier for in-process memory
 *	isolation (isolation tier c).  On Linux/x86 with PKU, a region
 *	can be tagged with a protection key and a thread can then disable
 *	its own access/write to that key's pages in O(1) (no syscall on
 *	the toggle, via the PKRU register), giving coarse, domain-based
 *	in-process isolation -- a guest can be denied access to host
 *	pages without a separate address space.
 *
 *	This is a hardening tier, NOT a portable guarantee: keys are
 *	scarce (16 on x86), it is Linux/x86-only (ARMv8.5 POE is newer
 *	and not wired here), and there is no macOS/Windows equivalent.
 *	Every call returns XTC_E_NOSYS where PKU is unavailable; probe
 *	with xtc_pkey_supported() first.  For real, portable isolation
 *	use process-per-task (xtc_osproc).
 */

#ifndef XTC_PKEY_H
#define XTC_PKEY_H

#include "xtc_export.h"

#include <stddef.h>

#include "xtc.h"

/*
 * PUBLIC: int xtc_pkey_supported __P((void));
 * PUBLIC: int xtc_pkey_alloc __P((int *));
 * PUBLIC: int xtc_pkey_protect __P((void *, size_t, int));
 * PUBLIC: int xtc_pkey_set_access __P((int, int, int));
 * PUBLIC: int xtc_pkey_free __P((int));
 */

/* 1 if protection keys are usable on this host right now, else 0. */
XTC_API int xtc_pkey_supported(void);

/* Allocate a protection key (initially full access).  XTC_OK + *out,
 * or XTC_E_NOSYS / XTC_E_INVAL. */
XTC_API int xtc_pkey_alloc(int *out_key);

/* Tag [addr,addr+len) (page-aligned) with key, keeping read+write
 * protection bits.  Access is then gated by the key's per-thread
 * rights (xtc_pkey_set_access). */
XTC_API int xtc_pkey_protect(void *addr, size_t len, int key);

/* Set the CALLING thread's rights for key: allow_read/allow_write
 * (0 disables).  Disabling read implies disabling write. */
XTC_API int xtc_pkey_set_access(int key, int allow_read, int allow_write);

/* Release a key. */
XTC_API int xtc_pkey_free(int key);

#endif /* XTC_PKEY_H */
