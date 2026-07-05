/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/os/os_unsafe_stub.c
 *	No-op fallbacks for the async-signal-unsafe-region bracket
 *	(__xtc_unsafe_enter / __xtc_unsafe_leave).
 *
 *	These are normally provided by src/ptc/preempt.c, which the full
 *	autoconf and amalgamation builds always link.  The MINIMAL meson
 *	M0 library, however, compiles only a tiny subset (xtc_version.c,
 *	xtc_strerror.c, os_alloc.c) and does NOT link preempt.c -- yet
 *	os_alloc.c calls these to bracket every allocation, so the M0
 *	library needs a definition to link standalone.  This file supplies
 *	the no-ops for exactly that case.
 *
 *	It is compiled ONLY by the meson build (see meson.build); the
 *	autoconf and amalgamation builds must NOT include it, or the
 *	strong preempt.c definitions would collide (a duplicate symbol in
 *	the amalgamation's single translation unit).  With preemption
 *	absent there is nothing to defer across the allocator window, so a
 *	no-op is exactly right.
 */

void __xtc_unsafe_enter(void);
void __xtc_unsafe_leave(void);

void __xtc_unsafe_enter(void) { }
void __xtc_unsafe_leave(void) { }
