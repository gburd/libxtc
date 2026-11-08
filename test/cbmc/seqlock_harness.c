/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/cbmc/seqlock_harness.c
 *	CBMC bounded model check of the OLC (optimistic-latch-coupling)
 *	version seqlock in the sqlxtc buffer manager
 *	(examples/06_sqlxtc/bufmgr.c: bm_read_begin / bm_read_valid, and
 *	the writer's even/odd version bumps).
 *
 *	INVARIANT PROVED: a lock-free optimistic reader never ACCEPTS a
 *	sample taken across a writer's mutation.  If bm_read_begin
 *	succeeds (even version) and bm_read_valid later returns true
 *	(version unchanged), then the page bytes the reader observed are
 *	a consistent, untorn snapshot -- no writer mutated the page
 *	between begin and valid.  A torn read is always rejected.
 *
 *	WHAT IS MODELLED: the seqlock functions bm_read_begin and
 *	bm_read_valid are transcribed VERBATIM from bufmgr.c (they touch
 *	only the frame's _Atomic version, so a minimal bm_frame_t with
 *	just that field compiles them unchanged -- including the real
 *	bufmgr.c would pull in the entire buffer pool, RCU, latches and
 *	I/O).  The writer's version protocol is likewise transcribed from
 *	bm_frame_wrlatch/unlatch (fetch_add to odd on entry, fetch_add to
 *	even on exit, both release-ordered).  A single "page byte" the
 *	writer mutates mid-section models the protected payload; the
 *	reader samples it and re-validates.  If the seqlock protocol in
 *	bufmgr.c drifts (ordering, even/odd discipline), this must be
 *	updated in lockstep.
 *
 *	BOUND: one optimistic reader racing one writer's mutation.  CBMC
 *	explores every interleaving of the version atomics and the page
 *	byte.
 *
 *	Run: cbmc seqlock_harness.c --unwind 6
 */

#include <stdint.h>
#include "xtc_atomic.h"
#include <stddef.h>

/* Minimal frame: only the seqlock version + a modelled "page byte". */
typedef struct bm_frame {
	uint64_t version; /* EVEN stable, ODD while a writer mutates */
	int page;         /* the protected payload (one byte) */
} bm_frame_t;

static bm_frame_t frame;

/* --- bm_read_begin: VERBATIM from bufmgr.c --- */
static int
bm_read_begin(const bm_frame_t *f, uint64_t *out_v)
{
	uint64_t v;
	if (f == NULL) return 0;
	v = xtc_atomic_load_u64(&((bm_frame_t *)f)->version, xtc_mo_acquire);
	if ((v & 1u) != 0)
		return 0;               /* a writer holds it: caller latches */
	*out_v = v;
	return 1;
}

/* --- bm_read_valid: VERBATIM from bufmgr.c --- */
static int
bm_read_valid(const bm_frame_t *f, uint64_t v)
{
	if (f == NULL) return 0;
	/* acquire so the version re-read is ordered after the page reads. */
	return xtc_atomic_load_u64(&((bm_frame_t *)f)->version,
	           xtc_mo_acquire) == v;
}

/* --- writer: version protocol transcribed from bm_frame_wrlatch /
 * bm_frame_unlatch (enter: bump to odd; exit: bump to even; both
 * release-ordered).  The write section mutates the page from a known
 * stable value to a new one, in two steps so a torn mid-write is
 * observable if the reader ever accepted it. --- */
static void
writer(void)
{
	/* Enter write section: version -> odd. */
	(void)xtc_atomic_fetch_add_u64(&frame.version, 1, xtc_mo_release);
	/* Mutate the payload (a reader that accepted this partial state
	 * would see a torn value; the seqlock must reject it). */
	xtc_atomic_store_i32(&frame.page, 2, xtc_mo_relaxed);
	xtc_atomic_store_i32(&frame.page, 3, xtc_mo_relaxed);
	/* Leave write section: version -> even. */
	(void)xtc_atomic_fetch_add_u64(&frame.version, 1, xtc_mo_release);
}

/* --- optimistic reader: the OLC read discipline from the bufmgr
 * header comment (begin -> read bytes into a local -> valid). --- */
static void
reader(void)
{
	uint64_t v;
	int seen;

	if (!bm_read_begin(&frame, &v))
		return;                 /* writer active: fall back to latch */
	/* Read the page bytes into a LOCAL copy. */
	seen = xtc_atomic_load_i32(&frame.page, xtc_mo_relaxed);
	if (bm_read_valid(&frame, v)) {
		/* Accepted: the snapshot MUST be a consistent, untorn
		 * value -- either the pre-write stable value (1) or the
		 * fully-committed new value (3), never the intermediate 2. */
		__CPROVER_assert(seen == 1 || seen == 3,
		    "seqlock accepts only untorn snapshots (no mid-write value)");
	}
}

int
main(void)
{
	xtc_atomic_store_u64(&frame.version, 0, xtc_mo_relaxed); /* even */
	xtc_atomic_store_i32(&frame.page, 1, xtc_mo_relaxed);    /* stable */

	__CPROVER_ASYNC_1: writer();
	reader();
	return 0;
}
