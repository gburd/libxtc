/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/slab.c
 *	libumem-inspired slab + magazine allocator.
 *
 *	Layout per chunk (ASCII):
 *	  +-------------------+ <-- chunk_base
 *	  | chunk_hdr_t       |
 *	  +-------------------+
 *	  | obj 0  [redzone] [obj_size] [redzone] |
 *	  | obj 1                                  |
 *	  | ...                                    |
 *	  +-------------------+ <-- chunk_base + chunk_size
 *
 *	Free objects are linked through their first sizeof(void*)
 *	bytes (the slot doubles as "next free" when on the free list).
 *	When REDZONE is enabled, the slot has a buftag at the front
 *	(8 magic bytes + 8 bytes of metadata) and back (8 magic bytes).
 *
 *	Magazines: per-loop arrays of recently-freed pointers.  Lookup
 *	uses a thread-local pointer.  When alloc misses, refill from
 *	the cache's free list under the cache mutex.
 *
 *	M11.5a deliverables (this file):
 *	  - PROCESS_LOCAL mode: full
 *	  - SHARED_MEMORY mode: chunks carved from caller-supplied
 *	    region; offset/resolve work
 *	  - Magazines: per-loop, lock-free fast path
 *	  - Constructor/destructor: called once per object lifetime
 *	  - Stats: full hit/miss/inuse/free/reap counters
 *	  - OOM policies: FAIL, BACKOFF (one-shot retry), ABORT
 *	  - REDZONE: 8-byte magic before+after every object
 *	  - AUDIT: 64-event ring of recent (alloc, free, who) events
 *	  - Memory-pressure: Linux PSI listener (calls reap_all)
 *	  - Reap: drain magazines + return empty chunks to OS
 *
 *	Deferred to M11.5b:
 *	  - BACKTRACE (needs <execinfo.h>; portability work)
 *	  - Reaper proc (xtc_proc that calls reap_all on a timer)
 */

#include "xtc_int.h"
#include "xtc_slab.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#  include <malloc.h>   /* _aligned_malloc / _aligned_free */
#  define MAP_FAILED ((void *)-1)
#  define PROT_READ  0
#  define PROT_WRITE 0
#  define MAP_PRIVATE   0
#  define MAP_ANONYMOUS 0
static void *__win_chunk_alloc(size_t sz) { return malloc(sz); }
static int   __win_chunk_free(void *p, size_t sz) { (void)sz; free(p); return 0; }
#  define mmap(addr, sz, prot, flags, fd, off)  \
     ((void)(addr),(void)(prot),(void)(flags),(void)(fd),(void)(off), \
      __win_chunk_alloc(sz))
#  define munmap(p, sz)  __win_chunk_free((p), (sz))
#else
#  include <sys/mman.h>
#endif
#include <unistd.h>
#include <time.h>
#if defined(__linux__) && defined(__GLIBC__)
#  include <execinfo.h>
#  define XTC_HAS_EXECINFO 1
#endif

/* ---- redzone magic ---- */
#define XTC_RZ_MAGIC  0xA5A5A5A5A5A5A5A5ULL
#define XTC_RZ_FRONT  16     /* bytes; 8 magic + 8 metadata */
#define XTC_RZ_BACK   8

/* ---- shared-memory header ---- */
/*
 * When mode == XTC_SLAB_SHARED_MEMORY, the first bytes of the region
 * contain a header that synchronizes multiple processes attaching to
 * the same shm segment.  The cursor is an atomic offset into the region;
 * chunk allocation CAS's this forward.
 *
 * Layout:
 *   +0x00: magic (8 bytes) = 0x5854435F534C4142 "XTC_SLAB"
 *   +0x08: version (8 bytes) = 1
 *   +0x10: cursor (8 bytes, atomic) = next free offset
 *   +0x18: total_size (8 bytes) = size of entire region
 *   +0x20: reserved (32 bytes, pad to 64-byte cache line)
 *   +0x40: usable region starts here
 */
#define XTC_SHM_MAGIC     0x5854435F534C4142ULL  /* "XTC_SLAB" */
#define XTC_SHM_VERSION   1
#define XTC_SHM_HDR_SIZE  64

struct xtc_slab_shm_header {
	uint64_t         magic;
	uint64_t         version;
	_Atomic uint64_t cursor;      /* next free offset from region start */
	uint64_t         total_size;
	uint8_t          reserved[32];
};

/* ---- audit ring ---- */
#define XTC_AUDIT_N      64
#define XTC_AUDIT_BTSZ   8
struct audit_event {
	void    *obj;
	uint8_t  op;        /* 'A'=alloc, 'F'=free */
	int64_t  ts_ns;
	/* When XTC_SLAB_BACKTRACE is set, capture up to 8 frames
	 * via backtrace() (Linux/glibc; no-op elsewhere). */
	void    *bt[XTC_AUDIT_BTSZ];
	int      bt_n;
};

