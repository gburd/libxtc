/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_slab.h
 *	libumem-inspired slab allocator with per-loop magazines.
 *	Foundation for fixed-size hot-path allocations across xtc.
 *	Replaces ad-hoc per-module pools (lockmgr entries, proc
 *	mailbox envelopes, channel slots, RCU retired nodes, timer
 *	nodes).
 *
 *	Design (v1):
 *	  - Each cache owns one or more "chunks" (large mmap regions),
 *	    carved into per-object slots.  No per-object malloc.
 *	  - Chunks are sub-divided into "slabs" (default 64 KiB each)
 *	    tracked on free/partial/full lists.
 *	  - Each loop has a magazine — a small array of recently-freed
 *	    object pointers — for lock-free fast-path alloc/free.
 *	    On magazine miss, the cache mutex is taken and the magazine
 *	    is refilled from a partial slab.
 *	  - On magazine overflow (free), the magazine spills back to
 *	    the cache's free list under the mutex.
 *	  - Constructor runs once per object lifetime (when first
 *	    pulled out of the slab); destructor runs once at cache
 *	    destroy or at slab reap.
 *	  - Magazines flush on memory pressure; full slabs return to
 *	    the OS via munmap.
 *
 *	Two storage modes:
 *	  PROCESS_LOCAL — chunks via mmap MAP_ANONYMOUS; objects are
 *	    process-local pointers.
 *	  SHARED_MEMORY — chunks carved from a single user-supplied
 *	    region (mmap MAP_SHARED of a tmpfs/posix-shm fd).  Object
 *	    handles are offsets (xtc_slab_off_t — BDB's roff_t
 *	    equivalent) so a peer process mapping the same fd at a
 *	    different VA can resolve them.  Use xtc_slab_resolve() and
 *	    xtc_slab_offset() to convert.
 *
 *	Debug flags (M11.5a):
 *	  REDZONE   — 16-byte guard before+after each object; checked
 *	              on free
 *	  AUDIT     — small ring of recent (alloc, free, who, when)
 *	              events per cache for postmortem
 *	  BACKTRACE — capture backtrace on alloc; combine with AUDIT
 *	              to identify leakers (Linux only, requires
 *	              <execinfo.h>)
 *
 *	OOM policies:
 *	  FAIL    — return NULL, set XTC_E_RESOURCE in caller
 *	  BACKOFF — short usleep + retry once, then FAIL
 *	  ABORT   — call abort() (debug builds only; never in prod)
 *
 *	Memory-pressure: callers can register `xtc_slab_pressure_listen`
 *	on Linux (/proc/pressure/memory PSI).  When pressure is high,
 *	all caches drop their magazines and reap empty slabs.
 */

#ifndef XTC_SLAB_H
#define XTC_SLAB_H

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_res.h"

typedef struct xtc_slab xtc_slab_t;

/* Offset handle for shared-memory caches (BDB roff_t analogue). */
typedef int64_t xtc_slab_off_t;
#define XTC_SLAB_OFF_NONE  ((xtc_slab_off_t)-1)

/* Per-object lifecycle hooks. */
typedef int  (*xtc_slab_ctor_fn)(void *obj, void *user);
typedef void (*xtc_slab_dtor_fn)(void *obj, void *user);

/* Memory-pressure callback signature.  level: 0=normal 1=warning
 * 2=critical.  Caller may reap, drop caches, etc. */
typedef void (*xtc_slab_pressure_fn)(int level, void *user);

/* Storage mode. */
typedef enum xtc_slab_mode {
	XTC_SLAB_PROCESS_LOCAL = 0,
	XTC_SLAB_SHARED_MEMORY = 1
} xtc_slab_mode_t;

/* OOM policy. */
typedef enum xtc_slab_oom_policy {
	XTC_SLAB_OOM_FAIL    = 0,    /* return NULL, increment rejects */
	XTC_SLAB_OOM_BACKOFF = 1,    /* brief usleep + retry once */
	XTC_SLAB_OOM_ABORT   = 2     /* call abort() (debug builds) */
} xtc_slab_oom_policy_t;

/* Debug flags (bit OR'd into opts.flags). */
#define XTC_SLAB_REDZONE    (1u << 0)
#define XTC_SLAB_AUDIT      (1u << 1)
#define XTC_SLAB_BACKTRACE  (1u << 2)
#define XTC_SLAB_NO_MAGAZINE (1u << 3)   /* always go through cache lock */

