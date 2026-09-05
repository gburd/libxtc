/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/bufmgr.c
 *	LeanStore-style buffer manager.  See bufmgr.h.
 *
 *	The Swip in a parent encodes the eviction state; the frame state
 *	tracks it in parallel.  Transitions are serialized by a CAS on
 *	the parent Swip word -- whoever wins the CAS owns the transition,
 *	the loser retries.  A pin count protects a frame from eviction
 *	while a worker holds it.  No thread-blocking lock is held across
 *	the offloaded page I/O, so a parked page-provider never wedges
 *	the loop.
 */

#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE   /* expose fcntl(F_NOCACHE) for direct I/O */
#endif

#include "bufmgr.h"

#include "xtc_aio.h"
#include "xtc_stats.h"
#include "xtc_sync.h"
#include "xtc_dio_sched.h"
#include <stdlib.h>
#include <fcntl.h>

#include <pthread.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* ---- frame states ---- */
enum { BM_FREE = 0, BM_HOT, BM_COOL, BM_LOADED, BM_WRITING };

/* Stage 5 sharing-degree classes (design 7.4).  0 == TRANSIENT so a
 * zeroed frame reads as "scan/unclassified" until the classifier tags
 * it.  Only meaningful when built -DBM_CLASSIFY. */
enum { BM_CLS_TRANSIENT = 0, BM_CLS_COLD, BM_CLS_LOCAL_HOT, BM_CLS_SHARED_HOT };

#ifdef BM_CLASSIFY
/*
 * Sharing-degree classifier (design Planes 1-2, sections 6-7),
 * productionised from numa_claim_probe.  Per-carrier-cpu sketches
 * (doorkeeper Bloom + decayed count-min) fed by the access hook in
 * bm_fix_pid -- core-private writes only (Invariant 1).  Aggregated per
 * NUMA domain on the provider tick; classify() then tags each resident
 * frame's cls_cache.  All heap state hangs off struct bm so multiple
 * pools are independent.
 */
#include <sched.h>
#define BM_CLS_WAYS      4
#define BM_CLS_LOG_SLOTS 15
#define BM_CLS_SLOTS     (1u << BM_CLS_LOG_SLOTS)
#define BM_CLS_GENS      4          /* decay ring */
#define BM_CLS_DK_BITS   17
#define BM_CLS_DK_WORDS  (1u << (BM_CLS_DK_BITS - 6))
#define BM_CLS_MAXCORES  512
#define BM_CLS_MAXDOM    8          /* coherence domains tracked */
/* classification thresholds (design section 13) */
#define BM_CLS_SHARE_MIN_FREQ   4   /* per-domain estimate to count a domain */
#define BM_CLS_SHARED_DEG_MIN   2   /* domains touching => SHARED_HOT */
#define BM_CLS_SHARED_FREQ_MIN  64
#define BM_CLS_LOCAL_FREQ_MIN   8

typedef struct {
	uint8_t  cm[BM_CLS_GENS][BM_CLS_SLOTS];  /* 8-bit count-min per gen */
	uint64_t dk[BM_CLS_DK_WORDS];            /* doorkeeper Bloom (cur gen) */
	uint32_t cur_gen;
	uint64_t last_tick;
} bm_cls_sketch_t;
#endif /* BM_CLASSIFY */

/* Cache line for the descriptor split (Invariant 2).  64 on x86-64 /
 * aarch64; harmless if a target's real line differs (only affects
 * padding, never correctness). */
#ifndef BM_CACHELINE
#define BM_CACHELINE 64
#endif

struct bm_frame {
	/*
	 * CACHE-LINE SPLIT (numa-buffer-pool-design.md section 5, Invariant
	 * 2: no line holds both read-mostly and write-mostly fields).  The
	 * read fast path (ht_lookup_pin_fast + the OLC descent) reads pid /
	 * hnext / page / version on EVERY fix of a resident page; the
	 * evictor / writers write state / pin / dirty / ref rarely.  Keeping
	 * them on separate lines means a reader holding line 0 in S state is
	 * not invalidated by an evictor writing line 1 -- the false sharing
	 * that would otherwise bounce the hottest pages' cache line on every
	 * state write.  Independent of the epoch work; a pure layout win.
	 */

	/* ---- LINE 0: READ-MOSTLY (the fix fast path) ----
	 * pid and hnext are _Atomic so a HIT re-pin of an already-resident
	 * frame can walk the bucket chain lock-free (see ht_lookup_pin_fast):
	 * the writer side (insert on a miss, remove on evict/drop) still
	 * holds the bucket's stripe mutex while it mutates the chain, but
	 * publishes with a release store; a lock-free reader loads with
	 * acquire and re-validates pid after pinning (the same
	 * pin-then-recheck discipline the OLC content-version seqlock below
	 * uses for page bytes).  Frames are never freed -- only recycled in
	 * place across the fixed-size pool -- so a lock-free reader mid-walk
	 * can safely dereference a frame that was just unlinked; it can only
	 * ever observe a STALE pid there (caught by the recheck), never
	 * invalid memory. */
	_Atomic bm_pid_t  pid;
	_Atomic(struct bm_frame *) hnext;   /* page-table hash chain */
	void             *page;       /* page_size bytes (into the pool) */
	xtc_arwlock_t    *latch;      /* content latch (fiber-yielding) */
	_Atomic uint64_t  version;    /* OLC seqlock: EVEN when stable, ODD while
	                               * an exclusive holder is mutating the page.
	                               * An optimistic reader samples it before and
	                               * after reading the page and retries if it
	                               * changed or is odd -- so a lockless read
	                               * never observes a torn mid-write page and
	                               * takes no shared-latch write of its own. */
	struct bm_frame  *next_free;  /* free-list link; touched only when FREE */

	/* ---- LINE 1: WRITE-MOSTLY (evictor / writers) ----
	 * _Alignas(BM_CACHELINE) forces this field onto a fresh cache line,
	 * so the compiler pads line 0 automatically -- no fragile manual
	 * pad arithmetic, and the write-mostly group can never share a line
	 * with the read-mostly group regardless of field sizes. */
	_Alignas(BM_CACHELINE) _Atomic uint8_t   state;
	_Atomic int       pin;        /* >0: a worker holds it; do not evict */
	_Atomic int       io_busy;    /* a write is in flight */
	_Atomic int       dirty;      /* page modified since last write */
	_Atomic int       doomed;     /* reclaimed (bm_free_pid) while pinned: the
	                               * page is unlinked and removed from the
	                               * table, but a worker still holds a pin from
	                               * a read it began before the unlink.  The
	                               * frame cannot be reclaimed under the pin
	                               * (the worker reads valid pre-merge bytes);
	                               * the LAST unpin completes the drop, freeing
	                               * the frame to the pool.  This deferred,
	                               * pin-safe drop is the reclamation interlock:
	                               * a frame is never torn out from under a
	                               * live pin, and no bucket lock is held across
	                               * a blocking wait for the pin to drain. */
	_Atomic int       ref;        /* CLOCK second-chance: set on access,
	                               * cleared by the eviction sweep -- a
	                               * recently used COOL page survives one
	                               * sweep, so a hot page is not evicted
	                               * out from under a fixer about to
	                               * re-pin it (anti-thrash). */
	_Atomic uint64_t  dirty_seq;  /* order it was dirtied (recLSN proxy) */
	_Atomic uint64_t  rec_lsn;    /* WAL LSN that first dirtied it since clean
	                               * (the ARIES recLSN; log truncation horizon) */
	_Atomic uint8_t   cls_cache;  /* stage 5: cached sharing-degree class
	                               * (BM_CLS_*), refreshed on the classify
	                               * tick; advisory input to sample_score.
	                               * On the write-mostly line (rare write,
	                               * batched) -- Invariant 2 safe. */
	char              _pad1[1];   /* keep the write-mostly group padded out;
	                               * the _Alignas on `state` gives the struct
	                               * BM_CACHELINE alignment, so sizeof rounds up
	                               * to a multiple of the line automatically */
};

/* Invariant 2 (numa-buffer-pool-design.md section 5): the write-mostly
 * group must start on a DIFFERENT cache line from the read-mostly group,
 * so an evictor writing state/pin/dirty does not invalidate the line a
 * reader holds for pid/version.  If a field is moved across the split,
 * these fire at compile time. */
_Static_assert(offsetof(struct bm_frame, state) == BM_CACHELINE,
    "bm_frame write-mostly fields must begin on cache line 1");
_Static_assert(offsetof(struct bm_frame, version) < BM_CACHELINE,
    "bm_frame read-mostly version must stay on cache line 0");

/*
 * Page-table lock striping: instead of one global mutex over all hash
 * buckets, an array of cache-line-isolated stripe locks, each guarding
 * the buckets b with (b & (BM_HT_STRIPES-1)) == stripe.  Every table
 * operation touches exactly one bucket, so it locks exactly one stripe;
 * fixes of unrelated pages then proceed in parallel.  The 128-byte
 * union keeps adjacent stripes off the same cache line (no false
 * sharing) regardless of array base alignment.
 *
 * These stripe latches -- and the free-list, page-id, prefetch-ring,
 * and double-write locks below -- are raw pthread mutexes by design,
 * NOT xtc_amutex.  The reason a pool lock would need to park is if it
 * were held across an xtc_aio page-in; this code is audited to release
 * every one of these locks BEFORE any I/O (a miss loads into a free
 * frame and only then re-takes the stripe to publish; the double-write
 * lock only hands out a ring slot and is dropped before the writes).
 * The page-CONTENT latch -- the one thing held across a page-in -- is
 * already an xtc_arwlock.  Since these locks never span a park, a
 * parking amutex would only add ownership-tracking overhead to the
 * hottest path in the engine (measured: ~18ns/fix with pthread vs
 * ~22ns with amutex, a 24% regression) for no loop-safety gain.
 */
#define BM_HT_STRIPES 256u
typedef union { pthread_mutex_t m; char pad[128]; } bm_htlock_t;

struct bm {
	int               fd;
	uint32_t          page_size;
	int               direct;             /* XTC_FS_DIRECT on fd */
	int               adaptive_writeback; /* GA-tuned trickler pacing */
	uint32_t          n_frames;
	uint32_t          cool_target; /* keep this many frames free+cool */
	int               scan_resist; /* probation + COOL-first eviction */
#ifdef BM_CLASSIFY
	/* Stage 5 classifier state (all lazily allocated).  sketch[cpu] is
	 * core-private on the fast path; agg[dom] is the per-domain merged
	 * count-min the classify tick reads. */
	bm_cls_sketch_t  *cls_sketch[BM_CLS_MAXCORES];
	uint32_t         *cls_agg[BM_CLS_MAXDOM];   /* [BM_CLS_SLOTS] each */
	int               cls_ncores;
	int               cls_ndom;
	uint8_t           cls_core_dom[BM_CLS_MAXCORES];
	_Atomic uint64_t  cls_gen_tick;
#endif
	bm_frame_t       *frames;
	unsigned char    *pool;        /* n_frames * page_size, aligned */

	pthread_mutex_t   free_mu;
	bm_frame_t       *free_head;
	_Atomic uint32_t  free_n;

	pthread_mutex_t   pid_mu;
	bm_pid_t          next_pid;

	/* Page-id freelist: page ids reclaimed by bm_free_pid (e.g. the
	 * B-tree freeing a merged-away node), reissued by bm_alloc_pid
	 * before the file is grown.  An in-memory stack guarded by pid_mu
	 * -- the same lock that hands out next_pid -- so allocation pops a
	 * freed pid or bumps next_pid under one lock and a pid is never
	 * issued twice.  Not persisted: a crash leaks the freed pages
	 * (the file does not shrink) but never double-allocates one, which
	 * is the only unsafe outcome.  On a clean reopen the file's tail
	 * pages are simply not on the freelist and stay leaked until the
	 * tree reuses them through fresh splits. */
	bm_pid_t         *free_pids;     /* stack of reclaimed page ids */
	uint32_t          free_pids_n;   /* live entries */
	uint32_t          free_pids_cap; /* allocated slots */
	/* Quarantine: pids freed during the current structure-modification
	 * epoch are parked here, not on free_pids, so a latch-free chaser
	 * that read a now-stale pid before the unlink cannot have it
	 * reissued under it for fresh contents.  bm_reclaim_quarantine
	 * drains them to free_pids at the next epoch boundary (the start of
	 * the next merge), by when any such chaser has completed or retried
	 * -- chasers never park indefinitely on a freed page (drop_resident
	 * already waits out an in-flight pin). */
	bm_pid_t         *quar_pids;
	uint32_t          quar_pids_n;
	uint32_t          quar_pids_cap;
	/* Quarantine membership set (the reclamation interlock).  Every pid
	 * currently quarantined (freed this epoch, not yet reissuable) is a
	 * member.  bm_fix_pid consults it on a table MISS: a quarantined pid
	 * must NOT be re-loaded from its dead on-disk image into a fresh
	 * (phantom) frame -- doing so creates a second frame that aliases the
	 * pid once it is reissued.  A miss on a quarantined pid returns
	 * XTC_E_AGAIN (the latch-free chaser that read the stale pid before
	 * the unlink simply retries its descent; the merge's B-link rewiring
	 * guarantees the retry no longer reaches the freed page).  An
	 * open-addressing table (power-of-two, linear probe) keyed by pid,
	 * guarded by pid_mu (the same lock that owns quar_pids/free_pids).
	 * Tombstone-free: drained en masse by bm_reclaim_quarantine, which
	 * clears the whole set at the epoch boundary. */
	bm_pid_t         *quar_set;      /* open-addressing slots; 0 == empty */
	uint32_t          quar_set_cap;  /* power of two */
	uint32_t          quar_set_n;    /* live members */
	_Atomic uint64_t  s_freed;       /* total pages put on the freelist */
	_Atomic uint64_t  s_reissued;    /* allocations served from the freelist */

	_Atomic uint32_t  clock;       /* round-robin victim cursor */
	_Atomic uint64_t  evict_rng;   /* sampled-eviction PRNG state (stage 2) */

	/* page table (pid mode): pid -> resident frame */
	bm_htlock_t      *ht_locks;    /* BM_HT_STRIPES striped bucket locks;
	                                * still taken for INSERT/REMOVE (the
	                                * chain-structure mutation), but no
	                                * longer for a HIT re-pin -- see
	                                * ht_lookup_pin_fast. */
	_Atomic(bm_frame_t *) *buckets;  /* bucket HEADS: atomic so a lock-free
	                                  * reader's chain walk sees a
	                                  * consistent, in-order-published
	                                  * list (release-published by
	                                  * insert, acquire-read by the
	                                  * fast-path walk). */
	uint32_t          nbucket;

	/* page-provider */
	_Atomic int       pp_running;
	_Atomic int       pp_alive;      /* proc is live; cleared on its exit */
	xtc_pid_t         pp_pid;

	/* trickler: paced, ordered dirty-page writeback */
	_Atomic int       tr_running;
	_Atomic int       tr_alive;      /* proc is live; cleared on its exit */
	xtc_pid_t         tr_pid;
	_Atomic uint64_t  dirty_clock;   /* stamps dirty_seq on clean->dirty */
	_Atomic uint64_t  s_trickled;    /* pages written by the trickler */
	_Atomic uint64_t  s_tr_writes;   /* trickler pwrite calls (coalescing) */
	_Atomic uint64_t  s_evict_flush; /* dirty pages flushed on the foreground
	                                  * eviction path (no clean victim) */
	_Atomic uint32_t  s_dirty_backlog; /* dirty frames seen by the last
	                                    * trickler pass (backlog gauge) */
	_Atomic uint64_t  s_tr_passes;   /* trickler passes (total) */
	_Atomic uint64_t  s_tr_adaptive; /* trickler passes that ran the tuner */

	/* prefetch ring: read-ahead requests, drained by the provider so a
	 * scanning fiber never blocks on a prefetch (best-effort). */
	pthread_mutex_t   pf_mu;
	bm_pid_t          pf_ring[256];
	uint32_t          pf_head, pf_tail;
	_Atomic uint64_t  s_prefetched;

	/* stats */
	_Atomic uint64_t  s_hits, s_rescues, s_loads, s_cooled, s_flushed, s_evicted;
	_Atomic uint64_t  resident;

