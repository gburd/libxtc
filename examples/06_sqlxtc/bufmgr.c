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
	bm_pid_t          pid;
	bm_swip_t        *parent;     /* the Swip word that points here */
	void             *page;       /* page_size bytes (into the pool) */
	struct bm_frame  *next_free;
};

struct bm {
	int               fd;
	uint32_t          page_size;
	uint32_t          n_frames;
	uint32_t          cool_target; /* keep this many frames free+cool */
	bm_frame_t       *frames;
	unsigned char    *pool;        /* n_frames * page_size, aligned */

	pthread_mutex_t   free_mu;
	bm_frame_t       *free_head;
	_Atomic uint32_t  free_n;

	pthread_mutex_t   pid_mu;
	bm_pid_t          next_pid;

	_Atomic uint32_t  clock;       /* round-robin victim cursor */

	/* page-provider */
	_Atomic int       pp_running;
	xtc_pid_t         pp_pid;

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

/* ---- free list ---- */
static void
free_push(bm_t *bm, bm_frame_t *f)
{
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

/* Write a dirty COOL/WRITING frame out, off the loop.  Returns 1 if it
 * is clean afterward.  Holds no lock across the I/O. */
static int
flush_frame(bm_t *bm, bm_frame_t *f)
{
	int expect = 0;
	if (!atomic_load_explicit(&f->dirty, memory_order_acquire))
		return 1;
	if (!atomic_compare_exchange_strong(&f->io_busy, &expect, 1))
		return 0;                       /* another writer owns it */
	(void)do_io(bm, f->page, f->pid, 1);
	/* Only clear dirty if the page was not rescued + remodified. */
	if (atomic_load_explicit(&f->state, memory_order_acquire) == BM_COOL)
		atomic_store_explicit(&f->dirty, 0, memory_order_release);
	atomic_store_explicit(&f->io_busy, 0, memory_order_release);
	atomic_fetch_add_explicit(&bm->s_flushed, 1, memory_order_relaxed);
	return atomic_load_explicit(&f->dirty, memory_order_acquire) == 0;
}

/* Try to drive one unpinned frame all the way to FREE.  Returns 1 if a
 * frame was reclaimed.  Coordinates with concurrent fixers via the CAS
 * on the parent Swip. */
static int
evict_one(bm_t *bm)
{
	uint32_t i, scanned = 0;
	for (scanned = 0; scanned < bm->n_frames * 2u; scanned++) {
		uint64_t w, repl;
		i = atomic_fetch_add_explicit(&bm->clock, 1, memory_order_relaxed)
		    % bm->n_frames;
		bm_frame_t *f = &bm->frames[i];
		uint8_t st = atomic_load_explicit(&f->state, memory_order_acquire);

		if (atomic_load_explicit(&f->pin, memory_order_acquire) != 0)
			continue;
		if (st == BM_HOT) {
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
		if (atomic_load_explicit(&f->pin, memory_order_acquire) != 0)
			continue;
		if (atomic_load_explicit(&f->io_busy, memory_order_acquire))
			continue;
		/* Evict: parent COOL -> EVICTED. */
		w = atomic_load_explicit(f->parent, memory_order_acquire);
		if (!sw_is_cool(w) || sw_frame(w) != f) continue;
		repl = sw_evicted(f->pid);
		if (!atomic_compare_exchange_strong(f->parent, &w, repl))
			continue;               /* rescued under us */
		atomic_fetch_sub_explicit(&bm->resident, 1, memory_order_relaxed);
		atomic_fetch_add_explicit(&bm->s_evicted, 1, memory_order_relaxed);
		free_push(bm, f);
		return 1;
	}
	return 0;
}

static bm_frame_t *
get_free_frame(bm_t *bm)
{
	bm_frame_t *f;
	int tries;
	for (tries = 0; tries < 10000; tries++) {
		if ((f = free_pop(bm)) != NULL)
			return f;
		if (!evict_one(bm)) {
			/* Everything pinned right now; yield and retry. */
			if (xtc_proc_sleep(200LL * 1000) != XTC_OK)
				xtc_yield();
		}
	}
	return NULL;
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
	bm->cool_target = opts->n_frames * (opts->cool_pct ? opts->cool_pct : 10)
	    / 100u;
	if (bm->cool_target < 1) bm->cool_target = 1;
	(void)pthread_mutex_init(&bm->free_mu, NULL);
	(void)pthread_mutex_init(&bm->pid_mu, NULL);

	bm->fd = open(opts->path ? opts->path : "/tmp/sqlxtc-bm.tmp",
	    O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (bm->fd < 0) { __os_free(bm); return XTC_E_INVAL; }

	if ((rc = __os_calloc(bm->n_frames, sizeof *bm->frames,
	    (void **)&bm->frames)) != XTC_OK) { close(bm->fd); __os_free(bm); return rc; }
	if ((rc = __os_aligned_alloc(4096,
	    (size_t)bm->n_frames * bm->page_size, (void **)&bm->pool)) != XTC_OK) {
		__os_free(bm->frames); close(bm->fd); __os_free(bm); return rc;
	}
	for (i = 0; i < bm->n_frames; i++) {
		bm->frames[i].page = bm->pool + (size_t)i * bm->page_size;
		free_push(bm, &bm->frames[i]);
	}
	bm->next_pid = 1;          /* pid 0 reserved as "none" */
	*out = bm;
	return XTC_OK;
}

void
bm_destroy(bm_t *bm)
{
	if (bm == NULL) return;
	bm_provider_stop(bm);
	if (bm->fd >= 0) close(bm->fd);
	(void)pthread_mutex_destroy(&bm->free_mu);
	(void)pthread_mutex_destroy(&bm->pid_mu);
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
			atomic_fetch_add_explicit(&f->pin, 1, memory_order_acquire);
			/* Recheck: it may have been cooled/evicted meanwhile. */
			if (atomic_load_explicit(slot, memory_order_acquire) == w) {
				atomic_fetch_add_explicit(&bm->s_hits, 1, memory_order_relaxed);
				*out_frame = f;
				return XTC_OK;
			}
			atomic_fetch_sub_explicit(&f->pin, 1, memory_order_release);
			continue;
		}
		if (sw_is_cool(w)) {
			bm_frame_t *f = sw_frame(w);
			/* Rescue: COOL -> HOT, then pin. */
			if (atomic_compare_exchange_strong(slot, &w, sw_hot(f))) {
				atomic_store_explicit(&f->state, BM_HOT, memory_order_release);
				atomic_fetch_add_explicit(&f->pin, 1, memory_order_acquire);
				atomic_fetch_add_explicit(&bm->s_rescues, 1, memory_order_relaxed);
				*out_frame = f;
				return XTC_OK;
			}
			continue;               /* changed; retry */
		}
		/* EVICTED: load from disk into a free frame. */
		{
			bm_pid_t pid = sw_pid(w);
			bm_frame_t *f = get_free_frame(bm);
			uint64_t expect = w;
			if (f == NULL) return XTC_E_RESOURCE;
			atomic_store_explicit(&f->state, BM_LOADED, memory_order_relaxed);
			f->pid = pid;
			f->parent = slot;
			atomic_store_explicit(&f->dirty, 0, memory_order_relaxed);
			atomic_store_explicit(&f->io_busy, 0, memory_order_relaxed);
			if (do_io(bm, f->page, pid, 0) != 0) {
				free_push(bm, f);
				return XTC_E_INTERNAL;
			}
			if (atomic_compare_exchange_strong(slot, &expect, sw_hot(f))) {
				atomic_store_explicit(&f->state, BM_HOT, memory_order_release);
				atomic_fetch_add_explicit(&f->pin, 1, memory_order_acquire);
				atomic_fetch_add_explicit(&bm->resident, 1, memory_order_relaxed);
				atomic_fetch_add_explicit(&bm->s_loads, 1, memory_order_relaxed);
				*out_frame = f;
				return XTC_OK;
			}
			/* Someone else resolved it; drop our frame and retry. */
			free_push(bm, f);
			continue;
		}
	}
}

void
bm_unfix(bm_t *bm, bm_frame_t *frame, int mark_dirty)
{
	(void)bm;
	if (frame == NULL) return;
	if (mark_dirty)
		atomic_store_explicit(&frame->dirty, 1, memory_order_release);
	atomic_fetch_sub_explicit(&frame->pin, 1, memory_order_release);
}

void *
bm_page(bm_frame_t *frame) { return frame ? frame->page : NULL; }

bm_pid_t
bm_frame_pid(const bm_frame_t *frame) { return frame ? frame->pid : BM_PID_NONE; }

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
		uint32_t i;
		/* Proactive pass: cool hot frames and flush dirty cool ones so
		 * reclaiming a frame later is a cheap state flip. */
		for (i = 0; i < bm->n_frames; i++) {
			bm_frame_t *f = &bm->frames[i];
			uint8_t st = atomic_load_explicit(&f->state, memory_order_acquire);
			if (atomic_load_explicit(&f->pin, memory_order_acquire) != 0)
				continue;
			if (st == BM_HOT &&
			    atomic_load_explicit(&bm->free_n, memory_order_relaxed)
			    < bm->cool_target) {
				uint64_t cw = atomic_load_explicit(f->parent, memory_order_acquire);
				if (sw_is_hot(cw) && sw_frame(cw) == f &&
				    atomic_compare_exchange_strong(f->parent, &cw, sw_cool(f))) {
					atomic_store_explicit(&f->state, BM_COOL, memory_order_release);
					atomic_fetch_add_explicit(&bm->s_cooled, 1, memory_order_relaxed);
				}
			} else if (st == BM_COOL &&
			    atomic_load_explicit(&f->dirty, memory_order_acquire)) {
				(void)flush_frame(bm, f);     /* proactive write-out */
			}
		}
		/* Keep the free list above the cool target. */
		while (atomic_load_explicit(&bm->free_n, memory_order_relaxed)
		    < bm->cool_target) {
			if (!evict_one(bm)) break;
		}
		if (xtc_proc_sleep(interval) != XTC_OK)
			break;
	}
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
	opts.name = "bm-provider";
	rc = xtc_proc_spawn(loop, pp_proc, pa, &opts, &bm->pp_pid);
	if (rc != XTC_OK) { atomic_store_explicit(&bm->pp_running, 0, memory_order_release); __os_free(pa); return rc; }
	if (out_pid) *out_pid = bm->pp_pid;
	return XTC_OK;
}

void
bm_provider_stop(bm_t *bm)
{
	if (bm == NULL) return;
	atomic_store_explicit(&bm->pp_running, 0, memory_order_release);
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
}