typedef struct xtc_slab_opts {
	const char            *name;          /* required, for stats/diag */
	size_t                 obj_size;      /* required */
	size_t                 align;          /* default cache-line, 64 */
	size_t                 chunk_size;     /* mmap unit; default 64 KiB */
	int                    magazine_size;  /* per-loop cache; default 16 */
	xtc_slab_ctor_fn       ctor;            /* optional */
	xtc_slab_dtor_fn       dtor;            /* optional */
	void                  *cb_user;
	unsigned               flags;
	xtc_slab_oom_policy_t  oom_policy;
	xtc_res_t             *res;             /* optional accounting */

	/* Shared-memory mode only: caller-supplied base + size of an
	 * already-mapped region.  When set, the cache carves chunks
	 * from this region instead of mmap'ing fresh ones.  Released
	 * via shm_unlink when the cache is destroyed. */
	xtc_slab_mode_t        mode;
	void                  *shm_base;
	size_t                 shm_size;
} xtc_slab_opts_t;

#define XTC_SLAB_OPTS_DEFAULT { \
	.name = "anon", \
	.obj_size = 0, \
	.align = 64, \
	.chunk_size = 64 * 1024, \
	.magazine_size = 16, \
	.ctor = NULL, .dtor = NULL, .cb_user = NULL, \
	.flags = 0, \
	.oom_policy = XTC_SLAB_OOM_FAIL, \
	.res = NULL, \
	.mode = XTC_SLAB_PROCESS_LOCAL, \
	.shm_base = NULL, \
	.shm_size = 0 \
}

typedef struct xtc_slab_stats {
	uint64_t alloc_fast;        /* magazine hit */
	uint64_t alloc_slow;        /* magazine miss → cache lock */
	uint64_t free_fast;
	uint64_t free_slow;
	uint64_t n_inuse;
	uint64_t n_free;            /* available in slabs */
	uint64_t n_chunks;
	uint64_t bytes_inuse;
	uint64_t bytes_total;
	uint64_t reaps;
	uint64_t oom_fails;
	uint64_t redzone_violations;
} xtc_slab_stats_t;

/*
 * PUBLIC: int  xtc_slab_create __P((const xtc_slab_opts_t *, xtc_slab_t **));
 * PUBLIC: void xtc_slab_destroy __P((xtc_slab_t *));
 *
 * PUBLIC: void *xtc_slab_alloc __P((xtc_slab_t *));
 * PUBLIC: void  xtc_slab_free __P((xtc_slab_t *, void *));
 *
 * PUBLIC: int  xtc_slab_reap __P((xtc_slab_t *));
 * PUBLIC: int  xtc_slab_stat __P((const xtc_slab_t *, xtc_slab_stats_t *));
 *
 * PUBLIC: xtc_slab_off_t xtc_slab_offset __P((const xtc_slab_t *, const void *));
 * PUBLIC: void *xtc_slab_resolve __P((const xtc_slab_t *, xtc_slab_off_t));
 *
 * PUBLIC: int  xtc_slab_pressure_listen __P((const char *, xtc_slab_pressure_fn, void *));
 * PUBLIC: int  xtc_slab_reap_all __P((void));
 */

int   xtc_slab_create(const xtc_slab_opts_t *opts, xtc_slab_t **out);
void  xtc_slab_destroy(xtc_slab_t *slab);

void *xtc_slab_alloc(xtc_slab_t *slab);
void  xtc_slab_free(xtc_slab_t *slab, void *obj);

/* Drop all magazines + return empty slabs to the OS. */
int   xtc_slab_reap(xtc_slab_t *slab);

int   xtc_slab_stat(const xtc_slab_t *slab, xtc_slab_stats_t *out);

/* Shared-memory helpers — convert between process-local pointer
 * and a stable offset.  In PROCESS_LOCAL mode these are still
 * meaningful but only valid within this process. */
xtc_slab_off_t xtc_slab_offset(const xtc_slab_t *slab, const void *p);
void          *xtc_slab_resolve(const xtc_slab_t *slab, xtc_slab_off_t off);

/* Install a process-wide memory-pressure listener.  On Linux, opens
 * /proc/pressure/memory in poll mode at the "some" trigger (>10 ms
 * stall in 1s); on platforms without PSI, returns XTC_E_NOSYS.  When
 * pressure fires, all registered slab caches receive a reap call.
 * `psi_path` is "/proc/pressure/memory" by default (NULL = use it).
 */
int   xtc_slab_pressure_listen(const char *psi_path,
                               xtc_slab_pressure_fn fn,
                               void *user);

/* Fan a reap across every cache currently registered.  Returns the
 * total number of objects reaped. */
int   xtc_slab_reap_all(void);

#endif /* XTC_SLAB_H */
