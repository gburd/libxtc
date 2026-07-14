/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_cskip.h
 *	RCU-protected concurrent ordered map -- a lock-free-reader
 *	skiplist (Pugh 1990), the ORDERED sibling of xtc_chash.  Where
 *	chash answers "is this exact key present," cskip additionally
 *	keeps keys in sorted order, so it answers "what is the smallest
 *	key," and "what is the largest key <= K" (a floor / predecessor
 *	lookup) -- the queries an unordered hash table cannot serve.
 *
 *	Same storage convention as xtc_chash / xtc_pdict / xtc_chan:
 *	caller-owned `void *` key and value stored verbatim (no deep
 *	copy), with a caller-supplied comparator (qsort(3) convention;
 *	cskip uses the FULL ordering, not just equality).  Same RCU
 *	reader contract: xtc_cskip_get / _min / _floor return a value
 *	pointer valid only inside the caller's OWN
 *	xtc_rcu_read_lock/_read_unlock bracket; insert/remove take the
 *	read-side internally for their traversal.
 *
 *	Concurrency model (see src/ptc/cskip.c for the full discipline):
 *	  - Readers are lock-free: they walk the tower with acquire
 *	    loads.  Each level's forward pointer is published with ONE
 *	    release store, so a reader sees a fully-formed node or the
 *	    prior chain, never a torn link.
 *	  - A SINGLE writer mutex serializes insert/remove (v1 -- the
 *	    reader path is the one that must scale; a fine-grained
 *	    per-node writer lock is a deferred optimization).  A node is
 *	    linked bottom-up (level 0 first, so the moment it is
 *	    reachable at the base level it is fully present at every
 *	    level it will ever occupy is NOT assumed -- readers tolerate
 *	    a node still being spliced into upper levels, because the
 *	    base level is the source of truth for membership).
 *	  - Removal unlinks top-down under the writer mutex, then
 *	    retires the node via xtc_rcu_retire -- never freed inline; a
 *	    reader already parked on the node keeps walking its
 *	    unchanged forward pointers until it leaves its read-side.
 *	  - This module never calls xtc_rcu_synchronize itself:
 *	    reclamation is caller/reaper-driven, exactly like xtc_chash
 *	    and every other xtc_rcu consumer here.
 */

#ifndef XTC_CSKIP_H
#define XTC_CSKIP_H

#include "xtc_export.h"

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"

typedef struct xtc_cskip xtc_cskip_t;

/* Key comparator, qsort(3) convention: <0 if a<b, 0 if equal, >0 if
 * a>b.  cskip uses the FULL ordering (it keeps keys sorted), so the
 * comparator MUST be a total order over the key space, not merely an
 * equality test. */
typedef int (*xtc_cskip_cmp_fn)(const void *a, const void *b);

/*
 * PUBLIC: int    xtc_cskip_create __P((xtc_cskip_cmp_fn, xtc_cskip_t **));
 * PUBLIC: void   xtc_cskip_destroy __P((xtc_cskip_t *));
 * PUBLIC: int    xtc_cskip_get __P((xtc_cskip_t *, const void *, void **));
 * PUBLIC: int    xtc_cskip_insert __P((xtc_cskip_t *, void *, void *, void **));
 * PUBLIC: int    xtc_cskip_remove __P((xtc_cskip_t *, const void *, void **));
 * PUBLIC: int    xtc_cskip_min __P((xtc_cskip_t *, void **, void **));
 * PUBLIC: int    xtc_cskip_floor __P((xtc_cskip_t *, const void *, void **, void **));
 * PUBLIC: size_t xtc_cskip_size __P((const xtc_cskip_t *));
 */

/*
 * Create an ordered map.  cmp is required (non-NULL) and must be a
 * total order.  Returns XTC_OK and writes *out, or XTC_E_INVAL /
 * XTC_E_NOMEM.
 */
XTC_API int    xtc_cskip_create(xtc_cskip_cmp_fn cmp, xtc_cskip_t **out);

/*
 * Destroy the map.  NOT concurrency-safe with any other call -- the
 * caller must quiesce first (same contract as xtc_chash_destroy /
 * xtc_rcu_fini).  Frees node bookkeeping but NOT the caller-owned
 * key/value pointers.
 */
XTC_API void   xtc_cskip_destroy(xtc_cskip_t *s);

/*
 * Look up `key`.  On a hit, XTC_OK + *out_value written; on a miss,
 * XTC_E_NOTFOUND and *out_value untouched.  MUST run inside the
 * caller's own xtc_rcu_read_lock/_read_unlock bracket; the returned
 * value pointer is valid only until the read-side ends.
 */
XTC_API int    xtc_cskip_get(xtc_cskip_t *s, const void *key,
                             void **out_value);

/*
 * Insert (key, value), replacing any existing entry for an equal key.
 * Takes the read-side internally.  On replace, the OLD value pointer
 * is written to *out_old_value (may be NULL) and is the caller's to
 * free/retire (this map never frees caller payloads); on a fresh
 * insert, *out_old_value (if non-NULL) is set to NULL.  Returns
 * XTC_OK or XTC_E_NOMEM.
 */
XTC_API int    xtc_cskip_insert(xtc_cskip_t *s, void *key, void *value,
                                void **out_old_value);

/*
 * Remove the entry for `key`.  On a hit, XTC_OK + removed value
 * written to *out_value (may be NULL); the node is retired via
 * xtc_rcu_retire, never freed synchronously.  On a miss,
 * XTC_E_NOTFOUND.
 */
XTC_API int    xtc_cskip_remove(xtc_cskip_t *s, const void *key,
                                void **out_value);

/*
 * Smallest key currently in the map (the ordered query a hash table
 * cannot serve).  On a non-empty map, XTC_OK and writes the key
 * pointer to *out_key (may be NULL) and the value to *out_value (may
 * be NULL); on an empty map, XTC_E_NOTFOUND.  MUST run inside the
 * caller's own read-side; the returned pointers are valid only until
 * the read-side ends.
 */
XTC_API int    xtc_cskip_min(xtc_cskip_t *s, void **out_key,
                             void **out_value);

/*
 * Floor lookup: the entry with the largest key <= `key`.  On a hit,
 * XTC_OK and writes *out_key / *out_value (either may be NULL); if no
 * key is <= `key` (or the map is empty), XTC_E_NOTFOUND.  MUST run
 * inside the caller's own read-side.
 */
XTC_API int    xtc_cskip_floor(xtc_cskip_t *s, const void *key,
                               void **out_key, void **out_value);

/* Approximate live entry count (an _Atomic counter; exact with no
 * concurrent writers). */
XTC_API size_t xtc_cskip_size(const xtc_cskip_t *s);

#endif /* XTC_CSKIP_H */
