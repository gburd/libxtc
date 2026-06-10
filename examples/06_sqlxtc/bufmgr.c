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

#include "xtc_int.h"
#include "xtc_aio.h"
#include "xtc_stats.h"
#include "xtc_sync.h"
#include "xtc_dio_sched.h"
#include "os_time.h"
#include <fcntl.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* ---- Swip encoding (two MSBs) ---- */
#define SW_EVICTED  (1ULL << 63)
#define SW_COOL     (1ULL << 62)
#define SW_PTRMASK  (~(3ULL << 62))

static inline int        sw_is_hot(uint64_t w)  { return (w & (SW_EVICTED | SW_COOL)) == 0; }
static inline int        sw_is_cool(uint64_t w) { return (w & SW_COOL) != 0 && (w & SW_EVICTED) == 0; }
static inline int        sw_is_evicted(uint64_t w) { return (w & SW_EVICTED) != 0; }
static inline bm_frame_t *sw_frame(uint64_t w)  { return (bm_frame_t *)(uintptr_t)(w & SW_PTRMASK); }
static inline bm_pid_t    sw_pid(uint64_t w)    { return (bm_pid_t)(w & ~SW_EVICTED); }
static inline uint64_t    sw_hot(bm_frame_t *f) { return (uint64_t)(uintptr_t)f; }
static inline uint64_t    sw_cool(bm_frame_t *f){ return (uint64_t)(uintptr_t)f | SW_COOL; }
static inline uint64_t    sw_evicted(bm_pid_t p){ return (uint64_t)p | SW_EVICTED; }

/* ---- frame states ---- */
enum { BM_FREE = 0, BM_HOT, BM_COOL, BM_LOADED, BM_WRITING };

struct bm_frame {
	_Atomic uint8_t   state;
	_Atomic int       pin;        /* >0: a worker holds it; do not evict */
	_Atomic int       io_busy;    /* a write is in flight */
	_Atomic int       dirty;      /* page modified since last write */
	_Atomic int       ref;        /* CLOCK second-chance: set on access,
	                               * cleared by the eviction sweep -- a
	                               * recently used COOL page survives one
	                               * sweep, so a hot page is not evicted
	                               * out from under a fixer about to
	                               * re-pin it (anti-thrash). */
	_Atomic uint64_t  dirty_seq;  /* order it was dirtied (recLSN proxy) */
	_Atomic uint64_t  rec_lsn;    /* WAL LSN that first dirtied it since clean
	                               * (the ARIES recLSN; log truncation horizon) */
	bm_pid_t          pid;
	bm_swip_t        *parent;     /* the Swip word that points here (swip mode) */
	int               via_pid;    /* 1: referenced through the page table */
	struct bm_frame  *hnext;      /* page-table hash chain (pid mode) */
	void             *page;       /* page_size bytes (into the pool) */
	struct bm_frame  *next_free;
	xtc_arwlock_t    *latch;      /* content latch (fiber-yielding) */
};

