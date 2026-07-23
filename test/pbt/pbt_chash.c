/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/pbt/pbt_chash.c
 *	Property-based tests for xtc_chash (src/ptc/chash.c) -- the
 *	RCU-protected concurrent hash table.
 *
 *	These are SEQUENTIAL model-equivalence properties: a random
 *	sequence of insert/remove/get operations must behave exactly
 *	like a simple reference map (an array keyed by int).  The
 *	concurrent / RCU-reclamation UAF story is covered separately by
 *	the DST test (test/sim/test_sim_chash.c); here we pin down the
 *	single-threaded contract that DST assumes.
 *
 *	Properties:
 *	  C1: model equivalence -- after any op sequence, get(k) agrees
 *	      with the reference (present value or NOTFOUND) for every k,
 *	      and size() equals the reference's live count.
 *	  C2: insert of an existing key overwrites and returns the old
 *	      value via *old, and does NOT change size.
 *	  C3: remove returns the stored value via *removed then get
 *	      misses; remove of an absent key returns NOTFOUND and yields
 *	      no value.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pbt_common.h"
#include "xtc.h"
#include "xtc_rcu.h"
#include "xtc_chash.h"

#if defined(XTC_HAVE_HEGEL)

/*
 * Keys are small ints in [0, KEYSPACE); the reference model is a flat
 * array indexed by key.  Keys and values are stable heap boxes so the
 * chash (which stores caller pointers, never copies) has valid storage
 * for the life of the test; we free them all at teardown.
 */
#define KEYSPACE 32

struct box { int64_t k; int64_t v; };

static int
box_cmp(const void *a, const void *b)
{
	int64_t x = ((const struct box *)a)->k;
	int64_t y = ((const struct box *)b)->k;
	return x < y ? -1 : (x > y ? 1 : 0);
}

static uint64_t
box_hash(const void *key)
{
	uint64_t x = (uint64_t)((const struct box *)key)->k;
	x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
	x ^= x >> 33;
	return x;
}

/* One live value box per key, or NULL if absent -- the reference map. */
struct refmap {
	struct box *slot[KEYSPACE];   /* current value box for key, or NULL */
	int         count;            /* number of non-NULL slots */
};

static int
chash_get_i(xtc_chash_t *h, int64_t k, struct box **out)
{
	struct box lookup;
	void *v = NULL;
	int rc;
	lookup.k = k;
	xtc_rcu_read_lock();
	rc = xtc_chash_get(h, &lookup, &v);
	xtc_rcu_read_unlock();
	if (out != NULL) *out = (struct box *)v;
	return rc;
}

/* ----- C1: model equivalence over a random op sequence ------- */

static void
prop_model_equivalence(hegel_test_case *tc, void *u)
{
	xtc_chash_t *h = NULL;
	struct refmap ref;
	struct box *allocated[512];   /* every box we malloc, for teardown */
	int n_alloc = 0;
	int nops, i, k;
	(void)u;

	memset(&ref, 0, sizeof ref);
	hegel_assume(xtc_rcu_init() == XTC_OK);
	hegel_assume(xtc_chash_create(box_cmp, box_hash, 4, &h) == XTC_OK);
	/* Auto-shrink ON so a random insert/remove sequence exercises
	 * BOTH resize directions (grow past 0.75, shrink below 0.1875)
	 * while the model-equivalence invariant below must still hold --
	 * a resize in either direction must never lose, duplicate, or
	 * corrupt an entry. */
	xtc_chash_set_auto_shrink(h, 1);

	nops = (int)hegel_draw_int(tc, hegel_integers(1, 200));
	for (i = 0; i < nops; i++) {
		int op = (int)hegel_draw_int(tc, hegel_integers(0, 1));
		int key = (int)hegel_draw_int(tc, hegel_integers(0,
		    KEYSPACE - 1));
		if (op == 0 && n_alloc < 512) {
			/* insert key -> a fresh value box */
			struct box *nb = malloc(sizeof *nb);
			void *old = NULL;
			int rc;
			hegel_assume(nb != NULL);
			nb->k = key;
			nb->v = (int64_t)i * KEYSPACE + key;
			allocated[n_alloc++] = nb;
			rc = xtc_chash_insert(h, nb, nb, &old);
			hegel_assume(rc == XTC_OK);
			if (ref.slot[key] == NULL)
				ref.count++;
			else
				/* overwrite: chash returns the old value */
				hegel_assume(old == ref.slot[key]);
			ref.slot[key] = nb;
		} else {
			/* remove key */
			struct box lookup;
			void *removed = NULL;
			int rc;
			lookup.k = key;
			rc = xtc_chash_remove(h, &lookup, &removed);
			if (ref.slot[key] != NULL) {
				hegel_assume(rc == XTC_OK);
				hegel_assume(removed == ref.slot[key]);
				ref.slot[key] = NULL;
				ref.count--;
			} else {
				hegel_assume(rc == XTC_E_NOTFOUND);
				hegel_assume(removed == NULL);
			}
		}
	}

	/* Full model check: every key in the keyspace agrees, and the
	 * table's size equals the reference live count. */
	for (k = 0; k < KEYSPACE; k++) {
		struct box *got = NULL;
		int rc = chash_get_i(h, k, &got);
		if (ref.slot[k] != NULL) {
			hegel_assume(rc == XTC_OK);
			hegel_assume(got == ref.slot[k]);
		} else {
			hegel_assume(rc == XTC_E_NOTFOUND);
		}
	}
	hegel_assume(xtc_chash_size(h) == (size_t)ref.count);

	xtc_chash_destroy(h);
	xtc_rcu_synchronize();
	for (i = 0; i < n_alloc; i++)
		free(allocated[i]);
	xtc_rcu_fini();
}