	/* double-write buffer (torn-page protection); dw_fd < 0 == disabled */
	int               dw_fd;
	pthread_mutex_t   dw_mu;
	uint32_t          dw_slots;      /* ring size in slots */
	uint32_t          dw_next;       /* next slot to claim (round-robin) */
	_Atomic uint64_t  dw_seq;        /* monotonic write sequence */
	_Atomic uint64_t  s_dw_repaired; /* pages restored from the DW on reopen */

	/* ARIES page LSN (active when lsn_off >= 0). */
	int               lsn_off;       /* byte offset of the page LSN, or -1 */
	_Atomic uint64_t  cur_lsn;       /* LSN stamped on the next clean->dirty edge */
	int             (*wal_flush)(void *ctx, uint64_t lsn);  /* write-ahead hook */
	void             *wal_ctx;
};

/* ---- page I/O via libxtc async file ops (xtc_aio): native io_uring
 * completion where available, a blocking-pool offload elsewhere.  The
 * loop is never blocked: the fiber parks until the op completes. ---- */
static int
do_io(bm_t *bm, void *buf, bm_pid_t pid, int write)
{
	off_t off = (off_t)pid * (off_t)bm->page_size;
	int n;
	if (write) {
		n = xtc_aio_pwrite(bm->fd, buf, bm->page_size, off);
		return (n == (int)bm->page_size) ? 0 : -1;
	}
	n = xtc_aio_pread(bm->fd, buf, bm->page_size, off);
	if (n == (int)bm->page_size)
		return 0;
	if (n >= 0) {                          /* short read: zero-fill the tail */
		memset((char *)buf + n, 0, bm->page_size - (size_t)n);
		return 0;
	}
	return -1;
}

/* ---- pin protocol ----
 *
 * The pin count doubles as the eviction gate, so acquiring a pin and
 * reserving a frame for eviction race on ONE atomic word (no separate
 * pin and swip stores that could reorder past each other -- the
 * store/load race that a plain "pin++ then re-check swip" has).  A
 * fixer acquires only if pin >= 0 (CAS pin -> pin+1); the evictor
 * reserves an unpinned frame with CAS(pin 0 -> -1), after which no
 * fixer can pin it.  -1 means EVICTING; free_push resets it to 0. */
static int
try_pin(bm_frame_t *f)
{
	int p = atomic_load_explicit(&f->pin, memory_order_acquire);
	for (;;) {
		if (p < 0)
			return 0;                 /* reserved for eviction */
		if (atomic_compare_exchange_weak_explicit(&f->pin, &p, p + 1,
		    memory_order_acquire, memory_order_acquire))
			return 1;
	}
}
/* Reserve an UNPINNED frame for eviction: pin 0 -> -1.  Returns 1 on
 * success (caller now owns it exclusively), 0 if it is pinned. */
static int
try_reserve(bm_frame_t *f)
{
	int e = 0;
	return atomic_compare_exchange_strong_explicit(&f->pin, &e, -1,
	    memory_order_acquire, memory_order_relaxed);
}

/* Release an eviction reservation made by try_reserve: -1 -> 0, but
 * ONLY if we still hold it.  A plain store would be unsafe: a frame
 * that was concurrently freed and re-popped can have a fresh loader's
 * pin == 1 (the loader's get_free_frame path stores pin = 1 directly,
 * bypassing try_pin), and an unconditional store of 0 would clobber
 * that, leaving the loader's frame at pin == 0 -- a later bm_unfix then
 * underflows it to -1, which is indistinguishable from a reservation
 * and wedges the frame forever (a fixer spins try_pin on it).  The CAS
 * leaves a loader's pin untouched. */
static void
release_reservation(bm_frame_t *f)
{
	int e = -1;
	(void)atomic_compare_exchange_strong_explicit(&f->pin, &e, 0,
	    memory_order_release, memory_order_relaxed);
}

/* ---- free list ---- */
static void
free_push(bm_t *bm, bm_frame_t *f)
{
	/* Mark FREE before clearing the reservation, so an eviction sweep
	 * that is mid-scan sees a non-COOL state and never reserves a frame
	 * that is on its way to the free list. */
	atomic_store_explicit(&f->state, BM_FREE, memory_order_release);
	/* Terminate the hash link so a lock-free fast-path walker that
	 * loaded a stale pointer to this frame (from a chain it was unlinked
	 * from) cannot wander into an unrelated bucket chain: a freed frame
	 * ends any lost-race walk cleanly at NULL.  (Safe either way -- the
	 * walker rechecks pid after pinning -- but this keeps a wandering
	 * walk bounded rather than relying on chains never forming a cycle
	 * across recycling.) */
	atomic_store_explicit(&f->hnext, NULL, memory_order_release);
	/* Leave the frame UNPINNABLE (pin = -1, the eviction-reservation
	 * sentinel) while it sits on the free list, NOT pin = 0.  A lock-free
	 * fast-path reader (ht_lookup_pin_fast / bm_fix_pid_nopin) that loaded
	 * a stale pointer to this just-unlinked frame must NOT be able to
	 * try_pin it: with pin = 0 the reader's CAS(0->1) would succeed, the
	 * pid recheck could pass (pid not yet overwritten), and the loader
	 * that then reuses this free frame would overwrite pid+content under
	 * the reader -> a content mismatch (observed 1-in-N under the higher
	 * eviction churn of sampled eviction; latent with the CLOCK path too).
	 * pin = -1 makes try_pin fail (frame reads as "reserved"), so the
	 * racing reader cleanly takes it as a miss.  The loader clears the
	 * sentinel by storing pin = 1 when it reuses the frame (get_free_frame
	 * caller). */
	atomic_store_explicit(&f->pin, -1, memory_order_release);
	(void)pthread_mutex_lock(&bm->free_mu);
	f->next_free = bm->free_head;
	bm->free_head = f;
	(void)pthread_mutex_unlock(&bm->free_mu);
	atomic_fetch_add_explicit(&bm->free_n, 1, memory_order_relaxed);
}
static bm_frame_t *
free_pop(bm_t *bm)
{
	bm_frame_t *f;
	(void)pthread_mutex_lock(&bm->free_mu);
	f = bm->free_head;
	if (f != NULL) bm->free_head = f->next_free;
	(void)pthread_mutex_unlock(&bm->free_mu);
	if (f != NULL) atomic_fetch_sub_explicit(&bm->free_n, 1, memory_order_relaxed);
	return f;
}

/* ---- double-write buffer: torn-page protection ----
 *
 * Every page write goes first to a slot in a separate double-write
 * file (a full page image plus a [pid, seq] header) which is fsync'd,
 * and only THEN to the page's final location in the main file.  The
 * ring holds the last dw_slots writes.  On reopen the ring is replayed
 * in sequence order onto the main file: a final write the crash left
 * torn is overwritten by its durable, complete double-write copy, and
 * because writes apply oldest-to-newest the latest image of each page
 * wins.  Re-applying an untorn final is a harmless idempotent rewrite.
 * This is InnoDB's double-write; recovery unconditionally re-applies
 * the (known-complete) ring rather than detecting which finals tore, so
 * no in-page checksum is needed.
 */
#define BM_DW_SLOTS 64
#define BM_DW_HDR   16            /* [pid:8][seq:8] per slot */

/* Log `page` (destined for `pid`) to the next double-write slot and make
 * it durable.  Called by flush_frame BEFORE the final write. */
static void
dw_protect(bm_t *bm, bm_pid_t pid, const void *page)
{
	uint8_t hdr[BM_DW_HDR];
	uint64_t seq;
	uint32_t slot;
	off_t off;

	if (bm->dw_fd < 0)
		return;                       /* double-write disabled */
	pthread_mutex_lock(&bm->dw_mu);
	slot = bm->dw_next;
	bm->dw_next = (slot + 1u) % bm->dw_slots;
	pthread_mutex_unlock(&bm->dw_mu);   /* mutex NOT held across the I/O */
	seq = atomic_fetch_add_explicit(&bm->dw_seq, 1, memory_order_relaxed) + 1;
	memcpy(hdr, &pid, 8);
	memcpy(hdr + 8, &seq, 8);
	off = (off_t)slot * (off_t)(BM_DW_HDR + bm->page_size);
	/* Full-page log to the slot, durable BEFORE the final write, via
	 * libxtc async file I/O (native io_uring completion, or offloaded
	 * on a readiness-only backend).  Best-effort like the page writes:
	 * a torn slot just means that page is re-logged at the next flush. */
	(void)xtc_aio_pwrite(bm->dw_fd, hdr, BM_DW_HDR, (int64_t)off);
	(void)xtc_aio_pwrite(bm->dw_fd, page, bm->page_size,
	    (int64_t)off + BM_DW_HDR);
	(void)xtc_aio_fdatasync(bm->dw_fd);
}

/* On reopen, replay the double-write ring onto the main file in sequence
 * order, repairing any torn final write, then clear the ring.  Single-
 * threaded: runs in bm_create before any proc is spawned. */
static void
dw_recover(bm_t *bm)
{
	struct dw_ent { bm_pid_t pid; uint64_t seq; uint32_t slot; } ent[BM_DW_SLOTS];
	uint32_t i, j, n = 0;
	uint8_t *pg;
	size_t slotsz = BM_DW_HDR + bm->page_size;

	if (bm->dw_fd < 0)
		return;
	if ((pg = xtc_malloc(bm->page_size)) == NULL)
		return;
	for (i = 0; i < bm->dw_slots; i++) {
		uint8_t hdr[BM_DW_HDR];
		bm_pid_t pid; uint64_t seq;
		if (pread(bm->dw_fd, hdr, BM_DW_HDR,
		    (off_t)i * (off_t)slotsz) != BM_DW_HDR)
			continue;
		memcpy(&pid, hdr, 8); memcpy(&seq, hdr + 8, 8);
		if (pid == BM_PID_NONE || seq == 0)
			continue;             /* empty slot */
		ent[n].pid = pid; ent[n].seq = seq; ent[n].slot = i; n++;
	}
	for (i = 1; i < n; i++) {        /* insertion sort by seq ascending */
		struct dw_ent tmp = ent[i];
		for (j = i; j > 0 && ent[j - 1].seq > tmp.seq; j--)
			ent[j] = ent[j - 1];
		ent[j] = tmp;
	}
	for (i = 0; i < n; i++) {
		if (pread(bm->dw_fd, pg, bm->page_size,
		    (off_t)ent[i].slot * (off_t)slotsz + BM_DW_HDR)
		    != (ssize_t)bm->page_size)
			continue;
		if (pwrite(bm->fd, pg, bm->page_size,
		    (off_t)ent[i].pid * (off_t)bm->page_size)
		    == (ssize_t)bm->page_size)
			atomic_fetch_add_explicit(&bm->s_dw_repaired, 1,
			    memory_order_relaxed);
	}
	if (n > 0)
		(void)fdatasync(bm->fd);
	if (ftruncate(bm->dw_fd, 0) != 0)
		{ /* best-effort: a stale ring is re-applied idempotently next time */ }
	xtc_free(pg);
}

/* Write a dirty page out, off the loop.  Returns 1 if it is clean
 * afterward.  Snapshots the page under a NON-BLOCKING shared latch so
 * it never writes a torn image while a writer mutates the page, and
 * never blocks on the latch (so it cannot deadlock with the B-tree's
 * latch coupling on the eviction path).  No latch is held across the
 * I/O -- the consistent snapshot is. */
static int
flush_frame(bm_t *bm, bm_frame_t *f)
{
	int expect = 0;
	void *snap;

	if (!atomic_load_explicit(&f->dirty, memory_order_acquire))
		return 1;
	if (!atomic_compare_exchange_strong(&f->io_busy, &expect, 1))
		return 0;                       /* another writer owns it */
	/* Try-shared the content latch: excludes an exclusive writer
	 * (torn-free) but never blocks (deadlock-free).  A writer holding
	 * the page -> skip; it is flushed on a later pass. */
	if (xtc_arwlock_rdlock(f->latch, 0) != XTC_OK) {
		atomic_store_explicit(&f->io_busy, 0, memory_order_release);
		return 0;
	}
	if (!atomic_load_explicit(&f->dirty, memory_order_acquire)) {
		xtc_arwlock_unlock(f->latch);
		atomic_store_explicit(&f->io_busy, 0, memory_order_release);
		return 1;                       /* raced with another flush */
	}
	if ((snap = xtc_aligned_alloc(4096, bm->page_size)) == NULL) {
		xtc_arwlock_unlock(f->latch);
		atomic_store_explicit(&f->io_busy, 0, memory_order_release);
		return 0;
	}
	memcpy(snap, f->page, bm->page_size);
	/* Write-ahead rule: never write a dirty page to disk until the log
	 * is durable through that page's LSN.  If it is not yet, abort this
	 * flush (the page stays dirty) and let a later pass retry once the
	 * log has caught up. */
	if (bm->lsn_off >= 0 && bm->wal_flush != NULL && f->pid != 0) {
		uint64_t plsn;
		memcpy(&plsn, (uint8_t *)snap + bm->lsn_off, sizeof plsn);
		if (bm->wal_flush(bm->wal_ctx, plsn) != XTC_OK) {
			xtc_aligned_free(snap);
			xtc_arwlock_unlock(f->latch);
			atomic_store_explicit(&f->io_busy, 0, memory_order_release);
			return 0;
		}
	}
	/* Clear dirty UNDER the latch: a later writer re-acquires the
	 * exclusive latch and re-dirties on bm_unfix, so its change is not
	 * lost -- the next flush captures it.  The snapshot we write is a
	 * consistent image as of this point. */
	atomic_store_explicit(&f->dirty, 0, memory_order_release);
	xtc_arwlock_unlock(f->latch);
	dw_protect(bm, f->pid, snap);       /* full-page log, durable, BEFORE the final write */
	(void)do_io(bm, snap, f->pid, 1);
	xtc_aligned_free(snap);
	atomic_store_explicit(&f->io_busy, 0, memory_order_release);
	atomic_fetch_add_explicit(&bm->s_flushed, 1, memory_order_relaxed);
	return 1;
}

/* ---- page table ---- */
/* The stripe lock guarding hash bucket `b`. */
static inline pthread_mutex_t *
ht_lock(bm_t *bm, uint32_t b)
{
	return &bm->ht_locks[b & (BM_HT_STRIPES - 1)].m;
}
/*
 * Publish a freshly allocated frame for f->pid, deduping under the
 * bucket lock: if some frame already maps f->pid, do NOT publish a
 * second one (two frames for one pid is the reclamation-race
 * corruption this whole interlock exists to prevent).  Returns the
 * existing frame (pinned) if one was found, in which case the caller
 * reuses it and returns the just-allocated f to the pool; returns NULL
 * (the common, expected case under the quarantine invariant) when f was
 * published cleanly.  This is a belt-and-suspenders guard: the
 * quarantine grace period already guarantees a reissued pid has no
 * resident frame, so the dup branch should never fire -- but if it ever
 * did, deduping here keeps the table single-valued instead of aliasing.
 */
