/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/preempt_int.h
 *	Internal preemption primitives -- the __xtc_* async-signal-unsafe
 *	bracket and the preemption-safe raw-pthread mutex wrappers.
 *
 *	These are library-internal (the __ prefix): they are used across
 *	the runtime (the allocator, latches, every subsystem embedding a
 *	bare pthread_mutex_t) but are NOT part of the consumer-facing API,
 *	so they live here rather than in the installed xtc_preempt.h.  A
 *	library source that needs them includes this header; a consumer
 *	never sees them.  (Split out of xtc_preempt.h so no __-prefixed
 *	symbol leaks into an installed public header.)
 */

#ifndef XTC_PREEMPT_INT_H
#define XTC_PREEMPT_INT_H

#include "xtc_export.h"

#include <pthread.h>

/*
 * Async-signal-unsafe-region depth (Phase 2 prerequisite).  A per-thread
 * nesting counter that is > 0 while the thread is inside an
 * async-signal-unsafe region (the allocator, a latch's internal lock).
 * The preemption timer handler must not do a signal-context involuntary
 * yield while it is > 0 (it defers).  __xtc_unsafe_enter/leave bracket
 * such a region; __xtc_unsafe_depth reads the current depth (also used
 * by the fault handler so a SIGSEGV inside malloc does not unwind out of
 * a corrupt arena).
 *
 * PUBLIC: void __xtc_unsafe_enter __P((void));
 * PUBLIC: void __xtc_unsafe_leave __P((void));
 * PUBLIC: int  __xtc_unsafe_depth __P((void));
 */
XTC_API void __xtc_unsafe_enter(void);
XTC_API void __xtc_unsafe_leave(void);
XTC_API int  __xtc_unsafe_depth(void);

/*
 * Preemption-safe raw-pthread mutex brackets.  A fiber that holds a
 * mutex must not be involuntarily preempted (a loop runs many fibers on
 * one OS thread; a preempted holder plus another same-loop fiber
 * blocking on the same mutex deadlocks the thread).  __xtc_mtx_lock/
 * unlock wrap pthread_mutex_lock/unlock with __xtc_unsafe_enter/leave
 * so the preemption timer defers while the lock is held -- the
 * raw-pthread counterpart of the preemption-safe __os_mutex_* locks,
 * for internal subsystems that embed a bare pthread_mutex_t.  Use only
 * for short critical sections that do NOT yield/park while holding the
 * lock.  They return the raw pthread errno (0 == success).
 */
int __xtc_mtx_lock(pthread_mutex_t *m);
int __xtc_mtx_unlock(pthread_mutex_t *m);

#endif /* XTC_PREEMPT_INT_H */
