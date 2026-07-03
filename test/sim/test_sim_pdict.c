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
#include "xtc_pdict.h"
#include "xtc_async.h"     /* xtc_yield */
#include "xtc_sim.h"

/*
 * DST coverage of the per-process dictionary (src/ptc/pdict.c): a
 * string-keyed kv store LOCAL to each xtc_proc, keyed on xtc_self()
 * under a single global mutex.  NON-blocking (no cond_wait), so
 * sim-safe as-is -- NO shim.  Because entries are keyed by the calling
 * proc's pid, each fiber sees ONLY its own slice; the interesting DST
 * property is that put/get/erase across yields stay correct and
 * ISOLATED between procs while the seeded scheduler interleaves them
 * (a bug that leaked one proc's entries into another's slice, or lost
 * an erase across a yield, would surface).
 *
 * Each fiber loops ITERS rounds: put K keys with proc-unique values,
 * yield, get each back (must equal what it put -- isolation), erase
 * half, yield, confirm the erased are gone and the rest remain, then
 * clear.  INVARIANTS: (a) quiescence (rc == XTC_OK); (b) every get
 * resolves to THIS proc's own value (no cross-proc leak), every erase
 * removes exactly its key, count tracks puts-minus-erases; (c)
 * byte-identical REPLAY (an order-sensitive fold + sim state hash);
 * (d) a different seed reorders but stays consistent.  Footprint tiny.
 */

#define N_LOOPS 4
#define WORKERS 6
#define ITERS   4
#define KEYS    4

static atomic_int  g_bad;        /* a get/erase/count assertion failed */
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
pdict_worker(void *arg)
{
	int id = (int)(intptr_t)arg;
	int it, k;
	char key[32];

	for (it = 0; it < ITERS; it++) {
		/* Put KEYS proc-unique values. */
		for (k = 0; k < KEYS; k++) {
			/* value encodes (id, it, k) so a cross-proc leak is
			 * detectable: another proc's value would not match. */
			intptr_t val = ((intptr_t)id << 16) |
			    ((intptr_t)it << 8) | k;
			snprintf(key, sizeof key, "k%d", k);
			if (xtc_pdict_put(key, (void *)val) != XTC_OK)
				atomic_fetch_add_explicit(&g_bad, 1,
				    memory_order_relaxed);
		}
		if (xtc_pdict_count() != KEYS)
			atomic_fetch_add_explicit(&g_bad, 1,
			    memory_order_relaxed);

		xtc_yield();            /* interleave other procs' slices */

		/* Get each back: must equal OUR own value (isolation). */
		for (k = 0; k < KEYS; k++) {
			void *got = NULL;
			intptr_t want = ((intptr_t)id << 16) |
			    ((intptr_t)it << 8) | k;
			snprintf(key, sizeof key, "k%d", k);
			if (xtc_pdict_get(key, &got) != XTC_OK ||
			    (intptr_t)got != want)
				atomic_fetch_add_explicit(&g_bad, 1,
				    memory_order_relaxed);
			fold((long)(intptr_t)got);
		}

		/* Erase the even keys; odd keys must remain. */
		for (k = 0; k < KEYS; k += 2) {
			snprintf(key, sizeof key, "k%d", k);
			if (xtc_pdict_erase(key) != XTC_OK)
				atomic_fetch_add_explicit(&g_bad, 1,
				    memory_order_relaxed);
		}

		xtc_yield();

		for (k = 0; k < KEYS; k++) {
			void *got = NULL;
			int rc;
			snprintf(key, sizeof key, "k%d", k);
			rc = xtc_pdict_get(key, &got);
			if ((k % 2) == 0) {
				if (rc == XTC_OK)     /* erased -> must miss */
					atomic_fetch_add_explicit(&g_bad, 1,
					    memory_order_relaxed);
			} else {
				if (rc != XTC_OK)     /* kept -> must hit */
					atomic_fetch_add_explicit(&g_bad, 1,
					    memory_order_relaxed);
			}
		}
		/* Clear our whole slice for the next round. */
		(void)xtc_pdict_clear();
		if (xtc_pdict_count() != 0)
			atomic_fetch_add_explicit(&g_bad, 1,
			    memory_order_relaxed);
	}
	atomic_fetch_add_explicit(&g_done, 1, memory_order_relaxed);
}

static int
run_pdict(uint64_t seed, int *out_done, int *out_bad, long *out_hash,
    uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	atomic_store(&g_bad, 0);
	atomic_store(&g_done, 0);
	atomic_store(&g_hash, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	for (i = 0; i < WORKERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(i % N_LOOPS)),
		    pdict_worker, (void *)(intptr_t)i, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_done = atomic_load(&g_done);
	*out_bad = atomic_load(&g_bad);
	*out_hash = atomic_load(&g_hash);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(void)
{
	int rc;
	int d1 = 0, b1 = 0, d2 = 0, b2 = 0, d3 = 0, b3 = 0;
	long h1 = 0, h2 = 0, h3 = 0;
	uint64_t s1 = 0, s2 = 0, s3 = 0;

	rc = run_pdict(0x9D1C7, &d1, &b1, &h1, &s1);
	if (rc != XTC_OK) {
		printf("FAIL: pdict rc=%d (hang?)\n", rc);
		return 1;
	}
	(void)run_pdict(0x9D1C7, &d2, &b2, &h2, &s2);
	rc = run_pdict(0xAE2D8, &d3, &b3, &h3, &s3);
	if (rc != XTC_OK) {
		printf("FAIL: pdict diff-seed rc=%d\n", rc);
		return 1;
	}
	printf("pdict run1: done=%d bad=%d hash=%ld state=%016llx\n",
	    d1, b1, h1, (unsigned long long)s1);
	if (d1 != WORKERS) {
		printf("FAIL: not all pdict workers finished (done=%d "
		    "want %d)\n", d1, WORKERS); return 1;
	}
	if (b1 != 0) {
		printf("FAIL: pdict correctness/isolation broke (%d "
		    "failures)\n", b1); return 1;
	}
	if (d1 != d2 || b1 != b2 || h1 != h2 || s1 != s2) {
		printf("FAIL: pdict did not replay (hash %ld/%ld state "
		    "%016llx/%016llx)\n", h1, h2, (unsigned long long)s1,
		    (unsigned long long)s2); return 1;
	}
	if (d3 != WORKERS || b3 != 0) {
		printf("FAIL: pdict diff-seed inconsistent (done=%d bad=%d)\n",
		    d3, b3); return 1;
	}

	printf("OK: pdict per-proc isolation (every get resolves to the "
	    "proc's own value) + exact erase/clear/count across yields, "
	    "replayed; different seed reorders and stays consistent\n");
	return 0;
}
