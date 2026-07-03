#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_mctx.h"
#include "xtc_async.h"     /* xtc_yield */
#include "xtc_sim.h"

/*
 * DST coverage of hierarchical memory contexts (src/ptc/mctx.c):
 * parent-tracked allocation pools with bulk reset/destroy and
 * before-destroy cleanups.  NON-blocking -- an alloc is malloc+chain, a
 * context carries an OPTIONAL mutex (unlocked by default for the
 * per-loop single-thread case).  Each fiber owns its OWN context tree
 * (no sharing), so no lock is needed; the seeded scheduler interleaves
 * the trees' create/alloc/reset/destroy across yields and the whole
 * run must replay.
 *
 * Per fiber: create a root, create C child contexts, alloc a known
 * number of bytes into each child (and register a cleanup per child),
 * yield, then exercise BOTH reclamation paths:
 *   - reset the root's first child -> its bytes/chunks drop to 0 and
 *     its cleanups fire, but the child (and root) stay alive;
 *   - destroy the root -> children are destroyed first (recursively),
 *     every remaining cleanup fires bottom-up, all chunks freed.
 *
 * INVARIANTS: (a) quiescence (rc == XTC_OK); (b) byte/chunk ACCOUNTING
 * -- total_bytes equals the exact sum allocated into a context, and is
 * 0 after reset; (c) cleanups fire EXACTLY once each (child-with-parent
 * lifetime: every registered cleanup runs by the time the tree is torn
 * down -- no leak, no double-run); (d) byte-identical REPLAY (an
 * accounting fold + sim state hash); (e) a different seed reorders but
 * stays consistent.  Footprint tiny + per-run free (ASan-clean).
 */

#define N_LOOPS   4
#define WORKERS   6
#define CHILDREN  3
#define PER_CHILD 4              /* allocs per child */
#define ALLOC_SZ  64

static atomic_int  g_cleanups;   /* total cleanup callbacks fired */
static atomic_int  g_acct_bad;   /* an accounting assertion failed */
static atomic_int  g_done;
static atomic_long g_hash;

static void
fold(long v)
{
	long h = atomic_load_explicit(&g_hash, memory_order_relaxed);
	h = h * 1000003L + (v + 1);
	atomic_store_explicit(&g_hash, h, memory_order_relaxed);
}

static void
on_cleanup(void *user)
{
	(void)user;
	atomic_fetch_add_explicit(&g_cleanups, 1, memory_order_relaxed);
}

static void
mctx_worker(void *arg)
{
	int id = (int)(intptr_t)arg;
	xtc_mctx_t *root = NULL, *kids[CHILDREN];
	int i, j;

	if (xtc_mctx_create(NULL, "root", XTC_MCTX_DEFAULT, &root) != XTC_OK) {
		atomic_fetch_add_explicit(&g_acct_bad, 1,
		    memory_order_relaxed);
		atomic_fetch_add_explicit(&g_done, 1, memory_order_relaxed);
		return;
	}
	for (i = 0; i < CHILDREN; i++) {
		size_t want;
		kids[i] = NULL;
		if (xtc_mctx_create(root, "kid", XTC_MCTX_DEFAULT, &kids[i])
		    != XTC_OK) {
			atomic_fetch_add_explicit(&g_acct_bad, 1,
			    memory_order_relaxed);
			continue;
		}
		(void)xtc_mctx_register_cleanup(kids[i], on_cleanup, NULL);
		for (j = 0; j < PER_CHILD; j++) {
			void *p = xtc_mctx_alloc(kids[i], ALLOC_SZ);
			if (p == NULL)
				atomic_fetch_add_explicit(&g_acct_bad, 1,
				    memory_order_relaxed);
		}
		/* Accounting: exactly PER_CHILD chunks totalling PER_CHILD *
		 * ALLOC_SZ bytes must be recorded on this child. */
		want = (size_t)PER_CHILD * ALLOC_SZ;
		if (xtc_mctx_total_bytes(kids[i]) != want ||
		    xtc_mctx_total_chunks(kids[i]) != (size_t)PER_CHILD)
			atomic_fetch_add_explicit(&g_acct_bad, 1,
			    memory_order_relaxed);
		fold((long)id * 31 + i);
	}

	xtc_yield();                /* interleave other trees' work */

	/* Reset the first child: its bytes/chunks drop to 0 and its
	 * cleanup fires, but it stays alive (usable). */
	xtc_mctx_reset(kids[0]);
	if (xtc_mctx_total_bytes(kids[0]) != 0 ||
	    xtc_mctx_total_chunks(kids[0]) != 0)
		atomic_fetch_add_explicit(&g_acct_bad, 1,
		    memory_order_relaxed);
	/* Re-arm a cleanup on the reset child so destroy still fires one
	 * for it (reset ran the first). */
	(void)xtc_mctx_register_cleanup(kids[0], on_cleanup, NULL);
	{
		void *p = xtc_mctx_alloc(kids[0], ALLOC_SZ);
		if (p == NULL || xtc_mctx_total_chunks(kids[0]) != 1)
			atomic_fetch_add_explicit(&g_acct_bad, 1,
			    memory_order_relaxed);
	}

	xtc_yield();

	/* Destroy the root: children destroyed first, remaining cleanups
	 * fire, all chunks freed (no leak under ASan). */
	xtc_mctx_destroy(root);
	atomic_fetch_add_explicit(&g_done, 1, memory_order_relaxed);
}