static bm_frame_t *
ht_insert_alloc(bm_t *bm, bm_frame_t *f)
{
	uint32_t b = (uint32_t)(f->pid % bm->nbucket);
	pthread_mutex_t *lk = ht_lock(bm, b);
	bm_frame_t *e;

	(void)pthread_mutex_lock(lk);
	for (e = bm->buckets[b]; e != NULL; e = e->hnext) {
		if (e->pid != f->pid)
			continue;
		if (!try_pin(e))
			break;            /* reserved for eviction: treat as none */
		if (atomic_load_explicit(&e->state, memory_order_acquire) == BM_COOL)
			atomic_store_explicit(&e->state, BM_HOT,
			    memory_order_release);
		(void)pthread_mutex_unlock(lk);
		return e;                 /* dup found: reuse, do not publish f */
	}
	/* Publish: f->pid/f->hnext must be fully written before any
	 * lock-free reader can observe f via the bucket head -- the release
	 * store on the head is what provides that ordering (paired with the
	 * fast path's acquire load in ht_lookup_pin_fast). */
	f->hnext = atomic_load_explicit(&bm->buckets[b], memory_order_relaxed);
	atomic_store_explicit(&bm->buckets[b], f, memory_order_release);
	(void)pthread_mutex_unlock(lk);
	return NULL;
}
static void
ht_remove(bm_t *bm, bm_frame_t *f)
{
	uint32_t b = (uint32_t)(f->pid % bm->nbucket);
	pthread_mutex_t *lk = ht_lock(bm, b);
	bm_frame_t *cur, *prev = NULL;
	(void)pthread_mutex_lock(lk);
	/*
	 * The **pp idiom doesn't typecheck against an _Atomic(bm_frame_t *)
	 * link (an atomic field's address is not a plain bm_frame_t **), so
	 * unlink with explicit prev/cur tracking instead.  Removal is a
	 * release store too: once a lock-free reader's acquire load of the
	 * head/hnext chain can no longer reach f, no later fast-path walk
	 * observes it (an in-flight walk that already loaded f is still
	 * safe -- f is never freed, and the pid recheck after pinning
	 * catches a since-reissued frame; see ht_lookup_pin_fast). */
	cur = atomic_load_explicit(&bm->buckets[b], memory_order_relaxed);
	while (cur != NULL) {
		bm_frame_t *next = atomic_load_explicit(&cur->hnext,
		    memory_order_relaxed);
		if (cur == f) {
			if (prev == NULL)
				atomic_store_explicit(&bm->buckets[b], next,
				    memory_order_release);
			else
				atomic_store_explicit(&prev->hnext, next,
				    memory_order_release);
			break;
		}
		prev = cur;
		cur = next;
	}
	(void)pthread_mutex_unlock(lk);
}
/* Look up pid; if resident, pin it and return the frame (caller holds
 * the pin).  Returns NULL on a miss. */
static bm_frame_t *
ht_lookup_pin(bm_t *bm, bm_pid_t pid)
{
	uint32_t b = (uint32_t)(pid % bm->nbucket);
	pthread_mutex_t *lk = ht_lock(bm, b);
	bm_frame_t *f;
	(void)pthread_mutex_lock(lk);
	for (f = bm->buckets[b]; f != NULL; f = f->hnext) {
		if (f->pid == pid) {
			if (!try_pin(f))
				break;            /* reserved for eviction: treat as a miss */
			if (atomic_load_explicit(&f->state, memory_order_acquire) == BM_COOL)
				atomic_store_explicit(&f->state, BM_HOT, memory_order_release);
			(void)pthread_mutex_unlock(lk);
			return f;
		}
	}
	(void)pthread_mutex_unlock(lk);
	return NULL;
}

/*
 * Lock-free HIT fast path: walk the bucket chain WITHOUT the stripe
 * mutex, using acquire loads on the atomic bucket head / hnext links
 * (published with a release store by the insert/remove paths below),
 * try_pin (already a lock-free CAS), then RE-VALIDATE pid after
 * pinning.
 *
 * Why the recheck is required and sufficient: a frame is never freed
 * (only recycled across the fixed-size pool), so a concurrent walker
 * can safely dereference ANY frame reachable from a head/hnext load,
 * even one just unlinked -- the only hazard is pinning a frame that,
 * between our pid compare and our successful pin, got reissued to a
 * DIFFERENT pid (unlinked, evicted, and re-published for someone
 * else's fix, all while we were mid-CAS).  Re-checking pid after the
 * pin closes that window: if it no longer matches, we unpin and treat
 * this as a miss, exactly like the OLC read path re-validates a page's
 * version after an optimistic read.  A false miss here just falls back
 * to the mutex-holding ht_lookup_pin, which is always correct; this
 * fast path is a pure latency/scalability optimization, never a
 * correctness dependency.
 *
 * Returns the pinned frame on a genuine hit, or NULL (miss OR a raced
 * reissue -- indistinguishable to the caller, which is fine: NULL just
 * means "take the slow path").
 */
static bm_frame_t *
ht_lookup_pin_fast(bm_t *bm, bm_pid_t pid)
{
	uint32_t b = (uint32_t)(pid % bm->nbucket);
	bm_frame_t *f = atomic_load_explicit(&bm->buckets[b], memory_order_acquire);

	while (f != NULL) {
		if (atomic_load_explicit(&f->pid, memory_order_acquire) == pid) {
			if (!try_pin(f)) {
				/* Reserved for eviction right now: fall back to the
				 * mutex path rather than treat this as a hard miss --
				 * ht_lookup_pin's identical break-on-reserved behavior
				 * is what production already relies on here. */
				return NULL;
			}
			/* Re-validate: did this frame get reissued to a DIFFERENT
			 * pid between our compare and our pin winning? */
			if (atomic_load_explicit(&f->pid, memory_order_acquire) != pid) {
				(void)atomic_fetch_sub_explicit(&f->pin, 1,
				    memory_order_release);
				return NULL;    /* raced a reissue; caller retries slow */
			}
			if (atomic_load_explicit(&f->state, memory_order_acquire) == BM_COOL)
				atomic_store_explicit(&f->state, BM_HOT, memory_order_release);
			return f;
		}
		f = atomic_load_explicit(&f->hnext, memory_order_acquire);
	}
	return NULL;   /* not resident (or a lock-free walk that lost a race) */
}

/*
 * bm_fix_pid_nopin -- resolve a resident pid to its frame with NO pin
 * and NO shared-line write (Invariant 1).  Same lock-free chain walk as
 * ht_lookup_pin_fast, but WITHOUT try_pin: the caller validates safety
 * by re-checking pid + the OLC version after reading, not by holding a
 * pin.  Frames are never freed, so the returned pointer is always safe
 * to dereference; the worst case is a stale/reissued frame, caught by
 * the caller's post-read revalidation.  Returns NULL on a miss (caller
 * falls back to the pinned path).
 */
bm_frame_t *
bm_fix_pid_nopin(bm_t *bm, bm_pid_t pid)
{
	uint32_t b;
	bm_frame_t *f;

	if (bm == NULL || pid == BM_PID_NONE)
		return NULL;
	b = (uint32_t)(pid % bm->nbucket);
	f = atomic_load_explicit(&bm->buckets[b], memory_order_acquire);
	while (f != NULL) {
		if (atomic_load_explicit(&f->pid, memory_order_acquire) == pid) {
			/* Do NOT touch state (no COOL->HOT flip): that would be a
			 * shared write on the hit path, defeating the purpose.  A
			 * pin-free reader does not rescue a COOL page; the pinned
			 * path (or a subsequent real fix) does.  Residency for the
			 * duration of this read is guaranteed by revalidation, not
			 * by keeping the page HOT. */
			return f;
		}
		f = atomic_load_explicit(&f->hnext, memory_order_acquire);
	}
	return NULL;
}

#ifdef BM_CLASSIFY
/* ---- sharing-degree classifier (stage 5) ---- */

static inline uint64_t
bm_cls_hash(bm_pid_t x)
{
	x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
	x ^= x >> 27; x *= 0x94D049BB133111EBull;
	x ^= x >> 31;
	return x;
}

static inline int
bm_cls_domain_of_cpu(int cpu)
{
	int n = xtc_numa_node_of_cpu(cpu);
	if (n < 0) n = 0;
	return n % BM_CLS_MAXDOM;
}

/* THE access-hook body: record one page access into THIS carrier's
 * private sketch.  Core-private stores only -- no coherence traffic
 * (Invariant 1).  Called from bm_fix_pid via the BM_ACCESS_PROBE seam. */
static void
bm_cls_record(bm_t *bm, bm_pid_t pid)
{
	int cpu = sched_getcpu();
	bm_cls_sketch_t *s;
	uint64_t h, tick;
	int w;

	if (cpu < 0) cpu = 0;
	if (cpu >= bm->cls_ncores) cpu %= (bm->cls_ncores > 0 ? bm->cls_ncores : 1);
	s = bm->cls_sketch[cpu];
	if (s == NULL) return;

	tick = atomic_load_explicit(&bm->cls_gen_tick, memory_order_relaxed);
	if (tick != s->last_tick) {
		s->last_tick = tick;
		s->cur_gen = (s->cur_gen + 1) & (BM_CLS_GENS - 1);
		memset(s->cm[s->cur_gen], 0, BM_CLS_SLOTS);
		memset(s->dk, 0, sizeof(s->dk));
	}

	h = bm_cls_hash(pid);
	{
		uint32_t b = (uint32_t)(h >> 20) & ((1u << BM_CLS_DK_BITS) - 1);
		uint64_t *dw = &s->dk[b >> 6];
		uint64_t m = 1ull << (b & 63);
		if (*dw & m) {                 /* seen before this gen: count it */
			for (w = 0; w < BM_CLS_WAYS; w++) {
				uint32_t i = (uint32_t)(h >> (w * 13)) & (BM_CLS_SLOTS - 1);
				uint8_t *p = &s->cm[s->cur_gen][i];
				if (*p < 255) (*p)++;
			}
		} else {
			*dw |= m;              /* first sight: doorkeeper only (scan filter) */
		}
	}
}

static const uint8_t bm_cls_decay[BM_CLS_GENS] = { 8, 4, 2, 1 };

static void
bm_cls_aggregate(bm_t *bm)
{
	int d, c;
	uint32_t i, age;
	for (d = 0; d < bm->cls_ndom; d++)
		if (bm->cls_agg[d] != NULL)
			memset(bm->cls_agg[d], 0, sizeof(uint32_t) * BM_CLS_SLOTS);
	for (c = 0; c < bm->cls_ncores; c++) {
		bm_cls_sketch_t *s = bm->cls_sketch[c];
		uint32_t *agg;
		if (s == NULL) continue;
		d = bm->cls_core_dom[c];
		agg = bm->cls_agg[d];
		if (agg == NULL) continue;
		for (age = 0; age < BM_CLS_GENS; age++) {
			uint32_t gi = (s->cur_gen - age) & (BM_CLS_GENS - 1);
			uint8_t wgt = bm_cls_decay[age];
			const uint8_t *cm = s->cm[gi];
			for (i = 0; i < BM_CLS_SLOTS; i++)
				agg[i] += (uint32_t)cm[i] * wgt;
		}
	}
}

static uint32_t
bm_cls_estimate(bm_t *bm, int d, uint64_t h)
{
	uint32_t m = 0xFFFFFFFFu;
	int w;
	const uint32_t *agg = bm->cls_agg[d];
	if (agg == NULL) return 0;
	for (w = 0; w < BM_CLS_WAYS; w++) {
		uint32_t v = agg[(uint32_t)(h >> (w * 13)) & (BM_CLS_SLOTS - 1)];
		if (v < m) m = v;
	}
	return m;
}

static uint8_t
bm_cls_classify(bm_t *bm, bm_pid_t pid)
{
	uint64_t h = bm_cls_hash(pid);
	uint32_t f = 0;
	int deg = 0, d;
	for (d = 0; d < bm->cls_ndom; d++) {
		uint32_t e = bm_cls_estimate(bm, d, h);
		f += e;
		if (e >= BM_CLS_SHARE_MIN_FREQ) deg++;
	}
	if (f == 0) return BM_CLS_TRANSIENT;
	if (deg >= BM_CLS_SHARED_DEG_MIN && f >= BM_CLS_SHARED_FREQ_MIN)
		return BM_CLS_SHARED_HOT;
	if (f >= BM_CLS_LOCAL_FREQ_MIN) return BM_CLS_LOCAL_HOT;
	return BM_CLS_COLD;
}

static void
bm_cls_tick(bm_t *bm)
{
	uint32_t i;
	if (bm->cls_ncores == 0) return;
	bm_cls_aggregate(bm);
	for (i = 0; i < bm->n_frames; i++) {
		bm_frame_t *f = &bm->frames[i];
		uint8_t st = atomic_load_explicit(&f->state, memory_order_acquire);
		if (st != BM_HOT && st != BM_COOL) continue;
		atomic_store_explicit(&f->cls_cache,
		    bm_cls_classify(bm, atomic_load_explicit(&f->pid, memory_order_relaxed)),
		    memory_order_relaxed);
	}
	atomic_fetch_add_explicit(&bm->cls_gen_tick, 1, memory_order_relaxed);
}

static void
bm_cls_ensure(bm_t *bm)
{
	int cpu = sched_getcpu();
	if (cpu < 0) cpu = 0;
	if (cpu >= bm->cls_ncores) return;
	if (bm->cls_sketch[cpu] == NULL) {
		bm_cls_sketch_t *s = calloc(1, sizeof(*s));
		if (s == NULL) return;
		bm->cls_sketch[cpu] = s;   /* benign racy install (single per cpu) */
		bm->cls_core_dom[cpu] = (uint8_t)bm_cls_domain_of_cpu(cpu);
	}
}
#endif /* BM_CLASSIFY */

/*
 * Stage 2 (numa-buffer-pool-design.md section 8.2): SAMPLED power-of-D
 * victim selection, an alternative to the CLOCK sweep in evict_one,
 * opt-in under -DBM_SAMPLED_EVICT.  Instead of a round-robin cursor, it
 * draws D_SAMPLE random frames, scores each (lower = evict first) using
 * the live per-frame signals (recency `ref`, `dirty`, HOT vs COOL), and
 * reclaims the single worst reclaimable one -- power-of-d-choices, which
 * approximates LRU/LFU without a global ordered list or a shared cursor
 * write.  The class/freq/peer score terms from the design's table plug
 * in here once the sharing-degree classifier (stages 5+) tags frames;
 * until then the recency + dirty + state signals are what the pool
 * actually has.  The reserve -> ht_remove -> free_push interlock is
 * IDENTICAL to evict_one's (unchanged correctness).
 */
#ifdef BM_SAMPLED_EVICT
#define BM_D_SAMPLE 16u

/* Lower score = better victim.  Weights follow the design's section-13
 * ratios, scaled to the signals available pre-classifier. */
static int64_t
sample_score(bm_t *bm, bm_frame_t *f)
{
	int64_t s = 0;
	uint8_t st = atomic_load_explicit(&f->state, memory_order_relaxed);
	(void)bm;
	if (st == BM_HOT)   s += 1000;   /* HOT: keep over COOL (W_LOCAL-ish) */
	if (atomic_load_explicit(&f->ref, memory_order_relaxed))
		s += 1000;                   /* recently used: strongly prefer to keep */
	if (atomic_load_explicit(&f->dirty, memory_order_relaxed))
		s += 200;                    /* prefer clean victims (W_DIRTY) */
#ifdef BM_CLASSIFY
	/* Stage 5: sharing-degree class dominates the score (design 8.2 /
	 * section-13 weights).  A SHARED_HOT page (the index root, read by
	 * every domain) is effectively pinned; LOCAL_HOT is protected; a
	 * TRANSIENT (scan) page is evicted first.  cls_cache is refreshed on
	 * the classify tick; 0 (BM_CLS_TRANSIENT) until first classified. */
	switch (atomic_load_explicit(&f->cls_cache, memory_order_relaxed)) {
	case BM_CLS_SHARED_HOT: s += 1000000; break;
	case BM_CLS_LOCAL_HOT:  s += 1000;    break;
	case BM_CLS_TRANSIENT:  s -= 500;     break;
	case BM_CLS_COLD:       default:      break;
	}
#endif
	return s;
}

