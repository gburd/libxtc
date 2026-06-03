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

#include "bufmgr.h"

#include "xtc_int.h"
#include "xtc_blocking.h"
#include "xtc_stats.h"
#include "xtc_sync.h"

#include <pthread.h>
#include <stdatomic.h>
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
	_Atomic uint64_t  dirty_seq;  /* order it was dirtied (recLSN proxy) */
	bm_pid_t          pid;
	bm_swip_t        *parent;     /* the Swip word that points here (swip mode) */
	int               via_pid;    /* 1: referenced through the page table */
	struct bm_frame  *hnext;      /* page-table hash chain (pid mode) */
	void             *page;       /* page_size bytes (into the pool) */
	struct bm_frame  *next_free;
	xtc_arwlock_t    *latch;      /* content latch (fiber-yielding) */
};

struct bm {
	int               fd;
	uint32_t          page_size;
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

	_Atomic uint32_t  clock;       /* round-robin victim cursor */

	/* page table (pid mode): pid -> resident frame */
	pthread_mutex_t   ht_mu;
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

	/* prefetch ring: read-ahead requests, drained by the provider so a
	 * scanning fiber never blocks on a prefetch (best-effort). */
	pthread_mutex_t   pf_mu;
	bm_pid_t          pf_ring[256];
	uint32_t          pf_head, pf_tail;
	_Atomic uint64_t  s_prefetched;

	/* stats */
	_Atomic uint64_t  s_hits, s_rescues, s_loads, s_cooled, s_flushed, s_evicted;
	_Atomic uint64_t  resident;
};

