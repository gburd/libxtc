/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/pbt/pbt_async.c
 *	Property-based tests for the L2 coroutine substrate (M4).
 */

#include <stdint.h>
#include <stdlib.h>

#include "pbt_common.h"
#include "xtc_loop.h"
#include "xtc_async.h"
#include "xtc.h"

/* ----- Workload helpers ----- */

#if defined(__GNUC__) || defined(__clang__)
# define MAYBE_UNUSED __attribute__((unused))
#else
# define MAYBE_UNUSED
#endif

/* C11: each task yields exactly its target count. */
struct yc_state {
	int target;
	int observed;   /* incremented per loop step the coro got */
};

MAYBE_UNUSED static intptr_t yc_coro(void *u) {
	struct yc_state *s = u;
	int i;
	for (i = 0; i < s->target; i++) {
		s->observed++;
		xtc_yield();
	}
	return s->observed;
}

#if defined(XTC_HAVE_HEGEL)

static void
prop_yield_count(hegel_test_case *tc, void *u)
{
	xtc_loop_t *loop;
	int n, i;
	struct yc_state *st;
	xtc_task_t **ts;
	(void)u;

	n = (int)hegel_draw_int(tc, hegel_integers(1, 16));

	hegel_assume(xtc_loop_init(&loop) == XTC_OK);
	st = calloc((size_t)n, sizeof *st);
	ts = calloc((size_t)n, sizeof *ts);
	hegel_assume(st != NULL && ts != NULL);

	for (i = 0; i < n; i++) {
		st[i].target = (int)hegel_draw_int(tc, hegel_integers(0, 30));
		st[i].observed = 0;
		hegel_assume(xtc_async(loop, yc_coro, &st[i], &ts[i]) == XTC_OK);
	}
	hegel_assume(xtc_loop_run(loop) == XTC_OK);
	for (i = 0; i < n; i++)
		hegel_assume(st[i].observed == st[i].target);

	free(st); free(ts);
	hegel_assume(xtc_loop_fini(loop) == XTC_OK);
}

/* C10: nested compose.  spawn N coros, each awaits a child that
 * computes a function of the input.  Result must equal the formula. */
struct nc_arg { int v; };
static intptr_t nc_inner(void *p) {
	struct nc_arg *a = p;
	return (intptr_t)(a->v * 3 + 7);
}

struct nc_outer { xtc_loop_t *loop; int v; };
static intptr_t nc_outer(void *p) {
	struct nc_outer *o = p;
	xtc_task_t *child = NULL;
	intptr_t r = 0;
	struct nc_arg a = { o->v };
	(void)xtc_async(o->loop, nc_inner, &a, &child);
	(void)xtc_await(child, &r);
	return r;
}

static void
prop_nested_compose(hegel_test_case *tc, void *u)
{
	xtc_loop_t *loop;
	int n, i;
	struct nc_outer *outers;
	xtc_task_t **ts;
	intptr_t *results;
	(void)u;

	n = (int)hegel_draw_int(tc, hegel_integers(1, 8));
	hegel_assume(xtc_loop_init(&loop) == XTC_OK);
	outers = calloc((size_t)n, sizeof *outers);
	ts     = calloc((size_t)n, sizeof *ts);
	results= calloc((size_t)n, sizeof *results);
	hegel_assume(outers != NULL && ts != NULL && results != NULL);

	for (i = 0; i < n; i++) {
		outers[i].loop = loop;
		outers[i].v    = (int)hegel_draw_int(tc, hegel_integers(0, 1000));
		hegel_assume(xtc_async(loop, nc_outer, &outers[i], &ts[i]) == XTC_OK);
	}
	hegel_assume(xtc_loop_run(loop) == XTC_OK);
	for (i = 0; i < n; i++) {
		hegel_assume(xtc_await(ts[i], &results[i]) == XTC_OK);
		hegel_assume(results[i] == (intptr_t)(outers[i].v * 3 + 7));
	}
	free(outers); free(ts); free(results);
	hegel_assume(xtc_loop_fini(loop) == XTC_OK);
}

static const pbt_entry_t tests[] = {
	{ "yield_count",     prop_yield_count,    30 },
	{ "nested_compose",  prop_nested_compose, 30 },
	{ NULL, NULL, 0 }
};
#else
static const pbt_entry_t tests[] = {
	{ "yield_count",     NULL, 0 },
	{ "nested_compose",  NULL, 0 },
	{ NULL, NULL, 0 }
};
#endif

PBT_MAIN("async", tests)