static int
sample_evict_one(bm_t *bm)
{
	uint64_t rng = atomic_load_explicit(&bm->evict_rng, memory_order_relaxed);
	int tries;

	/*
	 * Scan resistance (design stage 4 intent): a demand-loaded page is
	 * admitted probationally as COOL (the load path), and eviction must
	 * preferentially reclaim those COOL pages, NEVER cooling the HOT
	 * working set while any COOL victim exists -- else a scan trashes the
	 * hot set.  Random sampling alone cannot guarantee "find the sparse
	 * COOL victim" in a small pool, so escalation to cooling a HOT frame
	 * only happens after a bounded DETERMINISTIC sweep confirms there is
	 * genuinely no reclaimable COOL frame (same guarantee the CLOCK path
	 * gives via its full-sweep-before-force_cool).
	 */
	for (tries = 0; tries < (int)(bm->n_frames + 64u); tries++) {
		bm_frame_t *best = NULL;
		int64_t best_score = INT64_MAX;
		bm_pid_t best_pid = BM_PID_NONE;
		unsigned d;

		/* Sample D COOL candidates; score; pick the worst clean one. */
		for (d = 0; d < BM_D_SAMPLE; d++) {
			uint32_t i;
			bm_frame_t *f;
			rng = rng * 6364136223846793005ull + 1442695040888963407ull;
			i = (uint32_t)((rng >> 33) % bm->n_frames);
			f = &bm->frames[i];
			if (atomic_load_explicit(&f->state, memory_order_acquire) != BM_COOL)
				continue;            /* COOL only: never sample HOT here */
			if (atomic_load_explicit(&f->pin, memory_order_acquire) != 0)
				continue;
			if (atomic_load_explicit(&f->io_busy, memory_order_acquire))
				continue;
			if (atomic_load_explicit(&f->dirty, memory_order_acquire))
				continue;            /* prefer clean; trickler handles dirty */
			{
				int64_t sc = sample_score(bm, f);
				if (sc < best_score) {
					best_score = sc; best = f;
					best_pid = atomic_load_explicit(&f->pid,
					    memory_order_acquire);
				}
			}
		}
		if (best != NULL) {
			if (atomic_exchange_explicit(&best->ref, 0, memory_order_relaxed))
				continue;    /* recently used COOL page: spare one round */
			if (!try_reserve(best))
				continue;
			/* Between sampling `best` and reserving it, it could have been
			 * evicted and RELOADED as a different pid (now COOL again).
			 * try_reserve only checks pin==0, so also confirm both the
			 * state AND the pid are unchanged -- else we would evict a
			 * freshly-loaded, valid, different page out from under its
			 * reader (the 1-in-N content mismatch this closes). */
			if (atomic_load_explicit(&best->state, memory_order_acquire) != BM_COOL ||
			    atomic_load_explicit(&best->pid, memory_order_acquire) != best_pid) {
				release_reservation(best);
				continue;
			}
			ht_remove(bm, best);
			atomic_fetch_sub_explicit(&bm->resident, 1, memory_order_relaxed);
			atomic_fetch_add_explicit(&bm->s_evicted, 1, memory_order_relaxed);
			free_push(bm, best);
			atomic_store_explicit(&bm->evict_rng, rng, memory_order_relaxed);
			return 1;
		}

		/* No clean COOL victim in this sample.  Before escalating, do a
		 * bounded DETERMINISTIC pass to MAKE a future clean COOL victim,
		 * cooling exactly one eligible HOT frame (honoring its ref bit).
		 * We never flush inline here -- flushing a frame a concurrent
		 * fixer might re-dirty is a data race; dirty COOL pages are left
		 * for the background trickler, exactly as the CLOCK path's
		 * prefer-clean default does.  HOT is cooled only when the whole
		 * sweep finds no reclaimable clean COOL page, so scan resistance
		 * holds (a scan's probationary COOL pages are always preferred). */
		{
			uint32_t j;
			int made_progress = 0;
			/* First: cool an eligible HOT frame (honoring ref) to make a
			 * future clean COOL victim -- scan resistance holds because a
			 * scan's probationary COOL pages are always preferred above. */
			for (j = 0; j < bm->n_frames; j++) {
				bm_frame_t *f = &bm->frames[j];
				if (atomic_load_explicit(&f->state, memory_order_acquire) != BM_HOT)
					continue;
				if (atomic_load_explicit(&f->pin, memory_order_acquire) != 0)
					continue;
				if (atomic_exchange_explicit(&f->ref, 0, memory_order_relaxed))
					continue;   /* recently used: second chance */
				atomic_store_explicit(&f->state, BM_COOL, memory_order_release);
				atomic_fetch_add_explicit(&bm->s_cooled, 1, memory_order_relaxed);
				made_progress = 1;
				break;
			}
			/* If no HOT frame was coolable, the only victims are dirty COOL
			 * pages -- flush one so it becomes a clean victim next round.
			 * flush_frame is race-safe (CAS io_busy + try-shared latch), so
			 * it never flushes a frame a writer is mutating.  This is the
			 * no-trickler progress guarantee, matching evict_one's
			 * prefer_clean=0 fallback. */
			if (!made_progress) {
				for (j = 0; j < bm->n_frames; j++) {
					bm_frame_t *f = &bm->frames[j];
					if (atomic_load_explicit(&f->state, memory_order_acquire) != BM_COOL)
						continue;
					if (atomic_load_explicit(&f->pin, memory_order_acquire) != 0)
						continue;
					if (!atomic_load_explicit(&f->dirty, memory_order_acquire))
						continue;
					if (flush_frame(bm, f)) {
						atomic_fetch_add_explicit(&bm->s_evict_flush, 1,
						    memory_order_relaxed);
						made_progress = 1;
						break;
					}
				}
			}
			if (!made_progress) {
				atomic_store_explicit(&bm->evict_rng, rng, memory_order_relaxed);
				return 0;   /* nothing reclaimable (all pinned / in flight) */
			}
		}
	}
	atomic_store_explicit(&bm->evict_rng, rng, memory_order_relaxed);
	return 0;
}
#endif /* BM_SAMPLED_EVICT */

/* Try to drive one unpinned frame all the way to FREE.  Returns 1 if a
 * frame was reclaimed.  Swip-mode frames evict by CAS-ing the parent
 * Swip to EVICTED; page-table (pid-mode) frames evict by removing the
 * hash entry.  Dirty pages are written before eviction either way. */
static int
evict_one(bm_t *bm)
{
	uint32_t i, scanned;
	/*
	 * Prefer to reclaim an already-COOL frame; cool a HOT frame only
	 * when a full sweep finds no COOL victim (force_cool).  With
	 * probationary admission a scan supplies plenty of COOL pages, so
	 * the hot set is never cooled to make room for it.  Legacy policy
	 * (scan_resist off) cools eagerly on the first sweep.
	 */
	int force_cool = !bm->scan_resist;
	/*
	 * Decouple the foreground from synchronous writeback: prefer to
	 * reclaim an ALREADY-CLEAN victim and leave dirty pages for the
	 * background trickler, so a fix that misses is not blocked behind a
	 * device write.  Only when a full sweep finds no clean victim do we
	 * fall back to flushing a dirty one (to guarantee progress).
	 */
	int prefer_clean = 1;

	for (;;) {
	  for (scanned = 0; scanned < bm->n_frames * 2u; scanned++) {
		i = atomic_fetch_add_explicit(&bm->clock, 1, memory_order_relaxed)
		    % bm->n_frames;
		bm_frame_t *f = &bm->frames[i];
		uint8_t st = atomic_load_explicit(&f->state, memory_order_acquire);

		if (atomic_load_explicit(&f->pin, memory_order_acquire) != 0)
			continue;

		/* Cooling is a state flip; the page table keeps naming the
		 * page until it is reclaimed. */
		if (st == BM_HOT) {
			if (!force_cool)
				continue;            /* prefer COOL victims */
			atomic_store_explicit(&f->state, BM_COOL, memory_order_release);
			atomic_fetch_add_explicit(&bm->s_cooled, 1, memory_order_relaxed);
			st = BM_COOL;
		}
		if (st != BM_COOL) continue;
		if (atomic_load_explicit(&f->dirty, memory_order_acquire)) {
			if (!prefer_clean) {
				(void)flush_frame(bm, f);  /* fallback write-out */
				atomic_fetch_add_explicit(&bm->s_evict_flush,
				    1, memory_order_relaxed);
			}
			continue;          /* else leave it for the trickler */
		}
		if (atomic_load_explicit(&f->io_busy, memory_order_acquire))
			continue;
		if (atomic_exchange_explicit(&f->ref, 0, memory_order_relaxed))
			continue;            /* recently used: spare one sweep */
		if (!try_reserve(f))
			continue;            /* pinned: a fixer holds it */
		/* Reserved (pin == -1): no fixer can pin it now.  Re-validate
		 * the state under the reservation: a stale COOL read above
		 * could have raced a concurrent free, leaving this frame on
		 * the free list.  ht_remove under the table lock excludes a
		 * concurrent ht_lookup_pin. */
		if (atomic_load_explicit(&f->state, memory_order_acquire)
		    != BM_COOL) {
			release_reservation(f);
			continue;
		}
		ht_remove(bm, f);
		atomic_fetch_sub_explicit(&bm->resident, 1, memory_order_relaxed);
		atomic_fetch_add_explicit(&bm->s_evicted, 1, memory_order_relaxed);
		free_push(bm, f);            /* clears the reservation (pin = 0) */
		return 1;
	  }
	  if (!force_cool) { force_cool = 1; continue; }   /* cool HOT for victims */
	  if (prefer_clean) { prefer_clean = 0; continue; } /* no clean victim:
	                                                     * allow a dirty flush */
	  return 0;          /* nothing reclaimable (all pinned / in flight) */
	}
}

static bm_frame_t *
get_free_frame(bm_t *bm)
{
	bm_frame_t *f;
	int tries;
	/*
	 * A victim is unavailable only transiently: some fiber holds a pin,
	 * or a dirty COOL page is still being written back.  Yield first
	 * (cheap; lets a pin-holder run and unfix), then fall back to a
	 * short park so a CPU-starved box does not burn a core spinning
	 * while the writeback completes.  Eviction always eventually
	 * succeeds (no fiber holds two pins, so there is no circular wait),
	 * so we loop rather than fail -- callers treat a frame as a hard
	 * requirement.
	 */
	for (tries = 0; ; tries++) {
		if ((f = free_pop(bm)) != NULL)
			return f;
#ifdef BM_SAMPLED_EVICT
		/* Sampled power-of-D eviction is the scan-resistant production
		 * policy; it deliberately preserves the hot set (COOL-first,
		 * ref-honoring).  In LEGACY mode (scan_resist off) the pool must
		 * keep its no-probation trashing semantics, so fall through to
		 * the CLOCK evictor there. */
		if (bm->scan_resist ? sample_evict_one(bm) : evict_one(bm))
#else
		if (evict_one(bm))
#endif
			continue;            /* made progress; take it next spin */
		if (tries < 16)
			xtc_yield();         /* fast: let a pin-holder release */
		else if (xtc_proc_sleep(100LL * 1000) != XTC_OK)
			xtc_yield();         /* off a loop: yield */
	}
}

/* ---- prefetch (read-ahead) ----
 *
 * bm_prefetch_pid enqueues a page id the caller expects to need soon;
 * it returns immediately (never blocks on I/O), so a scanning fiber
 * keeps working.  The page-provider drains the ring in the background
 * and warms each page into the pool (resident COOL -- probationary, so
 * a read-ahead that is never used is evicted from the cooling stage
 * without displacing the hot set).  Best-effort: a full ring drops the
 * request, and with no provider running the page simply faults in on
 * demand. */
#define PF_RING 256
int
bm_prefetch_pid(bm_t *bm, bm_pid_t pid)
{
	uint32_t next;
	if (bm == NULL || pid == BM_PID_NONE)
		return XTC_E_INVAL;
	(void)pthread_mutex_lock(&bm->pf_mu);
	next = (bm->pf_tail + 1) % PF_RING;
	if (next != bm->pf_head) {
		bm->pf_ring[bm->pf_tail] = pid;
		bm->pf_tail = next;
	}
	(void)pthread_mutex_unlock(&bm->pf_mu);
	return XTC_OK;
}

/* Warm up to `max` queued prefetch requests into the pool (provider). */
static void
pf_drain(bm_t *bm, int max)
{
	int i;
	for (i = 0; i < max; i++) {
		bm_pid_t pid;
		bm_frame_t *f;
		(void)pthread_mutex_lock(&bm->pf_mu);
		if (bm->pf_head == bm->pf_tail) {
			(void)pthread_mutex_unlock(&bm->pf_mu);
			break;
		}
		pid = bm->pf_ring[bm->pf_head];
		bm->pf_head = (bm->pf_head + 1) % PF_RING;
		(void)pthread_mutex_unlock(&bm->pf_mu);
		if (bm_fix_pid(bm, pid, &f) == XTC_OK) {
			bm_unfix(bm, f, 0);     /* resident, ready for the real fix */
			atomic_fetch_add_explicit(&bm->s_prefetched, 1,
			    memory_order_relaxed);
		}
	}
}

/* ---- public API ---- */
int
bm_create(const bm_opts_t *opts, bm_t **out)
{
	bm_t *bm;
	uint32_t i;
	int rc;

	if (opts == NULL || out == NULL || opts->page_size < 64 ||
	    opts->n_frames < 2)
		return XTC_E_INVAL;
	if ((bm = xtc_calloc(1, sizeof *bm)) == NULL)
		return XTC_E_NOMEM;
	bm->page_size = opts->page_size;
	bm->n_frames = opts->n_frames;
	bm->cool_target = opts->n_frames * (opts->cool_pct ? opts->cool_pct : 10)
	    / 100u;
	if (bm->cool_target < 1) bm->cool_target = 1;
	bm->scan_resist = opts->scan_resist ? 1 : 0;
	atomic_store_explicit(&bm->evict_rng,
	    0x9E3779B97F4A7C15ull ^ ((uint64_t)(uintptr_t)bm),
	    memory_order_relaxed);   /* sampled-eviction PRNG seed (stage 2) */
#ifdef BM_CLASSIFY
	{
		int nc = xtc_ncpus();
		int dd, nd = 0, cc;
		if (nc < 1) nc = 1;
		if (nc > BM_CLS_MAXCORES) nc = BM_CLS_MAXCORES;
		bm->cls_ncores = nc;
		/* domain count = distinct NUMA nodes over the cpus, capped. */
		for (cc = 0; cc < nc; cc++) {
			int d = bm_cls_domain_of_cpu(cc);
			if (d + 1 > nd) nd = d + 1;
		}
		if (nd < 1) nd = 1;
		if (nd > BM_CLS_MAXDOM) nd = BM_CLS_MAXDOM;
		bm->cls_ndom = nd;
		for (dd = 0; dd < nd; dd++)
			bm->cls_agg[dd] = calloc(BM_CLS_SLOTS, sizeof(uint32_t));
	}
#endif
	bm->lsn_off = opts->lsn_off;       /* -1 disables page-LSN handling */
	atomic_store_explicit(&bm->cur_lsn, 0, memory_order_relaxed);
	bm->wal_flush = NULL;
	bm->wal_ctx = NULL;
	(void)pthread_mutex_init(&bm->free_mu, NULL);
	(void)pthread_mutex_init(&bm->pf_mu, NULL);
	(void)pthread_mutex_init(&bm->pid_mu, NULL);

	bm->fd = open(opts->path ? opts->path : "/tmp/sqlxtc-bm.tmp",
	    opts->reopen ? (O_RDWR | O_CREAT) : (O_RDWR | O_CREAT | O_TRUNC), 0644);
	if (bm->fd < 0) { xtc_free(bm); return XTC_E_INVAL; }

	bm->direct = opts->direct ? 1 : 0;
	bm->adaptive_writeback = opts->adaptive_writeback ? 1 : 0;
	if (bm->direct) {
		/* Pages and offsets are page_size-aligned, so the main store
		 * meets the direct-I/O contract; the double-write file stays
		 * buffered (its records carry unaligned headers). */
#if defined(__APPLE__)
		(void)fcntl(bm->fd, F_NOCACHE, 1);
#elif defined(O_DIRECT)
		{
			int fl = fcntl(bm->fd, F_GETFL);
			if (fl >= 0) (void)fcntl(bm->fd, F_SETFL, fl | O_DIRECT);
		}
#endif
	}

	/* Double-write area: a sibling "<path>.dwb" file.  Off unless
	 * requested (it costs an fsync per flush). */
	bm->dw_fd = -1;
	bm->dw_slots = BM_DW_SLOTS;
	bm->dw_next = 0;
	atomic_store(&bm->dw_seq, 0);
	atomic_store(&bm->s_dw_repaired, 0);
	(void)pthread_mutex_init(&bm->dw_mu, NULL);
	if (opts->double_write) {
		char dwpath[1024];
		const char *base = opts->path ? opts->path : "/tmp/sqlxtc-bm.tmp";
		if (snprintf(dwpath, sizeof dwpath, "%s.dwb", base) < (int)sizeof dwpath) {
			bm->dw_fd = open(dwpath,
			    opts->reopen ? (O_RDWR | O_CREAT) : (O_RDWR | O_CREAT | O_TRUNC),
			    0644);
			/* On reopen, repair the main file from the ring before the
			 * B-tree reads any page. */
			if (bm->dw_fd >= 0 && opts->reopen)
				dw_recover(bm);
		}
	}

	/* bm_frame is _Alignas(BM_CACHELINE) (descriptor split, Invariant 2),
	 * so the array base must be cache-line aligned or element 0 is
	 * misaligned (UBSan trap; also defeats the split).  xtc_aligned_alloc
	 * does not zero, so memset the array (the atomic fields rely on a
	 * zero initial state -- BM_FREE == 0, pin 0, etc.).  Pair with
	 * xtc_aligned_free everywhere. */
	if ((bm->frames = xtc_aligned_alloc(BM_CACHELINE,
	    (size_t)bm->n_frames * sizeof *bm->frames)) == NULL)
		{ close(bm->fd); xtc_free(bm); return XTC_E_NOMEM; }
	memset(bm->frames, 0, (size_t)bm->n_frames * sizeof *bm->frames);
	if ((bm->pool = xtc_aligned_alloc(4096,
	    (size_t)bm->n_frames * bm->page_size)) == NULL) {
		xtc_aligned_free(bm->frames); close(bm->fd); xtc_free(bm); return XTC_E_NOMEM;
	}
	for (i = 0; i < bm->n_frames; i++) {
		bm->frames[i].page = bm->pool + (size_t)i * bm->page_size;
		if ((rc = xtc_arwlock_create(&bm->frames[i].latch)) != XTC_OK) {
			xtc_aligned_free(bm->pool); xtc_aligned_free(bm->frames);
			close(bm->fd); xtc_free(bm); return rc;
		}
		free_push(bm, &bm->frames[i]);
	}
	/* page table: next pow2 >= 2*n_frames */
	bm->nbucket = 16;
	while (bm->nbucket < bm->n_frames * 2u) bm->nbucket <<= 1;
	if ((bm->ht_locks = xtc_calloc(BM_HT_STRIPES, sizeof *bm->ht_locks))
	    == NULL) {
		xtc_aligned_free(bm->pool); xtc_aligned_free(bm->frames);
		close(bm->fd); xtc_free(bm); return XTC_E_NOMEM;
	}
	for (i = 0; i < (int)BM_HT_STRIPES; i++)
		(void)pthread_mutex_init(&bm->ht_locks[i].m, NULL);
	if ((bm->buckets = xtc_calloc(bm->nbucket, sizeof *bm->buckets))
	    == NULL) {
		xtc_free(bm->ht_locks);
		xtc_aligned_free(bm->pool); xtc_aligned_free(bm->frames);
		close(bm->fd); xtc_free(bm); return XTC_E_NOMEM;
	}
	bm->next_pid = 1;          /* pid 0 reserved as "none" / superblock */
	bm->free_pids = NULL;      /* page-id reclaim freelist (grown on demand) */
	bm->free_pids_n = 0;
	bm->free_pids_cap = 0;
	atomic_store_explicit(&bm->s_freed, 0, memory_order_relaxed);
	atomic_store_explicit(&bm->s_reissued, 0, memory_order_relaxed);
	if (opts->reopen) {
		/* Resume page-id allocation past the file's existing pages
		 * (page 0 is the superblock; data pages are 1..N-1). */
		off_t end = lseek(bm->fd, 0, SEEK_END);
		if (end > 0) {
			bm_pid_t n = (bm_pid_t)(end / (off_t)bm->page_size);
			if (n > bm->next_pid) bm->next_pid = n;
		}
	}
	*out = bm;
	return XTC_OK;
}

