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
 *
 *	The M0 library also links os_time.c (for xtc_clock_mono/_real/
 *	_sleep_ns), which references the sim hooks defined in src/evt/sim.c
 *	-- absent from M0.  With no simulation active in the minimal build,
 *	the virtual-clock hooks report "inactive" (so the real host clock is
 *	used) and the determinism guard never fires, so no-ops are correct
 *	here too.
 */

#include <stdint.h>

void __xtc_unsafe_enter(void);
void __xtc_unsafe_leave(void);
int  __xtc_sim_vclock(int64_t *out_ns);
int  __xtc_sim_vclock_observed(int64_t *out_ns);
int  __xtc_sim_active(void);
void __xtc_sim_nondeterminism(const char *what);

void __xtc_unsafe_enter(void) { }
void __xtc_unsafe_leave(void) { }

/* No sim in the M0 build: the virtual clock is never active (return 0 so
 * os_time.c falls through to the real host clock), and the determinism
 * guard is a no-op. */
int  __xtc_sim_vclock(int64_t *out_ns) { (void)out_ns; return 0; }
int  __xtc_sim_vclock_observed(int64_t *out_ns) { (void)out_ns; return 0; }
int  __xtc_sim_active(void) { return 0; }
void __xtc_sim_nondeterminism(const char *what) { (void)what; }
