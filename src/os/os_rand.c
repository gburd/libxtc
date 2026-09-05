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
 *	Future: no DST hook here yet.  When the deterministic-simulation
 *	clock (src/evt/sim.c) is active, a replayed run should draw a
 *	reproducible sequence; wiring that seam is a separate task.  For
 *	now this is purely a clean thread-safe entropy source.
 */

#include "xtc_int.h"

#include "os_sharp.h"
#include "os_thread.h"   /* XTC_THREAD_LOCAL */
#include "os_time.h"     /* __os_clock_mono */

/* The DST determinism guard.  A no-op in production (a single relaxed
 * atomic load); under a simulated run it records the violation and makes
 * xtc_sim_exec_run refuse XTC_OK, so a nondeterministic primitive on a
 * sim-reachable path cannot silently break seed replay.  sim.c is always
 * linked into libxtc, so the symbol always resolves. */
void __xtc_sim_nondeterminism(const char *what);

/* Per-thread state.  seeded==0 means "auto-seed on next draw". */
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
	__rng_seeded = 1;
}

/*
 * PUBLIC: uint64_t __os_rand_u64 __P((void));
 */
uint64_t
__os_rand_u64(void)
{
	if (!__rng_seeded) {
		int64_t ns = 0;
		/*
		 * DETERMINISM GUARD: an auto-seeded stream is not replayable.
		 * The seed below mixes the clock with the ADDRESS of this
		 * thread's state cell, which is ASLR-dependent, so even a
		 * virtual sim clock cannot make it reproducible.  A
		 * sim-reachable draw must come from a stream the seed owns
		 * (xtc_sim_rng_*) or from an explicitly __os_rand_seed'ed
		 * generator.  Reported before the seeding so the trap names
		 * the real cause rather than the clock read underneath it.
		 */
		__xtc_sim_nondeterminism("unseeded RNG (__os_rand_u64 auto-seed)");
		(void)__os_clock_mono(&ns);
		/* Mix the clock with this thread's own state address so two
		 * threads seeding in the same nanosecond still diverge. */
		__rng_state = (uint64_t)ns ^ (uint64_t)(uintptr_t)&__rng_state;
		__rng_seeded = 1;
	}
	return __os_splitmix64(&__rng_state);
}
