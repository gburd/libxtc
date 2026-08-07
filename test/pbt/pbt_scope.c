/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/pbt/pbt_scope.c
 *	Property-based tests for A1 resource scope / bracket.
 *
 *	Properties:
 *	  Sc1: defer N finalizers into a scope, close it -> all N run,
 *	       exactly once each, in strict LIFO order.
 *	  Sc2: N nested scopes each with one finalizer close inner->outer
 *	       (LIFO across scopes).
 *	  Sc3: xtc_bracket always runs release exactly once whether use
 *	       returns OK or an error code (drawn).
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pbt_common.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

#if defined(XTC_HAVE_HEGEL)

/* ----- Sc1: N finalizers close LIFO ------------------------- */

#define SC_MAX 32
struct sc1_state { int n; int seq[SC_MAX]; int m; int ok; };

/* Each finalizer records the tag (the defer index) it carried.  arg is
 * (state, tag) packed via a per-index cell to avoid heap churn. */
struct sc1_cell { struct sc1_state *s; int tag; };

static void sc1_fin(void *arg)
{
	struct sc1_cell *c = arg;
	if (c->s->m < SC_MAX)
		c->s->seq[c->s->m++] = c->tag;
}

static void sc1_proc(void *arg)
{
	struct sc1_state *s = arg;
	static struct sc1_cell cells[SC_MAX];
	xtc_scope_t *sc;
	int i;
	sc = xtc_scope_open();
	if (sc == NULL) return;
	for (i = 0; i < s->n; i++) {
		cells[i].s = s;
		cells[i].tag = i;
		(void)xtc_scope_defer(sc, sc1_fin, &cells[i]);
	}
	xtc_scope_close(sc);
	/* Verify LIFO: seq should be n-1, n-2, ..., 0. */
	s->ok = (s->m == s->n);
	for (i = 0; i < s->m && s->ok; i++)
		if (s->seq[i] != s->n - 1 - i)
			s->ok = 0;
}

static void
prop_scope_lifo(hegel_test_case *tc, void *u)
{
	xtc_loop_t *loop;
	xtc_pid_t pid;
	struct sc1_state s = {0};
	(void)u;
	s.n = (int)hegel_draw_int(tc, hegel_integers(1, SC_MAX));
	hegel_assume(xtc_loop_init(&loop) == XTC_OK);
	hegel_assume(xtc_proc_spawn(loop, sc1_proc, &s, NULL, &pid) == XTC_OK);
	hegel_assume(xtc_loop_run(loop) == XTC_OK);
	hegel_assume(s.ok == 1);
	hegel_assume(xtc_loop_fini(loop) == XTC_OK);
}

/* ----- Sc2: N nested scopes close inner->outer -------------- */

struct sc2_state { int depth; int seq[SC_MAX]; int m; int ok; };
struct sc2_cell { struct sc2_state *s; int level; };

static void sc2_fin(void *arg)
{
	struct sc2_cell *c = arg;
	if (c->s->m < SC_MAX)
		c->s->seq[c->s->m++] = c->level;
}

static void sc2_proc(void *arg)
{
	struct sc2_state *s = arg;
	static xtc_scope_t *sc[SC_MAX];
	static struct sc2_cell cells[SC_MAX];
	int i;
	/* Open depth nested scopes, defer one finalizer per level. */
	for (i = 0; i < s->depth; i++) {
		sc[i] = xtc_scope_open();
		if (sc[i] == NULL) return;
		cells[i].s = s;
		cells[i].level = i;
		(void)xtc_scope_defer(sc[i], sc2_fin, &cells[i]);
	}
	/* Close inner->outer (highest index first). */
	for (i = s->depth - 1; i >= 0; i--)
		xtc_scope_close(sc[i]);
	/* seq should be depth-1, depth-2, ..., 0. */
	s->ok = (s->m == s->depth);
	for (i = 0; i < s->m && s->ok; i++)
		if (s->seq[i] != s->depth - 1 - i)
			s->ok = 0;
}

static void
prop_nested_scopes(hegel_test_case *tc, void *u)
{
	xtc_loop_t *loop;
	xtc_pid_t pid;
	struct sc2_state s = {0};
	(void)u;
	s.depth = (int)hegel_draw_int(tc, hegel_integers(1, 12));
	hegel_assume(xtc_loop_init(&loop) == XTC_OK);
	hegel_assume(xtc_proc_spawn(loop, sc2_proc, &s, NULL, &pid) == XTC_OK);
	hegel_assume(xtc_loop_run(loop) == XTC_OK);
	hegel_assume(s.ok == 1);
	hegel_assume(xtc_loop_fini(loop) == XTC_OK);
}

/* ----- Sc3: bracket always releases (OK or error) ----------- */

struct sc3_state { int use_rc; int released; int bracket_rc; };

static int sc3_acquire(void **res, void *ud)
{
	(void)ud;
	*res = (void *)(intptr_t)0xABCD;
	return XTC_OK;
}
static int sc3_use(void *res, void *ud)
{
	struct sc3_state *s = ud;
	(void)res;
	return s->use_rc;
}
static void sc3_release(void *res, void *ud)
{
	struct sc3_state *s = ud;
	(void)res;
	s->released++;
}

static void sc3_proc(void *arg)
{
	struct sc3_state *s = arg;
	s->bracket_rc = xtc_bracket(sc3_acquire, sc3_use, sc3_release, s);
}

static void
prop_bracket_always_releases(hegel_test_case *tc, void *u)
{
	xtc_loop_t *loop;
	xtc_pid_t pid;
	struct sc3_state s = {0};
	int pick;
	(void)u;
	/* Draw an outcome: OK or one of a few error codes. */
	pick = (int)hegel_draw_int(tc, hegel_integers(0, 3));
	s.use_rc = (pick == 0) ? XTC_OK :
	           (pick == 1) ? XTC_E_INVAL :
	           (pick == 2) ? XTC_E_AGAIN : XTC_E_RESOURCE;
	hegel_assume(xtc_loop_init(&loop) == XTC_OK);
	hegel_assume(xtc_proc_spawn(loop, sc3_proc, &s, NULL, &pid) == XTC_OK);
	hegel_assume(xtc_loop_run(loop) == XTC_OK);
	/* Release ran exactly once and bracket returned use's code. */
	hegel_assume(s.released == 1);
	hegel_assume(s.bracket_rc == s.use_rc);
	hegel_assume(xtc_loop_fini(loop) == XTC_OK);
}

static const pbt_entry_t tests[] = {
	{ "scope_lifo",              prop_scope_lifo,              20 },
	{ "nested_scopes",           prop_nested_scopes,           20 },
	{ "bracket_always_releases", prop_bracket_always_releases, 20 },
	{ NULL, NULL, 0 }
};
#else
static const pbt_entry_t tests[] = {
	{ "scope_lifo",              NULL, 0 },
	{ "nested_scopes",           NULL, 0 },
	{ "bracket_always_releases", NULL, 0 },
	{ NULL, NULL, 0 }
};
#endif

PBT_MAIN("scope", tests)
