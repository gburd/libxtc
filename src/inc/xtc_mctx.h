/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * src/inc/xtc_mctx.h
 *	Memory contexts: hierarchical allocation pools with parent-
 *	tracked lifetime and bulk reset/destroy.  Inspired by
 *	PostgreSQL's MemoryContext and libumem's allocation domains.
 *
 *	A context owns:
 *	  - all allocations made within it (chained on a free list);
 *	  - zero or more child contexts (siblings of each other);
 *	  - optional "before-destroy" callbacks for non-memory cleanup.
 *
 *	Destroying a context destroys its children first, runs the
 *	cleanup callbacks bottom-up, then frees all chunks.  Reset
 *	frees chunks but keeps children alive.
 *
 *	M11 deliberately ships a simple-but-correct allocator first
 *	(every allocation = malloc + chain).  M11.5 will swap in slab
 *	caches for fixed-size hot paths and arena-free for large
 *	short-lived contexts.  The public API doesn't change.
 *
 *	Thread-safety: each context carries an optional pthread mutex.
 *	The default factory makes contexts unlocked for the per-loop
 *	(single-thread) common case; a flag enables locking for
 *	contexts that legitimately span threads.
 */

#ifndef XTC_MCTX_H
#define XTC_MCTX_H

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"

typedef struct xtc_mctx xtc_mctx_t;

typedef enum xtc_mctx_flags {
	XTC_MCTX_DEFAULT      = 0,
	XTC_MCTX_THREAD_SAFE  = 1u << 0   /* internally locked */
} xtc_mctx_flags_t;

typedef void (*xtc_mctx_cleanup_fn)(void *user);

/*
 * PUBLIC: int     xtc_mctx_create __P((xtc_mctx_t *, const char *, unsigned, xtc_mctx_t **));
 * PUBLIC: void    xtc_mctx_destroy __P((xtc_mctx_t *));
 * PUBLIC: void    xtc_mctx_reset __P((xtc_mctx_t *));
 *
 * PUBLIC: void   *xtc_mctx_alloc __P((xtc_mctx_t *, size_t));
 * PUBLIC: void   *xtc_mctx_calloc __P((xtc_mctx_t *, size_t, size_t));
 * PUBLIC: void   *xtc_mctx_strdup __P((xtc_mctx_t *, const char *));
 * PUBLIC: void    xtc_mctx_free __P((xtc_mctx_t *, void *));
 *
 * PUBLIC: int     xtc_mctx_register_cleanup __P((xtc_mctx_t *, xtc_mctx_cleanup_fn, void *));
 *
 * PUBLIC: const char *xtc_mctx_name __P((const xtc_mctx_t *));
 * PUBLIC: size_t      xtc_mctx_total_bytes __P((const xtc_mctx_t *));
 * PUBLIC: size_t      xtc_mctx_total_chunks __P((const xtc_mctx_t *));
 */

/* Create a child context.  parent==NULL produces a root context.
 * name is copied for diagnostics.  flags = bitmask of XTC_MCTX_*. */
int     xtc_mctx_create(xtc_mctx_t *parent, const char *name,
                        unsigned flags, xtc_mctx_t **out);

/* Destroy a context: recursively destroys children, runs cleanups
 * bottom-up, frees all chunks.  Detaches from parent. */
void    xtc_mctx_destroy(xtc_mctx_t *m);

/* Free all allocations and run cleanups, but keep the context (and
 * its children) usable.  Useful for per-iteration scratch contexts. */
void    xtc_mctx_reset(xtc_mctx_t *m);

/* Allocate within the context.  Returns NULL on failure. */
void   *xtc_mctx_alloc(xtc_mctx_t *m, size_t size);
void   *xtc_mctx_calloc(xtc_mctx_t *m, size_t n, size_t size);

/* Strdup into the context.  Lives until reset/destroy. */
void   *xtc_mctx_strdup(xtc_mctx_t *m, const char *s);

/* Free a single allocation early.  Optional — most code just lets
 * destroy/reset reclaim. */
void    xtc_mctx_free(xtc_mctx_t *m, void *p);

/* Register a cleanup callback.  Runs at destroy/reset time, before
 * the chunks are freed.  Multiple callbacks run in LIFO order. */
int     xtc_mctx_register_cleanup(xtc_mctx_t *m,
                                  xtc_mctx_cleanup_fn fn, void *user);

const char *xtc_mctx_name(const xtc_mctx_t *m);
size_t      xtc_mctx_total_bytes(const xtc_mctx_t *m);
size_t      xtc_mctx_total_chunks(const xtc_mctx_t *m);

#endif /* XTC_MCTX_H */
