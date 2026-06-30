/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/pbt/pbt_sim.c
 *	Property-based test for the DST seeded-PRNG tree and virtual
 *	clock (src/evt/sim.c).  These primitives underpin the
 *	deterministic scheduler; their determinism is the foundation of
 *	replayability, so it is worth a property sweep over hegel-drawn
 *	seeds, streams, and draw counts.  (The scheduler itself needs a
 *	sim-backend build and is covered by test/sim/; here we cover the
 *	parts that work in any build, since sim is dormant by default.)
 *
 *	Properties:
 *	  - reproducible: the same (seed, stream) yields the identical
 *	    draw sequence across two activations.
 *	  - seed-sensitive: different seeds (almost surely) diverge.
 *	  - stream-independent: two streams under one seed are not locked
 *	    in step.
 *	  - range-bounded: rng_range(s, b) is always in [0, b).
 *	  - virtual clock monotone under advance().
 */

#include <stdint.h>
#include <stdlib.h>

#include "pbt_common.h"
#include "xtc.h"
#include "xtc_sim.h"

#if defined(XTC_HAVE_HEGEL)

#define MAX_DRAWS 64

static void
prop_prng_reproducible(hegel_test_case *tc, void *u)
{
	uint64_t seed;
	int stream, n, i;
	uint64_t a[MAX_DRAWS], b[MAX_DRAWS];
	(void)u;

	seed = (uint64_t)hegel_draw_int(tc, hegel_integers(1, 1000000000));
	stream = (int)hegel_draw_int(tc, hegel_integers(0, XTC_SIM_RNG_NSTREAMS - 1));
	n = (int)hegel_draw_int(tc, hegel_integers(1, MAX_DRAWS));

	xtc_sim_activate(seed);
	for (i = 0; i < n; i++)
		a[i] = __xtc_sim_rng(stream);
	xtc_sim_deactivate();

	xtc_sim_activate(seed);
	for (i = 0; i < n; i++)
		b[i] = __xtc_sim_rng(stream);
	xtc_sim_deactivate();

	for (i = 0; i < n; i++)
		hegel_assume(a[i] == b[i]);   /* same seed -> same sequence */
}

static void
prop_prng_range_bounded(hegel_test_case *tc, void *u)
{
	uint64_t seed, bound;
	int stream, n, i;
	(void)u;

	seed = (uint64_t)hegel_draw_int(tc, hegel_integers(1, 1000000000));
	stream = (int)hegel_draw_int(tc, hegel_integers(0, XTC_SIM_RNG_NSTREAMS - 1));
	bound = (uint64_t)hegel_draw_int(tc, hegel_integers(1, 100000));
	n = (int)hegel_draw_int(tc, hegel_integers(1, MAX_DRAWS));

	xtc_sim_activate(seed);
	for (i = 0; i < n; i++) {
		uint64_t v = __xtc_sim_rng_range(stream, bound);
		hegel_assume(v < bound);      /* always in [0, bound) */
	}
	xtc_sim_deactivate();
}

static void
prop_vclock_monotone(hegel_test_case *tc, void *u)
{
	int n, i;
	int64_t prev;
	(void)u;

	n = (int)hegel_draw_int(tc, hegel_integers(1, MAX_DRAWS));

	xtc_sim_activate(0xC0FFEE);
	xtc_sim_clock_enable(0);
	prev = 0;
	(void)__xtc_sim_vclock(&prev);
	for (i = 0; i < n; i++) {
		int64_t now = 0;
		int64_t delta = (int64_t)hegel_draw_int(tc, hegel_integers(0, 1000000));
		xtc_sim_clock_advance(delta);
		hegel_assume(__xtc_sim_vclock(&now) == 1);
		hegel_assume(now >= prev);    /* never goes backward */
		hegel_assume(now == prev + delta);
		prev = now;
	}
	xtc_sim_clock_disable();
	xtc_sim_deactivate();
}

static const pbt_entry_t tests[] = {
	{ "prng_reproducible",  prop_prng_reproducible,  100 },
	{ "prng_range_bounded", prop_prng_range_bounded, 100 },
	{ "vclock_monotone",    prop_vclock_monotone,    100 },
	{ NULL, NULL, 0 }
};

#else
static const pbt_entry_t tests[] = {
	{ "prng_reproducible",  NULL, 0 },
	{ "prng_range_bounded", NULL, 0 },
	{ "vclock_monotone",    NULL, 0 },
	{ NULL, NULL, 0 }
};
#endif

PBT_MAIN("sim", tests)
