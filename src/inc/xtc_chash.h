/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_chash.h
 *	RCU-protected concurrent hash table.  M13a's primary xtc_rcu
 *	consumer: chained buckets, an array of per-bucket mutexes for
 *	writers (insert/remove/put serialize only within their own
 *	bucket -- disjoint buckets proceed fully in parallel), and
 *	xtc_rcu for readers, so xtc_chash_get takes NO lock at all --
 *	it is wait-free, bounded only by the chain length it walks.
 *
 *	Storage convention: caller-owned `void *` key and value,
 *	same as xtc_pdict / xtc_chan (the store keeps the pointers
 *	verbatim, no deep copy).  A caller-supplied comparator and
 *	hash function make the table key-type-agnostic (the qsort(3)
 *	convention: cmp returns <0/0/>0, but chash only tests for
 *	equality via == 0).
 *
 *	Concurrency model:
 *	  - Bucket array is RCU-protected: a resize (grow-only, v1)
 *	    allocates a new, larger array, rehashes every live node
 *	    into it, publishes the new array pointer, and retires the
 *	    old array via xtc_rcu_retire.  A reader that loaded the old
 *	    array pointer before the swap keeps working against a
 *	    fully valid (if smaller) table until it leaves its
 *	    read-side; nothing under it moves.
 *	  - Node removal: unlinked from its bucket chain under that
 *	    bucket's mutex, then handed to xtc_rcu_retire -- NEVER
 *	    freed directly.  A concurrent reader that already read a
 *	    pointer to the node (chased into the chain before the
 *	    unlink) keeps a valid node until it leaves its read-side;
 *	    RCU reclaims the node only after every such reader has
 *	    drained.
 *	  - Node insertion: publish-then-link.  The new node is fully
 *	    initialized, then linked with a single release-ordered
 *	    store of its bucket's head pointer (or the previous node's
 *	    `next`) -- a concurrent reader either sees the old chain or
 *	    the new one with the new node fully formed, never a torn
 *	    write.
 *
 *	xtc_chash_get returns a value pointer valid only inside the
 *	caller's OWN xtc_rcu_read_lock/xtc_rcu_read_unlock bracket,
 *	exactly like every other xtc_rcu consumer: the table does not
 *	take the read-side for you (nesting is supported, see
 *	xtc_rcu.h, so a caller already inside a read-side may call get
 *	freely) because a caller usually wants to do more than one
 *	lookup, or read the value's contents, inside one critical
 *	section.  xtc_chash_insert / _remove take the read-side
 *	internally for their own traversal and enter/leave it around
 *	the mutable part; callers do not need an outer read-side for
 *	them.
 *
 *	Grow-only: xtc_chash never shrinks its bucket array.  A table
 *	that grows to a high-water mark and then drains stays at that
 *	array size until destroy.  Acceptable for v1; a real shrink
 *	would need the same RCU-swap machinery in reverse plus a
 *	load-factor-low policy, deferred until a workload needs it.
 */

#ifndef XTC_CHASH_H
#define XTC_CHASH_H

#include "xtc_export.h"

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"

typedef struct xtc_chash xtc_chash_t;

/* Key comparator: qsort(3) convention, but chash only ever tests the
 * sign of the result for == 0 (equal) vs. != 0 (not equal) -- it does
 * not order buckets, so a cmp that only distinguishes equal/unequal
 * (e.g. always returns 0 or 1) is fine too. */
typedef int      (*xtc_chash_cmp_fn)(const void *a, const void *b);

/* Key hash: any distribution is correct; a poor one just chains more.
 * Same key must always hash the same for the table's lifetime. */
typedef uint64_t (*xtc_chash_hash_fn)(const void *key);

/*
 * PUBLIC: int    xtc_chash_create __P((xtc_chash_cmp_fn, xtc_chash_hash_fn, size_t, xtc_chash_t **));
 * PUBLIC: void   xtc_chash_destroy __P((xtc_chash_t *));
 * PUBLIC: int    xtc_chash_get __P((xtc_chash_t *, const void *, void **));
 * PUBLIC: int    xtc_chash_insert __P((xtc_chash_t *, void *, void *, void **));
 * PUBLIC: int    xtc_chash_remove __P((xtc_chash_t *, const void *, void **));
 * PUBLIC: size_t xtc_chash_size __P((const xtc_chash_t *));
 */

/*
 * Create a table with `initial_capacity` buckets (rounded up to the
 * next power of two; 0 defaults to 16).  cmp and hash are required
 * (non-NULL).  Returns XTC_OK and writes *out, or XTC_E_INVAL /
 * XTC_E_NOMEM.
 */
XTC_API int    xtc_chash_create(xtc_chash_cmp_fn cmp, xtc_chash_hash_fn hash,
                                size_t initial_capacity, xtc_chash_t **out);

/*
 * Destroy the table.  NOT concurrency-safe with any other call on
 * this table -- the caller must ensure no reader or writer is active
 * (same contract as xtc_rcu_fini: quiesce first).  Frees every
 * remaining node's bookkeeping but NOT the caller-owned key/value
 * pointers (caller-owned lifetime, same as xtc_chan/xtc_pdict).
 */
XTC_API void   xtc_chash_destroy(xtc_chash_t *h);

/*
 * Look up `key`.  On a hit, returns XTC_OK and writes the value
 * pointer to *out_value.  On a miss, returns XTC_E_NOTFOUND and does
 * not touch *out_value.  MUST be called inside the caller's own
 * xtc_rcu_read_lock/_read_unlock bracket (nesting is fine); the value
 * pointer written to *out_value is valid only until the caller's
 * read-side ends -- do not stash it past xtc_rcu_read_unlock unless
 * you know the value's lifetime is otherwise pinned.
 */
XTC_API int    xtc_chash_get(xtc_chash_t *h, const void *key, void **out_value);

/*
 * Insert (key, value), replacing any existing entry for an equal key.
 * Takes the read-side internally for its traversal; safe to call
 * without an outer read-side.  On replace, the OLD value pointer is
 * written to *out_old_value (out_old_value may be NULL if the caller
 * does not care) and the old key/value pointers are the caller's
 * responsibility to free (this table never frees caller payloads).
 * On a fresh insert, *out_old_value (if non-NULL) is set to NULL.
 * May trigger a grow-only resize.  Returns XTC_OK or XTC_E_NOMEM.
 */
XTC_API int    xtc_chash_insert(xtc_chash_t *h, void *key, void *value,
                                void **out_old_value);

/*
 * Remove the entry for `key`.  On a hit, returns XTC_OK and writes
 * the removed value pointer to *out_value (may be NULL).  The node's
 * memory is retired via xtc_rcu_retire, never freed synchronously --
 * a concurrent reader that is already inside the chain sees a fully
 * valid node until it leaves its read-side.  On a miss, returns
 * XTC_E_NOTFOUND.
 */
XTC_API int    xtc_chash_remove(xtc_chash_t *h, const void *key, void **out_value);

/* Approximate live entry count (an _Atomic counter; exact at any
 * instant with no concurrent writers, a fresh-enough estimate
 * otherwise). */
XTC_API size_t xtc_chash_size(const xtc_chash_t *h);

#endif /* XTC_CHASH_H */