static int
run_mctx(uint64_t seed, int *out_done, int *out_cleanups, int *out_bad,
    long *out_hash, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	atomic_store(&g_cleanups, 0);
	atomic_store(&g_acct_bad, 0);
	atomic_store(&g_done, 0);
	atomic_store(&g_hash, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	for (i = 0; i < WORKERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(i % N_LOOPS)),
		    mctx_worker, (void *)(intptr_t)i, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_done = atomic_load(&g_done);
	*out_cleanups = atomic_load(&g_cleanups);
	*out_bad = atomic_load(&g_acct_bad);
	*out_hash = atomic_load(&g_hash);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(void)
{
	int rc;
	int d1 = 0, c1 = 0, b1 = 0, d2 = 0, c2 = 0, b2 = 0;
	int d3 = 0, c3 = 0, b3 = 0;
	long h1 = 0, h2 = 0, h3 = 0;
	uint64_t s1 = 0, s2 = 0, s3 = 0;
	/* Each worker: reset(kids[0]) fires its 1 cleanup, then destroy
	 * fires one per child (the re-armed cleanup on kids[0] plus the
	 * original on kids[1..]) = 1 + CHILDREN per worker. */
	int want_cleanups = WORKERS * (CHILDREN + 1);

	rc = run_mctx(0x4AC71, &d1, &c1, &b1, &h1, &s1);
	if (rc != XTC_OK) {
		printf("FAIL: mctx rc=%d (hang?)\n", rc);
		return 1;
	}
	(void)run_mctx(0x4AC71, &d2, &c2, &b2, &h2, &s2);
	rc = run_mctx(0x5B22, &d3, &c3, &b3, &h3, &s3);
	if (rc != XTC_OK) {
		printf("FAIL: mctx diff-seed rc=%d\n", rc);
		return 1;
	}
	printf("mctx run1: done=%d cleanups=%d (want %d) acct-bad=%d "
	    "hash=%ld state=%016llx\n", d1, c1, want_cleanups, b1, h1,
	    (unsigned long long)s1);
	if (d1 != WORKERS) {
		printf("FAIL: not all mctx workers finished (done=%d "
		    "want %d)\n", d1, WORKERS); return 1;
	}
	if (b1 != 0) {
		printf("FAIL: mctx accounting/hierarchy invariant broke "
		    "(%d failures)\n", b1); return 1;
	}
	if (c1 != want_cleanups) {
		printf("FAIL: cleanup count %d != %d (leaked or double-run "
		    "cleanup)\n", c1, want_cleanups); return 1;
	}
	if (d1 != d2 || c1 != c2 || b1 != b2 || h1 != h2 || s1 != s2) {
		printf("FAIL: mctx did not replay (cleanups %d/%d hash "
		    "%ld/%ld state %016llx/%016llx)\n", c1, c2, h1, h2,
		    (unsigned long long)s1, (unsigned long long)s2);
		return 1;
	}
	if (d3 != WORKERS || b3 != 0 || c3 != want_cleanups) {
		printf("FAIL: mctx diff-seed inconsistent (done=%d bad=%d "
		    "cleanups=%d)\n", d3, b3, c3); return 1;
	}

	printf("OK: mctx byte/chunk accounting exact, reset clears + keeps "
	    "alive, destroy cascades children + fires every cleanup once "
	    "(no leak), replayed; different seed reorders and stays "
	    "consistent\n");
	return 0;
}