/* ---- offloaded page I/O (never holds a lock) ---- */
struct io_req { int fd; void *buf; size_t len; off_t off; int write; };
static int
io_fn(void *arg)
{
	struct io_req *r = arg;
	ssize_t n = r->write ? pwrite(r->fd, r->buf, r->len, r->off)
	                      : pread(r->fd, r->buf, r->len, r->off);
	if (n == (ssize_t)r->len) return 0;
	if (!r->write && n >= 0) { memset((char *)r->buf + n, 0, r->len - (size_t)n); return 0; }
	return -1;
}
static int
do_io(bm_t *bm, void *buf, bm_pid_t pid, int write)
{
	struct io_req r = { bm->fd, buf, bm->page_size,
	    (off_t)pid * (off_t)bm->page_size, write };
	int rc;
	if (xtc_blocking_run(io_fn, &r, &rc) != XTC_OK)
		rc = io_fn(&r);
	return rc;
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

/* ---- free list ---- */
static void
free_push(bm_t *bm, bm_frame_t *f)
{
	atomic_store_explicit(&f->pin, 0, memory_order_release);   /* clear any reservation */
	atomic_store_explicit(&f->state, BM_FREE, memory_order_release);
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
	if (__os_calloc(1, bm->page_size, (void **)&snap) != XTC_OK) {
		xtc_arwlock_unlock(f->latch);
		atomic_store_explicit(&f->io_busy, 0, memory_order_release);
		return 0;
	}
	memcpy(snap, f->page, bm->page_size);
	/* Clear dirty UNDER the latch: a later writer re-acquires the
	 * exclusive latch and re-dirties on bm_unfix, so its change is not
	 * lost -- the next flush captures it.  The snapshot we write is a
	 * consistent image as of this point. */
	atomic_store_explicit(&f->dirty, 0, memory_order_release);
	xtc_arwlock_unlock(f->latch);
	(void)do_io(bm, snap, f->pid, 1);
	__os_free(snap);
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
static void
ht_insert(bm_t *bm, bm_frame_t *f)
{
	uint32_t b = (uint32_t)(f->pid % bm->nbucket);
	(void)pthread_mutex_lock(&bm->ht_mu);
	f->hnext = bm->buckets[b];
	bm->buckets[b] = f;
	(void)pthread_mutex_unlock(&bm->ht_mu);
}
static void
ht_remove(bm_t *bm, bm_frame_t *f)
{
	uint32_t b = (uint32_t)(f->pid % bm->nbucket);
	bm_frame_t **pp;
	(void)pthread_mutex_lock(&bm->ht_mu);
	for (pp = &bm->buckets[b]; *pp != NULL; pp = &(*pp)->hnext)
		if (*pp == f) { *pp = f->hnext; break; }
	(void)pthread_mutex_unlock(&bm->ht_mu);
}
/* Look up pid; if resident, pin it and return the frame (caller holds
 * the pin).  Returns NULL on a miss. */
static bm_frame_t *
ht_lookup_pin(bm_t *bm, bm_pid_t pid)
{
	uint32_t b = (uint32_t)(pid % bm->nbucket);
	bm_frame_t *f;
	(void)pthread_mutex_lock(&bm->ht_mu);
	for (f = bm->buckets[b]; f != NULL; f = f->hnext) {
		if (f->pid == pid && f->via_pid) {
			if (!try_pin(f))
				break;            /* reserved for eviction: treat as a miss */
			if (atomic_load_explicit(&f->state, memory_order_acquire) == BM_COOL)
				atomic_store_explicit(&f->state, BM_HOT, memory_order_release);
			(void)pthread_mutex_unlock(&bm->ht_mu);
			return f;
		}
	}
	(void)pthread_mutex_unlock(&bm->ht_mu);
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
				(void)flush_frame(bm, f);
				continue;
			}
			if (atomic_load_explicit(&f->io_busy, memory_order_acquire))
				continue;
			if (!try_reserve(f))
				continue;            /* pinned: a fixer holds it */
			/* Reserved (pin == -1): no fixer can pin it now.  ht_remove
			 * under the table lock excludes a concurrent ht_lookup_pin. */
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
			(void)flush_frame(bm, f);   /* proactive write-out */
			continue;                   /* revisit to evict once clean */
		}
		if (atomic_load_explicit(&f->io_busy, memory_order_acquire))
			continue;
		if (!try_reserve(f))
			continue;                   /* pinned: a fixer holds it */
		/* Evict: parent COOL -> EVICTED.  Reserved (pin == -1), so no
		 * fixer can rescue it; release the reservation if the parent
		 * changed under us. */
		w = atomic_load_explicit(f->parent, memory_order_acquire);
		if (!sw_is_cool(w) || sw_frame(w) != f) {
			atomic_store_explicit(&f->pin, 0, memory_order_release); continue;
		}
		repl = sw_evicted(f->pid);
		if (!atomic_compare_exchange_strong(f->parent, &w, repl)) {
			atomic_store_explicit(&f->pin, 0, memory_order_release); continue;
		}
		atomic_fetch_sub_explicit(&bm->resident, 1, memory_order_relaxed);
		atomic_fetch_add_explicit(&bm->s_evicted, 1, memory_order_relaxed);
		free_push(bm, f);                   /* clears the reservation */
		return 1;
	  }
	  if (force_cool)
		return 0;        /* a full sweep cooling freely still found nothing */
	  force_cool = 1;   /* no COOL victim available; cool HOT and retry */
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
	(void)pthread_mutex_init(&bm->free_mu, NULL);
	(void)pthread_mutex_init(&bm->pf_mu, NULL);
	(void)pthread_mutex_init(&bm->pid_mu, NULL);

	bm->fd = open(opts->path ? opts->path : "/tmp/sqlxtc-bm.tmp",
	    opts->reopen ? (O_RDWR | O_CREAT) : (O_RDWR | O_CREAT | O_TRUNC), 0644);
	if (bm->fd < 0) { __os_free(bm); return XTC_E_INVAL; }

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
	(void)pthread_mutex_init(&bm->ht_mu, NULL);
	if ((rc = __os_calloc(bm->nbucket, sizeof *bm->buckets,
	    (void **)&bm->buckets)) != XTC_OK) {
		__os_aligned_free(bm->pool); __os_free(bm->frames);
		close(bm->fd); __os_free(bm); return rc;
	}
	bm->next_pid = 1;          /* pid 0 reserved as "none" / superblock */
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
	if (bm->frames != NULL) {
		uint32_t i;
		for (i = 0; i < bm->n_frames; i++)
			xtc_arwlock_destroy(bm->frames[i].latch);
	}
	(void)pthread_mutex_destroy(&bm->free_mu);
	(void)pthread_mutex_destroy(&bm->pf_mu);
	(void)pthread_mutex_destroy(&bm->pid_mu);
	(void)pthread_mutex_destroy(&bm->ht_mu);
	__os_free(bm->buckets);
	__os_aligned_free(bm->pool);
	__os_free(bm->frames);
	__os_free(bm);
}

