/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/chash.c
 *	RCU-protected concurrent hash table.  See xtc_chash.h for the
 *	API contract; this file is the "get this exactly right" part --
 *	the RCU retire discipline that keeps concurrent readers safe.
 *
 *	Concurrency design:
 *
 *	  Write striping.  A FIXED set of XTC_CHASH_NSTRIPES mutexes
 *	  serializes writers; a key's stripe is (bucket_index &
 *	  (NSTRIPES-1)) -- computed from the SAME array snapshot as the
 *	  bucket lookup itself, so two writers whose keys land in the
 *	  SAME bucket always take the SAME stripe lock (mandatory: two
 *	  mutexes protecting one bucket's chain would be a lost-update
 *	  race, not just false sharing).  Two writers in different
 *	  buckets that happen to share bucket_index % NSTRIPES only pay a
 *	  false-sharing serialization cost.  Because bucket_index is
 *	  relative to the CURRENT array, a grow changes which stripe a
 *	  given key maps to -- see "Resize-race retry" below for how
 *	  insert/remove stay correct across that.
 *
 *	  Lock-free reads.  xtc_chash_get takes no lock: it loads the
 *	  current bucket array pointer once (the caller's read-side
 *	  keeps it alive), computes a bucket index, and walks a singly
 *	  linked chain with acquire loads.  A concurrent writer either
 *	  finishes before or after the read observes a given link --
 *	  never a torn pointer -- because every writer publishes with
 *	  ONE release-ordered store of a fully-formed node/pointer.
 *
 *	  Grow-only resize.  A grow claims ALL stripe locks (ascending
 *	  order -- the only place more than one stripe lock is held at
 *	  once, so this order is the sole deadlock-avoidance rule to
 *	  keep), which blocks every writer but NO reader.  It then
 *	  duplicates every live node into a freshly allocated, larger
 *	  bucket array (never mutating an OLD node's own `next` field,
 *	  since a concurrent reader might be mid-walk through it),
 *	  publishes the new array with a release store, and retires
 *	  the old array AND every one of its (now unreachable) original
 *	  nodes via xtc_rcu_retire -- never freed synchronously.  A
 *	  reader that loaded the OLD array before the swap keeps a
 *	  fully valid, if smaller, table until it leaves its read-side.
 *
 *	  Node removal.  Unlinked by overwriting its PREDECESSOR's link
 *	  (the bucket head or a sibling node's `next`) with a release
 *	  store of the removed node's OWN (unchanged) successor -- the
 *	  removed node's `next` field is never touched, so a reader
 *	  already parked on it can still walk the rest of the chain
 *	  exactly as it stood at the moment it arrived.  The node itself
 *	  is handed to xtc_rcu_retire, never freed inline.
 *
 *	  Resize-race retry.  insert/remove compute idx/stripe against a
 *	  SNAPSHOT of the array, then take that stripe lock, then
 *	  re-check the array pointer is still current: if a grow raced
 *	  in between (blocked them on the OLD stripe lock, rehashed
 *	  everything into a new array, released), the idx/stripe they
 *	  computed are for a now-orphaned array -- mutating through it
 *	  would silently vanish (the grow already made a private copy of
 *	  every node before the writer got in, so nothing is corrupted,
 *	  but the writer's insert/remove would apply to a dead copy no
 *	  reader will ever see again).  The re-check re-derives
 *	  idx/stripe against the NEW array and retries -- since stripe
 *	  now depends on the array snapshot, a retry may take a
 *	  DIFFERENT stripe lock than its first attempt.
 *
 *	Load-value replace (same key, new value) does NOT touch chain
 *	linkage at all: it is a single atomic store of the node's value
 *	slot.  The OLD value pointer handed back to the caller is under
 *	the exact same rule as a removed node -- a concurrent reader
 *	may have already loaded the old pointer and still be using its
 *	CONTENTS, so the caller must not free it synchronously; retire
 *	it via xtc_rcu_retire (or otherwise defer) exactly as for a
 *	removed value.
 *
 *	This module never calls xtc_rcu_synchronize itself -- exactly
 *	like every other xtc_rcu consumer in this tree, actual
 *	reclamation is caller/test/reaper-driven.  Retired arrays and
 *	nodes accumulate until something calls xtc_rcu_synchronize().
 */

#include "xtc_int.h"
#include "xtc_preempt.h"   /* __xtc_mtx_lock/unlock: preemption-safe locks */
#include "xtc_chash.h"
#include "xtc_rcu.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

/*
 * ponytail: 64 fixed write stripes.  A key's stripe is derived from
 * its BUCKET INDEX (idx & (NSTRIPES-1)), not from independent hash
 * bits -- two keys that land in the SAME bucket must always pick the
 * SAME stripe lock, or two writers could race unprotected on that
 * bucket's chain (different mutexes, same head pointer).  Two keys in
 * DIFFERENT buckets that happen to share idx % NSTRIPES only cost a
 * false-sharing serialization, never a correctness problem, because
 * whichever of them arrives second simply waits for the same mutex.
 * Bump NSTRIPES if that false-sharing shows up under heavy concurrent-
 * writer contention.
 */
#define XTC_CHASH_NSTRIPES  64u

static inline size_t
__chash_stripe(size_t bucket_idx)
{
	return bucket_idx & (XTC_CHASH_NSTRIPES - 1);
}

static size_t
__chash_next_pow2(size_t v)
{
	/* Largest power-of-two bucket count whose byte size still fits in
	 * size_t with 4x headroom -- portable across 32- and 64-bit.  A
	 * caller-supplied huge initial_capacity must never hang the
	 * doubling loop (p <<= 1 past the top bit becomes 0, so 0 < v
	 * would loop forever) or overflow the callers' size arithmetic.
	 * A bucket is a single atomic pointer; sizeof(void *) is a safe
	 * conservative stand-in here (the struct is defined below this
	 * function) and only ever makes the cap smaller, never larger. */
	size_t p = 1;
	size_t cap_max = (SIZE_MAX / 4) / sizeof(void *);
	if (v <= 1) return 1;
	while (p < v && (p << 1) != 0 && (p << 1) <= cap_max)
		p <<= 1;
	return p;
}

struct chash_node {
	_Atomic(struct chash_node *) next;
	uint64_t                      hash;
	void                          *key;    /* caller-owned */
	_Atomic(void *)                value;  /* caller-owned */
};

struct chash_bucket {
	_Atomic(struct chash_node *) head;
};

struct chash_arr {
	size_t               n;      /* bucket count, power of two */
	size_t               mask;   /* n - 1 */
	struct chash_bucket  buckets[];  /* flexible array member */
};

struct xtc_chash {
	xtc_chash_cmp_fn             cmp;
	xtc_chash_hash_fn            hash_fn;
	_Atomic(struct chash_arr *)  arr;
	pthread_mutex_t              stripe_locks[XTC_CHASH_NSTRIPES];
	_Atomic size_t               count;
	_Atomic int                  resizing;   /* 0 idle, 1 grow in progress */
};

static void
__chash_node_free_cb(void *p)
{
	__os_free(p);
}

static void
__chash_arr_free_cb(void *p)
{
	__os_free(p);
}

int
xtc_chash_create(xtc_chash_cmp_fn cmp, xtc_chash_hash_fn hash_fn,
    size_t initial_capacity, xtc_chash_t **out)
{
	xtc_chash_t *h;
	struct chash_arr *arr;
	size_t n;
	unsigned i;
	int rc;

	if (cmp == NULL || hash_fn == NULL || out == NULL) return XTC_E_INVAL;
	n = __chash_next_pow2(initial_capacity < 16 ? 16 : initial_capacity);

	if ((rc = __os_calloc(1, sizeof *h, (void **)&h)) != XTC_OK) return rc;
	rc = __os_calloc(1, sizeof(struct chash_arr) +
	    n * sizeof(struct chash_bucket), (void **)&arr);
	if (rc != XTC_OK) {
		__os_free(h);
		return rc;
	}
	/* calloc zeroes every bucket head -- the same "zeroed atomic ==
	 * atomic NULL" reliance src/ptc/lock_lr.c already makes for its
	 * active_mask array. */
	arr->n = n;
	arr->mask = n - 1;

	h->cmp = cmp;
	h->hash_fn = hash_fn;
	atomic_init(&h->arr, arr);
	atomic_init(&h->count, (size_t)0);
	atomic_init(&h->resizing, 0);
	for (i = 0; i < XTC_CHASH_NSTRIPES; i++)
		(void)pthread_mutex_init(&h->stripe_locks[i], NULL);

	*out = h;
	return XTC_OK;
}

void
xtc_chash_destroy(xtc_chash_t *h)
{
	struct chash_arr *arr;
	size_t i;
	unsigned s;

	if (h == NULL) return;
	arr = atomic_load_explicit(&h->arr, memory_order_relaxed);
	for (i = 0; i < arr->n; i++) {
		struct chash_node *n = atomic_load_explicit(
		    &arr->buckets[i].head, memory_order_relaxed);
		while (n != NULL) {
			struct chash_node *next = atomic_load_explicit(
			    &n->next, memory_order_relaxed);
			__os_free(n);
			n = next;
		}
	}
	__os_free(arr);
	for (s = 0; s < XTC_CHASH_NSTRIPES; s++)
		(void)pthread_mutex_destroy(&h->stripe_locks[s]);
	__os_free(h);
}

int
xtc_chash_get(xtc_chash_t *h, const void *key, void **out_value)
{
	uint64_t hh;
	struct chash_arr *arr;
	struct chash_node *n;
	size_t idx;

	if (h == NULL || key == NULL || out_value == NULL) return XTC_E_INVAL;
	hh = h->hash_fn(key);
	/* Caller's own read-side keeps this array (and every node we
	 * walk into) alive; we take no lock of our own. */
	arr = atomic_load_explicit(&h->arr, memory_order_acquire);
	idx = (size_t)(hh & (uint64_t)arr->mask);
	for (n = atomic_load_explicit(&arr->buckets[idx].head,
	    memory_order_acquire); n != NULL;
	    n = atomic_load_explicit(&n->next, memory_order_acquire)) {
		if (n->hash == hh && h->cmp(n->key, key) == 0) {
			*out_value = atomic_load_explicit(&n->value,
			    memory_order_acquire);
			return XTC_OK;
		}
	}
	return XTC_E_NOTFOUND;
}

/* Forward decl: called with no locks held, after a fresh insert. */
static void __chash_grow(xtc_chash_t *h);

int
xtc_chash_insert(xtc_chash_t *h, void *key, void *value, void **out_old_value)
{
	uint64_t hh;
	struct chash_arr *arr;
	struct chash_node *n, *new_node;
	_Atomic(struct chash_node *) *head_slot;
	size_t idx, stripe;

	if (h == NULL || key == NULL) return XTC_E_INVAL;
	hh = h->hash_fn(key);

	xtc_rcu_read_lock();
	arr = atomic_load_explicit(&h->arr, memory_order_acquire);
	for (;;) {
		idx = (size_t)(hh & (uint64_t)arr->mask);
		stripe = __chash_stripe(idx);
		(void)__xtc_mtx_lock(&h->stripe_locks[stripe]);
		{
			struct chash_arr *now = atomic_load_explicit(&h->arr,
			    memory_order_acquire);
			if (now == arr) break;
			/* A grow raced in between: idx (and possibly stripe)
			 * computed against the array we just LOST are stale.
			 * Retry against the current one -- grow itself held
			 * every stripe lock while swapping, so by the time we
			 * got this one it is fully done. */
			(void)__xtc_mtx_unlock(&h->stripe_locks[stripe]);
			arr = now;
		}
	}

	head_slot = &arr->buckets[idx].head;
	for (n = atomic_load_explicit(head_slot, memory_order_acquire);
	    n != NULL;
	    n = atomic_load_explicit(&n->next, memory_order_acquire)) {
		if (n->hash == hh && h->cmp(n->key, key) == 0) break;
	}

	if (n != NULL) {
		/* Replace: chain linkage is untouched, one atomic store of
		 * the value slot.  See the file header on why the caller
		 * must retire (not free) the returned old value. */
		void *old = atomic_load_explicit(&n->value,
		    memory_order_relaxed);
		atomic_store_explicit(&n->value, value, memory_order_release);
		(void)__xtc_mtx_unlock(&h->stripe_locks[stripe]);
		xtc_rcu_read_unlock();
		if (out_old_value != NULL) *out_old_value = old;
		return XTC_OK;
	}

	if (__os_malloc(sizeof *new_node, (void **)&new_node) != XTC_OK) {
		(void)__xtc_mtx_unlock(&h->stripe_locks[stripe]);
		xtc_rcu_read_unlock();
		return XTC_E_NOMEM;
	}
	new_node->hash = hh;
	new_node->key = key;
	atomic_init(&new_node->value, value);
	/* Build fully off to the side, then ONE release store splices it
	 * in -- a reader sees either the old chain or this node complete. */
	atomic_init(&new_node->next,
	    atomic_load_explicit(head_slot, memory_order_relaxed));
	atomic_store_explicit(head_slot, new_node, memory_order_release);
	(void)__xtc_mtx_unlock(&h->stripe_locks[stripe]);
	xtc_rcu_read_unlock();

	if (out_old_value != NULL) *out_old_value = NULL;
	{
		size_t cnt = atomic_fetch_add_explicit(&h->count, 1,
		    memory_order_relaxed) + 1;
		/* Grow past 75% load factor.  arr->n is a snapshot from
		 * before this insert; a heuristic trigger, not exact --
		 * fine, since grow itself re-reads the live array. */
		if (cnt * 4 > arr->n * 3)
			__chash_grow(h);
	}
	return XTC_OK;
}

int
xtc_chash_remove(xtc_chash_t *h, const void *key, void **out_value)
{
	uint64_t hh;
	struct chash_arr *arr;
	struct chash_node *n = NULL;
	_Atomic(struct chash_node *) *slot;
	size_t idx, stripe;

	if (h == NULL || key == NULL) return XTC_E_INVAL;
	hh = h->hash_fn(key);

	xtc_rcu_read_lock();
	arr = atomic_load_explicit(&h->arr, memory_order_acquire);
	for (;;) {
		idx = (size_t)(hh & (uint64_t)arr->mask);
		stripe = __chash_stripe(idx);
		(void)__xtc_mtx_lock(&h->stripe_locks[stripe]);
		{
			struct chash_arr *now = atomic_load_explicit(&h->arr,
			    memory_order_acquire);
			if (now == arr) break;
			(void)__xtc_mtx_unlock(&h->stripe_locks[stripe]);
			arr = now;
		}
	}

	slot = &arr->buckets[idx].head;
	for (n = atomic_load_explicit(slot, memory_order_acquire); n != NULL;
	    slot = &n->next, n = atomic_load_explicit(slot,
	    memory_order_acquire)) {
		if (n->hash == hh && h->cmp(n->key, key) == 0) {
			struct chash_node *nxt = atomic_load_explicit(
			    &n->next, memory_order_acquire);
			/* n's OWN next is left exactly as it was: a reader
			 * already parked on n keeps walking it unaffected. */
			atomic_store_explicit(slot, nxt, memory_order_release);
			break;
		}
	}
	(void)__xtc_mtx_unlock(&h->stripe_locks[stripe]);
	xtc_rcu_read_unlock();

	if (n == NULL) return XTC_E_NOTFOUND;
	if (out_value != NULL)
		*out_value = atomic_load_explicit(&n->value,
		    memory_order_relaxed);
	xtc_rcu_retire(n, __chash_node_free_cb);
	atomic_fetch_sub_explicit(&h->count, 1, memory_order_relaxed);
	return XTC_OK;
}

size_t
xtc_chash_size(const xtc_chash_t *h)
{
	if (h == NULL) return 0;
	return atomic_load_explicit(&((xtc_chash_t *)(uintptr_t)h)->count,
	    memory_order_relaxed);
}

/*
 * Double the bucket array.  Claims every stripe lock (ascending order:
 * the only place more than one is held at once) so no writer can be
 * mid-mutation on ANY bucket while we rehash; readers are never
 * blocked.  Duplicates every live node into the new array rather than
 * relinking the original in place, because an original node's `next`
 * field may be mid-walk by a concurrent reader on the OLD array right
 * now -- mutating it out from under that reader would be a bug.  On
 * OOM mid-rehash, unwinds everything allocated so far and leaves the
 * old array in place (no data lost, just no headroom gained yet).
 *
 * ponytail: one xtc_rcu_retire call per old node (no batch-retire API
 * on xtc_rcu); fine for the "rare, amortised" grow this is, revisit if
 * a workload greater than a few hundred thousand live keys makes grow
 * pauses (all stripes blocked for the O(n) rehash) show up in a
 * latency profile.
 */
static void
__chash_grow(xtc_chash_t *h)
{
	struct chash_arr *old_arr, *new_arr;
	size_t new_n, i;
	unsigned s;
	int expect = 0, ok = 1;

	if (!atomic_compare_exchange_strong_explicit(&h->resizing, &expect, 1,
	    memory_order_acq_rel, memory_order_relaxed))
		return;   /* someone else is already growing */

	for (s = 0; s < XTC_CHASH_NSTRIPES; s++)
		(void)__xtc_mtx_lock(&h->stripe_locks[s]);

	old_arr = atomic_load_explicit(&h->arr, memory_order_acquire);
	new_n = old_arr->n * 2;

	/* Refuse to grow past the size the pow2 cap already enforces, so
	 * the size arithmetic below cannot overflow (defense-in-depth; a
	 * table this large is not a real workload -- stay at current size
	 * rather than misbehave). */
	if (new_n <= old_arr->n ||
	    new_n > (SIZE_MAX / 2) / sizeof(struct chash_bucket)) {
		ok = 0;
		goto done;
	}

	if (__os_calloc(1, sizeof(struct chash_arr) +
	    new_n * sizeof(struct chash_bucket), (void **)&new_arr) !=
	    XTC_OK) {
		ok = 0;
		goto done;
	}
	new_arr->n = new_n;
	new_arr->mask = new_n - 1;

	for (i = 0; i < old_arr->n && ok; i++) {
		struct chash_node *n;
		for (n = atomic_load_explicit(&old_arr->buckets[i].head,
		    memory_order_acquire); n != NULL;
		    n = atomic_load_explicit(&n->next, memory_order_acquire)) {
			struct chash_node *dup;
			size_t b;
			if (__os_malloc(sizeof *dup, (void **)&dup) !=
			    XTC_OK) {
				ok = 0;
				break;
			}
			dup->hash = n->hash;
			dup->key = n->key;
			atomic_store_explicit(&dup->value,
			    atomic_load_explicit(&n->value,
			    memory_order_relaxed), memory_order_relaxed);
			b = (size_t)(dup->hash & (uint64_t)new_arr->mask);
			atomic_store_explicit(&dup->next,
			    atomic_load_explicit(&new_arr->buckets[b].head,
			    memory_order_relaxed), memory_order_relaxed);
			atomic_store_explicit(&new_arr->buckets[b].head, dup,
			    memory_order_relaxed);
		}
	}

	if (!ok) {
		/* Unwind: free every dup already linked into new_arr, then
		 * new_arr itself.  old_arr is completely untouched. */
		for (i = 0; i < new_arr->n; i++) {
			struct chash_node *n = atomic_load_explicit(
			    &new_arr->buckets[i].head, memory_order_relaxed);
			while (n != NULL) {
				struct chash_node *next =
				    atomic_load_explicit(&n->next,
				    memory_order_relaxed);
				__os_free(n);
				n = next;
			}
		}
		__os_free(new_arr);
		goto done;
	}

	atomic_store_explicit(&h->arr, new_arr, memory_order_release);

	/* old_arr and every one of its original nodes are now
	 * unreachable from h->arr but may still be mid-walk by a reader
	 * that loaded old_arr before the swap -- retire, never free. */
	for (i = 0; i < old_arr->n; i++) {
		struct chash_node *n = atomic_load_explicit(
		    &old_arr->buckets[i].head, memory_order_relaxed);
		while (n != NULL) {
			struct chash_node *next = atomic_load_explicit(
			    &n->next, memory_order_relaxed);
			xtc_rcu_retire(n, __chash_node_free_cb);
			n = next;
		}
	}
	xtc_rcu_retire(old_arr, __chash_arr_free_cb);

done:
	for (s = 0; s < XTC_CHASH_NSTRIPES; s++)
		(void)__xtc_mtx_unlock(&h->stripe_locks[s]);
	atomic_store_explicit(&h->resizing, 0, memory_order_release);
	(void)ok;
}