void
bm_destroy(bm_t *bm)
{
	if (bm == NULL) return;
	bm_provider_stop(bm);
	bm_trickler_stop(bm);
	if (bm->fd >= 0) close(bm->fd);
	if (bm->dw_fd >= 0) close(bm->dw_fd);
	(void)pthread_mutex_destroy(&bm->dw_mu);
	if (bm->frames != NULL) {
		uint32_t i;
		for (i = 0; i < bm->n_frames; i++)
			xtc_arwlock_destroy(bm->frames[i].latch);
	}
	(void)pthread_mutex_destroy(&bm->free_mu);
	(void)pthread_mutex_destroy(&bm->pf_mu);
	(void)pthread_mutex_destroy(&bm->pid_mu);
	{
		uint32_t i;
		for (i = 0; i < BM_HT_STRIPES; i++)
			(void)pthread_mutex_destroy(&bm->ht_locks[i].m);
	}
	xtc_free(bm->ht_locks);
	xtc_free(bm->buckets);
	xtc_free(bm->free_pids);
	xtc_free(bm->quar_pids);
	xtc_free(bm->quar_set);
#ifdef BM_CLASSIFY
	{
		int k;
		for (k = 0; k < bm->cls_ncores; k++)
			free(bm->cls_sketch[k]);
		for (k = 0; k < bm->cls_ndom; k++)
			free(bm->cls_agg[k]);
	}
#endif
	xtc_aligned_free(bm->pool);
	xtc_aligned_free(bm->frames);
	xtc_free(bm);
}

/* ---- quarantine membership set (the reclamation interlock) ----
 *
 * Open-addressing, power-of-two, linear-probe hash set keyed by pid,
 * guarded by pid_mu.  Membership marks a pid as quarantined: freed this
 * epoch and not yet reissuable, so bm_fix_pid must refuse to mint a
 * fresh frame from its (dead) on-disk image.  Slot 0 means empty;
 * BM_PID_NONE is never a real data pid, so it is a safe empty marker.
 * All callers hold pid_mu. */
static int
quar_set_resize(bm_t *bm, uint32_t newcap)
{
	bm_pid_t *ns;
	uint32_t i;

	if ((ns = xtc_calloc(newcap, sizeof *ns)) == NULL)
		return XTC_E_NOMEM;
	for (i = 0; i < bm->quar_set_cap; i++) {
		bm_pid_t p = bm->quar_set[i];
		uint32_t h;
		if (p == BM_PID_NONE)
			continue;
		h = (uint32_t)(p & (newcap - 1));
		while (ns[h] != BM_PID_NONE)
			h = (h + 1) & (newcap - 1);
		ns[h] = p;
	}
	xtc_free(bm->quar_set);
	bm->quar_set = ns;
	bm->quar_set_cap = newcap;
	return XTC_OK;
}

/* Add `pid` to the quarantine set.  Returns XTC_OK or XTC_E_NOMEM (the
 * caller then leaks the page rather than mis-reclaiming it). */
static int
quar_set_add(bm_t *bm, bm_pid_t pid)
{
	uint32_t h;

	/* Keep the load factor under 1/2 so linear probing stays cheap. */
	if ((bm->quar_set_n + 1u) * 2u > bm->quar_set_cap) {
		uint32_t newcap = bm->quar_set_cap ? bm->quar_set_cap * 2u : 64u;
		if (quar_set_resize(bm, newcap) != XTC_OK)
			return XTC_E_NOMEM;
	}
	h = (uint32_t)(pid & (bm->quar_set_cap - 1));
	while (bm->quar_set[h] != BM_PID_NONE) {
		if (bm->quar_set[h] == pid)
			return XTC_OK;       /* already a member */
		h = (h + 1) & (bm->quar_set_cap - 1);
	}
	bm->quar_set[h] = pid;
	bm->quar_set_n++;
	return XTC_OK;
}

/* Test membership.  Caller holds pid_mu. */
static int
quar_set_has(bm_t *bm, bm_pid_t pid)
{
	uint32_t h;

	if (bm->quar_set_cap == 0 || bm->quar_set_n == 0)
		return 0;
	h = (uint32_t)(pid & (bm->quar_set_cap - 1));
	while (bm->quar_set[h] != BM_PID_NONE) {
		if (bm->quar_set[h] == pid)
			return 1;
		h = (h + 1) & (bm->quar_set_cap - 1);
	}
	return 0;
}

/* True iff `pid` is quarantined (membership test under pid_mu). */
static int
pid_is_quarantined(bm_t *bm, bm_pid_t pid)
{
	int r;
	(void)pthread_mutex_lock(&bm->pid_mu);
	r = quar_set_has(bm, pid);
	(void)pthread_mutex_unlock(&bm->pid_mu);
	return r;
}

static bm_pid_t
next_pid(bm_t *bm)
{
	bm_pid_t p;
	(void)pthread_mutex_lock(&bm->pid_mu);
	if (bm->free_pids_n > 0) {
		/* Reissue a reclaimed page id before growing the file. */
		p = bm->free_pids[--bm->free_pids_n];
		atomic_fetch_add_explicit(&bm->s_reissued, 1, memory_order_relaxed);
	} else {
		p = bm->next_pid++;
	}
	(void)pthread_mutex_unlock(&bm->pid_mu);
	return p;
}

/* Finish reclaiming a doomed frame: it was removed from the page table
 * by drop_resident while a worker held a pin, so it could not be freed
 * then.  The LAST unpin (here, or the deferred path) reserves it and
 * returns it to the pool.  The frame was already removed from the table
 * and its resident count decremented at drop time, so this only flips
 * the pin reservation and pushes it onto the free list -- it must NOT
 * touch the table (a reissued pid may already name a DIFFERENT live
 * frame in the same bucket). */
static void
finish_doomed_drop(bm_t *bm, bm_frame_t *f)
{
	if (!try_reserve(f))
		return;               /* still pinned, or another path took it */
	atomic_store_explicit(&f->doomed, 0, memory_order_release);
	atomic_store_explicit(&f->dirty, 0, memory_order_release);
	free_push(bm, f);             /* clears the reservation (pin = 0) */
}

/*
 * Drop a resident frame for `pid`, bypassing the writeback path: the
 * page is being reclaimed, so its contents are dead and must NOT be
 * flushed (a flush could resurrect stale bytes under a reissued id).
 * Removes the page-table entry under the bucket's stripe lock so no
 * new fixer can find it.
 *
 * Pin-safe deferred drop: if the frame is UNPINNED, reserve it and free
 * it immediately.  If it is PINNED -- a worker began a read of this
 * page before the caller unlinked it and is finishing with valid
 * pre-merge bytes -- it cannot be freed under the pin, and the bucket
 * lock must not be held across a wait for the pin to drain.  Instead
 * mark the frame `doomed` and remove it from the table; the worker's
 * LAST bm_unfix completes the drop (finish_doomed_drop).  Either way
 * the page is gone from the table on return, so a subsequent fix sees a
 * miss -- and because the pid is now quarantined (the caller added it
 * before calling here), that miss returns XTC_E_AGAIN rather than
 * loading the dead image into a phantom frame.
 *
 * Returns 1 if a resident frame was found (dropped or doomed), 0 if the
 * page was not resident.
 */
static int
drop_resident(bm_t *bm, bm_pid_t pid)
{
	uint32_t b = (uint32_t)(pid % bm->nbucket);
	pthread_mutex_t *lk = ht_lock(bm, b);
	bm_frame_t *f, *cur, *prev;
	int pinned;

	(void)pthread_mutex_lock(lk);
	for (f = bm->buckets[b]; f != NULL; f = f->hnext)
		if (f->pid == pid)
			break;
	if (f == NULL) {
		(void)pthread_mutex_unlock(lk);
		return 0;
	}
	/* Dead page: never flush it (a flush under a reissued id would
	 * resurrect stale bytes). */
	atomic_store_explicit(&f->dirty, 0, memory_order_release);
	/* Remove from the table under the bucket lock so no NEW fixer (which
	 * looks up under the same lock) can reach this frame again; release
	 * store so the lock-free fast path stops seeing it too (an in-flight
	 * fast-path walk that already reached f is caught by its post-pin
	 * pid recheck; see ht_lookup_pin_fast). */
	prev = NULL;
	cur = atomic_load_explicit(&bm->buckets[b], memory_order_relaxed);
	while (cur != NULL) {
		bm_frame_t *next = atomic_load_explicit(&cur->hnext,
		    memory_order_relaxed);
		if (cur == f) {
			if (prev == NULL)
				atomic_store_explicit(&bm->buckets[b], next,
				    memory_order_release);
			else
				atomic_store_explicit(&prev->hnext, next,
				    memory_order_release);
			break;
		}
		prev = cur;
		cur = next;
	}
	atomic_fetch_sub_explicit(&bm->resident, 1, memory_order_relaxed);
	/* Try to reserve it (pin 0 -> -1).  Success means it is unpinned and
	 * we own it now: free it directly.  Failure means a worker holds a
	 * pin; mark it doomed and let the last unpin free it.  Marking
	 * doomed BEFORE releasing the lock, then completing the drop after
	 * the lock is dropped, closes the race with a concurrent bm_unfix
	 * that drives the pin to zero: finish_doomed_drop's try_reserve is
	 * the single arbiter -- exactly one of the two (this path or the
	 * unfix) wins and frees the frame. */
	pinned = !try_reserve(f);
	if (pinned) {
		atomic_store_explicit(&f->doomed, 1, memory_order_release);
		(void)pthread_mutex_unlock(lk);
		finish_doomed_drop(bm, f);
		return 1;
	}
	(void)pthread_mutex_unlock(lk);
	atomic_store_explicit(&f->doomed, 0, memory_order_release);
	free_push(bm, f);            /* clears the reservation (pin = 0) */
	return 1;
}

/* Mark a frame dirty and, on the clean->dirty edge, stamp its page LSN
 * and recLSN.  Shared by bm_unfix (the normal release) and bm_predirty
 * (a latch-held early stamp so the SMO can log the exact bytes that will
 * be written).  Idempotent: the stamp happens only on the first
 * transition, so a re-mark of an already-dirty page keeps its recLSN. */
static void
mark_dirty_edge(bm_t *bm, bm_frame_t *frame)
{
	/* Stamp the dirtying order on the clean -> dirty edge, so the
	 * trickler can write oldest dirt first (a recLSN proxy). */
	if (atomic_exchange_explicit(&frame->dirty, 1, memory_order_acq_rel) == 0) {
		frame->dirty_seq = atomic_fetch_add_explicit(&bm->dirty_clock,
		    1, memory_order_relaxed);
		/* ARIES page LSN: stamp the change's log LSN onto the page.
		 * The engine supplies it via bm_set_lsn before the mutation.
		 * Skip page 0 (the superblock is not a btnode). */
		if (bm->lsn_off >= 0 && frame->pid != 0) {
			uint64_t lsn = atomic_load_explicit(&bm->cur_lsn,
			    memory_order_relaxed);
			memcpy((uint8_t *)frame->page + bm->lsn_off,
			    &lsn, sizeof lsn);
			/* recLSN: the LSN of the change that first dirtied
			 * this page; the log is durable-needed up to here. */
			atomic_store_explicit(&frame->rec_lsn, lsn,
			    memory_order_relaxed);
		}
	}
}

void
bm_unfix(bm_t *bm, bm_frame_t *frame, int mark_dirty)
{
	int prev;

	if (frame == NULL) return;
	if (mark_dirty)
		mark_dirty_edge(bm, frame);
	atomic_store_explicit(&frame->ref, 1, memory_order_relaxed);  /* CLOCK: recently used */
	prev = atomic_fetch_sub_explicit(&frame->pin, 1, memory_order_acq_rel);
	/* Deferred reclamation: if this was the LAST pin (prev == 1, so the
	 * count is now 0) and the frame was doomed by a concurrent
	 * bm_free_pid (page reclaimed while we held the pin), complete the
	 * pin-safe drop now -- the frame is already out of the page table,
	 * so this just returns it to the pool.  finish_doomed_drop's
	 * try_reserve arbitrates against drop_resident's own completion
	 * attempt, so the frame is freed exactly once. */
	if (prev == 1 &&
	    atomic_load_explicit(&frame->doomed, memory_order_acquire))
		finish_doomed_drop(bm, frame);
}

