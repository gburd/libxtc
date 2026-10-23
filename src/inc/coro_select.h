/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/coro_select.h
 *	Which coroutine substrate this build uses.  Defines XTC_CORO_FCTX
 *	when the hand-written fcontext substrate (coro_fctx.c) is live;
 *	otherwise ucontext (coro_uctx.c) is, or Win32 fibers on Windows.
 *	Every site that depends on the choice (coro_fctx.c, coro_uctx.c,
 *	coro_int.h's struct xtc_coro) tests this one macro, so they cannot
 *	disagree and struct xtc_coro's layout cannot drift between
 *	translation units.  Include after xtc_int.h (xtc_config.h).
 *
 *	fcontext is live when not Windows and:
 *	  - the libc has no ucontext (musl), or
 *	  - forced: -DXTC_CORO_FORCE_FCTX / configure --with-coro=fctx, or
 *	  - by default on Linux x86-64 with glibc, and on Apple arm64.
 *	    ucontext's swapcontext makes a rt_sigprocmask system call on
 *	    every switch (two per round trip), and on Linux that call takes
 *	    the PROCESS-wide sighand lock: with many loops every fiber switch
 *	    in the process serializes on one kernel spinlock.  Measured on a
 *	    64-vCPU x86-64 host: 16 loops of cross-loop request/reply 2.2M
 *	    round trips/s on ucontext (56% of cycles spinning on that lock),
 *	    12.1M on fcontext; one fiber switch 550 -> 145 ns.  fcontext has
 *	    the same involuntary-preemption redirect there (coro_preempt.h).
 *	ucontext is kept by -DXTC_CORO_FORCE_UCONTEXT / --with-coro=ucontext
 *	and in the single-file amalgamation (it cannot carry the .S files).
 */

#ifndef XTC_CORO_SELECT_H
#define XTC_CORO_SELECT_H

#include "xtc_int.h"   /* xtc_config.h: XTC_HAVE_UCONTEXT */

#if !defined(_WIN32) && \
    (!defined(XTC_HAVE_UCONTEXT) || defined(XTC_CORO_FORCE_FCTX) || \
     (!defined(XTC_CORO_FORCE_UCONTEXT) && !defined(XTC_AMALGAMATION) && \
      ((defined(__APPLE__) && defined(__aarch64__)) || \
       (defined(__linux__) && defined(__x86_64__) && defined(__GLIBC__)))))
# define XTC_CORO_FCTX 1
#endif

#endif /* XTC_CORO_SELECT_H */
