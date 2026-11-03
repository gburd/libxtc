/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/cbmc/real/lockmgr_harness.c
 *
 *	REAL CODE: #includes src/ptc/lock_mgr.c unchanged (so the harness
 *	can read the manager's own granted list and conflict matrix).  The
 *	slab and allocator are thin stubs over malloc (not under test).
 *
 *	PROPERTY: mutual exclusion under the conflict matrix, through every
 *	way a lock changes mode.  Two lockers on one object, each driven
 *	through a nondeterministic sequence of xtc_lock_get (a new lock or
 *	a re-request: the 1.50 conversion path), xtc_lock_upgrade and
 *	xtc_lock_downgrade, every mode, timeout 0 so nothing parks.  After
 *	every step, walking the object's real granted list, no two entries
 *	of DIFFERENT lockers conflict (m->conflicts, both directions; one
 *	locker may hold several entries -- a re-request in an incomparable
 *	mode is granted as an additional lock, xtc_lockmgr.h).  1.50 fixed three ways this
 *	broke (KNOWN_ISSUES "lock-manager conversions broke mutual
 *	exclusion"): a re-request checked only the locker's own mode (IS
 *	holder asks S beside another's IX: granted), "stronger" was the
 *	enum order (S holder asking IS was weakened, admitting IX), and
 *	downgrade refused IX -> IS but allowed IX -> S.
 *
 *	Also, judged by an independent dominance check over the matrix:
 *	a successful get or upgrade leaves the locker holding an entry at
 *	least as strong as the mode it asked for; an upgrade only goes to a
 *	mode that dominates the held one; a successful downgrade leaves the
 *	newest entry at the asked mode, which the old one dominated.
 *
 *	Planted bug -DXTC_DST_INJECT_BUG=2 (LOCKEXCL, the DST tier's id:
 *	never see a conflicting holder) must FAIL.
 */
#include "../../../src/ptc/lock_mgr.c"

#include <stdlib.h>

#ifndef STEPS
#define STEPS 3
#endif

/* --- stubs: the allocator and slab are not under test --- */
int
__os_calloc(size_t n, size_t sz, void **out)
{
	void *p = calloc(n, sz);
	if (p == NULL)
		return XTC_E_NOMEM;
	*out = p;
	return XTC_OK;
}
int
__os_malloc(size_t sz, void **out)
{
	void *p = malloc(sz);
	if (p == NULL)
		return XTC_E_NOMEM;
	*out = p;
	return XTC_OK;
}
void __os_free(void *p) { free(p); }
/* A fixed clock: deadlines are "now" and so already past. */
int __os_clock_mono(int64_t *out) { *out = 1000; return XTC_OK; }
struct xtc_slab { size_t sz; };
static struct xtc_slab the_slab;
int
xtc_slab_create(const xtc_slab_opts_t *o, xtc_slab_t **out)
{
	the_slab.sz = o->obj_size;
	*out = &the_slab;
	return XTC_OK;
}
void xtc_slab_destroy(xtc_slab_t *s) { (void)s; }
void *xtc_slab_alloc(xtc_slab_t *s) { return calloc(1, s->sz); }
void xtc_slab_free(xtc_slab_t *s, void *p) { (void)s; free(p); }

int nondet_int(void);

/* "p dominates q" from the definition, not the library's __dominates. */
static int
spec_dominates(const xtc_lockmgr_t *m, int p, int q)
{
	int n = m->n_modes, k;
	for (k = 0; k < n; k++) {
		if (m->conflicts[q * n + k] && !m->conflicts[p * n + k])
			return 0;
		if (m->conflicts[k * n + q] && !m->conflicts[k * n + p])
			return 0;
	}
	return 1;
}

static int
spec_conflicts(const xtc_lockmgr_t *m, int a, int b)
{
	return m->conflicts[a * m->n_modes + b] != 0;
}

/* Does locker l hold, among its granted entries on o, one that
 * dominates `mode`?  (A re-request in a mode that neither dominates
 * nor is dominated by the held one is granted as an ADDITIONAL entry,
 * xtc_lockmgr.h, so a locker may hold several.) */
static int
holds_at_least(const xtc_lockmgr_t *m, struct lock_obj *o, xtc_locker_t l,
    int mode)
{
	struct lock_entry *e;
	int i;
	for (e = o->granted, i = 0; e != NULL && i < 4; e = e->next, i++)
		if (e->locker == l && spec_dominates(m, (int)e->mode, mode))
			return 1;
	return 0;
}

/* The mode of l's NEWEST granted entry (head-most: the one put and
 * downgrade act on), or NL. */
static int
newest_mode(struct lock_obj *o, xtc_locker_t l)
{
	struct lock_entry *e;
	int i;
	for (e = o->granted, i = 0; e != NULL && i < 4; e = e->next, i++)
		if (e->locker == l)
			return (int)e->mode;
	return XTC_LOCK_NL;
}

/* MUTUAL EXCLUSION: no granted entry of one locker conflicts with any
 * granted entry of another, in either direction. */
static int
exclusive(const xtc_lockmgr_t *m, struct lock_obj *o)
{
	struct lock_entry *a, *b;
	int i, j;
	for (a = o->granted, i = 0; a != NULL && i < 4; a = a->next, i++)
		for (b = o->granted, j = 0; b != NULL && j < 4;
		    b = b->next, j++)
			if (a->locker != b->locker &&
			    spec_conflicts(m, (int)a->mode, (int)b->mode))
				return 0;
	return 1;
}

/* The modes the 1.50 bugs needed: S, X, IX, IS (every pair of them is
 * a distinct row/column relation in the RIW matrix).  -DALL_MODES
 * widens it to all eight grantable modes (much slower). */
static xtc_lock_mode_t
any_mode(void)
{
	int m = nondet_int();
#ifdef ALL_MODES
	__CPROVER_assume(m >= XTC_LOCK_S && m <= XTC_LOCK_WW &&
	    m != XTC_LOCK_WAIT);
#else
	__CPROVER_assume(m == XTC_LOCK_S || m == XTC_LOCK_X ||
	    m == XTC_LOCK_IX || m == XTC_LOCK_IS);
#endif
	return (xtc_lock_mode_t)m;
}

int
main(void)
{
	xtc_lockmgr_opts_t opts = XTC_LOCKMGR_OPTS_DEFAULT;
	xtc_lockmgr_t *m;
	xtc_locker_t L[2];
	static const char obj[] = "o";
	struct lock_obj *o;
	int step;

	opts.n_partitions = 1;
	opts.detect_mode = XTC_LOCK_DETECT_NONE;   /* no detector thread */
	if (xtc_lockmgr_create(&opts, &m) != XTC_OK)
		return 0;
	if (xtc_lockmgr_id(m, &L[0]) != XTC_OK ||
	    xtc_lockmgr_id(m, &L[1]) != XTC_OK)
		return 0;
	/* A conflicting upgrade waits until the locker's deadline; a zero
	 * timeout makes it answer XTC_E_AGAIN at once (nothing parks). */
	(void)xtc_lockmgr_id_set_timeout(m, L[0], 0);
	(void)xtc_lockmgr_id_set_timeout(m, L[1], 0);

	for (step = 0; step < STEPS; step++) {
		int who = nondet_int(), op = nondet_int(), rc, before;
		xtc_lock_mode_t want = any_mode();

		__CPROVER_assume(who == 0 || who == 1);
		__CPROVER_assume(op >= 0 && op <= 2);
		o = m->parts[0].table;
		before = o != NULL ? newest_mode(o, L[who]) : XTC_LOCK_NL;
		if (op == 0)
			rc = xtc_lock_get(m, L[who], obj, 1, want, 0);
		else if (op == 1)
			rc = xtc_lock_upgrade(m, L[who], obj, 1, want);
		else
			rc = xtc_lock_downgrade(m, L[who], obj, 1, want);
		o = m->parts[0].table;
		if (o == NULL)
			continue;
		if (rc == XTC_OK && op != 2)
			__CPROVER_assert(holds_at_least(m, o, L[who], (int)want),
			    "a granted get/upgrade holds at least the asked mode");
		if (rc == XTC_OK && op == 1)
			__CPROVER_assert(before != XTC_LOCK_NL &&
			    spec_dominates(m, (int)want, before),
			    "upgrade only to a mode that dominates the held one");
		if (rc == XTC_OK && op == 2)
			__CPROVER_assert(before != XTC_LOCK_NL &&
			    newest_mode(o, L[who]) == (int)want &&
			    spec_dominates(m, before, (int)want),
			    "downgrade lands on the asked mode, dominated by the old");
		__CPROVER_assert(exclusive(m, o),
		    "no two lockers' granted entries conflict");
	}
	return 0;
}