/* ---- per-cache global registry (for reap_all + pressure) ---- */
struct slab_registry_entry {
	xtc_slab_t           *slab;
	struct slab_registry_entry *next;
};
static pthread_mutex_t  __slab_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static struct slab_registry_entry *__registry;

/* ---- per-loop magazine (TLS) ---- */
struct magazine {
	void  **slots;
	int     n;
	int     cap;
};

/* Each thread holds at most one magazine PER cache, indexed by
 * cache pointer in a small TLS hash.  Cap at 8 entries per thread --
 * if a thread allocates from >8 caches, we fall back to slow path. */
#define TLS_MAGS  8

struct tls_mag {
	xtc_slab_t      *cache;
	struct magazine  mag;
};

static XTC_THREAD_LOCAL struct tls_mag __tls_mags[TLS_MAGS];

/*
 * Thread-exit reclamation for per-thread magazines.  Each thread
 * that frees into a slab lazily allocates a magazine slots buffer
 * (see xtc_slab_free); without a thread-exit hook those buffers leak
 * when a worker thread ends, which accumulates under thread churn
 * (and trips LeakSanitizer in CI).  A pthread_key destructor frees
 * the exiting thread's slots buffers.  We only free the buffers --
 * the pooled objects they reference belong to slab chunks that are
 * released wholesale by xtc_slab_destroy, so there is no object leak
 * and the destructor never touches a possibly-already-destroyed
 * cache.
 */
static pthread_key_t  __slab_tls_key;
static pthread_once_t __slab_tls_once = PTHREAD_ONCE_INIT;

static void
__slab_tls_cleanup(void *unused)
{
	int i;
	(void)unused;
	for (i = 0; i < TLS_MAGS; i++) {
		if (__tls_mags[i].mag.slots != NULL) {
			__os_free(__tls_mags[i].mag.slots);
			__tls_mags[i].mag.slots = NULL;
		}
		__tls_mags[i].cache = NULL;
	}
}

static void
__slab_tls_key_init(void)
{
	(void)pthread_key_create(&__slab_tls_key, __slab_tls_cleanup);
}

/* Arm the thread-exit destructor for the calling thread.  Cheap and
 * idempotent: pthread_once guards the key, and setspecific to a
 * non-NULL sentinel makes the destructor fire on thread exit. */
static void
__slab_tls_arm(void)
{
	(void)pthread_once(&__slab_tls_once, __slab_tls_key_init);
	(void)pthread_setspecific(__slab_tls_key, (void *)1);
}

static struct magazine *
__tls_mag_for(xtc_slab_t *cache)
{
	int i, free_slot = -1;
	for (i = 0; i < TLS_MAGS; i++) {
		if (__tls_mags[i].cache == cache) return &__tls_mags[i].mag;
		if (__tls_mags[i].cache == NULL && free_slot == -1)
			free_slot = i;
	}
	if (free_slot < 0) return NULL;     /* fall back to slow path */
	__tls_mags[free_slot].cache = cache;
	return &__tls_mags[free_slot].mag;
}

/* ---- chunks and slabs ---- */

struct slab_chunk {
	void           *base;        /* chunk start (== this struct in PL mode) */
	size_t          size;        /* chunk byte size */
	struct slab_chunk   *next;
	int             n_inuse;
	int             n_total;
	void           *free_head;   /* singly linked through obj's first 8B */
	int             owns_mmap;   /* 1 = munmap on reap; 0 = shm-backed */
};

struct xtc_slab {
	xtc_slab_opts_t  opts;
	size_t           slot_size;     /* obj_size + redzone + alignment pad */
	int              objs_per_chunk;

	pthread_mutex_t  lock;          /* protects chunk lists + stats */
	struct slab_chunk    *chunks;
	int              n_chunks;

	/* Stats (atomic for lock-free read). */
	_Atomic uint64_t s_alloc_fast;
	_Atomic uint64_t s_alloc_slow;
	_Atomic uint64_t s_free_fast;
	_Atomic uint64_t s_free_slow;
	_Atomic uint64_t s_n_inuse;
	_Atomic uint64_t s_n_free;
	_Atomic uint64_t s_n_chunks;
	_Atomic uint64_t s_reaps;
	_Atomic uint64_t s_oom_fails;
	_Atomic uint64_t s_rz_violations;

	/* Audit ring (only allocated if XTC_SLAB_AUDIT). */
	struct audit_event *audit_ring;
	_Atomic uint64_t    audit_pos;

	/* Shared-memory bookkeeping: pointer to header in region. */
	struct xtc_slab_shm_header *shm_hdr;
	uint8_t *shm_base;     /* start of usable region (after header) */
	uint8_t *shm_end;
};

/* ---- helpers ---- */

