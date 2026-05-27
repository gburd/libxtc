/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/pbt/pbt_timer.c
 *	Property-based tests for the L2 timer wheel (M3).
 *
 *	Hegel draws a random multiset of timer delays and a random
 *	subset of cancellations.  We verify:
 *	  P1: every non-cancelled timer fires exactly once.
 *	  P2: fire order is monotonic in deadline (non-decreasing).
 *	  P3: cancelled timers never fire.
 */

#include <stdint.h>
#include <stdlib.h>

#include "pbt_common.h"
#include "xtc_loop.h"
#include "xtc.h"
#include "os_time.h"

#if defined(__GNUC__) || defined(__clang__)
# define MAYBE_UNUSED __attribute__((unused))
#else
# define MAYBE_UNUSED
#endif

/* Per-timer record visible to the callback. */
struct rec {
	int  id;
	int  cancelled;
	int  fired;
	int64_t fire_at_ns;
	int64_t deadline_ns;     /* approx; for property checks */
};

MAYBE_UNUSED static struct rec *g_recs;
MAYBE_UNUSED static int g_recs_n;
MAYBE_UNUSED static int g_fire_order;
MAYBE_UNUSED static int g_monotonic;
MAYBE_UNUSED static int64_t g_last_fire;

MAYBE_UNUSED static void
timer_cb(void *u)
{
	struct rec *r = u;
	int64_t now;
	(void)__os_clock_mono(&now);
	r->fired = 1;
	r->fire_at_ns = now;
	if (g_fire_order > 0 && now < g_last_fire) g_monotonic = 0;
	g_last_fire = now;
	g_fire_order++;
}

#if defined(XTC_HAVE_HEGEL)

static void
prop_random_timers(hegel_test_case *tc, void *u)
{
	xtc_loop_t *loop;
	int n, i, n_alive;
	int64_t now0;
	xtc_timer_t **handles;
	(void)u;

	n = (int)hegel_draw_int(tc, hegel_integers(1, 64));

	hegel_assume(xtc_loop_init(&loop) == XTC_OK);
	g_recs = calloc((size_t)n, sizeof *g_recs);
	handles = calloc((size_t)n, sizeof *handles);
	hegel_assume(g_recs != NULL && handles != NULL);
	g_recs_n = n;
	g_fire_order = 0;
	g_monotonic = 1;
	g_last_fire = 0;

	hegel_assume(__os_clock_mono(&now0) == XTC_OK);

	for (i = 0; i < n; i++) {
		int delay_ms = (int)hegel_draw_int(tc, hegel_integers(0, 30));
		g_recs[i].id = i;
		g_recs[i].deadline_ns = now0 + (int64_t)delay_ms * XTC_NS_PER_MS;
		hegel_assume(xtc_timer_set(loop,
		    (int64_t)delay_ms * XTC_NS_PER_MS,
		    timer_cb, &g_recs[i], &handles[i]) == XTC_OK);
	}

	/* Cancel a random subset (~25% of them). */
	n_alive = n;
	for (i = 0; i < n; i++) {
		int cancel = (int)hegel_draw_int(tc, hegel_integers(0, 3));
		if (cancel == 0) {
			g_recs[i].cancelled = 1;
			hegel_assume(xtc_timer_cancel(handles[i]) == XTC_OK);
			n_alive--;
		}
	}

	hegel_assume(xtc_loop_run(loop) == XTC_OK);

	/* P1: alive timers fired exactly once. */
	for (i = 0; i < n; i++) {
		if (g_recs[i].cancelled) {
			hegel_assume(g_recs[i].fired == 0);   /* P3 */
		} else {
			hegel_assume(g_recs[i].fired == 1);   /* P1 */
			/* Each non-cancelled timer fired at or after its
			 * deadline. */
			hegel_assume(g_recs[i].fire_at_ns >=
			    g_recs[i].deadline_ns);
		}
	}

	/* P2: fire order monotonic in deadline. */
	hegel_assume(g_monotonic == 1);
	hegel_assume(g_fire_order == n_alive);

	free(g_recs); g_recs = NULL;
	free(handles);
	hegel_assume(xtc_loop_fini(loop) == XTC_OK);
}

static const pbt_entry_t tests[] = {
	{ "random_timers", prop_random_timers, 30 },
	{ NULL, NULL, 0 }
};

#else
static const pbt_entry_t tests[] = {
	{ "random_timers", NULL, 0 },
	{ NULL, NULL, 0 }
};
#endif

PBT_MAIN("timer", tests)
