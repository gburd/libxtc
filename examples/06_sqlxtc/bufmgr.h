/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/bufmgr.h
 *	An xtc-native buffer manager: a page-id-addressed pool with
 *	cooling-stage eviction.
 *
 *	Pages are referenced by stable on-disk page ids (bm_pid_t).  A
 *	fix resolves a page id to a resident, pinned frame through an
 *	internal page table (a striped-lock hash); a miss loads the page
 *	from the backing file into a free frame, evicting another frame
 *	if the pool is full.  This is the addressing model the B-tree
 *	uses (its child pointers are page ids serialized into node
 *	cells), and the one a multi-process shared pool requires (a page
 *	id is meaningful under any mapping; an in-memory pointer is not).
 *
 *	Eviction is a CLOCK sweep with a cooling stage: a frame is HOT
 *	when actively used, COOL once it is an eviction candidate, and
 *	reclaimed (its page written first if dirty) when a COOL victim is
 *	found.  With scan resistance a demand-loaded page is admitted COOL
 *	(2Q probation) and promoted to HOT only on a second access, so a
 *	one-touch scan fills and drains the cool stage without displacing
 *	the hot working set.  A background trickler (an xtc_proc) writes
 *	dirty pages out ahead of demand; page I/O is offloaded
 *	(xtc_blocking) so the loop never stalls; metrics go to xtc_stats.
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

#define BM_PID_NONE  ((bm_pid_t)0)

typedef struct bm_opts {
	const char *path;        /* backing file (created/truncated) */
	uint32_t    page_size;   /* bytes per page (e.g. 4096, 16384) */
	uint32_t    n_frames;    /* resident pool size (frames) */
	uint32_t    cool_pct;    /* target % of frames kept cool/free */

	/* Persistence.  reopen != 0 opens an EXISTING backing file without
	 * truncating it (the page contents survive) and resumes page-id
	 * allocation past the file's current pages -- the cold-restart
	 * path.  Zero (the default) creates/truncates a fresh store. */
	uint8_t     reopen;

	/* Scan resistance (cooling stage + 2Q probation).  When set
	 * (the default), a demand-LOADED page is admitted to the COOL
	 * stage, not HOT: it becomes HOT only on a second access (rescue),
	 * and eviction prefers COOL victims, cooling HOT pages only to
	 * refill the cool budget.  So a scan -- which touches each page
	 * once -- fills the cool stage and is evicted from it without ever
	 * displacing the hot working set.  Clear it for the legacy policy
	 * (admit HOT, cool-then-evict in one sweep). */
	uint8_t     scan_resist;

	/* Torn-page protection (double-write).  When set, every page write
	 * goes first to a durable double-write area (full-page logging) and
	 * only then to its final location; a crash that tears the final
	 * write is repaired on reopen from the double-write copy.  Costs an
	 * extra write + fsync per flush, so it is off by default (enable it
	 * for a persistent, crash-safe store; leave it off for a scratch or
	 * pure-in-RAM pool). */
	uint8_t     double_write;

	/* ARIES page LSN.  Byte offset of the 8-byte page LSN within each
	 * page (0 if it is the first field, the btnode convention), or -1
	 * (the default) to disable page-LSN handling entirely.  When set:
	 * (1) the clean->dirty edge stamps the page's LSN with the value
	 * given to bm_set_lsn, and (2) before writing a dirty page the
	 * buffer manager calls the bm_set_wal_flush hook to ensure the log
	 * is durable through that page's LSN -- the write-ahead rule.
	 * Page 0 (the superblock) is never stamped. */
	int         lsn_off;

	/* Direct I/O: open the backing page file with XTC_FS_DIRECT
	 * semantics (cache bypass).  Pages are page_size-aligned and
	 * written at page-aligned offsets, so the alignment contract is
	 * met.  Off by default.  (The double-write file stays buffered --
	 * its records carry unaligned headers.) */
	uint8_t     direct;

	/* Adaptive writeback: the trickler genetically tunes its pacing
	 * (pages per pass + interval) to maximise flush throughput, after
	 * Moilanen's genetic scheduler.  Off by default (fixed pacing). */
	uint8_t     adaptive_writeback;
} bm_opts_t;

#define BM_OPTS_DEFAULT \
	{ .path = NULL, .page_size = 4096, .n_frames = 256, .cool_pct = 10, \
	  .scan_resist = 1, .reopen = 0, .double_write = 0, .lsn_off = -1, \
	  .direct = 0, .adaptive_writeback = 0 }

/* Lifecycle. */
int  bm_create(const bm_opts_t *opts, bm_t **out);
void bm_destroy(bm_t *bm);

/* ARIES page-LSN support (active only when bm_opts.lsn_off >= 0).
 * bm_set_lsn records the LSN stamped onto pages dirtied from here on
 * (the engine sets it to the log LSN of the change about to be made).
 * bm_set_wal_flush registers the write-ahead hook: before writing a
 * dirty page the manager calls flush(ctx, page_lsn) and defers the
 * write if it does not return XTC_OK (the log is not yet durable that
 * far). */
