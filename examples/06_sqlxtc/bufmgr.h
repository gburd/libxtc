/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/bufmgr.h
 *	An xtc-native buffer manager modelled on LeanStore: pointer
 *	swizzling (Swip) and cooling-stage eviction.
 *
 *	A Swip is a single 64-bit word held in a parent (a B-tree child
 *	slot, or a root pointer) that encodes one of three states in its
 *	two most significant bits:
 *
 *	    00......  HOT      low 62 bits are a bm_frame* (resident)
 *	    01......  COOL     low 62 bits are a bm_frame* (eviction
 *	                       candidate, parent unswizzled)
 *	    1.......  EVICTED  low 63 bits are a page id (on disk)
 *
 *	Resolving a HOT swip is a pointer load -- no lookup, no lock.  A
 *	COOL swip is rescued back to HOT on access.  An EVICTED swip
 *	triggers a load from the backing file into a free frame.
 *
 *	Eviction is two-phase and proactive: a page-provider process
 *	(an xtc_proc) samples resident frames, UNSWIZZLES cold ones to
 *	COOL (informing the manager which pages are cooling), and writes
 *	the dirty COOL pages out ahead of demand so that reclaiming a
 *	frame later is a cheap state flip rather than a synchronous
 *	write.  Page I/O is offloaded (xtc_blocking) so the loop never
 *	stalls; metrics go to xtc_stats.
 */

#ifndef SQLXTC_BUFMGR_H
#define SQLXTC_BUFMGR_H

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t bm_pid_t;          /* on-disk page id */
typedef struct bm        bm_t;      /* the buffer manager */
typedef struct bm_frame  bm_frame_t;/* a resident page frame */

/* A Swip: one machine word, an eviction-state-tagged reference.  It
 * lives in the parent that points at a page (the test uses a root
 * array; a B-tree uses child slots).  Access it only through the
 * buffer manager. */
typedef _Atomic uint64_t bm_swip_t;

#define BM_PID_NONE  ((bm_pid_t)0)

typedef struct bm_opts {
	const char *path;        /* backing file (created/truncated) */
	uint32_t    page_size;   /* bytes per page (e.g. 4096, 16384) */
	uint32_t    n_frames;    /* resident pool size (frames) */
	uint32_t    cool_pct;    /* target % of frames kept cool/free */
} bm_opts_t;

#define BM_OPTS_DEFAULT \
	{ .path = NULL, .page_size = 4096, .n_frames = 256, .cool_pct = 10 }

/* Lifecycle. */
int  bm_create(const bm_opts_t *opts, bm_t **out);
void bm_destroy(bm_t *bm);

/* Allocate a fresh page.  Installs a HOT swip into *slot and returns
 * the (pinned) frame; the caller fills frame's page and bm_unfix.
 * *out_pid receives the new page id. */
int  bm_alloc(bm_t *bm, bm_swip_t *slot, bm_frame_t **out_frame,
              bm_pid_t *out_pid);

/* Resolve *slot to a resident, pinned frame: a pointer load if HOT, a
 * rescue if COOL, a load-from-disk if EVICTED (which may evict another
 * frame to make room).  Returns XTC_OK and *out_frame on success. */
int  bm_fix(bm_t *bm, bm_swip_t *slot, bm_frame_t **out_frame);

/* Release a frame fixed by bm_alloc/bm_fix.  mark_dirty != 0 records
 * that the page was modified (so it is written before eviction). */
void bm_unfix(bm_t *bm, bm_frame_t *frame, int mark_dirty);

/* The page bytes of a fixed frame, and its page id. */
void    *bm_page(bm_frame_t *frame);
bm_pid_t bm_frame_pid(const bm_frame_t *frame);

/* Spawn the page-provider process on `loop`: it proactively cools and
 * flushes pages so free frames stay available.  Optional -- demand
 * eviction works without it.  Stop it with bm_provider_stop. */
int  bm_provider_spawn(bm_t *bm, xtc_loop_t *loop, int64_t interval_ns,
                       xtc_pid_t *out_pid);
void bm_provider_stop(bm_t *bm);

/* Observability snapshot. */
typedef struct bm_stats {
	uint64_t hits;          /* fix resolved a HOT swip */
	uint64_t rescues;       /* fix rescued a COOL swip */
	uint64_t loads;         /* fix read a page from disk */
	uint64_t cooled;        /* frames unswizzled to COOL */
	uint64_t flushed;       /* dirty COOL pages written out */
	uint64_t evicted;       /* frames reclaimed */
	uint64_t resident;      /* frames currently HOT or COOL */
	uint64_t free_frames;   /* frames on the free list */
} bm_stats_t;
void bm_get_stats(bm_t *bm, bm_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SQLXTC_BUFMGR_H */