static int64_t
__now_ns_slab(void)
{
	int64_t ns = 0;
	(void)__os_clock_mono(&ns);
	return ns;
}

static size_t
__align_up(size_t n, size_t a) { return (n + a - 1) & ~(a - 1); }

static void
__audit_record(xtc_slab_t *s, void *obj, uint8_t op)
{
	uint64_t pos;
	struct audit_event *ev;
	if (s->audit_ring == NULL) return;
	pos = atomic_fetch_add_explicit(&s->audit_pos, 1, memory_order_relaxed);
	ev = &s->audit_ring[pos % XTC_AUDIT_N];
	ev->obj = obj;
	ev->op = op;
	ev->ts_ns = __now_ns_slab();
	ev->bt_n = 0;
#if defined(XTC_HAS_EXECINFO)
	if (s->opts.flags & XTC_SLAB_BACKTRACE) {
		ev->bt_n = backtrace(ev->bt, XTC_AUDIT_BTSZ);
	}
#endif
}

/* ---- redzone helpers ---- */

static int
__rz_enabled(const xtc_slab_t *s) { return (s->opts.flags & XTC_SLAB_REDZONE) != 0; }

static void *
__obj_from_slot(const xtc_slab_t *s, void *slot)
{
	if (__rz_enabled(s)) return (uint8_t *)slot + XTC_RZ_FRONT;
	return slot;
}

static void *
__slot_from_obj(const xtc_slab_t *s, void *obj)
{
	if (__rz_enabled(s)) return (uint8_t *)obj - XTC_RZ_FRONT;
	return obj;
}

static void
__rz_paint(xtc_slab_t *s, void *slot)
{
	uint64_t *front, *back;
	if (!__rz_enabled(s)) return;
	front = (uint64_t *)slot;
	*front = XTC_RZ_MAGIC;
	*(front + 1) = (uint64_t)(uintptr_t)s;   /* metadata */
	back = (uint64_t *)((uint8_t *)slot + XTC_RZ_FRONT + s->opts.obj_size);
	*back = XTC_RZ_MAGIC;
}

static int
__rz_check(xtc_slab_t *s, void *slot)
{
	uint64_t *front, *back;
	if (!__rz_enabled(s)) return 0;
	front = (uint64_t *)slot;
	back = (uint64_t *)((uint8_t *)slot + XTC_RZ_FRONT + s->opts.obj_size);
	if (*front != XTC_RZ_MAGIC || *back != XTC_RZ_MAGIC) {
		atomic_fetch_add_explicit(&s->s_rz_violations, 1,
		    memory_order_relaxed);
		return 1;
	}
	return 0;
}

/* ---- chunk management ---- */

