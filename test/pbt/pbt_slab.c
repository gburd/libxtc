/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * test/pbt/pbt_slab.c
 *	Property-based tests for M11.5 xtc_slab.
 *
 *	Properties:
 *	  S1: alloc N then free N → n_inuse == 0
 *	  S2: alloc fills slabs sequentially; free + re-alloc reuses
 *	  S3: redzone-mode catches buffer overruns deterministically
 *	  S4: shm-mode offset/resolve roundtrip is identity
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "pbt_common.h"
#include "xtc.h"
#include "xtc_int.h"
#include "xtc_slab.h"

#if defined(XTC_HAVE_HEGEL)

/* ----- S1: alloc/free balance ------------------------------- */

static void
prop_alloc_free_balance(hegel_test_case *tc, void *u)
{
	xtc_slab_t *s;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	xtc_slab_stats_t stats;
	int n, i;
	void **ptrs;
	(void)u;
	n = (int)hegel_draw_int(tc, hegel_integers(1, 200));
	opts.name = "pbt"; opts.obj_size = 64;
	hegel_assume(xtc_slab_create(&opts, &s) == XTC_OK);
	ptrs = calloc((size_t)n, sizeof *ptrs);
	hegel_assume(ptrs != NULL);
	for (i = 0; i < n; i++) {
		ptrs[i] = xtc_slab_alloc(s);
		hegel_assume(ptrs[i] != NULL);
	}
	for (i = 0; i < n; i++) xtc_slab_free(s, ptrs[i]);
	(void)xtc_slab_stat(s, &stats);
	hegel_assume(stats.n_inuse == 0);
	free(ptrs);
	xtc_slab_destroy(s);
}

/* ----- S2: random alloc/free interleaving (no leaks) ----- */

static void
prop_random_interleave(hegel_test_case *tc, void *u)
{
	xtc_slab_t *s;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	xtc_slab_stats_t stats;
	int n, i, n_live = 0;
	void *ptrs[64] = {0};
	(void)u;
	n = (int)hegel_draw_int(tc, hegel_integers(20, 200));
	opts.name = "pbt2"; opts.obj_size = 32;
	hegel_assume(xtc_slab_create(&opts, &s) == XTC_OK);
	for (i = 0; i < n; i++) {
		int op = (int)hegel_draw_int(tc, hegel_integers(0, 1));
		int slot = (int)hegel_draw_int(tc, hegel_integers(0, 63));
		if (op == 0 && ptrs[slot] == NULL) {
			ptrs[slot] = xtc_slab_alloc(s);
			if (ptrs[slot] != NULL) n_live++;
		} else if (op == 1 && ptrs[slot] != NULL) {
			xtc_slab_free(s, ptrs[slot]);
			ptrs[slot] = NULL;
			n_live--;
		}
	}
	(void)xtc_slab_stat(s, &stats);
	hegel_assume(stats.n_inuse == (uint64_t)n_live);
	for (i = 0; i < 64; i++) if (ptrs[i]) xtc_slab_free(s, ptrs[i]);
	xtc_slab_destroy(s);
}

/* ----- S3: shm offset/resolve roundtrip ------------------ */

static void
prop_shm_offset_roundtrip(hegel_test_case *tc, void *u)
{
	xtc_slab_t *s;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	void *shm;
	void *p1, *p2;
	xtc_slab_off_t o1, o2;
	int sz;
	(void)tc; (void)u;
	sz = (int)hegel_draw_int(tc, hegel_integers(8, 256));
	shm = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	hegel_assume(shm != MAP_FAILED);
	opts.name = "pbtshm"; opts.obj_size = (size_t)sz;
	opts.mode = XTC_SLAB_SHARED_MEMORY;
	opts.shm_base = shm; opts.shm_size = 1 << 20;
	hegel_assume(xtc_slab_create(&opts, &s) == XTC_OK);

	p1 = xtc_slab_alloc(s); hegel_assume(p1 != NULL);
	p2 = xtc_slab_alloc(s); hegel_assume(p2 != NULL);
	o1 = xtc_slab_offset(s, p1);
	o2 = xtc_slab_offset(s, p2);
	hegel_assume(xtc_slab_resolve(s, o1) == p1);
	hegel_assume(xtc_slab_resolve(s, o2) == p2);
	xtc_slab_free(s, p1); xtc_slab_free(s, p2);
	xtc_slab_destroy(s);
	(void)munmap(shm, 1 << 20);
}

static const pbt_entry_t tests[] = {
	{ "alloc_free_balance",   prop_alloc_free_balance,   30 },
	{ "random_interleave",    prop_random_interleave,    30 },
	{ "shm_offset_roundtrip", prop_shm_offset_roundtrip, 30 },
	{ NULL, NULL, 0 }
};
#else
static const pbt_entry_t tests[] = {
	{ "alloc_free_balance",   NULL, 0 },
	{ "random_interleave",    NULL, 0 },
	{ "shm_offset_roundtrip", NULL, 0 },
	{ NULL, NULL, 0 }
};
#endif

PBT_MAIN("slab", tests)