void
bm_predirty(bm_t *bm, bm_frame_t *frame)
{
	if (bm == NULL || frame == NULL)
		return;
	/*
	 * A structure modification is about to log this page's after-image
	 * (physiological redo), so the image must carry the SMO's LSN.
	 * Unlike the clean->dirty edge -- which stamps only the FIRST
	 * dirtying change's LSN (the recLSN) -- stamp cur_lsn UNCONDITIONALLY
	 * here: an SMO page is typically already dirty (just allocated, or
	 * modified by the triggering insert), and its page LSN must advance
	 * to the latest change folded into the image, or recovery's page-LSN
	 * gate would refuse a newer image.  Then take the normal dirty edge
	 * so recLSN/dirty bookkeeping is set if this is the first touch.
	 */
	if (bm->lsn_off >= 0 && frame->pid != 0) {
		uint64_t lsn = atomic_load_explicit(&bm->cur_lsn,
		    memory_order_relaxed);
		memcpy((uint8_t *)frame->page + bm->lsn_off, &lsn, sizeof lsn);
	}
	mark_dirty_edge(bm, frame);
}

void
bm_set_lsn(bm_t *bm, uint64_t lsn)
{
	atomic_store_explicit(&bm->cur_lsn, lsn, memory_order_relaxed);
}

uint64_t
bm_get_lsn(bm_t *bm)
{
	return bm == NULL ? 0
	    : atomic_load_explicit(&bm->cur_lsn, memory_order_relaxed);
}

void
bm_set_wal_flush(bm_t *bm, int (*flush)(void *ctx, uint64_t lsn), void *ctx)
{
	bm->wal_flush = flush;
	bm->wal_ctx = ctx;
}

int
bm_apply_page_image(bm_t *bm, bm_pid_t pid, const void *image, uint32_t image_len)
{
	bm_frame_t *f;
	uint64_t on_disk, in_image;
	int rc;

	if (bm == NULL || image == NULL || bm->lsn_off < 0 ||
	    image_len != bm->page_size)
		return XTC_E_INVAL;
	if ((rc = bm_fix_pid(bm, pid, &f)) != XTC_OK)
		return rc;
	bm_latch_exclusive(f);
	memcpy(&on_disk, (uint8_t *)bm_page(f) + bm->lsn_off, sizeof on_disk);
	memcpy(&in_image, (const uint8_t *)image + bm->lsn_off, sizeof in_image);
	if (in_image > on_disk) {
		memcpy(bm_page(f), image, bm->page_size);   /* redo this page */
		/* bm_unfix stamps page_lsn = cur_lsn on the clean->dirty edge;
		 * point it at the image's own LSN so the stamp preserves it. */
		atomic_store_explicit(&bm->cur_lsn, in_image, memory_order_relaxed);
		bm_unlatch(f);
		bm_unfix(bm, f, 1);                          /* dirty: write it back */
		return 1;
	}
	bm_unlatch(f);
	bm_unfix(bm, f, 0);                                  /* already current */
	return 0;
}

int
bm_apply_page_image_at(bm_t *bm, bm_pid_t pid, const void *image,
    uint32_t image_len, uint64_t apply_lsn)
{
	bm_frame_t *f;
	uint64_t on_disk;
	int rc;

	if (bm == NULL || image == NULL || bm->lsn_off < 0 ||
	    image_len != bm->page_size)
		return XTC_E_INVAL;
	if ((rc = bm_fix_pid(bm, pid, &f)) != XTC_OK)
		return rc;
	bm_latch_exclusive(f);
	memcpy(&on_disk, (uint8_t *)bm_page(f) + bm->lsn_off, sizeof on_disk);
	if (apply_lsn > on_disk) {
		memcpy(bm_page(f), image, bm->page_size);   /* redo this page */
		/* Stamp the page LSN field = the record's LSN so a re-run gates
		 * to a no-op and the write-ahead hook flushes through a real,
		 * durable WAL LSN.  Also point cur_lsn there so bm_unfix's
		 * clean->dirty edge preserves it. */
		memcpy((uint8_t *)bm_page(f) + bm->lsn_off, &apply_lsn,
		    sizeof apply_lsn);
		atomic_store_explicit(&bm->cur_lsn, apply_lsn, memory_order_relaxed);
		bm_unlatch(f);
		bm_unfix(bm, f, 1);                          /* dirty: write it back */
		return 1;
	}
	bm_unlatch(f);
	bm_unfix(bm, f, 0);                                  /* already current */
	return 0;
}

void
bm_stamp_lsn(bm_t *bm, void *page, uint64_t lsn)
{
	if (bm == NULL || page == NULL || bm->lsn_off < 0)
		return;
	memcpy((uint8_t *)page + bm->lsn_off, &lsn, sizeof lsn);
}

uint64_t
bm_min_rec_lsn(bm_t *bm)
{
	uint64_t m = 0;
	int seen = 0;
	uint32_t i;

	if (bm == NULL)
		return 0;
	/* The oldest recLSN among dirty pages: the log before it describes
	 * only changes already on the data file, so it can be truncated.
	 * 0 means no page is dirty (no constraint from the pool). */
	for (i = 0; i < bm->n_frames; i++) {
		bm_frame_t *f = &bm->frames[i];
		if (atomic_load_explicit(&f->dirty, memory_order_acquire)) {
			uint64_t r = atomic_load_explicit(&f->rec_lsn,
			    memory_order_relaxed);
			if (!seen || r < m) { m = r; seen = 1; }
		}
	}
	return seen ? m : 0;
}

int
bm_alloc_pid(bm_t *bm, bm_frame_t **out_frame, bm_pid_t *out_pid)
{
	bm_frame_t *f, *dup;
	if (bm == NULL || out_frame == NULL) return XTC_E_INVAL;
	if ((f = get_free_frame(bm)) == NULL) return XTC_E_RESOURCE;
	atomic_store_explicit(&f->pid, next_pid(bm), memory_order_release);
	atomic_store_explicit(&f->pin, 1, memory_order_relaxed);
	atomic_store_explicit(&f->dirty, 1, memory_order_relaxed);
	atomic_store_explicit(&f->io_busy, 0, memory_order_relaxed);
	atomic_store_explicit(&f->doomed, 0, memory_order_relaxed);
	memset(f->page, 0, bm->page_size);
	atomic_store_explicit(&f->state, BM_HOT, memory_order_release);
	/*
	 * Publish with dedup under the bucket lock.  Under the quarantine
	 * invariant the reissued pid has no resident frame, so the common
	 * path publishes f cleanly (dup == NULL).  If a frame somehow still
	 * maps the pid, reuse it instead of publishing a second -- reinit it
	 * as the fresh page and return f to the pool.  This keeps the page
	 * table strictly single-valued: no pid ever names two frames.
	 */
	dup = ht_insert_alloc(bm, f);
	if (dup != NULL) {
		atomic_store_explicit(&dup->dirty, 1, memory_order_relaxed);
		atomic_store_explicit(&dup->doomed, 0, memory_order_relaxed);
		memset(dup->page, 0, bm->page_size);
		atomic_store_explicit(&dup->state, BM_HOT, memory_order_release);
		free_push(bm, f);          /* spare frame back to the pool */
		if (out_pid) *out_pid = dup->pid;
		*out_frame = dup;
		return XTC_OK;
	}
	atomic_fetch_add_explicit(&bm->resident, 1, memory_order_relaxed);
	if (out_pid) *out_pid = f->pid;
	*out_frame = f;
	return XTC_OK;
}

int
bm_free_pid(bm_t *bm, bm_pid_t pid)
{
	uint32_t cap;
	int rc = XTC_OK;
	void *nb;

	if (bm == NULL || pid == BM_PID_NONE)
		return XTC_E_INVAL;
	/*
	 * Quarantine the id BEFORE dropping its frame.  The membership set
	 * is the reclamation interlock: once `pid` is a member, a fix that
	 * MISSES the table refuses to mint a fresh frame from the dead
	 * on-disk image (it returns XTC_E_AGAIN) -- so the window between
	 * removing the frame from the table (in drop_resident) and a racing
	 * chaser's fix cannot produce a phantom frame.  Record it on the
	 * quar_pids list too, so the next epoch drains it to the reusable
	 * freelist.
	 */
	(void)pthread_mutex_lock(&bm->pid_mu);
	if ((rc = quar_set_add(bm, pid)) != XTC_OK) {
		(void)pthread_mutex_unlock(&bm->pid_mu);
		return rc;            /* page leaked, never double-allocated */
	}
	if (bm->quar_pids_n == bm->quar_pids_cap) {
		cap = bm->quar_pids_cap ? bm->quar_pids_cap * 2u : 64u;
		nb = xtc_realloc(bm->quar_pids,
		    (size_t)cap * sizeof *bm->quar_pids);
		if (nb == NULL) {
			(void)pthread_mutex_unlock(&bm->pid_mu);
			return XTC_E_NOMEM;   /* page leaked, never double-allocated */
		}
		bm->quar_pids = nb;
		bm->quar_pids_cap = cap;
	}
	bm->quar_pids[bm->quar_pids_n++] = pid;   /* parked, not yet reusable */
	(void)pthread_mutex_unlock(&bm->pid_mu);

	/*
	 * Now drop any resident copy: a stale resident frame for `pid` must
	 * not survive to be re-fixed once the id is reissued.  The caller
	 * has unlinked the page, so no live pointer reaches it; a worker
	 * that holds a pin from a read begun before the unlink keeps a valid
	 * pre-merge image and finishes shortly, at which point its last
	 * unpin completes the pin-safe deferred drop.
	 */
	(void)drop_resident(bm, pid);

	atomic_fetch_add_explicit(&bm->s_freed, 1, memory_order_relaxed);
	return XTC_OK;
}

/*
 * Drain the quarantine: move pids freed in the previous epoch onto the
 * reusable freelist.  Called at a structure-modification epoch
 * boundary (the start of a merge), by when any latch-free chaser that
 * observed a now-freed pid has finished -- so reissuing it for fresh
 * contents can no longer mislead an in-flight operation.
 *
 * Draining clears the quarantine membership set as well: the pids are
 * now reissuable, so bm_fix_pid may once again load them (a reissue
 * installs a fresh frame, and the dedup in ht_insert/bm_alloc_pid
 * guarantees only one frame ever maps the reissued pid).
 */
void
bm_reclaim_quarantine(bm_t *bm)
{
	bm_pid_t *grow;
	uint32_t cap, need, i;

	if (bm == NULL)
		return;
	(void)pthread_mutex_lock(&bm->pid_mu);
	if (bm->quar_pids_n == 0) {
		(void)pthread_mutex_unlock(&bm->pid_mu);
		return;
	}
	need = bm->free_pids_n + bm->quar_pids_n;
	if (need > bm->free_pids_cap) {
		cap = bm->free_pids_cap ? bm->free_pids_cap : 64u;
		while (cap < need)
			cap *= 2u;
		grow = xtc_realloc(bm->free_pids,
		    (size_t)cap * sizeof *bm->free_pids);
		if (grow == NULL) {
			/* Out of memory: leave them quarantined (still safe;
			 * the pages stay leaked until a later drain). */
			(void)pthread_mutex_unlock(&bm->pid_mu);
			return;
		}
		bm->free_pids = grow;
		bm->free_pids_cap = cap;
	}
	grow = bm->free_pids;
	for (i = 0; i < bm->quar_pids_n; i++)
		grow[bm->free_pids_n++] = bm->quar_pids[i];
	bm->quar_pids_n = 0;
	/* The whole epoch's worth of pids is now reissuable: clear the
	 * membership set so a fresh fix/load of any of them is allowed
	 * again.  Clearing the slot array (not freeing it) keeps the
	 * allocation for the next epoch. */
	if (bm->quar_set_cap > 0)
		memset(bm->quar_set, 0,
		    (size_t)bm->quar_set_cap * sizeof *bm->quar_set);
	bm->quar_set_n = 0;
	(void)pthread_mutex_unlock(&bm->pid_mu);
}

int
bm_fix_pid(bm_t *bm, bm_pid_t pid, bm_frame_t **out_frame)
{
	bm_frame_t *f;
	if (bm == NULL || out_frame == NULL || pid == BM_PID_NONE)
		return XTC_E_INVAL;
#ifdef BM_CLASSIFY
	/* Stage 5: feed the sharing-degree classifier at the single access
	 * choke point -- core-private sketch writes only (Invariant 1). */
	bm_cls_ensure(bm);
	bm_cls_record(bm, pid);
#endif
#ifdef BM_ACCESS_PROBE
	/* Research spike (numa-buffer-pool-design.md sharing-degree premise):
	 * observe every page access at the single choke point.  Zero-cost
	 * and absent unless the probe build defines BM_ACCESS_PROBE; a weak
	 * hook the probe program supplies.  Not part of the shipped bufmgr. */
	extern void bm_access_probe(bm_pid_t pid) __attribute__((weak));
	if (bm_access_probe)
		bm_access_probe(pid);
#endif
	for (;;) {
		/* Lock-free hit path first (no bucket-stripe mutex): this is
		 * what removes the read-scaling ceiling item 1a measured --
		 * re-pinning an already-resident (especially hot/root) frame
		 * no longer serializes every reader on that page's bucket
		 * stripe.  Falls back to the mutex-holding slow path on a
		 * genuine miss OR a raced reissue (rare; always correct
		 * either way -- see ht_lookup_pin_fast's comment). */
		if ((f = ht_lookup_pin_fast(bm, pid)) != NULL) {
			atomic_fetch_add_explicit(&bm->s_hits, 1, memory_order_relaxed);
			*out_frame = f;
			return XTC_OK;
		}
		if ((f = ht_lookup_pin(bm, pid)) != NULL) {
			atomic_fetch_add_explicit(&bm->s_hits, 1, memory_order_relaxed);
			*out_frame = f;
			return XTC_OK;
		}
		/* Miss: the page is not resident.  If its id is quarantined
		 * (freed this epoch, awaiting the grace period), refuse to mint
		 * a fresh frame from the dead on-disk image -- that phantom
		 * frame would alias the id once it is reissued.  Tell the caller
		 * to retry; a latch-free chaser that read the stale id before
		 * the unlink re-descends, and the merge's B-link rewiring keeps
		 * the retry from reaching the freed page.  By the next epoch the
		 * id is drained out of the set and reloadable again. */
		if (pid_is_quarantined(bm, pid))
			return XTC_E_AGAIN;
		/* Load into a free frame, then publish in the table.
		 * A concurrent loader of the same pid is resolved by re-checking
		 * the table after acquiring a frame. */
		f = get_free_frame(bm);
		if (f == NULL) return XTC_E_RESOURCE;
		/* No stale fixer can transiently pin this frame -- a plain store
		 * of the pin is safe here. */
		atomic_store_explicit(&f->pin, 1, memory_order_release);
		atomic_store_explicit(&f->state, BM_LOADED, memory_order_relaxed);
		/* Bump the frame version across a (re)load so the OLC seqlock
		 * ALSO detects a content reissue, not just an in-place write.
		 * This is what lets a PIN-FREE epoch reader
		 * (bm_fix_pid_nopin / bt_lookup_optimistic) validate safety by
		 * version alone: a frame evicted out from under it and reloaded
		 * as a different pid changes version, so bm_read_valid fails and
		 * the reader retries.  +2 keeps it even (stable) for the next
		 * reader.  Rare path (a miss), so this write to the read-mostly
		 * line is not on the hot hit path. */
		atomic_fetch_add_explicit(&f->version, 2, memory_order_release);
		atomic_store_explicit(&f->pid, pid, memory_order_release);
		atomic_store_explicit(&f->dirty, 0, memory_order_relaxed);
		atomic_store_explicit(&f->doomed, 0, memory_order_relaxed);
		atomic_store_explicit(&f->io_busy, 0, memory_order_relaxed);
		if (do_io(bm, f->page, pid, 0) != 0) { free_push(bm, f); return XTC_E_INTERNAL; }
		/* Publish: under the bucket's stripe lock, re-check no one beat us. */
		{
			uint32_t b = (uint32_t)(pid % bm->nbucket);
			pthread_mutex_t *lk = ht_lock(bm, b);
			bm_frame_t *e;
			(void)pthread_mutex_lock(lk);
			for (e = bm->buckets[b]; e != NULL; e = e->hnext)
				if (e->pid == pid) break;
			if (e != NULL) {
				/* Lost the race; use the resident frame if we can pin it
				 * (it may be reserved for eviction -- then retry as a
				 * miss). */
				int pinned = try_pin(e);
				if (pinned &&
				    atomic_load_explicit(&e->state, memory_order_acquire) == BM_COOL)
					atomic_store_explicit(&e->state, BM_HOT, memory_order_release);
				(void)pthread_mutex_unlock(lk);
				free_push(bm, f);
				if (!pinned)
					continue;            /* being evicted: retry the lookup */
				*out_frame = e;
				atomic_fetch_add_explicit(&bm->s_hits, 1, memory_order_relaxed);
				return XTC_OK;
			}
			/* Re-check quarantine UNDER the bucket lock: a concurrent
			 * bm_free_pid may have quarantined and dropped this pid while
			 * we were loading from disk.  Publishing now would recreate
			 * exactly the phantom frame the interlock prevents.  pid_mu
			 * nests inside the bucket lock safely (bm_free_pid releases
			 * pid_mu before taking any bucket lock, so the two never nest
			 * the other way). */
			if (pid_is_quarantined(bm, pid)) {
				(void)pthread_mutex_unlock(lk);
				free_push(bm, f);
				return XTC_E_AGAIN;
			}
			/* Publish: f->pid was stored before this (see above), and
			 * the release store on the bucket head orders it before any
			 * lock-free reader can observe f (ht_lookup_pin_fast). */
			f->hnext = atomic_load_explicit(&bm->buckets[b],
			    memory_order_relaxed);
			atomic_store_explicit(&bm->buckets[b], f, memory_order_release);
			/* Probationary admission: a demand-loaded page enters
			 * COOL (LeanStore cooling stage / 2Q A1), promoted to
			 * HOT only on a second access (ht_lookup_pin rescue).
			 * A scan touches each page once, so its pages never
			 * displace the hot working set.  Pin BEFORE publishing
			 * the COOL state so a concurrent evict_one (whose
			 * try_reserve is not under a stripe lock) cannot reserve and
			 * race the pin store. */
			atomic_store_explicit(&f->pin, 1, memory_order_release);
			atomic_store_explicit(&f->state,
			    bm->scan_resist ? BM_COOL : BM_HOT, memory_order_release);
			(void)pthread_mutex_unlock(lk);
		}
		atomic_fetch_add_explicit(&bm->resident, 1, memory_order_relaxed);
		atomic_fetch_add_explicit(&bm->s_loads, 1, memory_order_relaxed);
		*out_frame = f;
		return XTC_OK;
	}
}