void bm_set_lsn(bm_t *bm, uint64_t lsn);
/* Read back the LSN bm_set_lsn last recorded (the value the next
 * clean->dirty edge will stamp).  Lets a structure-modification log a
 * page's after-image carrying the same LSN the page is stamped with. */
uint64_t bm_get_lsn(bm_t *bm);
void bm_set_wal_flush(bm_t *bm, int (*flush)(void *ctx, uint64_t lsn), void *ctx);

/*
 * Physiological redo: apply a full-page after-image during recovery.
 * Fixes page `pid` and, only if its on-disk page LSN is older than the
 * image's (read at lsn_off), overwrites it and marks it dirty.  Page-LSN
 * gated, hence idempotent -- replaying the same image, or one already
 * superseded, is a no-op.  Requires lsn_off >= 0 and image_len ==
 * page_size.  Returns 1 if applied, 0 if skipped, or a negative XTC_E_*.
 */
int bm_apply_page_image(bm_t *bm, bm_pid_t pid, const void *image,
    uint32_t image_len);

/*
 * Physiological redo gated on an EXTERNAL LSN (the log record's own
 * LSN), not the LSN embedded in the image bytes.  Used when successive
 * after-images of one page share an embedded LSN (e.g. many in-leaf
 * inserts in one group-committed transaction) but each was logged as
 * its own monotonically-numbered WAL record: gating on the record LSN
 * makes the LAST image win.  Applies `image` onto page `pid` only if
 * `apply_lsn` is newer than the page's on-disk LSN, then stamps the
 * page LSN field = `apply_lsn` (so a re-run is idempotent).  Returns 1
 * if applied, 0 if skipped, or a negative XTC_E_*.
 */
int bm_apply_page_image_at(bm_t *bm, bm_pid_t pid, const void *image,
    uint32_t image_len, uint64_t apply_lsn);

/*
 * Stamp `lsn` into the page LSN field (at lsn_off) of the page buffer
 * `page` NOW, in place.  Used by a physiological-logging hook after it
 * has logged the page image and learned the image record's LSN, so the
 * live page carries the same LSN the recovery gate will use.  No-op if
 * lsn_off < 0.
 */
void bm_stamp_lsn(bm_t *bm, void *page, uint64_t lsn);

/*
 * The smallest recLSN among currently dirty pages -- the oldest change
 * not yet on the data file.  The log up to (but not including) this LSN
 * describes only already-written pages and may be truncated.  Returns 0
 * when no page is dirty (the pool imposes no constraint).  Meaningful
 * only when lsn_off >= 0.
 */
uint64_t bm_min_rec_lsn(bm_t *bm);

/* Allocate a fresh page and return its id and a pinned frame; the
 * caller fills the frame's page and bm_unfix.  *out_pid receives the
 * new page id. */
int  bm_alloc_pid(bm_t *bm, bm_frame_t **out_frame, bm_pid_t *out_pid);

/* Resolve a page id to a resident, pinned frame through the internal
 * page table, loading from disk on a miss (which may evict another
 * frame to make room).  Returns XTC_OK and *out_frame on success. */
int  bm_fix_pid(bm_t *bm, bm_pid_t pid, bm_frame_t **out_frame);

/* Reclaim a page id.  The caller guarantees the page is no longer
 * reachable from any live page (its parent separator and any sibling
 * right-link have been rewired away) and holds no other reference to
 * it: the page is unlinked.  bm_free_pid evicts the page's resident
 * frame if any (so a stale resident copy cannot be re-fixed) and puts
 * the id on an in-memory freelist that bm_alloc_pid reissues before
 * growing the file.  Returns XTC_OK, or an error if the id cannot be
 * recorded (in which case the page is simply leaked, never reused). */
int  bm_free_pid(bm_t *bm, bm_pid_t pid);

/* Drain the freed-page quarantine onto the reusable freelist.  Call at
 * a structure-modification epoch boundary (the start of a merge pass)
 * so a pid freed in the previous epoch is only reissued once any
 * latch-free chaser that may have observed it has finished. */
void bm_reclaim_quarantine(bm_t *bm);

/* Read-ahead: request that `pid` be warmed into the pool soon.  Returns
 * immediately (never blocks on I/O); the page-provider drains the
 * request in the background and loads the page (resident, probationary)
 * so a subsequent bm_fix_pid is likely a hit.  Best-effort.  Requires a
 * provider (bm_provider_spawn) to do the warming. */
int  bm_prefetch_pid(bm_t *bm, bm_pid_t pid);

/* Release a frame fixed by bm_alloc_pid/bm_fix_pid.  mark_dirty != 0
 * records that the page was modified (so it is written before
 * eviction). */
void bm_unfix(bm_t *bm, bm_frame_t *frame, int mark_dirty);

