/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/tail_int.h
 *	Internal xtc_tail hook-point primitives.  __xtc_tail_emit records
 *	one event from a runtime hook point; __xtc_tail_on reports whether
 *	a source is enabled (so a hook guards extra work).  These are
 *	library-internal (the __ prefix) -- consumers use the public
 *	xtc_tail_* API (enable + read/dump/count) in xtc_tail.h and never
 *	call these directly.  Split out of xtc_tail.h so no __-prefixed
 *	symbol leaks into an installed public header.
 */

#ifndef XTC_TAIL_INT_H
#define XTC_TAIL_INT_H

#include "xtc.h"

/* Record one event from a runtime hook point (proc spawn/exit, etc.).
 * A no-op fast path (one branch) when `source` is disabled. */
void __xtc_tail_emit(unsigned source, unsigned kind, xtc_pid_t pid,
                     uint64_t detail);

/* 1 if `source` is currently enabled.  A hook point uses this to guard
 * extra work (e.g. a clock read) so a disabled tail costs one branch
 * and has no side effects. */
int  __xtc_tail_on(unsigned source);

#endif /* XTC_TAIL_INT_H */