void *
bm_page(bm_frame_t *frame) { return frame ? frame->page : NULL; }

bm_pid_t
bm_frame_pid(const bm_frame_t *frame) { return frame ? frame->pid : BM_PID_NONE; }

/*
 * Debug probe (the reclamation-race oracle).  Scans every page-table
 * bucket and returns the number of pids that map MORE than one resident
 * frame -- which must always be zero: a single pid naming two frames is
 * exactly the reclamation race the interlock prevents (a fix would then
 * hand divergent page copies to different callers).  Takes each stripe
 * lock in turn (every bucket guarded by its stripe), so it observes a
 * consistent per-bucket view; cross-bucket it is a best-effort scan,
 * but a pid hashes to exactly one bucket, so a duplicate is always
 * visible within the one bucket it would land in.  Intended only for
 * tests (assert the return is 0); cheap enough to call between rounds.
 */
uint32_t
bm_dbg_dup_pid(bm_t *bm)
{
	uint32_t b, dups = 0;

	if (bm == NULL)
		return 0;
	for (b = 0; b < bm->nbucket; b++) {
		pthread_mutex_t *lk = ht_lock(bm, b);
		bm_frame_t *f, *g;

		(void)pthread_mutex_lock(lk);
		for (f = bm->buckets[b]; f != NULL; f = f->hnext) {
			int seen = 0;
			for (g = bm->buckets[b]; g != f; g = g->hnext)
				if (g->pid == f->pid) { seen = 1; break; }
			if (!seen) {
				/* count occurrences of f->pid in this bucket */
				int n = 0;
				for (g = bm->buckets[b]; g != NULL; g = g->hnext)
					if (g->pid == f->pid) n++;
				if (n > 1)
					dups++;
			}
		}
		(void)pthread_mutex_unlock(lk);
	}
	return dups;
}

void bm_latch_shared(bm_frame_t *f)    { if (f) (void)xtc_arwlock_rdlock(f->latch, -1); }
void bm_latch_exclusive(bm_frame_t *f)
{
	if (f) {
		(void)xtc_arwlock_wrlock(f->latch, -1);
		/* Enter the write section: make the version ODD so a
		 * concurrent optimistic reader that samples it now retries. */
		atomic_fetch_add_explicit(&f->version, 1, memory_order_release);
	}
}
void bm_unlatch(bm_frame_t *f)
{
	if (!f) return;
	/* If this was an EXCLUSIVE hold the version is odd; bump it back to
	 * even (write section complete) with release ordering so an
	 * optimistic reader that then samples an even version is guaranteed
	 * to see all the page writes.  A shared-latch release finds it even
	 * already -- no-op.  (Only the holder unlatches, so this read of
	 * version is not racing another writer on this frame.) */
	if ((atomic_load_explicit(&f->version, memory_order_relaxed) & 1u) != 0)
		atomic_fetch_add_explicit(&f->version, 1, memory_order_release);
	(void)xtc_arwlock_unlock(f->latch);
}

/* ---- OLC (optimistic latch coupling) read-side helpers ----
 *
 * A reader that only READS a resident, pinned page can avoid the shared
 * content latch (whose acquire/release is a contended write to the
 * latch word -- the read-scalability wall measured by
 * bench_btree_concurrent).  Instead it brackets the read with the
 * frame's version seqlock:
 *
 *   uint64_t v;
 *   if (!bm_read_begin(f, &v)) { take the shared latch (writer active) }
 *   ... read page bytes into a LOCAL copy ...
 *   if (!bm_read_valid(f, v)) { retry / fall back to the shared latch }
 *
 * bm_read_begin fails (returns 0) when a writer currently holds the
 * frame (odd version); the caller then falls back to the blocking
 * shared latch rather than spin.  bm_read_valid returns 1 iff the
 * version is unchanged since bm_read_begin -- i.e. no writer mutated
 * the page during the read, so the bytes the reader saw are a
 * consistent snapshot.  The reader MUST still hold a pin on the frame
 * across the read (so it cannot be evicted); OLC removes only the
 * content latch, not the pin. */
int
bm_read_begin(const bm_frame_t *f, uint64_t *out_v)
{
	uint64_t v;
	if (f == NULL) return 0;
	v = atomic_load_explicit(&((bm_frame_t *)f)->version,
	    memory_order_acquire);
	if ((v & 1u) != 0)
		return 0;               /* a writer holds it: caller latches */
	*out_v = v;
	return 1;
}

int
bm_read_valid(const bm_frame_t *f, uint64_t v)
{
	if (f == NULL) return 0;
	/* acquire so the version re-read is ordered after the page reads. */
	return atomic_load_explicit(&((bm_frame_t *)f)->version,
	    memory_order_acquire) == v;
}

/* ---- page-provider process ---- */
struct pp_arg { bm_t *bm; int64_t interval; };

static void
pp_proc(void *arg)
{
	struct pp_arg *pa = arg;
	bm_t *bm = pa->bm;
	int64_t interval = pa->interval;
	xtc_free(pa);

	while (atomic_load_explicit(&bm->pp_running, memory_order_acquire)) {
		uint32_t i, cool_clean = 0, need;
		pf_drain(bm, 32);     /* warm queued read-ahead pages first */
		/* Pass 1: flush dirty COOL frames (so reclaiming them later is a
		 * cheap state flip) and count the clean cool supply. */
		for (i = 0; i < bm->n_frames; i++) {
			bm_frame_t *f = &bm->frames[i];
			uint8_t st = atomic_load_explicit(&f->state, memory_order_acquire);
			if (atomic_load_explicit(&f->pin, memory_order_acquire) != 0)
				continue;
			if (st != BM_COOL)
				continue;
			if (atomic_load_explicit(&f->dirty, memory_order_acquire))
				(void)flush_frame(bm, f);   /* proactive write-out */
			else
				cool_clean++;
		}
		/* Pass 2: top the cool stage up toward the target by cooling
		 * HOT frames -- but only the shortfall.  An abundant supply of
		 * probationary/scan COOL pages keeps need == 0, so a scan does
		 * not provoke cooling of the hot working set. */
		need = cool_clean < bm->cool_target ? bm->cool_target - cool_clean : 0;
		for (i = 0; i < bm->n_frames && need > 0; i++) {
			bm_frame_t *f = &bm->frames[i];
			uint8_t st = atomic_load_explicit(&f->state, memory_order_acquire);
			if (atomic_load_explicit(&f->pin, memory_order_acquire) != 0)
				continue;
			if (st != BM_HOT)
				continue;
			/* Cooling is a state flip; the page table keeps naming
			 * the page until it is reclaimed. */
			atomic_store_explicit(&f->state, BM_COOL,
			    memory_order_release);
			atomic_fetch_add_explicit(&bm->s_cooled, 1,
			    memory_order_relaxed);
			need--;
		}
		/* Pass 3: keep the free list above the cool target. */
		while (atomic_load_explicit(&bm->free_n, memory_order_relaxed)
		    < bm->cool_target) {
#ifdef BM_SAMPLED_EVICT
			if (!(bm->scan_resist ? sample_evict_one(bm) : evict_one(bm))) break;
#else
			if (!evict_one(bm)) break;
#endif
		}
#ifdef BM_CLASSIFY
		/* Stage 5: aggregate the per-carrier sketches and re-tag every
		 * resident frame's class on the provider cadence. */
		bm_cls_tick(bm);
#endif
		if (xtc_proc_sleep(interval) != XTC_OK)
			break;
	}
	atomic_store_explicit(&bm->pp_alive, 0, memory_order_release);
}

int
bm_provider_spawn(bm_t *bm, xtc_loop_t *loop, int64_t interval_ns,
                  xtc_pid_t *out_pid)
{
	struct pp_arg *pa;
	xtc_proc_opts_t opts = { 0 };
	int rc;
	if (bm == NULL || loop == NULL) return XTC_E_INVAL;
	if ((pa = xtc_calloc(1, sizeof *pa)) == NULL) return XTC_E_NOMEM;
	pa->bm = bm;
	pa->interval = interval_ns > 0 ? interval_ns : 5LL * 1000 * 1000;
	atomic_store_explicit(&bm->pp_running, 1, memory_order_release);
	atomic_store_explicit(&bm->pp_alive, 1, memory_order_release);
	opts.name = "bm-provider";
	rc = xtc_proc_spawn(loop, pp_proc, pa, &opts, &bm->pp_pid);
	if (rc != XTC_OK) { atomic_store_explicit(&bm->pp_running, 0, memory_order_release); atomic_store_explicit(&bm->pp_alive, 0, memory_order_release); xtc_free(pa); return rc; }
	if (out_pid) *out_pid = bm->pp_pid;
	return XTC_OK;
}

/* ---- trickler: paced, ordered dirty-page writeback ----
 *
 * A dedicated process that writes dirty pages out AHEAD of eviction so
 * that reclaiming a frame is a cheap state flip, never a synchronous
 * write on the fault path.  It uses the eviction algorithm's own view
 * of the pool to choose WHAT and in WHICH ORDER:
 *   1. COOL dirty pages first -- COOL is the cooling stage's "about to
 *      be evicted" mark, so cleaning them directly unblocks eviction.
 *   2. within each class, oldest-dirtied first (dirty_seq, a recLSN
 *      proxy) -- the ARIES order that lets the log truncate sooner,
 *      since the oldest dirty page pins the recovery start point.
 * It writes only a bounded batch per pass and sleeps between (the
 * "trickle"): writeback is smoothed over time instead of bursting, so
 * it does not spike I/O latency for foreground operations.
 *
 * WAL safety: in the log-before-apply commit protocol a page is
 * dirtied only after the commit that produced it is durable, so the
 * write-ahead rule (log durable past the page's changes before the
 * page is written) is already satisfied here; an explicit pageLSN
 * gate belongs with the physiological-logging stage. */
#define TR_BATCH 16
struct tr_cand { bm_frame_t *f; uint8_t cool; uint64_t seq; };
static int
tr_cmp(const void *a, const void *b)
{
	const struct tr_cand *x = a, *y = b;
	if (x->cool != y->cool) return (int)y->cool - (int)x->cool;  /* COOL first */
	if (x->seq < y->seq) return -1;
	if (x->seq > y->seq) return 1;
	return 0;
}
struct tr_arg { bm_t *bm; int64_t interval; };

/* Elevator ordering: sort a selected batch by on-disk page number so the
 * device sees ascending offsets (sequential, mergeable) instead of
 * dirty-recency order. */
static int
tr_cmp_pid(const void *a, const void *b)
{
	const struct tr_cand *x = a, *y = b;
	if (x->f->pid < y->f->pid) return -1;
	if (x->f->pid > y->f->pid) return 1;
	return 0;
}

/* Claim a dirty frame for writeback and snapshot it into `dst` (which
 * the caller owns; for coalescing it is a slot in a larger gather
 * buffer).  Mirrors flush_frame's claim protocol -- io_busy CAS, a
 * non-blocking shared latch, the write-ahead-log gate, and clearing
 * dirty under the latch -- but DEFERS the device write so the caller
 * can batch several pages into one pwrite.  Returns 1 when the page is
 * claimed (io_busy is held; the caller MUST write it and then release
 * via tr_release), 0 when it could not be claimed (nothing to do). */
static int
tr_prepare(bm_t *bm, bm_frame_t *f, void *dst)
{
	int expect = 0;

	if (!atomic_load_explicit(&f->dirty, memory_order_acquire))
		return 0;
	if (!atomic_compare_exchange_strong(&f->io_busy, &expect, 1))
		return 0;
	if (xtc_arwlock_rdlock(f->latch, 0) != XTC_OK) {
		atomic_store_explicit(&f->io_busy, 0, memory_order_release);
		return 0;
	}
	if (!atomic_load_explicit(&f->dirty, memory_order_acquire)) {
		xtc_arwlock_unlock(f->latch);
		atomic_store_explicit(&f->io_busy, 0, memory_order_release);
		return 0;
	}
	memcpy(dst, f->page, bm->page_size);
	if (bm->lsn_off >= 0 && bm->wal_flush != NULL && f->pid != 0) {
		uint64_t plsn;
		memcpy(&plsn, (uint8_t *)dst + bm->lsn_off, sizeof plsn);
		if (bm->wal_flush(bm->wal_ctx, plsn) != XTC_OK) {
			xtc_arwlock_unlock(f->latch);
			atomic_store_explicit(&f->io_busy, 0, memory_order_release);
			return 0;
		}
	}
	atomic_store_explicit(&f->dirty, 0, memory_order_release);
	xtc_arwlock_unlock(f->latch);
	dw_protect(bm, f->pid, dst);    /* double-write log (no-op if disabled) */
	return 1;
}

/* Release a claimed frame after its (coalesced) write completed.  On
 * write failure the page is re-dirtied so a later pass retries it. */
static void
tr_release(bm_frame_t *f, int wrote_ok)
{
	if (!wrote_ok)
		atomic_store_explicit(&f->dirty, 1, memory_order_release);
	atomic_store_explicit(&f->io_busy, 0, memory_order_release);
}

/* Write one coalesced run [base, run_len pages) at page pid0 and release
 * its frames.  Counts one pwrite call (for the coalescing ratio).
 * Returns pages durably written. */
static int
tr_emit_run(bm_t *bm, uint8_t *base, int run_len, bm_pid_t pid0,
            bm_frame_t **run_f)
{
	off_t off = (off_t)pid0 * (off_t)bm->page_size;
	int len = run_len * (int)bm->page_size, k, ok, written = 0;
	ok = xtc_aio_pwrite(bm->fd, base, len, off) == len;
	atomic_fetch_add_explicit(&bm->s_tr_writes, 1, memory_order_relaxed);
	for (k = 0; k < run_len; k++) {
		tr_release(run_f[k], ok);
		if (ok) {
			written++;
			atomic_fetch_add_explicit(&bm->s_flushed, 1,
			    memory_order_relaxed);
		}
	}
	return written;
}

