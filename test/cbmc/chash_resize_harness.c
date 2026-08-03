/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/cbmc/chash_resize_harness.c
 *	CBMC bounded model check of the concurrent hash table resize
 *	racing a lookup (src/ptc/chash.c: __chash_resize + xtc_chash_get).
 *
 *	INVARIANT PROVED: across a resize (grow/shrink) racing a lookup,
 *	no key is lost and the table stays single-valued -- a concurrent
 *	get for a key present before the resize always finds it (in the
 *	old array before the pointer swap, or the new array after), and
 *	always returns THE one value bound to that key.  A resize that
 *	dropped or duplicated a key, or a get that saw a half-built new
 *	array, would violate this.
 *
 *	WHAT IS MODELLED: the resize's publish protocol and the lookup's
 *	array-pointer discipline, transcribed faithfully from chash.c --
 *	  get (xtc_chash_get): load the CURRENT array pointer (acquire),
 *	      walk it for the key -- taking no lock (RCU read-side keeps
 *	      the array alive).
 *	  resize (__chash_resize under all stripe locks): build the ENTIRE
 *	      new array off to the side (copying every live key), then ONE
 *	      release store swaps h->arr to it; the old array is retired
 *	      (not freed) so an in-flight reader on it stays valid.
 *	chash.c is far too large to include standalone (RCU epochs, slab,
 *	NSTRIPES locks, open-chained buckets, grow/shrink heuristics), so
 *	this models the ESSENTIAL resize+lookup interleaving: two array
 *	"generations" (old and new) as fixed integer key->value maps, a
 *	published generation index (the h->arr pointer), and the atomic
 *	build-then-swap.  Keys/values are small integers (CBMC reasons
 *	over integers -- sound + fast -- exactly as deque_harness.c does),
 *	sidestepping the concurrently-mutated pointer chains CBMC cannot
 *	soundly reason about.  The safety argument -- readers see either
 *	the complete old array or the complete new array, never a partial
 *	one, because publication is a single release store of a fully-
 *	built array -- is preserved exactly.  If chash.c's build-then-swap
 *	publication drifts, this must be updated in lockstep.
 *
 *	BOUND: a table with 2 keys, one resize (rebuild + swap) racing one
 *	lookup.  CBMC explores every interleaving of the publish store and
 *	the reader's load.
 *
 *	Run: cbmc chash_resize_harness.c --unwind 6
 */

#include <stdint.h>
#include <stdatomic.h>

#define NKEYS 2
#define GEN_OLD 0
#define GEN_NEW 1

/* Two array generations.  gen[g].val[k] is the value bound to key k in
 * generation g (0 == absent).  A generation is "complete" once fully
 * built; resize builds gen NEW completely before publishing it. */
static int gen_val[2][NKEYS];
static _Atomic int gen_complete[2];    /* 1 once the generation is fully built */

/* The published array pointer (h->arr): which generation a reader sees. */
static _Atomic int published_gen;

/* xtc_chash_get: load the current array (acquire), look up the key.
 * Returns the value, or 0 if absent.  Takes no lock. */
static int
chash_get(int key)
{
	int g = atomic_load_explicit(&published_gen, memory_order_acquire);
	/* A reader must NEVER observe a half-built published array: the
	 * generation the pointer names is always fully constructed
	 * (resize publishes only after building it completely). */
	__CPROVER_assert(atomic_load_explicit(&gen_complete[g],
	    memory_order_acquire) == 1,
	    "a lookup never sees a half-built array (publish is one store "
	    "of a fully-built generation)");
	return gen_val[g][key];
}

/* __chash_resize: build the new generation completely (copy every live
 * key from the old one), THEN one release store swaps the published
 * pointer.  Runs under all stripe locks in chash.c, so it is the sole
 * writer -- modelled as an uninterrupted build followed by the atomic
 * publish. */
static void
resize(void)
{
	int k;
	int old_g = atomic_load_explicit(&published_gen, memory_order_relaxed);
	int new_g = 1 - old_g;

	/* Build the new generation off to the side: duplicate every key's
	 * value from the old array (chash.c dups every live node into the
	 * new array).  Mark incomplete until fully built. */
	atomic_store_explicit(&gen_complete[new_g], 0, memory_order_relaxed);
	for (k = 0; k < NKEYS; k++)
		gen_val[new_g][k] = gen_val[old_g][k];
	/* Fully built: mark complete BEFORE publishing (release), so the
	 * reader's acquire-load of published_gen is ordered after this. */
	atomic_store_explicit(&gen_complete[new_g], 1, memory_order_release);

	/* Publish: one release store swaps the array pointer.  A reader
	 * loading it sees either old_g (complete) or new_g (complete),
	 * never a partial array. */
	atomic_store_explicit(&published_gen, new_g, memory_order_release);
}

/* The reader: look up each key concurrently with the resize; every key
 * present before the resize must still be found, with its one value. */
static void
reader(void)
{
	int k;
	for (k = 0; k < NKEYS; k++) {
		int v = chash_get(k);
		/* Key k was inserted with value (k + 1) and the resize only
		 * copies values -- so a lookup that finds the key sees
		 * exactly that value (single-valued), and a key present
		 * before the resize is never lost (v != 0). */
		__CPROVER_assert(v == k + 1,
		    "no key lost and table stays single-valued across resize");
	}
}

int
main(void)
{
	int k;

	/* Seed the OLD generation with NKEYS keys: key k -> value k+1. */
	for (k = 0; k < NKEYS; k++) {
		gen_val[GEN_OLD][k] = k + 1;
		gen_val[GEN_NEW][k] = 0;
	}
	atomic_store_explicit(&gen_complete[GEN_OLD], 1, memory_order_relaxed);
	atomic_store_explicit(&gen_complete[GEN_NEW], 0, memory_order_relaxed);
	atomic_store_explicit(&published_gen, GEN_OLD, memory_order_relaxed);

	/* A resize races a concurrent reader; CBMC explores every
	 * interleaving of the publish store and the reader's loads. */
	__CPROVER_ASYNC_1: resize();
	reader();
	return 0;
}