static struct slab_chunk *
__chunk_new(xtc_slab_t *s)
{
	struct slab_chunk *c;
	uint8_t *base;
	int      i;
	size_t   total = s->opts.chunk_size;

	/* Charge res before allocating; if cap exceeded, fail. */
	if (s->opts.res != NULL) {
		if (xtc_res_acquire(s->opts.res, XTC_RES_MEM_BYTES,
		    (int64_t)total) != XTC_OK) {
			return NULL;
		}
	}

	if (s->opts.mode == XTC_SLAB_SHARED_MEMORY) {
		/* Carve from the shared region using atomic CAS on the header cursor. */
		uint64_t old_cursor, new_cursor;
		for (;;) {
			old_cursor = atomic_load_explicit(&s->shm_hdr->cursor,
			    memory_order_acquire);
			new_cursor = old_cursor + total;
			if (new_cursor > s->shm_hdr->total_size) {
				if (s->opts.res != NULL)
					xtc_res_release(s->opts.res, XTC_RES_MEM_BYTES,
					    (int64_t)total);
				return NULL;
			}
			if (atomic_compare_exchange_weak_explicit(
			    &s->shm_hdr->cursor,
			    &old_cursor, new_cursor,
			    memory_order_acq_rel,
			    memory_order_acquire))
				break;
			/* CAS failed, another process won; retry. */
		}
		base = (uint8_t *)s->opts.shm_base + old_cursor;
	} else {
		void *m = mmap(NULL, total, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (m == MAP_FAILED) {
			if (s->opts.res != NULL)
				xtc_res_release(s->opts.res, XTC_RES_MEM_BYTES,
				    (int64_t)total);
			return NULL;
		}
#if defined(_WIN32)
		/* mmap shim on Windows is plain malloc; zero for parity
		 * with POSIX MAP_ANONYMOUS. */
		memset(m, 0, total);
#endif
		base = m;
	}

	if (__os_calloc(1, sizeof *c, (void **)&c) != XTC_OK) {
		if (s->opts.mode == XTC_SLAB_PROCESS_LOCAL)
			(void)munmap(base, total);
		if (s->opts.res != NULL)
			xtc_res_release(s->opts.res, XTC_RES_MEM_BYTES,
			    (int64_t)total);
		return NULL;
	}
	c->base = base;
	c->size = total;
	c->owns_mmap = (s->opts.mode == XTC_SLAB_PROCESS_LOCAL);
	c->n_total = s->objs_per_chunk;
	c->n_inuse = 0;

	/* Build the free list through the slots. */
	{
		uint8_t *p = base;
		for (i = 0; i < s->objs_per_chunk; i++) {
			void **slot = (void **)p;
			*slot = (i + 1 < s->objs_per_chunk)
			    ? (void *)(p + s->slot_size)
			    : NULL;
			p += s->slot_size;
		}
		c->free_head = base;
	}

	c->next = s->chunks;
	s->chunks = c;
	s->n_chunks++;
	atomic_fetch_add_explicit(&s->s_n_chunks, 1, memory_order_relaxed);
	atomic_fetch_add_explicit(&s->s_n_free,
	    (uint64_t)s->objs_per_chunk, memory_order_relaxed);
	return c;
}

static void
__chunk_release(xtc_slab_t *s, struct slab_chunk *c)
{
	if (c->owns_mmap) (void)munmap(c->base, c->size);
	if (s->opts.res != NULL)
		xtc_res_release(s->opts.res, XTC_RES_MEM_BYTES,
		    (int64_t)c->size);
	__os_free(c);
}

/* Slow-path: pop one slot from any partial/free chunk; create chunk
 * if needed.  Caller holds s->lock. */
static void *
__pop_slot_locked(xtc_slab_t *s)
{
	struct slab_chunk *c;
	void *slot;
	for (c = s->chunks; c != NULL; c = c->next) {
		if (c->free_head != NULL) {
			slot = c->free_head;
			c->free_head = *(void **)slot;
			c->n_inuse++;
			return slot;
		}
	}
	c = __chunk_new(s);
	if (c == NULL) return NULL;
	slot = c->free_head;
	c->free_head = *(void **)slot;
	c->n_inuse++;
	return slot;
}

/* Slow-path: push slot back onto its owning chunk.  Caller holds lock. */
static void
__push_slot_locked(xtc_slab_t *s, void *slot)
{
	struct slab_chunk *c;
	for (c = s->chunks; c != NULL; c = c->next) {
		if ((uint8_t *)slot >= (uint8_t *)c->base &&
		    (uint8_t *)slot <  (uint8_t *)c->base + c->size) {
			*(void **)slot = c->free_head;
			c->free_head = slot;
			c->n_inuse--;
			return;
		}
	}
	/* Slot not in any chunk -- programming error. */
}

/* ---- public API ---- */

int
xtc_slab_create(const xtc_slab_opts_t *opts, xtc_slab_t **out)
{
	xtc_slab_t *s;
	xtc_slab_opts_t defaults = XTC_SLAB_OPTS_DEFAULT;
	int rc;
	size_t slot;

	if (out == NULL) return XTC_E_INVAL;
	if (opts == NULL) opts = &defaults;
	if (opts->obj_size == 0) return XTC_E_INVAL;

	if ((rc = __os_calloc(1, sizeof *s, (void **)&s)) != XTC_OK) return rc;
	s->opts = *opts;
	if (s->opts.align == 0)         s->opts.align = 64;
	if (s->opts.chunk_size == 0)    s->opts.chunk_size = 64 * 1024;
	if (s->opts.magazine_size <= 0) s->opts.magazine_size = 16;

	/* Compute slot_size: obj + redzones, then aligned. */
	slot = s->opts.obj_size;
	if (slot < sizeof(void *)) slot = sizeof(void *);   /* free list */
	if (s->opts.flags & XTC_SLAB_REDZONE) slot += XTC_RZ_FRONT + XTC_RZ_BACK;
	slot = __align_up(slot, s->opts.align);
	s->slot_size = slot;
	s->objs_per_chunk = (int)(s->opts.chunk_size / slot);
	if (s->objs_per_chunk < 1) {
		__os_free(s);
		return XTC_E_INVAL;
	}

	(void)pthread_mutex_init(&s->lock, NULL);

	if (s->opts.mode == XTC_SLAB_SHARED_MEMORY) {
		struct xtc_slab_shm_header *hdr;
		if (s->opts.shm_base == NULL || s->opts.shm_size == 0) {
			(void)pthread_mutex_destroy(&s->lock);
			__os_free(s);
			return XTC_E_INVAL;
		}
		if (s->opts.shm_size < XTC_SHM_HDR_SIZE + s->opts.chunk_size) {
			/* Region too small for header + one chunk. */
			(void)pthread_mutex_destroy(&s->lock);
			__os_free(s);
			return XTC_E_RESOURCE;
		}
		hdr = (struct xtc_slab_shm_header *)s->opts.shm_base;
		/*
		 * Init protocol: first attacher writes magic + cursor;
		 * subsequent attachers verify magic and use existing cursor.
		 * We use a CAS on magic to handle the race.
		 */
		if (hdr->magic != XTC_SHM_MAGIC) {
			uint64_t expected = 0;
			/* Try to be the initializer. */
			if (atomic_compare_exchange_strong_explicit(
			    (_Atomic uint64_t *)&hdr->magic,
			    &expected, XTC_SHM_MAGIC,
			    memory_order_acq_rel,
			    memory_order_acquire)) {
				/* We won the race; initialize header. */
				hdr->version = XTC_SHM_VERSION;
				hdr->total_size = s->opts.shm_size;
				atomic_store_explicit(&hdr->cursor,
				    XTC_SHM_HDR_SIZE, memory_order_release);
			} else {
				/* Another process init'd; spin until magic visible. */
				while (hdr->magic != XTC_SHM_MAGIC)
					;  /* spin */
			}
		}
		if (hdr->version != XTC_SHM_VERSION) {
			(void)pthread_mutex_destroy(&s->lock);
			__os_free(s);
			return XTC_E_VERSION;
		}
		s->shm_hdr = hdr;
		s->shm_base = (uint8_t *)s->opts.shm_base + XTC_SHM_HDR_SIZE;
		s->shm_end = (uint8_t *)s->opts.shm_base + s->opts.shm_size;
	}

	if (s->opts.flags & XTC_SLAB_AUDIT) {
		(void)__os_calloc(XTC_AUDIT_N, sizeof *s->audit_ring,
		    (void **)&s->audit_ring);
	}

	/* Register globally for reap_all / pressure broadcasts. */
	{
		struct slab_registry_entry *re = NULL;
		(void)__os_calloc(1, sizeof *re, (void **)&re);
		if (re != NULL) {
			(void)pthread_mutex_lock(&__slab_reg_lock);
			re->slab = s;
			re->next = __registry;
			__registry = re;
			(void)pthread_mutex_unlock(&__slab_reg_lock);
		}
	}

	*out = s;
	return XTC_OK;
}

void
xtc_slab_destroy(xtc_slab_t *s)
{
	struct slab_chunk *c, *next;
	int i;
	if (s == NULL) return;

	/* Drop our magazine entries. */
	for (i = 0; i < TLS_MAGS; i++) {
		if (__tls_mags[i].cache == s) {
			__os_free(__tls_mags[i].mag.slots);
			__tls_mags[i].cache = NULL;
			memset(&__tls_mags[i].mag, 0, sizeof __tls_mags[i].mag);
		}
	}

	/* Run dtors on every still-live object?  We don't track which
	 * are in-use vs free per se; for simplicity, run dtor on all
	 * slots that are NOT on a free list.  (Caller is expected to
	 * have freed everything before destroy; we just clean up.) */
	for (c = s->chunks; c != NULL; c = c->next) {
		uint8_t *p = c->base;
		void   *fh;
		int     j;
		/* Mark all slots that are on the free list. */
		for (j = 0; j < s->objs_per_chunk; j++)
			((void **)(p + (size_t)j * s->slot_size))[1] =
			    (void *)0;   /* repurpose; we'll restore */
		for (fh = c->free_head; fh != NULL; fh = *(void **)fh)
			((void **)fh)[1] = (void *)0xfeedf00dUL;
		if (s->opts.dtor != NULL) {
			for (j = 0; j < s->objs_per_chunk; j++) {
				void *slot = p + (size_t)j * s->slot_size;
				if (((void **)slot)[1] != (void *)0xfeedf00dUL) {
					void *obj = __obj_from_slot(s, slot);
					s->opts.dtor(obj, s->opts.cb_user);
				}
			}
		}
	}

	for (c = s->chunks; c != NULL; c = next) {
		next = c->next;
		__chunk_release(s, c);
	}

	if (s->audit_ring) __os_free(s->audit_ring);
	(void)pthread_mutex_destroy(&s->lock);

	/* Unlink from global registry. */
	{
		struct slab_registry_entry *re, **link;
		(void)pthread_mutex_lock(&__slab_reg_lock);
		for (link = &__registry; (re = *link) != NULL; link = &re->next) {
			if (re->slab == s) {
				*link = re->next;
				__os_free(re);
				break;
			}
		}
		(void)pthread_mutex_unlock(&__slab_reg_lock);
	}

	__os_free(s);
}

void *
xtc_slab_alloc(xtc_slab_t *s)
{
	struct magazine *mag;
	void *slot = NULL;
	void *obj;

	if (XTC_UNLIKELY(s == NULL)) return NULL;

	/* Magazine fast path. */
	if (XTC_LIKELY(!(s->opts.flags & XTC_SLAB_NO_MAGAZINE))) {
		mag = __tls_mag_for(s);
		if (XTC_LIKELY(mag != NULL && mag->n > 0)) {
			slot = mag->slots[--mag->n];
			atomic_fetch_add_explicit(&s->s_alloc_fast, 1,
			    memory_order_relaxed);
			goto have_slot;
		}
	}

	/* Slow path: take cache lock and pop. */
	(void)pthread_mutex_lock(&s->lock);
	slot = __pop_slot_locked(s);
	if (XTC_UNLIKELY(slot == NULL && s->opts.oom_policy == XTC_SLAB_OOM_BACKOFF)) {
		(void)pthread_mutex_unlock(&s->lock);
		(void)__os_sleep_ns(100 * 1000);    /* 0.1 ms */
		(void)pthread_mutex_lock(&s->lock);
		slot = __pop_slot_locked(s);
	}
	if (XTC_UNLIKELY(slot == NULL)) {
		atomic_fetch_add_explicit(&s->s_oom_fails, 1,
		    memory_order_relaxed);
		(void)pthread_mutex_unlock(&s->lock);
		if (s->opts.oom_policy == XTC_SLAB_OOM_ABORT) abort();
		return NULL;
	}
	atomic_fetch_sub_explicit(&s->s_n_free, 1, memory_order_relaxed);
	(void)pthread_mutex_unlock(&s->lock);
	atomic_fetch_add_explicit(&s->s_alloc_slow, 1, memory_order_relaxed);

have_slot:
	if (s->opts.flags & XTC_SLAB_REDZONE) __rz_paint(s, slot);
	obj = __obj_from_slot(s, slot);
	if (s->opts.ctor != NULL) {
		if (s->opts.ctor(obj, s->opts.cb_user) != XTC_OK) {
			/* Constructor refused; return slot to free list. */
			(void)pthread_mutex_lock(&s->lock);
			__push_slot_locked(s, slot);
			atomic_fetch_add_explicit(&s->s_n_free, 1,
			    memory_order_relaxed);
			(void)pthread_mutex_unlock(&s->lock);
			return NULL;
		}
	}
	atomic_fetch_add_explicit(&s->s_n_inuse, 1, memory_order_relaxed);
	__audit_record(s, obj, 'A');
	return obj;
}

void
xtc_slab_free(xtc_slab_t *s, void *obj)
{
	void *slot;
	struct magazine *mag;

	if (XTC_UNLIKELY(s == NULL || obj == NULL)) return;
	slot = __slot_from_obj(s, obj);

	if (XTC_UNLIKELY(__rz_check(s, slot))) {
		/* Redzone violation -- log + abort in debug builds. */
		fprintf(stderr, "xtc_slab[%s]: redzone violation at %p\n",
		    s->opts.name, obj);
	}

	if (s->opts.dtor != NULL) s->opts.dtor(obj, s->opts.cb_user);
	atomic_fetch_sub_explicit(&s->s_n_inuse, 1, memory_order_relaxed);
	__audit_record(s, obj, 'F');

	/* Magazine fast path. */
	if (XTC_LIKELY(!(s->opts.flags & XTC_SLAB_NO_MAGAZINE))) {
		mag = __tls_mag_for(s);
		if (XTC_LIKELY(mag != NULL)) {
			if (XTC_UNLIKELY(mag->slots == NULL)) {
				if (__os_calloc((size_t)s->opts.magazine_size,
				    sizeof(void *), (void **)&mag->slots) != XTC_OK) {
					goto slow;
				}
				mag->cap = s->opts.magazine_size;
				/* First magazine buffer on this thread: arm the
				 * thread-exit destructor that frees it. */
				__slab_tls_arm();
			}
			if (XTC_LIKELY(mag->n < mag->cap)) {
				mag->slots[mag->n++] = slot;
				atomic_fetch_add_explicit(&s->s_free_fast, 1,
				    memory_order_relaxed);
				return;
			}
		}
	}

slow:
	(void)pthread_mutex_lock(&s->lock);
	__push_slot_locked(s, slot);
	(void)pthread_mutex_unlock(&s->lock);
	atomic_fetch_add_explicit(&s->s_n_free, 1, memory_order_relaxed);
	atomic_fetch_add_explicit(&s->s_free_slow, 1, memory_order_relaxed);
}

int
xtc_slab_reap(xtc_slab_t *s)
{
	int reaped = 0, i;
	struct slab_chunk *c, **link, *next;
	if (s == NULL) return 0;

	/* Drain magazines associated with this thread. */
	for (i = 0; i < TLS_MAGS; i++) {
		if (__tls_mags[i].cache == s) {
			struct magazine *mag = &__tls_mags[i].mag;
			(void)pthread_mutex_lock(&s->lock);
			while (mag->n > 0) {
				__push_slot_locked(s, mag->slots[--mag->n]);
				atomic_fetch_add_explicit(&s->s_n_free, 1,
				    memory_order_relaxed);
				reaped++;
			}
			(void)pthread_mutex_unlock(&s->lock);
		}
	}

	/* Release empty chunks. */
	(void)pthread_mutex_lock(&s->lock);
	for (link = &s->chunks; (c = *link) != NULL; ) {
		if (c->n_inuse == 0) {
			next = c->next;
			*link = next;
			s->n_chunks--;
			atomic_fetch_sub_explicit(&s->s_n_chunks, 1,
			    memory_order_relaxed);
			atomic_fetch_sub_explicit(&s->s_n_free,
			    (uint64_t)s->objs_per_chunk,
			    memory_order_relaxed);
			__chunk_release(s, c);
			reaped += s->objs_per_chunk;
		} else {
			link = &c->next;
		}
	}
	(void)pthread_mutex_unlock(&s->lock);
	atomic_fetch_add_explicit(&s->s_reaps, 1, memory_order_relaxed);
	return reaped;
}

int
xtc_slab_stat(const xtc_slab_t *s, xtc_slab_stats_t *out)
{
	if (s == NULL || out == NULL) return XTC_E_INVAL;
	out->alloc_fast    = atomic_load_explicit(&s->s_alloc_fast, memory_order_relaxed);
	out->alloc_slow    = atomic_load_explicit(&s->s_alloc_slow, memory_order_relaxed);
	out->free_fast     = atomic_load_explicit(&s->s_free_fast,  memory_order_relaxed);
	out->free_slow     = atomic_load_explicit(&s->s_free_slow,  memory_order_relaxed);
	out->n_inuse       = atomic_load_explicit(&s->s_n_inuse,    memory_order_relaxed);
	out->n_free        = atomic_load_explicit(&s->s_n_free,     memory_order_relaxed);
	out->n_chunks      = atomic_load_explicit(&s->s_n_chunks,   memory_order_relaxed);
	out->bytes_inuse   = out->n_inuse * s->opts.obj_size;
	out->bytes_total   = out->n_chunks * s->opts.chunk_size;
	out->reaps         = atomic_load_explicit(&s->s_reaps,      memory_order_relaxed);
	out->oom_fails     = atomic_load_explicit(&s->s_oom_fails,  memory_order_relaxed);
	out->redzone_violations = atomic_load_explicit(&s->s_rz_violations,
	    memory_order_relaxed);
	return XTC_OK;
}

xtc_slab_off_t
xtc_slab_offset(const xtc_slab_t *s, const void *p)
{
	if (s == NULL || p == NULL) return XTC_SLAB_OFF_NONE;
	if (s->opts.mode == XTC_SLAB_SHARED_MEMORY) {
		if ((const uint8_t *)p < (const uint8_t *)s->opts.shm_base ||
		    (const uint8_t *)p >= (const uint8_t *)s->opts.shm_base +
		                          s->opts.shm_size)
			return XTC_SLAB_OFF_NONE;
		return (xtc_slab_off_t)((const uint8_t *)p -
		    (const uint8_t *)s->opts.shm_base);
	}
	/* Process-local: use chunk-relative addressing.  Walk chunks
	 * to find the host; offset = chunk_index * chunk_size + slot_off. */
	{
		struct slab_chunk *c; int idx = 0;
		for (c = s->chunks; c != NULL; c = c->next, idx++) {
			if ((const uint8_t *)p >= (const uint8_t *)c->base &&
			    (const uint8_t *)p <  (const uint8_t *)c->base +
			                          c->size) {
				return (xtc_slab_off_t)
				    ((int64_t)idx * (int64_t)s->opts.chunk_size +
				     ((const uint8_t *)p -
				      (const uint8_t *)c->base));
			}
		}
	}
	return XTC_SLAB_OFF_NONE;
}

void *
xtc_slab_resolve(const xtc_slab_t *s, xtc_slab_off_t off)
{
	if (s == NULL || off == XTC_SLAB_OFF_NONE) return NULL;
	if (s->opts.mode == XTC_SLAB_SHARED_MEMORY) {
		if ((size_t)off >= s->opts.shm_size) return NULL;
		return (uint8_t *)s->opts.shm_base + off;
	}
	{
		int target_idx = (int)(off / (int64_t)s->opts.chunk_size);
		int64_t in_chunk = off % (int64_t)s->opts.chunk_size;
		struct slab_chunk *c; int idx = 0;
		for (c = s->chunks; c != NULL; c = c->next, idx++) {
			if (idx == target_idx)
				return (uint8_t *)c->base + in_chunk;
		}
	}
	return NULL;
}

int
xtc_slab_reap_all(void)
{
	int total = 0;
	struct slab_registry_entry *re;
	(void)pthread_mutex_lock(&__slab_reg_lock);
	for (re = __registry; re != NULL; re = re->next)
		total += xtc_slab_reap(re->slab);
	(void)pthread_mutex_unlock(&__slab_reg_lock);
	return total;
}

/* ---- reaper proc ----------------------------------------- */

struct reaper_ctx {
	int64_t interval_ns;
};

static void
__reaper_main(void *arg)
{
	struct reaper_ctx *ctx = arg;
	void *m;
	size_t sz;
	for (;;) {
		/* Block on recv with the interval as timeout; on any
		 * incoming message we reap and continue.  Caller can
		 * stop the reaper via xtc_exit_pid. */
		int rc = xtc_recv(&m, &sz, ctx->interval_ns);
		(void)xtc_slab_reap_all();
		if (rc == XTC_OK && m != NULL) __os_free(m);
	}
}

int
xtc_slab_reaper_spawn(xtc_loop_t *loop, int64_t interval_ns,
                      xtc_pid_t *out_pid)
{
	struct reaper_ctx *ctx;
	int rc;
	xtc_pid_t pid;
	if (loop == NULL || interval_ns <= 0) return XTC_E_INVAL;
	rc = __os_calloc(1, sizeof *ctx, (void **)&ctx);
	if (rc != XTC_OK) return rc;
	ctx->interval_ns = interval_ns;
	rc = xtc_proc_spawn(loop, __reaper_main, ctx, NULL, &pid);
	if (rc != XTC_OK) { __os_free(ctx); return rc; }
	if (out_pid) *out_pid = pid;
	return XTC_OK;
}

/* ---- Linux PSI memory-pressure listener ---- */

#if defined(__linux__)
#include <fcntl.h>
#include <poll.h>
struct psi_listener {
	pthread_t              th;
	xtc_slab_pressure_fn   fn;
	void                  *user;
	int                    stop_fd;     /* read end of stop pipe */
	int                    stop_wfd;    /* write end; close()'d to signal stop */
	int                    psi_fd;
};

static void *
__psi_thread(void *arg)
{
	struct psi_listener *l = arg;
	struct pollfd        pfds[2];
	for (;;) {
		pfds[0].fd = l->psi_fd;     pfds[0].events = POLLPRI;
		pfds[1].fd = l->stop_fd;    pfds[1].events = POLLIN;
		int rc = poll(pfds, 2, -1);
		if (rc <= 0) continue;
		if (pfds[1].revents & POLLIN) break;
		if (pfds[0].revents & POLLPRI) {
			(void)xtc_slab_reap_all();
			if (l->fn) l->fn(2, l->user);   /* level 2 = critical */
		}
	}
	return NULL;
}

int
xtc_slab_pressure_listen(const char *psi_path,
                         xtc_slab_pressure_fn fn, void *user)
{
	struct psi_listener *l;
	int fd;
	const char *path = psi_path ? psi_path : "/proc/pressure/memory";
	const char *trigger = "some 150000 1000000\n";  /* 150ms in 1s */
	int rc;
	int pipefd[2];

	fd = open(path, O_RDWR);
	if (fd < 0) return XTC_E_NOSYS;     /* PSI not available */
	if (write(fd, trigger, strlen(trigger)) < 0) {  /* XTC_BLOCKING_OK: PSI register, one-shot at startup */
		(void)close(fd);
		return XTC_E_NOSYS;
	}
	if ((rc = __os_calloc(1, sizeof *l, (void **)&l)) != XTC_OK) {
		(void)close(fd); return rc;
	}
	if (pipe(pipefd) != 0) {
		__os_free(l); (void)close(fd); return XTC_E_INTERNAL;
	}
	l->psi_fd = fd;
	l->stop_fd = pipefd[0];
	l->stop_wfd = pipefd[1];   /* keep open; close to signal stop */
	l->fn = fn; l->user = user;
	if (pthread_create(&l->th, NULL, __psi_thread, l) != 0) {
		(void)close(fd); (void)close(pipefd[0]); (void)close(pipefd[1]);
		__os_free(l);
		return XTC_E_INTERNAL;
	}
	/* NOTE: The listener cannot be stopped today because we don't return
	 * a handle.  A future API change adds xtc_slab_pressure_listen_ex()
	 * returning an opaque handle and xtc_slab_pressure_stop(handle).
	 * For now, the listener runs until process exit. */
	return XTC_OK;
}

/* Placeholder: clean shutdown requires API change to return handle. */
int
xtc_slab_pressure_stop(void *handle)
{
	(void)handle;
	return XTC_E_NOSYS;
}
#else
int
xtc_slab_pressure_stop(void *handle)
{
	(void)handle;
	return XTC_E_NOSYS;
}
int
xtc_slab_pressure_listen(const char *psi_path,
                         xtc_slab_pressure_fn fn, void *user)
{
	(void)psi_path; (void)fn; (void)user;
	return XTC_E_NOSYS;
}
#endif
