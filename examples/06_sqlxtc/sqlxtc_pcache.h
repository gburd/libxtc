/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/sqlxtc_pcache.h
 *	An xtc_slab-backed SQLite page cache.
 *
 *	Every page in one SQLite cache is the same size (szPage +
 *	szExtra), which is exactly the workload a slab allocator is
 *	built for: a single object-size class, no fragmentation,
 *	O(1) alloc/free.  This registers a custom
 *	sqlite3_pcache_methods2 whose page bodies come from a per-cache
 *	xtc_slab, with a small chained hash table for lookup and an LRU
 *	list of unpinned pages for recycling.
 *
 *	The pcache methods are always invoked under SQLite's own mutex
 *	(one cache belongs to one pager), so the implementation needs
 *	no internal locking.
 */

#ifndef SQLXTC_PCACHE_XTC_H
#define SQLXTC_PCACHE_XTC_H

#include <stdint.h>

/*
 * Install the xtc page cache as SQLite's default pcache.  Must be
 * called before sqlite3_initialize() / the first database handle.
 * Idempotent.  Returns SQLITE_OK on success.
 */
int sqlxtc_pcache_register(void);

/* Page-cache statistics, for the metrics path. */
typedef struct {
	uint64_t fetch_hit;      /* xFetch found the page resident */
	uint64_t fetch_miss;     /* xFetch had to allocate or recycle */
	uint64_t slab_alloc;     /* fresh slab allocations */
	uint64_t recycle;        /* unpinned pages reused for a new key */
	uint64_t live_pages;     /* pages currently resident across caches */
} sqlxtc_pcache_stats_t;

void sqlxtc_pcache_get_stats(sqlxtc_pcache_stats_t *out);

#endif /* SQLXTC_PCACHE_XTC_H */
