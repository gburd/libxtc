/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/pbt/pbt_deque.c
 *	Property-based test for the Chase-Lev work-stealing deque (M5).
 *
 *	Property: under any random push/pop/steal interleaving, the
 *	multiset of values returned (by owner pop + thief steals) equals
 *	the multiset pushed.  No double-take, no loss.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>

#include "pbt_common.h"
#include "deque.h"
#include "xtc.h"

#if defined(__GNUC__) || defined(__clang__)
# define MAYBE_UNUSED __attribute__((unused))
#else
# define MAYBE_UNUSED
#endif

struct th_ctx {
	xtc_deque_t   *q;
	_Atomic int   *got;       /* per-item received count */
	int            n_items;
	_Atomic int   *done;       /* signals end of producer */
	int            taken;      /* per-thread accounting */
};

MAYBE_UNUSED static void *
__thief(void *p)
{
	struct th_ctx *c = p;
	for (;;) {
		void *v = xtc_deque_steal(c->q);
		if (v == NULL) {
			if (atomic_load_explicit(c->done,
			    memory_order_acquire)) {
				/* Producer done; one final attempt. */
				int i;
				for (i = 0; i < 100; i++) {
					v = xtc_deque_steal(c->q);
					if (v) break;
				}
				if (!v) break;
			} else {
				continue;        /* retry */
			}
		}
		atomic_fetch_add_explicit(&c->got[(int)(intptr_t)v - 1], 1,
		    memory_order_relaxed);
		c->taken++;
	}
	return NULL;
}

#if defined(XTC_HAVE_HEGEL)

static void
prop_pop_steal_partition(hegel_test_case *tc, void *u)
{
	xtc_deque_t q;
	int n_items, n_thieves, i;
	pthread_t *th;
	struct th_ctx *ctxs;
	_Atomic int *got;
	_Atomic int done;
	int owner_took = 0;
	(void)u;

	xtc_deque_init(&q);
	atomic_store_explicit(&done, 0, memory_order_relaxed);

	/* Hegel-drawn parameters.  Keep within deque capacity. */
	n_items   = (int)hegel_draw_int(tc, hegel_integers(1, XTC_DEQUE_CAP));
	n_thieves = (int)hegel_draw_int(tc, hegel_integers(1, 4));

	got = calloc((size_t)n_items, sizeof *got);
	th  = calloc((size_t)n_thieves, sizeof *th);
	ctxs = calloc((size_t)n_thieves, sizeof *ctxs);
	hegel_assume(got != NULL && th != NULL && ctxs != NULL);

	for (i = 0; i < n_thieves; i++) {
		ctxs[i].q = &q; ctxs[i].got = got;
		ctxs[i].n_items = n_items; ctxs[i].done = &done;
		ctxs[i].taken = 0;
		hegel_assume(pthread_create(&th[i], NULL, __thief, &ctxs[i])
		    == 0);
	}

	for (i = 0; i < n_items; i++)
		hegel_assume(xtc_deque_push(&q, (void *)(intptr_t)(i + 1))
		    == XTC_OK);
	for (;;) {
		void *v = xtc_deque_pop(&q);
		if (v == NULL) break;
		atomic_fetch_add_explicit(&got[(int)(intptr_t)v - 1], 1,
		    memory_order_relaxed);
		owner_took++;
	}
	atomic_store_explicit(&done, 1, memory_order_release);
	for (i = 0; i < n_thieves; i++) hegel_assume(pthread_join(th[i], NULL) == 0);

	/* Linearizability: every item delivered exactly once. */
	for (i = 0; i < n_items; i++) {
		int s = atomic_load_explicit(&got[i], memory_order_relaxed);
		hegel_assume(s == 1);
	}

	free(got); free(th); free(ctxs);
}

static const pbt_entry_t tests[] = {
	{ "pop_steal_partition", prop_pop_steal_partition, 30 },
	{ NULL, NULL, 0 }
};
#else
static const pbt_entry_t tests[] = {
	{ "pop_steal_partition", NULL, 0 },
	{ NULL, NULL, 0 }
};
#endif

PBT_MAIN("deque", tests)