/* Flush a priority-selected batch with elevator scheduling + write
 * coalescing.  `sel[0..nsel)` are the chosen victims (any order on
 * entry); they are sorted by page number, claimed/snapshotted into the
 * contiguous gather buffer `wbuf` (>= nsel pages), and runs of
 * consecutive page numbers are written with a single multi-page pwrite.
 * `run_f` is caller scratch for >= nsel frame pointers.  Returns the
 * number of pages durably written. */
static int
tr_flush_batch(bm_t *bm, struct tr_cand *sel, int nsel, uint8_t *wbuf,
               bm_frame_t **run_f)
{
	uint32_t psz = bm->page_size;
	int slot = 0, run_len = 0, written = 0, j;
	bm_pid_t run_pid0 = 0, prev_pid = 0;

	qsort(sel, (size_t)nsel, sizeof *sel, tr_cmp_pid);   /* elevator */

	for (j = 0; j < nsel; j++) {
		bm_frame_t *f = sel[j].f;
		if (!tr_prepare(bm, f, wbuf + (size_t)slot * psz))
			continue;
		if (run_len > 0 && f->pid == prev_pid + 1) {
			run_f[run_len++] = f;        /* extend the contiguous run */
		} else {
			if (run_len > 0)             /* emit the finished run */
				written += tr_emit_run(bm,
				    wbuf + (size_t)(slot - run_len) * psz,
				    run_len, run_pid0, run_f);
			run_pid0 = f->pid;           /* start a new run at this slot */
			run_f[0] = f;
			run_len = 1;
		}
		prev_pid = f->pid;
		slot++;
	}
	if (run_len > 0)                             /* emit the trailing run */
		written += tr_emit_run(bm,
		    wbuf + (size_t)(slot - run_len) * psz, run_len, run_pid0,
		    run_f);
	return written;
}
static void
tr_proc(void *arg)
{
	struct tr_arg *ta = arg;
	bm_t *bm = ta->bm;
	int64_t iv = ta->interval;
	int64_t iv_default = ta->interval;   /* fixed pacing when backlog low */
	uint64_t prev_tr_writes = 0, prev_evicted = 0;
	double pp_ewma = 1.0;                 /* EWMA of trickler pages-per-write */
	double ev_ewma = 1.0;                 /* EWMA of evictions per pass */
	struct tr_cand *cand;
	xtc_dio_sched_t *tuner = NULL;
	int batch = TR_BATCH;
	int cap = bm->n_frames < 256 ? (int)bm->n_frames : 256;
	int max_batch = bm->adaptive_writeback ? (cap < 1 ? 1 : cap) : TR_BATCH;
	uint8_t *wbuf = NULL;
	bm_frame_t **run_f = NULL;
	xtc_free(ta);
	if (max_batch < 1) max_batch = 1;
	if ((cand = xtc_calloc(bm->n_frames, sizeof *cand)) == NULL)
		return;
	/* Gather buffer for coalesced multi-page writes (direct-I/O aligned)
	 * plus per-run frame scratch. */
	wbuf = xtc_aligned_alloc(4096, (size_t)max_batch * bm->page_size);
	run_f = xtc_calloc((size_t)max_batch, sizeof *run_f);
	if (wbuf == NULL || run_f == NULL) {
		xtc_aligned_free(wbuf);
		xtc_free(run_f);
		xtc_free(cand);
		return;
	}
	if (bm->adaptive_writeback) {
		xtc_dio_sched_spec_t spec;
		memset(&spec, 0, sizeof spec);
		spec.n_genes = 2;
		spec.min[0] = 1;
		spec.max[0] = cap < 1 ? 1 : cap;
		spec.init[0] = TR_BATCH > spec.max[0] ? spec.max[0] : TR_BATCH;
		spec.min[1] = 1;                 /* interval, ms */
		spec.max[1] = 50;
		spec.init[1] = (int)(iv / 1000000);
		if (spec.init[1] < 1)  spec.init[1] = 1;
		if (spec.init[1] > 50) spec.init[1] = 50;
		spec.population = 8;
		/* Two phenotypes (Moilanen): batch (gene 0) is tuned by
		 * throughput, interval (gene 1) by the dirty backlog, so the
		 * two objectives do not muddy each other's gradient. */
		spec.n_phenos = 2;
		spec.gene_pheno[0] = 0;     /* batch    -> throughput phenotype */
		spec.gene_pheno[1] = 1;     /* interval -> backlog phenotype */
		if (xtc_dio_sched_create(&spec, &tuner) == XTC_OK) {
			int g[XTC_DIO_SCHED_MAX_GENES];
			xtc_dio_sched_current(tuner, g);
			batch = g[0];
			iv = (int64_t)g[1] * 1000000;
		}
	}
	while (atomic_load_explicit(&bm->tr_running, memory_order_acquire)) {
		uint32_t i;
		int n = 0, w = 0, dirty_total = 0;
		for (i = 0; i < bm->n_frames; i++) {
			bm_frame_t *f = &bm->frames[i];
			uint8_t st;
			if (!atomic_load_explicit(&f->dirty, memory_order_acquire))
				continue;
			dirty_total++;          /* writeback backlog (all dirty) */
			if (atomic_load_explicit(&f->pin, memory_order_acquire) != 0)
				continue;
			if (atomic_load_explicit(&f->io_busy, memory_order_acquire))
				continue;
			st = atomic_load_explicit(&f->state, memory_order_acquire);
			if (st != BM_HOT && st != BM_COOL)
				continue;
			cand[n].f = f;
			cand[n].cool = (st == BM_COOL);
			cand[n].seq = atomic_load_explicit(&f->dirty_seq,
			    memory_order_relaxed);
			n++;
		}
		atomic_store_explicit(&bm->s_dirty_backlog, (uint32_t)dirty_total,
		    memory_order_relaxed);
		int64_t t0 = 0, t1 = 0;
		int nsel;
		qsort(cand, (size_t)n, sizeof *cand, tr_cmp);   /* priority select */
		nsel = n < batch ? n : batch;
		if (nsel > max_batch) nsel = max_batch;         /* guard wbuf */
		t0 = xtc_clock_mono();
		/* Elevator-ordered, write-coalesced flush of the selected batch. */
		w = tr_flush_batch(bm, cand, nsel, wbuf, run_f);
		atomic_fetch_add_explicit(&bm->s_trickled, w, memory_order_relaxed);
		t1 = xtc_clock_mono();
		/* Adaptive pacing, GATED by backlog.  When the trickler is
		 * comfortably keeping up (few dirty frames), the backlog-based
		 * fitness signal has no gradient, so letting the GA run just
		 * mis-tunes and steals CPU; fall back to fixed pacing.  Only
		 * when the backlog is non-trivial (>= 1/4 of the pool) do we
		 * feed the tuner and adopt its batch/interval. */
		/* Gate adaptive on whether it can actually help.  It helps only
		 * when (a) writes COALESCE -- a locality/sequential workload,
		 * detected by an EWMA of pages-per-write rising above 1 -- or
		 * (b) the working set fits and there is NO demand eviction, so
		 * aggressive trickling cuts fsync cost.  On a random,
		 * eviction-bound workload neither holds (pages_per_write ~= 1
		 * and evictions keep coming), so the GA is pure overhead: fall
		 * back to fixed pacing. */
		{
			uint64_t twr = atomic_load_explicit(&bm->s_tr_writes,
			    memory_order_relaxed);
			uint64_t ev = atomic_load_explicit(&bm->s_evicted,
			    memory_order_relaxed);
			uint64_t calls = twr - prev_tr_writes;
			uint64_t evd = ev - prev_evicted;
			int use_adaptive, fits;
			prev_tr_writes = twr;
			prev_evicted = ev;
			if (calls > 0) {
				double pp = (double)w / (double)calls;
				pp_ewma = 0.8 * pp_ewma + 0.2 * pp;
			}
			/* Smoothed eviction rate: "fits in RAM" (no demand
			 * eviction) is a sustained property, so average per-pass
			 * evictions instead of trusting a single twitchy sample.
			 * Require it near zero (not merely low): even a few
			 * adaptive passes in an eviction-bound run pick a
			 * whole-pool batch / long interval that wrecks pacing, so
			 * the gate must let essentially none through there.  The
			 * EWMA's inertia from 1.0 also suppresses warmup leakage. */
			ev_ewma = 0.9 * ev_ewma + 0.1 * (double)evd;
			fits = ev_ewma < 0.05;
			use_adaptive = (pp_ewma >= 1.5) || fits;
			atomic_fetch_add_explicit(&bm->s_tr_passes, 1,
			    memory_order_relaxed);
			if (tuner != NULL && use_adaptive && w > 0) {
				int g[XTC_DIO_SCHED_MAX_GENES];
				double cycle = (double)((t1 - t0) + iv) / 1e9;
				double rate, dfrac, f[2];
				atomic_fetch_add_explicit(&bm->s_tr_adaptive, 1,
				    memory_order_relaxed);
				if (cycle <= 0.0) cycle = 1e-9;
				rate = (double)w / cycle;        /* sustained pages/s */
				dfrac = bm->n_frames
				    ? (double)dirty_total / (double)bm->n_frames : 0.0;
				if (dfrac > 1.0) dfrac = 1.0;
				/* Phenotype 0 (batch): maximise throughput.
				 * Phenotype 1 (interval): maximise backlog quality. */
				f[0] = rate;
				f[1] = 1.0 - dfrac;
				xtc_dio_sched_report_multi(tuner, f, 2);
				xtc_dio_sched_current(tuner, g);
				batch = g[0];
				iv = (int64_t)g[1] * 1000000;
				if (iv <= 0) iv = 1000000;
			} else if (tuner != NULL) {     /* gated off: fixed pacing */
				batch = TR_BATCH > max_batch ? max_batch : TR_BATCH;
				iv = iv_default;
			}
		}
		if (xtc_proc_sleep(iv) != XTC_OK)
			break;
	}
	xtc_free(cand);
	xtc_aligned_free(wbuf);
	xtc_free(run_f);
	xtc_dio_sched_destroy(tuner);
	atomic_store_explicit(&bm->tr_alive, 0, memory_order_release);
}

int
bm_trickler_spawn(bm_t *bm, xtc_loop_t *loop, int64_t interval_ns,
                  xtc_pid_t *out_pid)
{
	struct tr_arg *ta;
	xtc_proc_opts_t opts = { 0 };
	int rc;
	if (bm == NULL || loop == NULL) return XTC_E_INVAL;
	if ((ta = xtc_calloc(1, sizeof *ta)) == NULL) return XTC_E_NOMEM;
	ta->bm = bm;
	ta->interval = interval_ns > 0 ? interval_ns : 2LL * 1000 * 1000;
	atomic_store_explicit(&bm->tr_running, 1, memory_order_release);
	atomic_store_explicit(&bm->tr_alive, 1, memory_order_release);
	opts.name = "bm-trickler";
	rc = xtc_proc_spawn(loop, tr_proc, ta, &opts, &bm->tr_pid);
	if (rc != XTC_OK) { atomic_store_explicit(&bm->tr_running, 0, memory_order_release); atomic_store_explicit(&bm->tr_alive, 0, memory_order_release); xtc_free(ta); return rc; }
	if (out_pid) *out_pid = bm->tr_pid;
	return XTC_OK;
}

void
bm_trickler_stop(bm_t *bm)
{
	int i;
	if (bm == NULL) return;
	atomic_store_explicit(&bm->tr_running, 0, memory_order_release);
	for (i = 0; i < 100000 &&
	    atomic_load_explicit(&bm->tr_alive, memory_order_acquire); i++)
		if (xtc_proc_sleep(200LL * 1000) != XTC_OK) break;
}

void
bm_provider_stop(bm_t *bm)
{
	int i;
	if (bm == NULL) return;
	atomic_store_explicit(&bm->pp_running, 0, memory_order_release);
	/* Wait for the proc to observe the flag and exit before a caller
	 * (e.g. bm_destroy) frees the buffer manager out from under it.
	 * On a loop, parking lets the proc wake from its interval sleep,
	 * see the flag, and return.  Off a loop xtc_proc_sleep fails and
	 * the proc has already drained, so we stop immediately. */
	for (i = 0; i < 100000 &&
	    atomic_load_explicit(&bm->pp_alive, memory_order_acquire); i++)
		if (xtc_proc_sleep(200LL * 1000) != XTC_OK) break;
}

void
bm_get_stats(bm_t *bm, bm_stats_t *out)
{
	if (bm == NULL || out == NULL) return;
	out->hits = atomic_load_explicit(&bm->s_hits, memory_order_relaxed);
	out->rescues = atomic_load_explicit(&bm->s_rescues, memory_order_relaxed);
	out->loads = atomic_load_explicit(&bm->s_loads, memory_order_relaxed);
	out->cooled = atomic_load_explicit(&bm->s_cooled, memory_order_relaxed);
	out->flushed = atomic_load_explicit(&bm->s_flushed, memory_order_relaxed);
	out->evicted = atomic_load_explicit(&bm->s_evicted, memory_order_relaxed);
	out->resident = atomic_load_explicit(&bm->resident, memory_order_relaxed);
	out->free_frames = atomic_load_explicit(&bm->free_n, memory_order_relaxed);
	out->prefetched = atomic_load_explicit(&bm->s_prefetched, memory_order_relaxed);
	out->trickled = atomic_load_explicit(&bm->s_trickled, memory_order_relaxed);
	out->tr_writes = atomic_load_explicit(&bm->s_tr_writes, memory_order_relaxed);
	out->evict_flushes = atomic_load_explicit(&bm->s_evict_flush, memory_order_relaxed);
	out->dirty_backlog = atomic_load_explicit(&bm->s_dirty_backlog, memory_order_relaxed);
	out->tr_passes = atomic_load_explicit(&bm->s_tr_passes, memory_order_relaxed);
	out->tr_adaptive = atomic_load_explicit(&bm->s_tr_adaptive, memory_order_relaxed);
	out->dw_repaired = atomic_load_explicit(&bm->s_dw_repaired, memory_order_relaxed);
	out->freed = atomic_load_explicit(&bm->s_freed, memory_order_relaxed);
	out->reissued = atomic_load_explicit(&bm->s_reissued, memory_order_relaxed);
	out->free_pids = bm->free_pids_n;   /* approximate (read without the pid lock) */
}

/* ---- persistence: superblock, sync, checkpoint ---- */

int
bm_write_super(bm_t *bm, const void *buf, size_t len)
{
	ssize_t n;
	if (bm == NULL || buf == NULL || len > bm->page_size)
		return XTC_E_INVAL;
	n = pwrite(bm->fd, buf, len, 0);          /* page 0 == superblock */
	return (n == (ssize_t)len) ? XTC_OK : XTC_E_INTERNAL;
}

int
bm_read_super(bm_t *bm, void *buf, size_t len)
{
	ssize_t n;
	if (bm == NULL || buf == NULL || len > bm->page_size)
		return XTC_E_INVAL;
	n = pread(bm->fd, buf, len, 0);
	if (n < 0)
		return XTC_E_INTERNAL;
	if ((size_t)n < len)                       /* short/empty: zero-fill */
		memset((char *)buf + n, 0, len - (size_t)n);
	return XTC_OK;
}

/* fdatasync the backing file via xtc_aio (native or offloaded), so the
 * loop is not blocked.  Data-only is correct here: page contents must
 * be durable, not file metadata. */
int
bm_sync(bm_t *bm)
{
	if (bm == NULL) return XTC_E_INVAL;
	return xtc_aio_fdatasync(bm->fd) == 0 ? XTC_OK : XTC_E_INTERNAL;
}

int
bm_checkpoint(bm_t *bm)
{
	uint32_t i;
	if (bm == NULL) return XTC_E_INVAL;
	/* Write back every dirty page (flush_frame snapshots under a
	 * try-shared latch, so this is torn-free even with writers), then
	 * fdatasync.  Unpinned dirty pages are flushed now; a page pinned
	 * by an active writer is left -- a quiesced checkpoint should run
	 * with no writes in flight, which the WAL-truncation caller
	 * arranges. */
	for (i = 0; i < bm->n_frames; i++) {
		bm_frame_t *f = &bm->frames[i];
		if (atomic_load_explicit(&f->dirty, memory_order_acquire))
			(void)flush_frame(bm, f);
	}
	return bm_sync(bm);
}