static bm_pid_t
next_pid(bm_t *bm)
{
	bm_pid_t p;
	(void)pthread_mutex_lock(&bm->pid_mu);
	p = bm->next_pid++;
	(void)pthread_mutex_unlock(&bm->pid_mu);
	return p;
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
	atomic_store_explicit(&f->pin, 1, memory_order_relaxed);
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
			if (!try_pin(f))
				continue;             /* reserved for eviction: retry */
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
			if (!try_pin(f))
				continue;             /* reserved for eviction: retry */
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
			atomic_store_explicit(&f->pin, 1, memory_order_release);
			atomic_store_explicit(&f->state, BM_LOADED, memory_order_relaxed);
			f->pid = pid;
			f->parent = slot;
			atomic_store_explicit(&f->dirty, 0, memory_order_relaxed);
			atomic_store_explicit(&f->io_busy, 0, memory_order_relaxed);
			if (do_io(bm, f->page, pid, 0) != 0) {
				atomic_store_explicit(&f->pin, 0, memory_order_release);
				free_push(bm, f);
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
			/* Someone else resolved it; unpin, drop our frame, retry. */
			atomic_store_explicit(&f->pin, 0, memory_order_release);
			free_push(bm, f);
			continue;
		}
	}
}

void
bm_unfix(bm_t *bm, bm_frame_t *frame, int mark_dirty)
{
	if (frame == NULL) return;
	if (mark_dirty) {
		/* Stamp the dirtying order on the clean -> dirty edge, so the
		 * trickler can write oldest dirt first (a recLSN proxy). */
		if (atomic_exchange_explicit(&frame->dirty, 1, memory_order_acq_rel) == 0)
			frame->dirty_seq = atomic_fetch_add_explicit(&bm->dirty_clock,
			    1, memory_order_relaxed);
	}
	atomic_fetch_sub_explicit(&frame->pin, 1, memory_order_release);
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
		atomic_store_explicit(&f->pin, 1, memory_order_release);
		atomic_store_explicit(&f->state, BM_LOADED, memory_order_relaxed);
		f->pid = pid;
		f->parent = NULL;
		f->via_pid = 1;
		atomic_store_explicit(&f->dirty, 0, memory_order_relaxed);
		atomic_store_explicit(&f->io_busy, 0, memory_order_relaxed);
		if (do_io(bm, f->page, pid, 0) != 0) { atomic_store_explicit(&f->pin, 0, memory_order_release); free_push(bm, f); return XTC_E_INTERNAL; }
		/* Publish: under the table lock, re-check no one beat us. */
		{
			uint32_t b = (uint32_t)(pid % bm->nbucket);
			bm_frame_t *e;
			(void)pthread_mutex_lock(&bm->ht_mu);
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
				(void)pthread_mutex_unlock(&bm->ht_mu);
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
			 * try_reserve is not under ht_mu) cannot reserve and
			 * race the pin store. */
			atomic_store_explicit(&f->pin, 1, memory_order_release);
			atomic_store_explicit(&f->state,
			    bm->scan_resist ? BM_COOL : BM_HOT, memory_order_release);
			(void)pthread_mutex_unlock(&bm->ht_mu);
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
static void
tr_proc(void *arg)
{
	struct tr_arg *ta = arg;
	bm_t *bm = ta->bm;
	int64_t iv = ta->interval;
	struct tr_cand *cand;
	__os_free(ta);
	if (__os_calloc(bm->n_frames, sizeof *cand, (void **)&cand) != XTC_OK)
		return;
	while (atomic_load_explicit(&bm->tr_running, memory_order_acquire)) {
		uint32_t i;
		int n = 0, w = 0;
		for (i = 0; i < bm->n_frames; i++) {
			bm_frame_t *f = &bm->frames[i];
			uint8_t st;
			if (atomic_load_explicit(&f->pin, memory_order_acquire) != 0)
				continue;
			if (!atomic_load_explicit(&f->dirty, memory_order_acquire))
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
		qsort(cand, (size_t)n, sizeof *cand, tr_cmp);
		for (i = 0; i < (uint32_t)n && w < TR_BATCH; i++) {
			if (flush_frame(bm, cand[i].f)) {
				atomic_fetch_add_explicit(&bm->s_trickled, 1,
				    memory_order_relaxed);
				w++;
			}
		}
		if (xtc_proc_sleep(iv) != XTC_OK)
			break;
	}
	__os_free(cand);
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

/* fdatasync the backing file, offloaded so the loop is not blocked. */
struct sync_io { int fd; };
static int
sync_io_fn(void *arg)
{
	struct sync_io *s = arg;
	return fdatasync(s->fd) == 0 ? 0 : -1;
}
int
bm_sync(bm_t *bm)
{
	struct sync_io s;
	int rc;
	if (bm == NULL) return XTC_E_INVAL;
	s.fd = bm->fd;
	if (xtc_blocking_run(sync_io_fn, &s, &rc) != XTC_OK)
		rc = sync_io_fn(&s);
	return rc == 0 ? XTC_OK : XTC_E_INTERNAL;
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
