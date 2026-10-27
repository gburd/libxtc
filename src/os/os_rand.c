/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/os_rand.c
 *	Per-thread, seedable splitmix64 PRNG.  rand(3)/random(3) share
 *	process-global state and are not thread-safe; a thread-local
 *	state cell gives each thread its own independent stream with no
 *	shared-state race and no cross-thread contention.
 *
 *	splitmix64 is the reference initializer for xoshiro; it is a
 *	single-word state, statistically solid for non-cryptographic use,
 *	and trivially reproducible from a seed -- which is all a runtime
 *	needs for jitter, backoff, and test fixtures.  It is NOT a CSPRNG.
 *
 *	The stream is deterministic from a seed set via __os_rand_seed; an
 *	un-seeded thread auto-seeds on first draw from the monotonic clock
 *	XORed with the address of its own state cell, so distinct threads
 *	get distinct default streams.
 *
 *	Under the simulator a thread that has not called __os_rand_seed
 *	draws from the run's own stream instead (XTC_SIM_RNG_RAND, reset by
 *	every xtc_sim_activate), so its draws are a function of the run's
 *	seed and replay, the way __os_clock_mono is virtualized.  A thread
 *	that seeded itself keeps its own stream: it asked for that sequence.
 *	The simulator runs every loop on one thread, so one stream serves
 *	them all, in schedule order.
 */

#include "xtc_int.h"

#include "os_sharp.h"
#include "os_thread.h"   /* XTC_THREAD_LOCAL */
#include "os_time.h"     /* __os_clock_mono */
/* __xtc_sim_active, __xtc_sim_rng and the stream id; sim.c is always
 * linked into libxtc. */
#include "xtc_sim.h"

/* Per-thread state: RNG_UNSEEDED auto-seeds on the next draw off the
 * simulator; RNG_AUTO was seeded from the clock; RNG_EXPLICIT by
 * __os_rand_seed. */
enum { RNG_UNSEEDED = 0, RNG_AUTO, RNG_EXPLICIT };
static XTC_THREAD_LOCAL uint64_t __rng_state;
static XTC_THREAD_LOCAL int      __rng_seeded;

/* splitmix64 -- one 64-bit output per step, advancing the state.
 * Named __os_splitmix64 (not the bare splitmix64 in src/evt/sim.c) so
 * the two static definitions do not collide in the single-TU
 * amalgamation build. */
static uint64_t
__os_splitmix64(uint64_t *s)
{
	uint64_t z;

	*s += 0x9e3779b97f4a7c15ULL;
	z = *s;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

/*
 * PUBLIC: void __os_rand_seed __P((uint64_t));
 */
void
__os_rand_seed(uint64_t seed)
{
	__rng_state = seed;
	__rng_seeded = RNG_EXPLICIT;
}

/*
 * PUBLIC: uint64_t __os_rand_u64 __P((void));
 */
uint64_t
__os_rand_u64(void)
{
	/* Under the simulator an un-seeded (or clock-seeded: say, before
	 * the run started) thread draws from the run's stream; the clock
	 * and the ASLR-dependent address below would not replay. */
	if (__rng_seeded != RNG_EXPLICIT && __xtc_sim_active())
		return __xtc_sim_rng(XTC_SIM_RNG_RAND);
	if (__rng_seeded == RNG_UNSEEDED) {
		int64_t ns = 0;
		(void)__os_clock_mono(&ns);
		/* Mix the clock with this thread's own state address so two
		 * threads seeding in the same nanosecond still diverge. */
		__rng_state = (uint64_t)ns ^ (uint64_t)(uintptr_t)&__rng_state;
		__rng_seeded = RNG_AUTO;
	}
	return __os_splitmix64(&__rng_state);
}