/* Stamp a latched frame's page LSN with the current bm_set_lsn value
 * NOW (unconditionally, even if already dirty) and ensure it is marked
 * dirty, so a structure-modification can log the page's after-image
 * carrying the LSN recovery gates the image apply by.  Distinct from
 * the clean->dirty edge (which records only the first dirtying change's
 * recLSN); a following bm_unfix(frame, 1) keeps the stamp. */
void bm_predirty(bm_t *bm, bm_frame_t *frame);

/* The page bytes of a fixed frame, and its page id. */
void    *bm_page(bm_frame_t *frame);
bm_pid_t bm_frame_pid(const bm_frame_t *frame);

/* Per-frame content latch, distinct from the pin (which guards against
 * eviction).  A B-tree holds the pin across an operation and takes the
 * content latch for the brief in-memory node mutation/search; eviction
 * never takes the latch (it gates on the pin), and the latch is never
 * held across page I/O.  bm_latch_shared allows concurrent readers;
 * bm_latch_exclusive is a single writer. */
void bm_latch_shared(bm_frame_t *frame);
void bm_latch_exclusive(bm_frame_t *frame);
void bm_unlatch(bm_frame_t *frame);

/* Spawn the page-provider process on `loop`: it proactively cools and
 * flushes pages so free frames stay available.  Optional -- demand
 * eviction works without it.  Stop it with bm_provider_stop. */
int  bm_provider_spawn(bm_t *bm, xtc_loop_t *loop, int64_t interval_ns,
                       xtc_pid_t *out_pid);
void bm_provider_stop(bm_t *bm);

/* Spawn the trickler process on `loop`: it writes dirty pages out ahead
 * of eviction, choosing COOL (imminent-victim) pages first and, within
 * each class, oldest-dirtied first, a bounded paced batch per pass.
 * So reclaiming a frame is a cheap state flip rather than a synchronous
 * write, and writeback is smoothed instead of bursting.  Optional.
 * Stop it with bm_trickler_stop. */
int  bm_trickler_spawn(bm_t *bm, xtc_loop_t *loop, int64_t interval_ns,
                       xtc_pid_t *out_pid);
void bm_trickler_stop(bm_t *bm);

/* Persistence.  The buffer manager reserves page 0 as a SUPERBLOCK slot
 * (page-id allocation starts at 1), which a higher layer (the B-tree)
 * uses to record its root.  bm_read_super / bm_write_super do a direct,
 * synchronous read/write of up to page_size bytes at file offset 0.
 * bm_sync fdatasyncs the backing file.  bm_checkpoint writes back every
 * dirty page and fdatasyncs -- after it returns, the file durably
 * reflects all committed data, so the WAL prefix may be truncated. */
int  bm_read_super(bm_t *bm, void *buf, size_t len);
int  bm_write_super(bm_t *bm, const void *buf, size_t len);
int  bm_sync(bm_t *bm);
int  bm_checkpoint(bm_t *bm);

/* Observability snapshot. */
typedef struct bm_stats {
	uint64_t hits;          /* fix found the page resident and HOT */
	uint64_t rescues;       /* fix promoted a resident COOL page to HOT */
	uint64_t loads;         /* fix read a page from disk */
	uint64_t cooled;        /* frames flipped HOT -> COOL */
	uint64_t flushed;       /* dirty COOL pages written out */
	uint64_t evicted;       /* frames reclaimed */
	uint64_t resident;      /* frames currently HOT or COOL */
	uint64_t free_frames;   /* frames on the free list */
	uint64_t prefetched;    /* pages warmed by read-ahead */
	uint64_t trickled;      /* dirty pages written ahead by the trickler */
	uint64_t tr_writes;     /* trickler pwrite calls (pages/call = coalescing) */
	uint64_t evict_flushes; /* dirty pages flushed on the foreground eviction
	                         * path (no clean victim was available) */
	uint64_t dirty_backlog; /* frames currently dirty (live gauge) */
	uint64_t tr_passes;     /* trickler passes total */
	uint64_t tr_adaptive;   /* trickler passes that ran the GA tuner */
	uint64_t dw_repaired;   /* pages restored from the double-write on reopen */
	uint64_t freed;         /* page ids put on the reclaim freelist */
	uint64_t reissued;      /* allocations served from the reclaim freelist */
	uint64_t free_pids;     /* page ids currently on the reclaim freelist */
} bm_stats_t;
void bm_get_stats(bm_t *bm, bm_stats_t *out);

/* Debug probe: returns the number of page ids that currently map more
 * than one resident frame -- always 0 in a correct buffer manager (a
 * duplicate is the page-reclamation aliasing race).  Intended for the
 * concurrent-merge stress tests, which assert the result is 0. */
uint32_t bm_dbg_dup_pid(bm_t *bm);

#ifdef __cplusplus
}
#endif

#endif /* SQLXTC_BUFMGR_H */
