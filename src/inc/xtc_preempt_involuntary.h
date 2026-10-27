/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * @stability experimental
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_preempt_involuntary.h
 *	Involuntary (signal-context) preemption: a timer tick that lands
 *	in a running fiber yields it to the scheduler at the interrupted
 *	instruction, so a loop-mate runs even while the fiber never reaches
 *	a yield point.  EXPERIMENTAL (docs/abi-stability.md): the contract
 *	below may change in any minor release.  The timer itself, and
 *	cooperative-assisted preemption, are in "xtc_preempt.h" (stable).
 *
 *	THE CONTRACT (xtc_preempt_involuntary(3)):
 *	  - A tick yields the fiber only at a SAFE POINT: outside a
 *	    critical section and libxtc's own internal regions, and at a PC
 *	    inside a PREEMPTIBLE range.  By default that is the main
 *	    executable's own code (when libc is a shared object), plus every
 *	    function marked XTC_PREEMPTIBLE, plus ranges added with
 *	    xtc_preempt_region_add.  Never libc, other shared libraries, or
 *	    libxtc itself: code compiled for a thread (malloc's per-thread
 *	    cache, a FILE's per-thread lock) is not safe to interleave with a
 *	    loop-mate on the same thread.  Elsewhere the tick stays pending
 *	    and the fiber yields at its next yield point.
 *	  - The interrupted code resumes with its whole register file,
 *	    flags, vector state, x87 state, PKRU and errno.  The scheduler
 *	    and the fibers it runs in the slice start from a call-boundary
 *	    state (DF/AC clear, empty x87 stack, clean AVX upper halves; SME
 *	    streaming mode and ZA off on aarch64).
 *	  - Code in a preemptible range must not hold per-thread state a
 *	    loop-mate could disturb (a thread-local scratch buffer, a lock
 *	    that a loop-mate also takes) across instructions without a
 *	    critical section (xtc_proc_critical_enter / _leave).
 *	  - Linux x86-64 (ucontext; fcontext with glibc) and Linux aarch64
 *	    only: xtc_preempt_involuntary_supported() says which.  CET
 *	    shadow stacks, AMX tile state and the aarch64 guarded control
 *	    stack are not supported.
 */

#ifndef XTC_PREEMPT_INVOLUNTARY_H
#define XTC_PREEMPT_INVOLUNTARY_H

#include "xtc_export.h"

#include "xtc.h"

/*
 * Mark a function preemptible: the build places it in the
 * xtc_preemptible section, which the safe-point check trusts even in a
 * static executable (where the main text holds libc and is not
 * trusted).  A marked function must follow the contract above; it may
 * call anything, but a tick that lands in a callee outside a preemptible
 * range is deferred.  ELF with GCC or Clang; empty elsewhere.
 */
#if defined(__ELF__) && (defined(__GNUC__) || defined(__clang__))
# define XTC_PREEMPTIBLE \
	__attribute__((section("xtc_preemptible"), noinline))
#else
# define XTC_PREEMPTIBLE
#endif

/*
 * PUBLIC: int xtc_preempt_set_involuntary __P((int));
 * PUBLIC: int xtc_preempt_involuntary_supported __P((void));
 * PUBLIC: int xtc_preempt_region_add __P((const void *, const void *));
 */

/* Enable (on != 0) or disable involuntary preemption, process-wide.  It
 * acts on threads whose preemption timer is armed (xtc_preempt_arm,
 * xtc_exec_set_preempt).  Off by default.  Returns XTC_OK; enabling
 * returns XTC_E_NOSYS, and leaves it off, where
 * xtc_preempt_involuntary_supported() is 0 or no preemptible range can
 * be trusted (a static executable with no XTC_PREEMPTIBLE function and
 * no region added; adding one later lifts that refusal); the reason is
 * reported once, at XTC_LOG_ERROR and on stderr. */
XTC_API int xtc_preempt_set_involuntary(int on);

/* 1 if this build and platform implement the involuntary redirect
 * (Linux x86-64 on ucontext or glibc fcontext; Linux aarch64), else 0. */
XTC_API int xtc_preempt_involuntary_supported(void);

/* Add [start, end) to the preemptible ranges (code the application
 * vouches for, e.g. a JIT buffer or a static build's own objects).  Call
 * before enabling.  XTC_E_INVAL for an empty range or one overlapping
 * libxtc's own text; XTC_E_RESOURCE when the table (32 ranges) is full.
 * Ranges cannot be removed. */
XTC_API int xtc_preempt_region_add(const void *start, const void *end);

#endif /* XTC_PREEMPT_INVOLUNTARY_H */