/*
 * Page-table lock striping: instead of one global mutex over all hash
 * buckets, an array of cache-line-isolated stripe locks, each guarding
 * the buckets b with (b & (BM_HT_STRIPES-1)) == stripe.  Every table
 * operation touches exactly one bucket, so it locks exactly one stripe;
 * fixes of unrelated pages then proceed in parallel.  The 128-byte
 * union keeps adjacent stripes off the same cache line (no false
 * sharing) regardless of array base alignment.
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
	bm_frame_t       *frames;
	unsigned char    *pool;        /* n_frames * page_size, aligned */

	int             (*has_resident_child)(const void *page, void *user);
	void             *cb_user;

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
	_Atomic uint64_t  s_freed;       /* total pages put on the freelist */
	_Atomic uint64_t  s_reissued;    /* allocations served from the freelist */

	_Atomic uint32_t  clock;       /* round-robin victim cursor */

	/* page table (pid mode): pid -> resident frame */
	bm_htlock_t      *ht_locks;    /* BM_HT_STRIPES striped bucket locks */
	bm_frame_t      **buckets;
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
static void
unpin(bm_frame_t *f)
{
	atomic_fetch_sub_explicit(&f->pin, 1, memory_order_release);
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

/* Claim a just-allocated frame for loading: pin 0 -> 1.  Must NOT be a
 * blind store: a frame taken off the free list can be transiently
 * try_pin'd by a fixer that still holds a STALE swip pointing here (the
 * frame used to live at that fixer's slot before it was evicted, freed,
 * and recycled to us).  Such a fixer does try_pin (pin++), fails its
 * slot recheck, then unpin (pin--) -- net zero, but a blind store of 1
 * over its transient pin would be lost and the fixer's unpin would then
 * underflow our pin to -1, wedging the frame forever.  CAS-claim from 0
 * coexists with those transient stale pins; we spin (briefly -- stale
 * holders drain as their rechecks fail) until we own it cleanly. */
static void
claim_frame(bm_frame_t *f)
{
	int e = 0;
	while (!atomic_compare_exchange_weak_explicit(&f->pin, &e, 1,
	    memory_order_acq_rel, memory_order_relaxed)) {
		e = 0;
		xtc_yield();
	}
}

/* ---- free list ---- */
static void
free_push(bm_t *bm, bm_frame_t *f)
{
	/* Mark FREE before clearing the reservation, so an eviction sweep
	 * that is mid-scan sees a non-COOL state and never reserves a frame
	 * that is on its way to the free list. */
	atomic_store_explicit(&f->state, BM_FREE, memory_order_release);
	atomic_store_explicit(&f->pin, 0, memory_order_release);   /* clear any reservation */
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
	if (__os_malloc(bm->page_size, (void **)&pg) != XTC_OK)
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
	__os_free(pg);
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
	if (__os_aligned_alloc(4096, bm->page_size, (void **)&snap) != XTC_OK) {
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
			__os_aligned_free(snap);
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
	__os_aligned_free(snap);
	atomic_store_explicit(&f->io_busy, 0, memory_order_release);
	atomic_fetch_add_explicit(&bm->s_flushed, 1, memory_order_relaxed);
	return 1;
}

/* A page with resident children must not be cooled or evicted. */
static int
has_resident_child(bm_t *bm, bm_frame_t *f)
{
	if (bm->has_resident_child == NULL)
		return 0;
	return bm->has_resident_child(f->page, bm->cb_user);
}

/* ---- page table (pid mode) ---- */
/* The stripe lock guarding hash bucket `b`. */
static inline pthread_mutex_t *
ht_lock(bm_t *bm, uint32_t b)
{
	return &bm->ht_locks[b & (BM_HT_STRIPES - 1)].m;
}
static void
ht_insert(bm_t *bm, bm_frame_t *f)
{
	uint32_t b = (uint32_t)(f->pid % bm->nbucket);
	pthread_mutex_t *lk = ht_lock(bm, b);
	(void)pthread_mutex_lock(lk);
	f->hnext = bm->buckets[b];
	bm->buckets[b] = f;
	(void)pthread_mutex_unlock(lk);
}
static void
ht_remove(bm_t *bm, bm_frame_t *f)
{
	uint32_t b = (uint32_t)(f->pid % bm->nbucket);
	pthread_mutex_t *lk = ht_lock(bm, b);
	bm_frame_t **pp;
	(void)pthread_mutex_lock(lk);
	for (pp = &bm->buckets[b]; *pp != NULL; pp = &(*pp)->hnext)
		if (*pp == f) { *pp = f->hnext; break; }
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
		if (f->pid == pid && f->via_pid) {
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
		uint64_t w, repl;
		i = atomic_fetch_add_explicit(&bm->clock, 1, memory_order_relaxed)
		    % bm->n_frames;
		bm_frame_t *f = &bm->frames[i];
		uint8_t st = atomic_load_explicit(&f->state, memory_order_acquire);

		if (atomic_load_explicit(&f->pin, memory_order_acquire) != 0)
			continue;

		if (f->via_pid) {
			/* page-table mode: cool is a state flip, no parent swip. */
			if (st == BM_HOT) {
				if (!force_cool)
					continue;            /* prefer COOL victims */
				atomic_store_explicit(&f->state, BM_COOL, memory_order_release);
				atomic_fetch_add_explicit(&bm->s_cooled, 1, memory_order_relaxed);
				st = BM_COOL;
			}
			if (st != BM_COOL) continue;
			if (atomic_load_explicit(&f->dirty, memory_order_acquire)) {
				if (!prefer_clean)
					(void)flush_frame(bm, f);  /* fallback write-out */
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
			f->via_pid = 0;
			atomic_fetch_sub_explicit(&bm->resident, 1, memory_order_relaxed);
			atomic_fetch_add_explicit(&bm->s_evicted, 1, memory_order_relaxed);
			free_push(bm, f);            /* clears the reservation (pin = 0) */
			return 1;
		}

		/* swip mode */
		if ((st == BM_HOT || st == BM_COOL) && has_resident_child(bm, f))
			continue;
		if (st == BM_HOT) {
			if (!force_cool)
				continue;            /* prefer COOL victims */
			/* Cool it: unswizzle the parent HOT -> COOL. */
			w = atomic_load_explicit(f->parent, memory_order_acquire);
			if (!sw_is_hot(w) || sw_frame(w) != f) continue;
			if (!atomic_compare_exchange_strong(f->parent, &w, sw_cool(f)))
				continue;
			atomic_store_explicit(&f->state, BM_COOL, memory_order_release);
			atomic_fetch_add_explicit(&bm->s_cooled, 1, memory_order_relaxed);
			st = BM_COOL;
		}
		if (st != BM_COOL)
			continue;
		if (atomic_load_explicit(&f->dirty, memory_order_acquire)) {
			if (!prefer_clean)
				(void)flush_frame(bm, f);   /* fallback write-out */
			continue;                       /* else leave it for the trickler */
		}
		if (atomic_load_explicit(&f->io_busy, memory_order_acquire))
			continue;
		if (atomic_exchange_explicit(&f->ref, 0, memory_order_relaxed))
			continue;                   /* recently used: spare one sweep */
		if (!try_reserve(f))
			continue;                   /* pinned: a fixer holds it */
		/* Evict: parent COOL -> EVICTED.  Reserved (pin == -1), so no
		 * fixer can rescue it; release the reservation if the parent
		 * changed under us. */
		w = atomic_load_explicit(f->parent, memory_order_acquire);
		if (atomic_load_explicit(&f->state, memory_order_acquire) != BM_COOL ||
		    !sw_is_cool(w) || sw_frame(w) != f) {
			release_reservation(f); continue;
		}
		repl = sw_evicted(f->pid);
		if (!atomic_compare_exchange_strong(f->parent, &w, repl)) {
			release_reservation(f); continue;
		}
		atomic_fetch_sub_explicit(&bm->resident, 1, memory_order_relaxed);
		atomic_fetch_add_explicit(&bm->s_evicted, 1, memory_order_relaxed);
		free_push(bm, f);                   /* clears the reservation */
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
		if (evict_one(bm))
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
	if ((rc = __os_calloc(1, sizeof *bm, (void **)&bm)) != XTC_OK)
		return rc;
	bm->page_size = opts->page_size;
	bm->n_frames = opts->n_frames;
	bm->has_resident_child = opts->has_resident_child;
	bm->cb_user = opts->cb_user;
	bm->cool_target = opts->n_frames * (opts->cool_pct ? opts->cool_pct : 10)
	    / 100u;
	if (bm->cool_target < 1) bm->cool_target = 1;
	bm->scan_resist = opts->scan_resist ? 1 : 0;
	bm->lsn_off = opts->lsn_off;       /* -1 disables page-LSN handling */
	atomic_store_explicit(&bm->cur_lsn, 0, memory_order_relaxed);
	bm->wal_flush = NULL;
	bm->wal_ctx = NULL;
	(void)pthread_mutex_init(&bm->free_mu, NULL);
	(void)pthread_mutex_init(&bm->pf_mu, NULL);
	(void)pthread_mutex_init(&bm->pid_mu, NULL);

	bm->fd = open(opts->path ? opts->path : "/tmp/sqlxtc-bm.tmp",
	    opts->reopen ? (O_RDWR | O_CREAT) : (O_RDWR | O_CREAT | O_TRUNC), 0644);
	if (bm->fd < 0) { __os_free(bm); return XTC_E_INVAL; }

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

	if ((rc = __os_calloc(bm->n_frames, sizeof *bm->frames,
	    (void **)&bm->frames)) != XTC_OK) { close(bm->fd); __os_free(bm); return rc; }
	if ((rc = __os_aligned_alloc(4096,
	    (size_t)bm->n_frames * bm->page_size, (void **)&bm->pool)) != XTC_OK) {
		__os_free(bm->frames); close(bm->fd); __os_free(bm); return rc;
	}
	for (i = 0; i < bm->n_frames; i++) {
		bm->frames[i].page = bm->pool + (size_t)i * bm->page_size;
		if ((rc = xtc_arwlock_create(&bm->frames[i].latch)) != XTC_OK) {
			__os_aligned_free(bm->pool); __os_free(bm->frames);
			close(bm->fd); __os_free(bm); return rc;
		}
		free_push(bm, &bm->frames[i]);
	}
	/* page table: next pow2 >= 2*n_frames */
	bm->nbucket = 16;
	while (bm->nbucket < bm->n_frames * 2u) bm->nbucket <<= 1;
	if ((rc = __os_calloc(BM_HT_STRIPES, sizeof *bm->ht_locks,
	    (void **)&bm->ht_locks)) != XTC_OK) {
		__os_aligned_free(bm->pool); __os_free(bm->frames);
		close(bm->fd); __os_free(bm); return rc;
	}
	for (i = 0; i < (int)BM_HT_STRIPES; i++)
		(void)pthread_mutex_init(&bm->ht_locks[i].m, NULL);
	if ((rc = __os_calloc(bm->nbucket, sizeof *bm->buckets,
	    (void **)&bm->buckets)) != XTC_OK) {
		__os_free(bm->ht_locks);
		__os_aligned_free(bm->pool); __os_free(bm->frames);
		close(bm->fd); __os_free(bm); return rc;
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
	__os_free(bm->ht_locks);
	__os_free(bm->buckets);
	__os_free(bm->free_pids);
	__os_free(bm->quar_pids);
	__os_aligned_free(bm->pool);
	__os_free(bm->frames);
	__os_free(bm);
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

/*
 * Drop a resident frame for `pid` to the free list, bypassing the
 * writeback path: the page is being reclaimed, so its contents are
 * dead and must NOT be flushed (a flush could resurrect stale bytes
 * under a reissued id).  Removes the page-table entry under the
 * bucket's stripe lock and reserves the frame so no concurrent fixer
 * can re-pin it.  Returns 1 if a resident frame was dropped, 0 if the
 * page was not resident.  The caller (bm_free_pid) holds no latch and
 * guarantees no live pointer reaches `pid`, so any concurrent fixer
 * is already draining (it found the page through a now-removed link).
 */
static int
drop_resident(bm_t *bm, bm_pid_t pid)
{
	uint32_t b = (uint32_t)(pid % bm->nbucket);
	pthread_mutex_t *lk = ht_lock(bm, b);
	bm_frame_t *f;

	(void)pthread_mutex_lock(lk);
	for (f = bm->buckets[b]; f != NULL; f = f->hnext)
		if (f->pid == pid && f->via_pid)
			break;
	if (f == NULL) {
		(void)pthread_mutex_unlock(lk);
		return 0;
	}
	/* Reserve the frame (pin 0 -> -1).  If it is pinned, a worker still
	 * holds it; spin briefly -- the caller has unlinked the page, so any
	 * such holder is finishing a read it began before the unlink and
	 * will unpin shortly. */
	while (!try_reserve(f)) {
		(void)pthread_mutex_unlock(lk);
		xtc_yield();
		(void)pthread_mutex_lock(lk);
		/* The page cannot reappear under a different frame (it is
		 * unlinked, so no fixer can load it), so re-find is stable. */
	}
	atomic_store_explicit(&f->dirty, 0, memory_order_release); /* dead: do not flush */
	{
		bm_frame_t **pp;
		for (pp = &bm->buckets[b]; *pp != NULL; pp = &(*pp)->hnext)
			if (*pp == f) { *pp = f->hnext; break; }
	}
	f->via_pid = 0;
	(void)pthread_mutex_unlock(lk);
	atomic_fetch_sub_explicit(&bm->resident, 1, memory_order_relaxed);
	free_push(bm, f);            /* clears the reservation (pin = 0) */
	return 1;
}

int
bm_alloc(bm_t *bm, bm_swip_t *slot, bm_frame_t **out_frame, bm_pid_t *out_pid)
{
	bm_frame_t *f;
	bm_pid_t pid;
	if (bm == NULL || slot == NULL || out_frame == NULL) return XTC_E_INVAL;
	if ((f = get_free_frame(bm)) == NULL) return XTC_E_RESOURCE;
	pid = next_pid(bm);
	atomic_store_explicit(&f->state, BM_LOADED, memory_order_relaxed);
	claim_frame(f);
	atomic_store_explicit(&f->dirty, 1, memory_order_relaxed);  /* fresh */
	atomic_store_explicit(&f->io_busy, 0, memory_order_relaxed);
	f->pid = pid;
	f->parent = slot;
	memset(f->page, 0, bm->page_size);
	atomic_store_explicit(slot, sw_hot(f), memory_order_release);
	atomic_store_explicit(&f->state, BM_HOT, memory_order_release);
	atomic_fetch_add_explicit(&bm->resident, 1, memory_order_relaxed);
	if (out_pid) *out_pid = pid;
	*out_frame = f;
	return XTC_OK;
}

int
bm_fix(bm_t *bm, bm_swip_t *slot, bm_frame_t **out_frame)
{
	if (bm == NULL || slot == NULL || out_frame == NULL) return XTC_E_INVAL;
	for (;;) {
		uint64_t w = atomic_load_explicit(slot, memory_order_acquire);

		if (sw_is_hot(w)) {
			bm_frame_t *f = sw_frame(w);
			if (!try_pin(f)) {
				xtc_yield();          /* reserved for eviction: yield so
				                       * the loop can dispatch the
				                       * evictor's flush completion */
				continue;
			}
			/* Recheck: it may have been cooled/evicted meanwhile. */
			if (atomic_load_explicit(slot, memory_order_acquire) == w) {
				atomic_fetch_add_explicit(&bm->s_hits, 1, memory_order_relaxed);
				*out_frame = f;
				return XTC_OK;
			}
			unpin(f);
			continue;
		}
		if (sw_is_cool(w)) {
			bm_frame_t *f = sw_frame(w);
			if (!try_pin(f)) {
				xtc_yield();          /* reserved for eviction: yield */
				continue;
			}
			if (atomic_compare_exchange_strong(slot, &w, sw_hot(f))) {
				atomic_store_explicit(&f->state, BM_HOT, memory_order_release);
				atomic_fetch_add_explicit(&bm->s_rescues, 1, memory_order_relaxed);
				*out_frame = f;
				return XTC_OK;
			}
			unpin(f);
			continue;               /* changed; retry */
		}
		/* EVICTED: load from disk into a free frame. */
		{
			bm_pid_t pid = sw_pid(w);
			bm_frame_t *f = get_free_frame(bm);
			uint64_t expect = w;
			if (f == NULL) return XTC_E_RESOURCE;
			/* Pin BEFORE the frame is ever visible as COOL/HOT.  We own
			 * it exclusively (just took it off the free list), and a
			 * non-zero pin makes eviction's try_reserve (CAS pin 0 -> -1)
			 * fail, so a concurrent evict_one cannot reserve this frame
			 * during the load+publish and race our pin store -- which
			 * would otherwise double-own the frame (published AND back
			 * on the free list) and corrupt the free list.  While it is
			 * BM_LOADED below, eviction skips it by state anyway. */
			claim_frame(f);
			atomic_store_explicit(&f->state, BM_LOADED, memory_order_relaxed);
			f->pid = pid;
			f->parent = slot;
			atomic_store_explicit(&f->dirty, 0, memory_order_relaxed);
			atomic_store_explicit(&f->io_busy, 0, memory_order_relaxed);
			if (do_io(bm, f->page, pid, 0) != 0) {
				free_push(bm, f);   /* sets state=FREE then pin=0 */
				return XTC_E_INTERNAL;
			}
			atomic_store_explicit(&f->state,
			    bm->scan_resist ? BM_COOL : BM_HOT, memory_order_release);
			if (atomic_compare_exchange_strong(slot, &expect,
			    bm->scan_resist ? sw_cool(f) : sw_hot(f))) {
				/* Probationary admission (LeanStore cooling + 2Q): a
				 * demand-loaded page enters COOL, so a single-touch
				 * scan never promotes it to HOT; a second access
				 * rescues it (COOL -> HOT) above.  The frame has been
				 * pinned since before it became COOL, so eviction
				 * never raced it. */
				atomic_fetch_add_explicit(&bm->resident, 1, memory_order_relaxed);
				atomic_fetch_add_explicit(&bm->s_loads, 1, memory_order_relaxed);
				*out_frame = f;
				return XTC_OK;
			}
			/* Someone else resolved it; drop our frame, retry.
			 * free_push sets state=FREE before pin=0, so no eviction
			 * sweep can reserve it in a COOL+pin==0 window. */
			free_push(bm, f);
			continue;
		}
	}
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
	if (frame == NULL) return;
	if (mark_dirty)
		mark_dirty_edge(bm, frame);
	atomic_store_explicit(&frame->ref, 1, memory_order_relaxed);  /* CLOCK: recently used */
	atomic_fetch_sub_explicit(&frame->pin, 1, memory_order_release);
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
	bm_frame_t *f;
	if (bm == NULL || out_frame == NULL) return XTC_E_INVAL;
	if ((f = get_free_frame(bm)) == NULL) return XTC_E_RESOURCE;
	f->pid = next_pid(bm);
	f->parent = NULL;
	f->via_pid = 1;
	atomic_store_explicit(&f->pin, 1, memory_order_relaxed);
	atomic_store_explicit(&f->dirty, 1, memory_order_relaxed);
	atomic_store_explicit(&f->io_busy, 0, memory_order_relaxed);
	memset(f->page, 0, bm->page_size);
	atomic_store_explicit(&f->state, BM_HOT, memory_order_release);
	ht_insert(bm, f);
	atomic_fetch_add_explicit(&bm->resident, 1, memory_order_relaxed);
	if (out_pid) *out_pid = f->pid;
	*out_frame = f;
	return XTC_OK;
}

int
bm_free_pid(bm_t *bm, bm_pid_t pid)
{
	bm_pid_t *grow;
	uint32_t cap;

	if (bm == NULL || pid == BM_PID_NONE)
		return XTC_E_INVAL;
	/*
	 * Drop any resident copy first, OUTSIDE the pid lock: a stale
	 * resident frame for `pid` must not survive to be re-fixed once
	 * the id is reissued for fresh contents.  The caller has unlinked
	 * the page, so no live pointer reaches it and no fixer can load it
	 * anew; drop_resident only has to evict an already-resident frame.
	 */
	(void)drop_resident(bm, pid);

	(void)pthread_mutex_lock(&bm->pid_mu);
	if (bm->quar_pids_n == bm->quar_pids_cap) {
		cap = bm->quar_pids_cap ? bm->quar_pids_cap * 2u : 64u;
		if (__os_realloc(bm->quar_pids,
		    (size_t)cap * sizeof *bm->quar_pids,
		    (void **)&bm->quar_pids) != XTC_OK) {
			(void)pthread_mutex_unlock(&bm->pid_mu);
			return XTC_E_NOMEM;   /* page leaked, never double-allocated */
		}
		bm->quar_pids_cap = cap;
	}
	bm->quar_pids[bm->quar_pids_n++] = pid;   /* parked, not yet reusable */
	(void)pthread_mutex_unlock(&bm->pid_mu);
	atomic_fetch_add_explicit(&bm->s_freed, 1, memory_order_relaxed);
	return XTC_OK;
}

/*
 * Drain the quarantine: move pids freed in the previous epoch onto the
 * reusable freelist.  Called at a structure-modification epoch
 * boundary (the start of a merge), by when any latch-free chaser that
 * observed a now-freed pid has finished -- so reissuing it for fresh
 * contents can no longer mislead an in-flight operation.
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
		if (__os_realloc(bm->free_pids,
		    (size_t)cap * sizeof *bm->free_pids,
		    (void **)&bm->free_pids) != XTC_OK) {
			/* Out of memory: leave them quarantined (still safe;
			 * the pages stay leaked until a later drain). */
			(void)pthread_mutex_unlock(&bm->pid_mu);
			return;
		}
		bm->free_pids_cap = cap;
	}
	grow = bm->free_pids;
	for (i = 0; i < bm->quar_pids_n; i++)
		grow[bm->free_pids_n++] = bm->quar_pids[i];
	bm->quar_pids_n = 0;
	(void)pthread_mutex_unlock(&bm->pid_mu);
}

int
bm_fix_pid(bm_t *bm, bm_pid_t pid, bm_frame_t **out_frame)
{
	bm_frame_t *f;
	if (bm == NULL || out_frame == NULL || pid == BM_PID_NONE)
		return XTC_E_INVAL;
	for (;;) {
		if ((f = ht_lookup_pin(bm, pid)) != NULL) {
			atomic_fetch_add_explicit(&bm->s_hits, 1, memory_order_relaxed);
			*out_frame = f;
			return XTC_OK;
		}
		/* Miss: load into a free frame, then publish in the table.
		 * A concurrent loader of the same pid is resolved by re-checking
		 * the table after acquiring a frame. */
		f = get_free_frame(bm);
		if (f == NULL) return XTC_E_RESOURCE;
		/* pid-mode: no swip references this frame, so no stale fixer can
		 * transiently pin it -- a plain store is safe (and claim_frame's
		 * CAS-wait would deadlock against a latch-coupling pin). */
		atomic_store_explicit(&f->pin, 1, memory_order_release);
		atomic_store_explicit(&f->state, BM_LOADED, memory_order_relaxed);
		f->pid = pid;
		f->parent = NULL;
		f->via_pid = 1;
		atomic_store_explicit(&f->dirty, 0, memory_order_relaxed);
		atomic_store_explicit(&f->io_busy, 0, memory_order_relaxed);
		if (do_io(bm, f->page, pid, 0) != 0) { free_push(bm, f); return XTC_E_INTERNAL; }
		/* Publish: under the bucket's stripe lock, re-check no one beat us. */
		{
			uint32_t b = (uint32_t)(pid % bm->nbucket);
			pthread_mutex_t *lk = ht_lock(bm, b);
			bm_frame_t *e;
			(void)pthread_mutex_lock(lk);
			for (e = bm->buckets[b]; e != NULL; e = e->hnext)
				if (e->pid == pid && e->via_pid) break;
			if (e != NULL) {
				/* Lost the race; use the resident frame if we can pin it
				 * (it may be reserved for eviction -- then retry as a
				 * miss). */
				int pinned = try_pin(e);
				if (pinned &&
				    atomic_load_explicit(&e->state, memory_order_acquire) == BM_COOL)
					atomic_store_explicit(&e->state, BM_HOT, memory_order_release);
				(void)pthread_mutex_unlock(lk);
				f->via_pid = 0;
				free_push(bm, f);
				if (!pinned)
					continue;            /* being evicted: retry the lookup */
				*out_frame = e;
				atomic_fetch_add_explicit(&bm->s_hits, 1, memory_order_relaxed);
				return XTC_OK;
			}
			f->hnext = bm->buckets[b];
			bm->buckets[b] = f;
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

void bm_latch_shared(bm_frame_t *f)    { if (f) (void)xtc_arwlock_rdlock(f->latch, -1); }
void bm_latch_exclusive(bm_frame_t *f) { if (f) (void)xtc_arwlock_wrlock(f->latch, -1); }
void bm_unlatch(bm_frame_t *f)         { if (f) (void)xtc_arwlock_unlock(f->latch); }

/* ---- page-provider process ---- */
struct pp_arg { bm_t *bm; int64_t interval; };

static void
pp_proc(void *arg)
{
	struct pp_arg *pa = arg;
	bm_t *bm = pa->bm;
	int64_t interval = pa->interval;
	__os_free(pa);

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
			if (f->via_pid) {
				/* page-table mode: cool is a state flip. */
				atomic_store_explicit(&f->state, BM_COOL,
				    memory_order_release);
				atomic_fetch_add_explicit(&bm->s_cooled, 1,
				    memory_order_relaxed);
				need--;
			} else {
				uint64_t cw;
				if (has_resident_child(bm, f))
					continue;   /* cool children first */
				cw = atomic_load_explicit(f->parent, memory_order_acquire);
				if (sw_is_hot(cw) && sw_frame(cw) == f &&
				    atomic_compare_exchange_strong(f->parent, &cw, sw_cool(f))) {
					atomic_store_explicit(&f->state, BM_COOL, memory_order_release);
					atomic_fetch_add_explicit(&bm->s_cooled, 1, memory_order_relaxed);
					need--;
				}
			}
		}
		/* Pass 3: keep the free list above the cool target. */
		while (atomic_load_explicit(&bm->free_n, memory_order_relaxed)
		    < bm->cool_target) {
			if (!evict_one(bm)) break;
		}
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
	if ((rc = __os_calloc(1, sizeof *pa, (void **)&pa)) != XTC_OK) return rc;
	pa->bm = bm;
	pa->interval = interval_ns > 0 ? interval_ns : 5LL * 1000 * 1000;
	atomic_store_explicit(&bm->pp_running, 1, memory_order_release);
	atomic_store_explicit(&bm->pp_alive, 1, memory_order_release);
	opts.name = "bm-provider";
	rc = xtc_proc_spawn(loop, pp_proc, pa, &opts, &bm->pp_pid);
	if (rc != XTC_OK) { atomic_store_explicit(&bm->pp_running, 0, memory_order_release); atomic_store_explicit(&bm->pp_alive, 0, memory_order_release); __os_free(pa); return rc; }
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
	struct tr_cand *cand;
	xtc_dio_sched_t *tuner = NULL;
	int batch = TR_BATCH;
	int cap = bm->n_frames < 256 ? (int)bm->n_frames : 256;
	int max_batch = bm->adaptive_writeback ? (cap < 1 ? 1 : cap) : TR_BATCH;
	uint8_t *wbuf = NULL;
	bm_frame_t **run_f = NULL;
	__os_free(ta);
	if (max_batch < 1) max_batch = 1;
	if (__os_calloc(bm->n_frames, sizeof *cand, (void **)&cand) != XTC_OK)
		return;
	/* Gather buffer for coalesced multi-page writes (direct-I/O aligned)
	 * plus per-run frame scratch. */
	if (__os_aligned_alloc(4096, (size_t)max_batch * bm->page_size,
	    (void **)&wbuf) != XTC_OK ||
	    __os_calloc((size_t)max_batch, sizeof *run_f, (void **)&run_f)
	    != XTC_OK) {
		__os_aligned_free(wbuf);
		__os_free(run_f);
		__os_free(cand);
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
			cand[n].seq = atomic_load_explicit(&f->dirty_seq, memory_order_relaxed);
			n++;
		}
		int64_t t0 = 0, t1 = 0;
		int nsel;
		qsort(cand, (size_t)n, sizeof *cand, tr_cmp);   /* priority select */
		nsel = n < batch ? n : batch;
		if (nsel > max_batch) nsel = max_batch;         /* guard wbuf */
		(void)__os_clock_mono(&t0);
		/* Elevator-ordered, write-coalesced flush of the selected batch. */
		w = tr_flush_batch(bm, cand, nsel, wbuf, run_f);
		atomic_fetch_add_explicit(&bm->s_trickled, w, memory_order_relaxed);
		(void)__os_clock_mono(&t1);
		/* Adaptive pacing fitness: SUSTAINED writeback rate (pages
		 * per real second, counting the inter-pass sleep) DISCOUNTED
		 * by the dirty backlog.  Per-pass throughput alone rewarded
		 * large, infrequent batches that let dirty pages pile up and
		 * stalled eviction; multiplying by (1 - dirty_frac) makes the
		 * tuner favour pacing that keeps clean frames available while
		 * still batching for efficiency. */
		if (tuner != NULL && w > 0) {
			int g[XTC_DIO_SCHED_MAX_GENES];
			double cycle = (double)((t1 - t0) + iv) / 1e9;
			double rate, dfrac, f[2];
			if (cycle <= 0.0) cycle = 1e-9;
			rate = (double)w / cycle;            /* sustained pages/s */
			dfrac = bm->n_frames
			    ? (double)dirty_total / (double)bm->n_frames : 0.0;
			if (dfrac > 1.0) dfrac = 1.0;
			/* Phenotype 0 (batch): maximise throughput.
			 * Phenotype 1 (interval): maximise backlog quality
			 * (keep clean frames available). */
			f[0] = rate;
			f[1] = 1.0 - dfrac;
			xtc_dio_sched_report_multi(tuner, f, 2);
			xtc_dio_sched_current(tuner, g);
			batch = g[0];
			iv = (int64_t)g[1] * 1000000;
			if (iv <= 0) iv = 1000000;
		}
		if (xtc_proc_sleep(iv) != XTC_OK)
			break;
	}
	__os_free(cand);
	__os_aligned_free(wbuf);
	__os_free(run_f);
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
	if ((rc = __os_calloc(1, sizeof *ta, (void **)&ta)) != XTC_OK) return rc;
	ta->bm = bm;
	ta->interval = interval_ns > 0 ? interval_ns : 2LL * 1000 * 1000;
	atomic_store_explicit(&bm->tr_running, 1, memory_order_release);
	atomic_store_explicit(&bm->tr_alive, 1, memory_order_release);
	opts.name = "bm-trickler";
	rc = xtc_proc_spawn(loop, tr_proc, ta, &opts, &bm->tr_pid);
	if (rc != XTC_OK) { atomic_store_explicit(&bm->tr_running, 0, memory_order_release); atomic_store_explicit(&bm->tr_alive, 0, memory_order_release); __os_free(ta); return rc; }
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
