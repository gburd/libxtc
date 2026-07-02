/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_preempt.h
 *	Per-worker preemption timer seam.  See docs/M_PREEMPTION.md.
 *
 *	Phase 0 (this): a per-thread CPU-time interval timer whose handler
 *	records a tick.  No preemption yet -- the seam later phases build
 *	on.  OFF by default (nothing arms it unless xtc_preempt_arm is
 *	called); the cooperative fast path is unchanged when unused.
 */

#ifndef XTC_PREEMPT_H
#define XTC_PREEMPT_H

#include <stdint.h>

#include "xtc.h"

/*
 * Arm a per-thread preemption timer that fires every `interval_ns` of
 * THIS thread's CPU time (not wall time), delivering SIGVTALRM to this
 * thread.  Idempotent re-arm re-sets the interval.  Returns XTC_OK, or
 * XTC_E_NOSYS where POSIX per-thread CPU-time timers are unavailable, or
 * XTC_E_INVAL for interval_ns <= 0.  Call from the worker thread it
 * should time.
 *
 * PUBLIC: int      xtc_preempt_arm __P((int64_t));
 * PUBLIC: int      xtc_preempt_disarm __P((void));
 * PUBLIC: int      xtc_preempt_supported __P((void));
 * PUBLIC: uint64_t xtc_preempt_ticks __P((void));
 * PUBLIC: int      xtc_preempt_tick_pending __P((void));
 */
int      xtc_preempt_arm(int64_t interval_ns);

/* Stop + delete this thread's preemption timer.  Safe if not armed. */
int      xtc_preempt_disarm(void);

/* Enable (on != 0) / disable signal-context involuntary yield (Phase
 * 2).  When on and the timer is armed, a tick preempts the running
 * fiber in the handler -- resumably and only when safe (crit_depth ==
 * 0, unsafe_depth == 0) -- on the ucontext substrate; the fctx/winfiber
 * substrate declines and falls back to cooperative-assisted preemption.
 * Off by default.
 *
 * PUBLIC: void xtc_preempt_set_involuntary __P((int));
 */
void     xtc_preempt_set_involuntary(int on);

/* 1 if per-thread CPU-time preemption timers are available on this
 * platform, 0 otherwise (arm returns NOSYS then). */
int      xtc_preempt_supported(void);

/* Total timer ticks this thread has observed since arming (Phase 0
 * seam-works metric / telemetry). */
uint64_t xtc_preempt_ticks(void);

/* 1 if a timer tick fired and is unconsumed; consumes (clears) it.
 * Phase 1 consults this at safe points to decide whether to yield. */
int      xtc_preempt_tick_pending(void);

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
void __xtc_unsafe_enter(void);
void __xtc_unsafe_leave(void);
int  __xtc_unsafe_depth(void);

#endif /* XTC_PREEMPT_H */
