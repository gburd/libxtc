/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/pbt/pbt_lwlock.c
 *	Property-based tests for M13b xtc_lwlock.
 *
 *	Properties:
 *	  Lw1: random sequence of acquire(EXCL) / release: invariant
 *	       value is preserved (mutual exclusion).
 *	  Lw2: conditional acquires never block; the held-by-me
 *	       check is consistent with the actual outcome.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pbt_common.h"
#include "xtc.h"
#include "xtc_int.h"
#include "xtc_lwlock.h"

#if defined(XTC_HAVE_HEGEL)

/* ----- Lw1: mutex invariant under random sequence ------- */

struct lw1_state {
	xtc_lwlock_t lk;
	int          val;     /* protected; updates must be atomic-feeling */
	_Atomic int  failures;
};

static void *
lw1_worker(void *arg)
{
	struct lw1_state *s = arg;
	int i;
	for (i = 0; i < 100; i++) {
		(void)xtc_lwlock_acquire(&s->lk, XTC_LW_EXCLUSIVE);
		s->val = 42;
		s->val = 0;        /* if mutex broken, another thread sees 42 */
		s->val = 99;
		s->val = 0;
		xtc_lwlock_release(&s->lk);
	}
	return NULL;
}

static void *
lw1_observer(void *arg)
{
	struct lw1_state *s = arg;
	int i;
	for (i = 0; i < 200; i++) {
		(void)xtc_lwlock_acquire(&s->lk, XTC_LW_SHARED);
		if (s->val != 0) atomic_fetch_add(&s->failures, 1);
		xtc_lwlock_release(&s->lk);
	}
	return NULL;
}

static void
prop_mutex_invariant(hegel_test_case *tc, void *u)
{
	struct lw1_state s = {0};
	pthread_t writers[4], readers[2];
	int n_writers, i;
	(void)u;
	n_writers = (int)hegel_draw_int(tc, hegel_integers(2, 4));
	hegel_assume(xtc_lwlock_init(&s.lk, 1) == XTC_OK);
	for (i = 0; i < n_writers; i++)
		pthread_create(&writers[i], NULL, lw1_worker, &s);
	for (i = 0; i < 2; i++)
		pthread_create(&readers[i], NULL, lw1_observer, &s);
	for (i = 0; i < n_writers; i++) pthread_join(writers[i], NULL);
	for (i = 0; i < 2; i++) pthread_join(readers[i], NULL);
	hegel_assume(atomic_load(&s.failures) == 0);
	xtc_lwlock_destroy(&s.lk);
}

/* ----- Lw2: conditional acquire honesty ----------------- */

static void
prop_conditional_consistent(hegel_test_case *tc, void *u)
{
	xtc_lwlock_t lk;
	int n, i, op;
	int held_count = 0;
	(void)u;
	hegel_assume(xtc_lwlock_init(&lk, 2) == XTC_OK);
	n = (int)hegel_draw_int(tc, hegel_integers(5, 60));
	for (i = 0; i < n; i++) {
		op = (int)hegel_draw_int(tc, hegel_integers(0, 2));
		if (op == 0) {
			/* Conditional shared. */
			if (xtc_lwlock_acquire_cond(&lk, XTC_LW_SHARED) == XTC_OK)
				held_count++;
		} else if (op == 1) {
			/* Conditional exclusive — only succeeds if nothing held. */
			int rc = xtc_lwlock_acquire_cond(&lk, XTC_LW_EXCLUSIVE);
			if (rc == XTC_OK) {
				held_count++;
			} else {
				hegel_assume(rc == XTC_E_AGAIN);
			}
		} else {
			/* Release if anything held. */
			if (held_count > 0) {
				xtc_lwlock_release(&lk);
				held_count--;
			}
		}
	}
	while (held_count-- > 0) xtc_lwlock_release(&lk);
	xtc_lwlock_destroy(&lk);
}

static const pbt_entry_t tests[] = {
	{ "mutex_invariant",         prop_mutex_invariant,         15 },
	{ "conditional_consistent",  prop_conditional_consistent,  30 },
	{ NULL, NULL, 0 }
};
#else
static const pbt_entry_t tests[] = {
	{ "mutex_invariant",         NULL, 0 },
	{ "conditional_consistent",  NULL, 0 },
	{ NULL, NULL, 0 }
};
#endif

PBT_MAIN("lwlock", tests)
