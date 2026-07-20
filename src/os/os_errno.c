/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/os_errno.c
 *	Canonical POSIX errno -> XTC_E_* translation + internal override
 *	hook (public xtc_* wrapper added on demand; see os_errno.h).
 *	See src/inc/os_errno.h and M1_CLAIMS.md, Tm-errno.
 */

#include "xtc.h"
#include "os_errno.h"

#include <errno.h>
#include <stdatomic.h>

/*
 * Optional override hook (internal __os_* surface; a public xtc_*
 * wrapper is added if a real embedder needs it -- see os_errno.h).
 * A function pointer swapped atomically.
 * ISO C forbids round-tripping a function pointer through void* (so the
 * object-pointer atomics in os_atomic.h are not usable here); a C11
 * _Atomic function-pointer type is the standard-clean way to publish it
 * lock-free.  NULL == no hook.  Readers do a relaxed load (the pointer
 * is set-once at startup in the normal case; a later swap is still
 * atomic, only unordered w.r.t. concurrent maps, which is fine -- a map
 * either sees the old or the new hook, never a torn pointer).
 */
typedef int (*errno_hook_fn)(int);
static _Atomic errno_hook_fn __errno_hook = NULL;

/*
 * PUBLIC: int __os_errno_map __P((int));
 */
int
__os_errno_map(int e)
{
	errno_hook_fn hook;

	hook = atomic_load_explicit(&__errno_hook, memory_order_relaxed);
	if (hook != NULL) {
		int rc = hook(e);
		if (rc != 0)          /* 0 == "defer to the built-in table" */
			return rc;
	}

	/*
	 * The built-in table.  A superset of the per-file err_map switches
	 * this module replaces, so consolidating onto it never LOSES a
	 * distinction any caller previously made.  Anything not listed is
	 * XTC_E_IO -- an I/O-class error the runtime does not further
	 * distinguish.  (This function is only reached on a KNOWN failure.)
	 */
	switch (e) {
	case 0:			return XTC_E_IO;   /* misuse: no error to map */
	case EINVAL:		return XTC_E_INVAL;
	case EFAULT:		return XTC_E_INVAL;
	case ENAMETOOLONG:	return XTC_E_INVAL;
	case EEXIST:		return XTC_E_INVAL;
	case ENOMEM:		return XTC_E_NOMEM;
	case ENOENT:		return XTC_E_NOTFOUND;
	case EAGAIN:		return XTC_E_AGAIN;
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
	case EWOULDBLOCK:	return XTC_E_AGAIN;
#endif
	case EINPROGRESS:	return XTC_E_AGAIN;
	case ECANCELED:		return XTC_E_ABORTED;
	case ENOSYS:		return XTC_E_NOSYS;
#if defined(ENOTSUP)
	case ENOTSUP:		return XTC_E_NOSYS;
#endif
#if defined(EOPNOTSUPP) && (!defined(ENOTSUP) || EOPNOTSUPP != ENOTSUP)
	case EOPNOTSUPP:	return XTC_E_NOSYS;
#endif
	case ERANGE:		return XTC_E_RANGE;
	case EOVERFLOW:		return XTC_E_RANGE;
	/*
	 * The remaining POSIX failures the OS layer encounters -- EACCES,
	 * EPERM, EBUSY, ENOSPC, EIO, ETIMEDOUT, EINTR, EBADF, EPIPE, ... --
	 * have no more specific XTC_E_ code today, so they fold to
	 * XTC_E_IO.  Add a case here (not in a caller) if one earns a
	 * distinct code.
	 */
	default:		return XTC_E_IO;
	}
}

/*
 * PUBLIC: void __os_errno_set_hook __P((int (*)(int)));
 */
void
__os_errno_set_hook(int (*fn)(int))
{
	atomic_store_explicit(&__errno_hook, (errno_hook_fn)fn,
	    memory_order_relaxed);
}

/*
 * PUBLIC: int (*__os_errno_get_hook __P((void)))(int);
 */
int
(*__os_errno_get_hook(void))(int)
{
	return atomic_load_explicit(&__errno_hook, memory_order_relaxed);
}
