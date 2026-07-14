/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/pbt/pbt_cskip.c
 *	Property-based tests for xtc_cskip (src/ptc/cskip.c) -- the
 *	RCU-protected concurrent ordered map.
 *
 *	Sequential model-equivalence properties against a sorted-array
 *	reference (the concurrent / RCU-reclamation UAF story is the DST
 *	test's job, test/sim/test_sim_cskip.c).
 *
 *	Properties:
 *	  K1: model equivalence -- after any insert/remove/get sequence,
 *	      get(k) agrees with the reference for every key and size()
 *	      matches the live count.
 *	  K2: ordered queries -- min() equals the smallest live key, and
 *	      floor(q) equals the largest live key <= q (or NOTFOUND if
 *	      none), for random q, against the reference.
 *	  K3: overwrite returns the old value and leaves size unchanged;
 *	      remove returns the stored value then get misses.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pbt_common.h"
#include "xtc.h"
#include "xtc_rcu.h"
#include "xtc_cskip.h"

#if defined(XTC_HAVE_HEGEL)

#define KEYSPACE 32

struct box { int64_t k; int64_t v; };

static int
box_cmp(const void *a, const void *b)
{
	int64_t x = ((const struct box *)a)->k;
	int64_t y = ((const struct box *)b)->k;
	return x < y ? -1 : (x > y ? 1 : 0);
}

struct refmap {
	struct box *slot[KEYSPACE];   /* value box for key, or NULL */
	int         count;
};

static int
cskip_get_i(xtc_cskip_t *s, int64_t k, struct box **out)
{
	struct box lookup; void *v = NULL; int rc;
	lookup.k = k;
	xtc_rcu_read_lock();
	rc = xtc_cskip_get(s, &lookup, &v);
	xtc_rcu_read_unlock();
	if (out != NULL) *out = (struct box *)v;
	return rc;
}

/* ----- K1 + K2: model equivalence incl. ordered queries ------ */

static void
prop_model_ordered(hegel_test_case *tc, void *u)
{
	xtc_cskip_t *s = NULL;
	struct refmap ref;
	struct box *allocated[512];
	int n_alloc = 0, nops, i, k;
	(void)u;

	memset(&ref, 0, sizeof ref);
	hegel_assume(xtc_rcu_init() == XTC_OK);
	hegel_assume(xtc_cskip_create(box_cmp, &s) == XTC_OK);

	nops = (int)hegel_draw_int(tc, hegel_integers(1, 200));
	for (i = 0; i < nops; i++) {
		int op = (int)hegel_draw_int(tc, hegel_integers(0, 1));
		int key = (int)hegel_draw_int(tc, hegel_integers(0,
		    KEYSPACE - 1));
		if (op == 0 && n_alloc < 512) {
			struct box *nb = malloc(sizeof *nb);
			void *old = NULL; int rc;
			hegel_assume(nb != NULL);
			nb->k = key; nb->v = (int64_t)i * KEYSPACE + key;
			allocated[n_alloc++] = nb;
			rc = xtc_cskip_insert(s, nb, nb, &old);
			hegel_assume(rc == XTC_OK);
			if (ref.slot[key] == NULL) ref.count++;
			else hegel_assume(old == ref.slot[key]);
			ref.slot[key] = nb;
		} else {
			struct box lookup; void *removed = NULL; int rc;
			lookup.k = key;
			rc = xtc_cskip_remove(s, &lookup, &removed);
			if (ref.slot[key] != NULL) {
				hegel_assume(rc == XTC_OK);
				hegel_assume(removed == ref.slot[key]);
				ref.slot[key] = NULL; ref.count--;
			} else {
				hegel_assume(rc == XTC_E_NOTFOUND);
				hegel_assume(removed == NULL);
			}
		}
	}

	/* K1: every key agrees; size matches. */
	for (k = 0; k < KEYSPACE; k++) {
		struct box *got = NULL;
		int rc = cskip_get_i(s, k, &got);
		if (ref.slot[k] != NULL) {
			hegel_assume(rc == XTC_OK);
			hegel_assume(got == ref.slot[k]);
		} else {
			hegel_assume(rc == XTC_E_NOTFOUND);
		}
	}
	hegel_assume(xtc_cskip_size(s) == (size_t)ref.count);

	/* K2a: min == smallest live key (or NOTFOUND when empty). */
	{
		void *mk = NULL; int rc;
		int smallest = -1;
		for (k = 0; k < KEYSPACE; k++)
			if (ref.slot[k] != NULL) { smallest = k; break; }
		xtc_rcu_read_lock();
		rc = xtc_cskip_min(s, &mk, NULL);
		xtc_rcu_read_unlock();
		if (smallest < 0) {
			hegel_assume(rc == XTC_E_NOTFOUND);
		} else {
			hegel_assume(rc == XTC_OK);
			hegel_assume(((struct box *)mk)->k == smallest);
		}
	}

	/* K2b: floor(q) == largest live key <= q, for a random q. */
	{
		int q = (int)hegel_draw_int(tc, hegel_integers(-1, KEYSPACE));
		void *fk = NULL; struct box lookup; int rc;
		int expect = -1;
		for (k = q < KEYSPACE ? q : KEYSPACE - 1; k >= 0; k--)
			if (ref.slot[k] != NULL) { expect = k; break; }
		lookup.k = q;
		xtc_rcu_read_lock();
		rc = xtc_cskip_floor(s, &lookup, &fk, NULL);
		xtc_rcu_read_unlock();
		if (expect < 0) {
			hegel_assume(rc == XTC_E_NOTFOUND);
		} else {
			hegel_assume(rc == XTC_OK);
			hegel_assume(((struct box *)fk)->k == expect);
		}
	}

	xtc_cskip_destroy(s);
	xtc_rcu_synchronize();
	for (i = 0; i < n_alloc; i++) free(allocated[i]);
	xtc_rcu_fini();
}

/* ----- K3: overwrite + remove semantics ---------------------- */

static void
prop_overwrite_remove(hegel_test_case *tc, void *u)
{
	xtc_cskip_t *s = NULL;
	struct box *b1, *b2;
	void *old = NULL, *removed = NULL;
	struct box lookup;
	int64_t key;
	(void)u;

	hegel_assume(xtc_rcu_init() == XTC_OK);
	hegel_assume(xtc_cskip_create(box_cmp, &s) == XTC_OK);
	key = hegel_draw_int(tc, hegel_integers(0, KEYSPACE - 1));

	b1 = malloc(sizeof *b1); hegel_assume(b1 != NULL); b1->k = key; b1->v = 1;
	b2 = malloc(sizeof *b2); hegel_assume(b2 != NULL); b2->k = key; b2->v = 2;

	hegel_assume(xtc_cskip_insert(s, b1, b1, &old) == XTC_OK);
	hegel_assume(old == NULL);
	hegel_assume(xtc_cskip_size(s) == 1);

	old = NULL;
	hegel_assume(xtc_cskip_insert(s, b2, b2, &old) == XTC_OK);
	hegel_assume(old == b1);
	hegel_assume(xtc_cskip_size(s) == 1);

	{
		struct box *got = NULL;
		hegel_assume(cskip_get_i(s, key, &got) == XTC_OK);
		hegel_assume(got == b2);
	}

	lookup.k = key;
	hegel_assume(xtc_cskip_remove(s, &lookup, &removed) == XTC_OK);
	hegel_assume(removed == b2);
	hegel_assume(cskip_get_i(s, key, NULL) == XTC_E_NOTFOUND);
	hegel_assume(xtc_cskip_size(s) == 0);

	xtc_cskip_destroy(s);
	xtc_rcu_synchronize();
	free(b1); free(b2);
	xtc_rcu_fini();
}

static const pbt_entry_t tests[] = {
	{ "model_ordered",     prop_model_ordered,     60 },
	{ "overwrite_remove",  prop_overwrite_remove,  40 },
	{ NULL, NULL, 0 }
};
#else
static const pbt_entry_t tests[] = {
	{ "model_ordered",     NULL, 0 },
	{ "overwrite_remove",  NULL, 0 },
	{ NULL, NULL, 0 }
};
#endif

PBT_MAIN("cskip", tests)
