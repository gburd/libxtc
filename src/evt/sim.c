/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/evt/sim.c
 *	Deterministic Simulation Testing (DST) core.  See docs/M_DST.md.
 *
 *	Phase 0: the seeded PRNG tree.  Later phases add the virtual clock
 *	and the single-thread deterministic scheduler that drives the N
 *	loops as N fibers under a seed-determined interleaving.
 *
 *	The PRNG is a per-stream splitmix64: the root seed mixes with the
 *	stream id to give each stream an independent, well-distributed
 *	sequence, so a draw added at one decision site never perturbs the
 *	sequence another site observes (stable replay under code change).
 */

#include "xtc_int.h"
#include "xtc_sim.h"

#include <stdatomic.h>

/* Activation state is process-global and read on hot paths, so it is a
 * relaxed atomic flag rather than a lock. */
static _Atomic int      g_sim_active;
static uint64_t         g_sim_seed;
static uint64_t         g_sim_stream[XTC_SIM_RNG_NSTREAMS];

/* Virtual (logical) clock.  When sim is active and the clock mode is
 * VIRTUAL, __os_clock_mono returns this instead of the host monotonic
 * clock, so time is a pure function of the schedule (hence the seed).
 * The scheduler advances it (later phase); phase 0/1 just establish the
 * seam and let a test drive it manually. */
static _Atomic int      g_vclock_on;
static _Atomic int64_t  g_vclock_ns;

/* splitmix64: the standard finalizer used to derive independent streams
 * from a seed (the 0x9E3779B97F4A7C15 increment is the golden-ratio
 * constant already used elsewhere in the tree). */
static uint64_t
splitmix64(uint64_t *s)
{
	uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

/* PUBLIC: int __xtc_sim_active __P((void)); */
int
__xtc_sim_active(void)
{
	return atomic_load_explicit(&g_sim_active, memory_order_relaxed);
}

/* PUBLIC: void xtc_sim_activate __P((uint64_t)); */
void
xtc_sim_activate(uint64_t seed)
{
	int i;
	uint64_t s = seed ? seed : 0x9E3779B97F4A7C15ull;
	g_sim_seed = s;
	/* Derive each stream's initial state from the root seed so the
	 * streams are independent yet fully determined by the seed. */
	for (i = 0; i < XTC_SIM_RNG_NSTREAMS; i++) {
		uint64_t t = s + (uint64_t)(i + 1) * 0x9E3779B97F4A7C15ull;
		g_sim_stream[i] = splitmix64(&t);
	}
	atomic_store_explicit(&g_sim_active, 1, memory_order_release);
}

/* PUBLIC: void xtc_sim_deactivate __P((void)); */
void
xtc_sim_deactivate(void)
{
	atomic_store_explicit(&g_sim_active, 0, memory_order_release);
}

/* PUBLIC: uint64_t __xtc_sim_rng __P((int)); */
uint64_t
__xtc_sim_rng(int s)
{
	if (s < 0 || s >= XTC_SIM_RNG_NSTREAMS)
		s = XTC_SIM_RNG_APP;
	return splitmix64(&g_sim_stream[s]);
}

/* PUBLIC: uint64_t __xtc_sim_rng_range __P((int, uint64_t)); */
uint64_t
__xtc_sim_rng_range(int s, uint64_t bound)
{
	if (bound == 0)
		return 0;
	/* Unbiased enough for scheduling decisions; the modulo bias over a
	 * 64-bit draw with a small bound is negligible and -- crucially --
	 * deterministic. */
	return __xtc_sim_rng(s) % bound;
}

/* ---- virtual clock ---- */

/* PUBLIC: void xtc_sim_clock_enable __P((int64_t)); */
void
xtc_sim_clock_enable(int64_t start_ns)
{
	atomic_store_explicit(&g_vclock_ns, start_ns, memory_order_relaxed);
	atomic_store_explicit(&g_vclock_on, 1, memory_order_release);
}

/* PUBLIC: void xtc_sim_clock_disable __P((void)); */
void
xtc_sim_clock_disable(void)
{
	atomic_store_explicit(&g_vclock_on, 0, memory_order_release);
}

/* PUBLIC: void xtc_sim_clock_advance __P((int64_t)); */
void
xtc_sim_clock_advance(int64_t delta_ns)
{
	if (delta_ns > 0)
		(void)atomic_fetch_add_explicit(&g_vclock_ns, delta_ns,
		    memory_order_relaxed);
}

/* PUBLIC: void xtc_sim_clock_set __P((int64_t)); */
void
xtc_sim_clock_set(int64_t ns)
{
	atomic_store_explicit(&g_vclock_ns, ns, memory_order_relaxed);
}

/*
 * Query the virtual clock.  Returns 1 and writes *out_ns when the
 * virtual clock is active; returns 0 otherwise (caller uses the host
 * clock).  __os_clock_mono consults this -- the single seam point.
 *
 * PUBLIC: int __xtc_sim_vclock __P((int64_t *));
 */
int
__xtc_sim_vclock(int64_t *out_ns)
{
	if (!atomic_load_explicit(&g_vclock_on, memory_order_acquire))
		return 0;
	if (out_ns != NULL)
		*out_ns = atomic_load_explicit(&g_vclock_ns,
		    memory_order_relaxed);
	return 1;
}
