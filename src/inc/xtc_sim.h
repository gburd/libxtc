/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_sim.h
 *	Deterministic Simulation Testing (DST) seams.  See docs/M_DST.md.
 *
 *	Phase 0 is the seeded PRNG tree: a single root seed splits into
 *	per-stream sub-streams so a draw at one decision site (steal
 *	victim, placement, lock victim, fault toggle) does not perturb
 *	another site's sequence -- the FoundationDB discipline that keeps
 *	a replay stable when a new draw site is added.
 *
 *	When sim is INACTIVE (the production default) __xtc_sim_active()
 *	is 0 and the runtime's existing ad-hoc randomness is used
 *	unchanged; the call sites consult __xtc_sim_active() and only draw
 *	from the seeded tree under sim.  This module compiles into every
 *	build but is dormant unless a sim run activates it.
 */

#ifndef XTC_SIM_H
#define XTC_SIM_H

#include <stdint.h>

/* Well-known PRNG streams.  Each scheduler-relevant random draw site
 * owns a stream so adding a draw in one site cannot shift another. */
enum xtc_sim_stream {
	XTC_SIM_RNG_SCHED   = 0,   /* which runnable loop advances next */
	XTC_SIM_RNG_STEAL   = 1,   /* work-stealing victim selection */
	XTC_SIM_RNG_PLACE   = 2,   /* round-robin proc/task placement */
	XTC_SIM_RNG_LOCKVIC = 3,   /* lock-manager deadlock victim */
	XTC_SIM_RNG_IO      = 4,   /* simulated I/O latency / completion order */
	XTC_SIM_RNG_FAULT   = 5,   /* fault-injection toggles */
	XTC_SIM_RNG_APP     = 6,   /* application/test draws */
	XTC_SIM_RNG_NSTREAMS
};

/*
 * 1 while a deterministic simulation run is active (set by
 * xtc_sim_activate, cleared by xtc_sim_deactivate).  Hot-path call
 * sites branch on this to decide between seeded and ad-hoc randomness.
 *
 * PUBLIC: int      __xtc_sim_active __P((void));
 * PUBLIC: void     xtc_sim_activate __P((uint64_t));
 * PUBLIC: void     xtc_sim_deactivate __P((void));
 * PUBLIC: uint64_t __xtc_sim_rng __P((int));
 * PUBLIC: uint64_t __xtc_sim_rng_range __P((int, uint64_t));
 * PUBLIC: void     xtc_sim_clock_enable __P((int64_t));
 * PUBLIC: void     xtc_sim_clock_disable __P((void));
 * PUBLIC: void     xtc_sim_clock_advance __P((int64_t));
 * PUBLIC: void     xtc_sim_clock_set __P((int64_t));
 * PUBLIC: int      __xtc_sim_vclock __P((int64_t *));
 */
int      __xtc_sim_active(void);

/* Activate sim with a root seed (also resets every stream).  Idempotent
 * re-activation re-seeds.  Test-only; never called in production. */
void     xtc_sim_activate(uint64_t seed);
void     xtc_sim_deactivate(void);

/* Draw the next 64-bit value from stream `s` (0..XTC_SIM_RNG_NSTREAMS-1).
 * Deterministic given the activation seed.  Undefined if sim inactive
 * (callers must gate on __xtc_sim_active()). */
uint64_t __xtc_sim_rng(int s);

/* Convenience: a uniform value in [0, bound) from stream `s`.  bound==0
 * returns 0. */
uint64_t __xtc_sim_rng_range(int s, uint64_t bound);

/* Virtual (logical) clock.  When enabled, __os_clock_mono returns the
 * virtual time instead of the host monotonic clock, so time is a pure
 * function of the schedule.  Test/scheduler-only. */
void     xtc_sim_clock_enable(int64_t start_ns);
void     xtc_sim_clock_disable(void);
void     xtc_sim_clock_advance(int64_t delta_ns);
void     xtc_sim_clock_set(int64_t ns);

/* Query the virtual clock: returns 1 and writes *out_ns when active,
 * 0 otherwise.  The single seam __os_clock_mono consults. */
int      __xtc_sim_vclock(int64_t *out_ns);

/*
 * Run an executor's loops deterministically (DST scheduler).  Activates
 * sim with `seed` + the virtual clock, then drives the N loops as N
 * cooperatively-scheduled entities on the calling thread under a
 * seed-determined interleaving, until quiescence, the step budget
 * (max_steps; <= 0 = unbounded), or xtc_exec_stop.  Requires a sim
 * build (--with-io-backend=sim).  Returns XTC_OK on quiescence,
 * XTC_E_AGAIN if the budget was hit with work remaining, or a negative
 * code on a loop-step error.  Declared opaquely (xtc_exec is defined in
 * xtc_exec.h).
 *
 * PUBLIC: int xtc_sim_exec_run __P((struct xtc_exec *, uint64_t, long));
 */
struct xtc_exec;
int      xtc_sim_exec_run(struct xtc_exec *e, uint64_t seed, long max_steps);

#endif /* XTC_SIM_H */
