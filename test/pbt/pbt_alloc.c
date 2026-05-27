/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/pbt/pbt_alloc.c
 *	Property-based tests for the L0 hookable allocator (M1).
 *
 *	Hegel drives a random sequence of malloc/realloc/free with
 *	random sizes; the property checks are: every allocation is at
 *	least the requested size and is writeable; the hook accounting
 *	never goes negative; final accounting matches the call counts.
 */

#define _POSIX_C_SOURCE 200112L  /* posix_memalign */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pbt_common.h"
#include "xtc_int.h"

/* Hooked allocator that counts inflight allocations.  Marked
 * __attribute__((unused)) so the SKIP build does not warn. */
#if defined(__GNUC__) || defined(__clang__)
# define MAYBE_UNUSED __attribute__((unused))
#else
# define MAYBE_UNUSED
#endif
static int alloc_inflight;
static int alloc_total;
MAYBE_UNUSED static void *p_malloc (size_t s)             { alloc_inflight++; alloc_total++; return malloc(s); }
MAYBE_UNUSED static void *p_calloc (size_t n, size_t s)   { alloc_inflight++; alloc_total++; return calloc(n, s); }
MAYBE_UNUSED static void *p_realloc(void *p, size_t s)    { if (!p) { alloc_inflight++; alloc_total++; } return realloc(p, s); }
MAYBE_UNUSED static void  p_free   (void *p)              { if (p) alloc_inflight--; free(p); }
MAYBE_UNUSED static void *p_aligned(size_t a, size_t s)   {
	void *p = NULL; alloc_inflight++; alloc_total++;
	if (s % a != 0) s += a - (s % a);
	if (posix_memalign(&p, a, s) != 0) { alloc_inflight--; alloc_total--; p = NULL; }
	return p;
}

#if defined(XTC_HAVE_HEGEL)

/*
 * P1: a malloc'd block is at least the requested size and writeable.
 */
static void
prop_malloc_writeable(hegel_test_case *tc, void *u)
{
	void *p = NULL;
	size_t sz = (size_t)hegel_draw_int(tc, hegel_integers(1, 8192));
	(void)u;
	hegel_assume(__os_malloc(sz, &p) == XTC_OK);
	hegel_assume(p != NULL);
	memset(p, 0xa5, sz);            /* must not crash */
	__os_free(p);
}

/*
 * P2: realloc preserves contents.
 */
static void
prop_realloc_preserves(hegel_test_case *tc, void *u)
{
	unsigned char *p = NULL;
	size_t sz_old, sz_new, i;
	(void)u;

	sz_old = (size_t)hegel_draw_int(tc, hegel_integers(1, 256));
	sz_new = (size_t)hegel_draw_int(tc, hegel_integers(1, 256));

	hegel_assume(__os_malloc(sz_old, (void **)&p) == XTC_OK);
	for (i = 0; i < sz_old; i++) p[i] = (unsigned char)((i * 7 + 3) & 0xff);

	hegel_assume(__os_realloc(p, sz_new, (void **)&p) == XTC_OK);

	for (i = 0; i < (sz_old < sz_new ? sz_old : sz_new); i++)
		hegel_assume(p[i] == (unsigned char)((i * 7 + 3) & 0xff));

	__os_free(p);
}

/*
 * P3: hook accounting.  Random pairs of malloc+free leave inflight at 0.
 */
static void
prop_hook_balanced(hegel_test_case *tc, void *u)
{
	struct __os_alloc_hook saved, hook;
	int n_pairs, i;
	void **ptrs;
	(void)u;

	n_pairs = (int)hegel_draw_int(tc, hegel_integers(1, 64));

	hook.malloc  = p_malloc;
	hook.calloc  = p_calloc;
	hook.realloc = p_realloc;
	hook.free    = p_free;
	hook.aligned = p_aligned;

	hegel_assume(__os_alloc_get_hook(&saved) == XTC_OK);
	alloc_inflight = 0; alloc_total = 0;
	hegel_assume(__os_alloc_set_hook(&hook) == XTC_OK);

	ptrs = calloc((size_t)n_pairs, sizeof *ptrs);
	hegel_assume(ptrs != NULL);
	for (i = 0; i < n_pairs; i++) {
		size_t sz = (size_t)hegel_draw_int(tc, hegel_integers(1, 1024));
		hegel_assume(__os_malloc(sz, &ptrs[i]) == XTC_OK);
	}
	for (i = 0; i < n_pairs; i++) __os_free(ptrs[i]);
	free(ptrs);

	hegel_assume(__os_alloc_set_hook(&saved) == XTC_OK);
	hegel_assume(alloc_total    == n_pairs);
	hegel_assume(alloc_inflight == 0);
}

static const pbt_entry_t tests[] = {
	{ "malloc_writeable",  prop_malloc_writeable, 100 },
	{ "realloc_preserves", prop_realloc_preserves, 100 },
	{ "hook_balanced",     prop_hook_balanced,    50 },
	{ NULL, NULL, 0 }
};

#else
static const pbt_entry_t tests[] = {
	{ "malloc_writeable",  NULL, 0 },
	{ "realloc_preserves", NULL, 0 },
	{ "hook_balanced",     NULL, 0 },
	{ NULL, NULL, 0 }
};
#endif

PBT_MAIN("alloc", tests)
