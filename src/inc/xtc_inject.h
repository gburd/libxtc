/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_inject.h
 *	Injection points: named locations in production code that
 *	can be intercepted by tests to drive the runtime to a
 *	specific state and pause/resume execution from outside.
 *	Modeled directly on PostgreSQL's `injection_points.h` /
 *	`src/include/utils/injection_point.h`.
 *
 *	Three operations on a named point at runtime:
 *	  ATTACH   bind a callback (or "wait" semantics) to the name
 *	  DETACH   remove
 *	  TRIGGER  hit the point -- the macro INJECTION_POINT(name) in
 *	           production code calls into this
 *
 *	When TRIGGER fires:
 *	  - if no callback attached, no-op (production fast-path)
 *	  - if a callback is attached, it runs synchronously in the
 *	    triggering proc/thread
 *	  - if "wait" is attached, the trigger blocks on a condition
 *	    variable until inject_wakeup(name) is called from a
 *	    test thread
 *
 *	Compile-time gating: define `XTC_INJECT_DISABLE` to elide
 *	all INJECTION_POINT(...) calls to a no-op.  Default: enabled.
 *	(PG defaults to disabled in non-debug builds.)
 *
 *	Per-name max attached callbacks: 4.  Names are fixed-size
 *	(64 bytes including NUL).
 */

#ifndef XTC_INJECT_H
#define XTC_INJECT_H

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"

#define XTC_INJECT_NAME_MAX  64

typedef void (*xtc_inject_fn)(const char *name, void *user);

/*
 * PUBLIC: int  xtc_inject_attach __P((const char *, xtc_inject_fn, void *));
 * PUBLIC: int  xtc_inject_attach_wait __P((const char *));
 * PUBLIC: int  xtc_inject_detach __P((const char *));
 * PUBLIC: void xtc_inject_trigger __P((const char *));
 * PUBLIC: int  xtc_inject_wakeup __P((const char *));
 * PUBLIC: int  xtc_inject_n_attached __P((void));
 * PUBLIC: int  xtc_inject_check __P((const char *));
 */

/* Attach a callback.  Multiple attaches accumulate (up to 4 per
 * name).  Returns XTC_E_RESOURCE if the slot table is full. */
int  xtc_inject_attach(const char *name, xtc_inject_fn fn, void *user);

/* Attach "wait" semantics: when the point fires, block until
 * xtc_inject_wakeup(name) is called.  Useful for racing tests. */
int  xtc_inject_attach_wait(const char *name);

/* Detach all attachments for a name. */
int  xtc_inject_detach(const char *name);

/* Trigger from production code.  Internal -- usually called via the
 * INJECTION_POINT() macro below, which compiles to a no-op when
 * XTC_INJECT_DISABLE is set. */
void xtc_inject_trigger(const char *name);

/* Release a "wait" attachment so the triggering thread can proceed. */
int  xtc_inject_wakeup(const char *name);

/* Diagnostics: how many names currently have attachments. */
int  xtc_inject_n_attached(void);

/* Lock-free check: 1 if `name` has any attachments, 0 otherwise.
 * Hot-path-friendly: when no inject points are attached anywhere,
 * this returns 0 immediately via an atomic load. */
int  xtc_inject_check(const char *name);

/* The macro production code uses.  In PG-style this is INJECTION_POINT;
 * we use XTC_INJECTION_POINT so caller code reading a stack trace
 * sees the namespace immediately. */
#if defined(XTC_INJECT_DISABLE)
# define XTC_INJECTION_POINT(name)  ((void)0)
#else
# define XTC_INJECTION_POINT(name)  xtc_inject_trigger(name)
#endif

#endif /* XTC_INJECT_H */