/* ----- C2: overwrite returns old value, size unchanged ------- */

static void
prop_overwrite(hegel_test_case *tc, void *u)
{
	xtc_chash_t *h = NULL;
	struct box *b1, *b2;
	void *old = NULL;
	int64_t key;
	(void)u;

	hegel_assume(xtc_rcu_init() == XTC_OK);
	hegel_assume(xtc_chash_create(box_cmp, box_hash, 4, &h) == XTC_OK);
	key = hegel_draw_int(tc, hegel_integers(0, KEYSPACE - 1));

	b1 = malloc(sizeof *b1); hegel_assume(b1 != NULL); b1->k = key; b1->v = 1;
	b2 = malloc(sizeof *b2); hegel_assume(b2 != NULL); b2->k = key; b2->v = 2;

	hegel_assume(xtc_chash_insert(h, b1, b1, &old) == XTC_OK);
	hegel_assume(old == NULL);                       /* fresh: no prior */
	hegel_assume(xtc_chash_size(h) == 1);

	old = NULL;
	hegel_assume(xtc_chash_insert(h, b2, b2, &old) == XTC_OK);
	hegel_assume(old == b1);                         /* returns old value */
	hegel_assume(xtc_chash_size(h) == 1);            /* size unchanged */

	{
		struct box *got = NULL;
		hegel_assume(chash_get_i(h, key, &got) == XTC_OK);
		hegel_assume(got == b2);                     /* newest wins */
	}

	xtc_chash_destroy(h);
	xtc_rcu_synchronize();
	free(b1); free(b2);
	xtc_rcu_fini();
}

/* ----- C3: remove semantics (present + absent) --------------- */

static void
prop_remove(hegel_test_case *tc, void *u)
{
	xtc_chash_t *h = NULL;
	struct box *b;
	void *removed = NULL;
	struct box lookup;
	int64_t key, absent;
	(void)u;

	hegel_assume(xtc_rcu_init() == XTC_OK);
	hegel_assume(xtc_chash_create(box_cmp, box_hash, 4, &h) == XTC_OK);
	key = hegel_draw_int(tc, hegel_integers(0, KEYSPACE / 2 - 1));
	absent = hegel_draw_int(tc, hegel_integers(KEYSPACE / 2, KEYSPACE - 1));

	b = malloc(sizeof *b); hegel_assume(b != NULL); b->k = key; b->v = 7;
	hegel_assume(xtc_chash_insert(h, b, b, NULL) == XTC_OK);

	/* remove absent -> NOTFOUND, no value, size unchanged */
	lookup.k = absent;
	hegel_assume(xtc_chash_remove(h, &lookup, &removed) == XTC_E_NOTFOUND);
	hegel_assume(removed == NULL);
	hegel_assume(xtc_chash_size(h) == 1);

	/* remove present -> value returned, then get misses, size 0 */
	removed = NULL;
	lookup.k = key;
	hegel_assume(xtc_chash_remove(h, &lookup, &removed) == XTC_OK);
	hegel_assume(removed == b);
	hegel_assume(chash_get_i(h, key, NULL) == XTC_E_NOTFOUND);
	hegel_assume(xtc_chash_size(h) == 0);

	/* remove again -> NOTFOUND (idempotent) */
	removed = NULL;
	hegel_assume(xtc_chash_remove(h, &lookup, &removed) == XTC_E_NOTFOUND);
	hegel_assume(removed == NULL);

	xtc_chash_destroy(h);
	xtc_rcu_synchronize();
	free(b);
	xtc_rcu_fini();
}

static const pbt_entry_t tests[] = {
	{ "model_equivalence", prop_model_equivalence, 60 },
	{ "overwrite",         prop_overwrite,         40 },
	{ "remove",            prop_remove,            40 },
	{ NULL, NULL, 0 }
};
#else
static const pbt_entry_t tests[] = {
	{ "model_equivalence", NULL, 0 },
	{ "overwrite",         NULL, 0 },
	{ "remove",            NULL, 0 },
	{ NULL, NULL, 0 }
};
#endif

PBT_MAIN("chash", tests)
