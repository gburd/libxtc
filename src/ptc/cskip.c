/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/cskip.c
 *	RCU-protected concurrent ordered map -- a lock-free-reader
 *	skiplist (Pugh 1990).  See xtc_cskip.h for the API contract;
 *	this file is the "get the RCU + tower discipline exactly right"
 *	part.  It is the ordered sibling of xtc_chash and follows the
 *	same reclamation rule: removed nodes are xtc_rcu_retire'd, never
 *	freed inline, so a reader mid-walk keeps a valid node until it
 *	leaves its read-side.
 *
 *	Concurrency design:
 *
 *	  Lock-free reads.  get / min / floor take NO lock.  A search
 *	  starts at the top level of the head sentinel and walks forward
 *	  with acquire loads, dropping a level whenever the next node at
 *	  the current level would overshoot the search key.  Every
 *	  forward pointer a writer publishes is a SINGLE release store of
 *	  an already-fully-formed node, so a reader sees either the old
 *	  chain or the new node complete at that level -- never a torn
 *	  link.  LEVEL 0 IS THE SOURCE OF TRUTH FOR MEMBERSHIP: a node
 *	  linked at level 0 is present even if the writer has not yet
 *	  finished splicing it into upper levels (upper levels are only
 *	  a search accelerator).
 *
 *	  Single writer mutex (v1).  insert / remove serialize on one
 *	  per-map mutex.  The reader path is the one that must scale, and
 *	  it takes no lock; a fine-grained per-node writer lock (the
 *	  Fraser/Herlihy-Shavit lazy scheme) is a deferred optimization
 *	  recorded below.  Under the writer mutex an insert links the new
 *	  node BOTTOM-UP (level 0 first) so it becomes a member the
 *	  instant its base link is published; a remove unlinks TOP-DOWN
 *	  so the node disappears from the accelerator levels before its
 *	  base link, then it is retired.
 *
 *	  Removal.  The predecessor's forward pointer at each level is
 *	  overwritten (release store) with the removed node's OWN
 *	  (unchanged) successor at that level -- the removed node's own
 *	  forward pointers are never touched, so a reader already parked
 *	  on it walks the rest of the chain exactly as it stood on
 *	  arrival.  The node is handed to xtc_rcu_retire; its memory is
 *	  reclaimed only after every reader that could hold it drains.
 *
 *	  Deterministic level source.  A skiplist's per-node height is
 *	  normally a coin-flip PRNG -- a classic determinism hazard.
 *	  Height affects only search PERFORMANCE, never membership or any
 *	  observable result (get/min/floor/size are a pure function of
 *	  the key set + ordering), so replay-identity of OBSERVABLES does
 *	  not depend on it.  Still, to keep the module honest under DST,
 *	  the level source is a per-map splitmix64 advanced under the
 *	  writer mutex (so it is a deterministic function of the insert
 *	  call sequence, which under single-thread DST is itself
 *	  deterministic); under an active sim run it draws from the
 *	  seeded __xtc_sim_rng.  Neither path touches a real clock, an
 *	  unseeded RNG, or global state, so it never trips the
 *	  determinism guard.
 *
 *	  This module never calls xtc_rcu_synchronize itself:
 *	  reclamation is caller/reaper-driven, exactly like xtc_chash.
 *
 *	Single writer mutex + per-map level PRNG.  Upgrade path
 *	if concurrent-WRITER throughput ever matters: lazy per-node
 *	lock-coupling (mark-then-physically-unlink) for writers, keeping
 *	the reader path exactly as it is.  Not built until a workload
 *	shows writer contention -- readers already scale.
 */

#include "xtc_int.h"
#include "preempt_int.h"   /* __xtc_unsafe_* / __xtc_mtx_*: internal preemption brackets */
#include "xtc_cskip.h"
#include "xtc_rcu.h"
#include "xtc_sim.h"

#include <pthread.h>
#include <stdint.h>

/*
 * Max tower height.  32 levels with p=1/4 supports ~4^32 keys before
 * the top level stops helping -- vastly more than any in-memory map.
 * A node allocates exactly its own height's worth of forward
 * pointers (a flexible array), so a tall MAX costs nothing per short
 * node.
 */
#define XTC_CSKIP_MAXLEVEL 32

struct cskip_node {
	void                          *key;    /* caller-owned */
	void *value;                           /* caller-owned */
	int                            level;  /* # of forward slots, >=1 */
	/* forward[i] = next node at level i.  Flexible array: a node of
	 * height L allocates L slots.  Atomic: readers load with acquire,
	 * writers publish with release. */
	struct cskip_node *forward[];
};

struct xtc_cskip {
	xtc_cskip_cmp_fn      cmp;
	struct cskip_node    *head;      /* sentinel, height MAXLEVEL, key=NULL */
	int level;                       /* current highest occupied level+1 */
	size_t count;
	pthread_mutex_t       wlock;     /* serializes insert/remove */
	uint64_t              rng;       /* splitmix64 state, under wlock */
};

/* splitmix64: a good, tiny, fully deterministic PRNG. */
static inline uint64_t
__cskip_splitmix64(uint64_t *s)
{
	uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

/*
 * Random level for a new node: geometric with p=1/4 (two coin bits per
 * level).  Called only under the writer mutex.  Under an active sim
 * run the entropy is the seeded sim RNG (replay-identical); otherwise
 * the per-map splitmix64.  Either way it is deterministic and never
 * trips the determinism guard.
 */
static int
__cskip_random_level(struct xtc_cskip *s)
{
	uint64_t bits;
	int lvl = 1;
	if (__xtc_sim_active())
		bits = __xtc_sim_rng(0);
	else
		bits = __cskip_splitmix64(&s->rng);
	/* consume 2 bits per extra level: both set (prob 1/4) => go up */
	while (lvl < XTC_CSKIP_MAXLEVEL && (bits & 3u) == 3u) {
		bits >>= 2;
		lvl++;
	}
	return lvl;
}

static struct cskip_node *
__cskip_node_alloc(int level, void *key, void *value)
{
	struct cskip_node *n;
	size_t sz = sizeof *n + (size_t)level * sizeof(struct cskip_node *);
	int i;
	if (__os_malloc(sz, (void **)&n) != XTC_OK)
		return NULL;
	n->key = key;
	xtc_atomic_store_ptr(&n->value, value, xtc_mo_relaxed);
	n->level = level;
	for (i = 0; i < level; i++)
		xtc_atomic_store_ptr(&n->forward[i], NULL, xtc_mo_relaxed);
	return n;
}

static void
__cskip_node_free_cb(void *p)
{
	__os_free(p);
}

int
xtc_cskip_create(xtc_cskip_cmp_fn cmp, xtc_cskip_t **out)
{
	struct xtc_cskip *s;
	int i;

	if (cmp == NULL || out == NULL)
		return XTC_E_INVAL;
	if (__os_malloc(sizeof *s, (void **)&s) != XTC_OK)
		return XTC_E_NOMEM;
	s->cmp = cmp;
	xtc_atomic_store_i32(&s->level, 1, xtc_mo_relaxed);
	xtc_atomic_store_size(&s->count, 0, xtc_mo_relaxed);
	s->rng = 0x243F6A8885A308D3ULL;   /* fixed seed: deterministic levels */
	(void)pthread_mutex_init(&s->wlock, NULL);
	/* Head sentinel is full height with all-NULL forward pointers. */
	s->head = __cskip_node_alloc(XTC_CSKIP_MAXLEVEL, NULL, NULL);
	if (s->head == NULL) {
		(void)pthread_mutex_destroy(&s->wlock);
		__os_free(s);
		return XTC_E_NOMEM;
	}
	for (i = 0; i < XTC_CSKIP_MAXLEVEL; i++)
		xtc_atomic_store_ptr(&s->head->forward[i], NULL,
		    xtc_mo_relaxed);
	*out = s;
	return XTC_OK;
}

void
xtc_cskip_destroy(xtc_cskip_t *s)
{
	struct cskip_node *n, *next;
	if (s == NULL)
		return;
	/* Not concurrency-safe: caller has quiesced.  Walk level 0 and
	 * free every node (bookkeeping only; caller owns key/value). */
	n = xtc_atomic_load_ptr(&s->head->forward[0], xtc_mo_relaxed);
	while (n != NULL) {
		next = xtc_atomic_load_ptr(&n->forward[0], xtc_mo_relaxed);
		__os_free(n);
		n = next;
	}
	__os_free(s->head);
	(void)pthread_mutex_destroy(&s->wlock);
	__os_free(s);
}

/*
 * Lock-free search.  Returns the FIRST node whose key is >= `key` (the
 * successor-or-equal), or NULL if none.  Fills preds[] (the last node
 * seen at each level whose forward pointer we would splice through) if
 * preds is non-NULL -- writers pass a preds array, readers pass NULL.
 * Runs inside the caller's RCU read-side.
 */
static struct cskip_node *
__cskip_find_ge(struct xtc_cskip *s, const void *key,
    struct cskip_node **preds, struct cskip_node **succs)
{
	struct cskip_node *x = s->head;
	struct cskip_node *nxt;
	int top = xtc_atomic_load_i32(&s->level, xtc_mo_acquire);
	int i;

	if (top < 1)
		top = 1;
	for (i = top - 1; i >= 0; i--) {
		for (;;) {
			nxt =
			    xtc_atomic_load_ptr(&x->forward[i], xtc_mo_acquire);
			if (nxt != NULL && s->cmp(nxt->key, key) < 0)
				x = nxt;   /* nxt still strictly before key */
			else
				break;
		}
		if (preds != NULL) preds[i] = x;
		if (succs != NULL) succs[i] = nxt;
	}
	/* nxt is now the level-0 successor-or-equal (>= key). */
	return nxt;
}

int
xtc_cskip_get(xtc_cskip_t *s, const void *key, void **out_value)
{
	struct cskip_node *n;
	if (s == NULL || key == NULL || out_value == NULL)
		return XTC_E_INVAL;
	n = __cskip_find_ge(s, key, NULL, NULL);
	if (n != NULL && s->cmp(n->key, key) == 0) {
		*out_value = xtc_atomic_load_ptr(&n->value, xtc_mo_acquire);
		return XTC_OK;
	}
	return XTC_E_NOTFOUND;
}

int
xtc_cskip_min(xtc_cskip_t *s, void **out_key, void **out_value)
{
	struct cskip_node *n;
	if (s == NULL)
		return XTC_E_INVAL;
	n = xtc_atomic_load_ptr(&s->head->forward[0], xtc_mo_acquire);
	if (n == NULL)
		return XTC_E_NOTFOUND;
	if (out_key != NULL) *out_key = n->key;
	if (out_value != NULL)
		*out_value = xtc_atomic_load_ptr(&n->value, xtc_mo_acquire);
	return XTC_OK;
}

int
xtc_cskip_floor(xtc_cskip_t *s, const void *key, void **out_key,
    void **out_value)
{
	struct cskip_node *x, *nxt;
	int top, i;
	if (s == NULL || key == NULL)
		return XTC_E_INVAL;
	/* Walk down keeping the last node strictly < or == key.  We want
	 * the largest key <= `key`: advance while next->key <= key. */
	x = s->head;
	top = xtc_atomic_load_i32(&s->level, xtc_mo_acquire);
	if (top < 1) top = 1;
	for (i = top - 1; i >= 0; i--) {
		for (;;) {
			nxt =
			    xtc_atomic_load_ptr(&x->forward[i], xtc_mo_acquire);
			if (nxt != NULL && s->cmp(nxt->key, key) <= 0)
				x = nxt;
			else
				break;
		}
	}
	/* x is the head sentinel (key NULL) iff no key <= `key`. */
	if (x == s->head)
		return XTC_E_NOTFOUND;
	if (out_key != NULL) *out_key = x->key;
	if (out_value != NULL)
		*out_value = xtc_atomic_load_ptr(&x->value, xtc_mo_acquire);
	return XTC_OK;
}

int
xtc_cskip_insert(xtc_cskip_t *s, void *key, void *value, void **out_old_value)
{
	struct cskip_node *preds[XTC_CSKIP_MAXLEVEL];
	struct cskip_node *succs[XTC_CSKIP_MAXLEVEL];
	struct cskip_node *n, *cand;
	int lvl, i, cur_level;

	if (s == NULL || key == NULL)
		return XTC_E_INVAL;

	xtc_rcu_read_lock();
	(void)__xtc_mtx_lock(&s->wlock);

	cand = __cskip_find_ge(s, key, preds, succs);
	if (cand != NULL && s->cmp(cand->key, key) == 0) {
		/* Replace value in place: one atomic store, no linkage. */
		void *old = xtc_atomic_load_ptr(&cand->value, xtc_mo_relaxed);
		xtc_atomic_store_ptr(&cand->value, value, xtc_mo_release);
		(void)__xtc_mtx_unlock(&s->wlock);
		xtc_rcu_read_unlock();
		if (out_old_value != NULL) *out_old_value = old;
		return XTC_OK;
	}

	lvl = __cskip_random_level(s);
	cur_level = xtc_atomic_load_i32(&s->level, xtc_mo_relaxed);
	if (lvl > cur_level) {
		/* New levels: their predecessor is the head sentinel, whose
		 * forward[i] is NULL (succs[i] for those levels is NULL). */
		for (i = cur_level; i < lvl; i++) {
			preds[i] = s->head;
			succs[i] = NULL;
		}
	}

	n = __cskip_node_alloc(lvl, key, value);
	if (n == NULL) {
		(void)__xtc_mtx_unlock(&s->wlock);
		xtc_rcu_read_unlock();
		return XTC_E_NOMEM;
	}
	/* Point the new node's forward pointers at its successors FIRST
	 * (private, not yet visible), then publish bottom-up. */
	for (i = 0; i < lvl; i++)
		xtc_atomic_store_ptr(&n->forward[i], succs[i], xtc_mo_relaxed);
	/* Publish bottom-up: level 0 first makes the node a member the
	 * instant its base link is visible; upper levels only accelerate
	 * future searches. */
	for (i = 0; i < lvl; i++)
		xtc_atomic_store_ptr(&preds[i]->forward[i], n, xtc_mo_release);
	if (lvl > cur_level)
		xtc_atomic_store_i32(&s->level, lvl, xtc_mo_release);

	xtc_atomic_fetch_add_size(&s->count, 1, xtc_mo_relaxed);
	(void)__xtc_mtx_unlock(&s->wlock);
	xtc_rcu_read_unlock();
	if (out_old_value != NULL) *out_old_value = NULL;
	return XTC_OK;
}

int
xtc_cskip_remove(xtc_cskip_t *s, const void *key, void **out_value)
{
	struct cskip_node *preds[XTC_CSKIP_MAXLEVEL];
	struct cskip_node *succs[XTC_CSKIP_MAXLEVEL];
	struct cskip_node *victim;
	void *val;
	int i, top;

	if (s == NULL || key == NULL)
		return XTC_E_INVAL;

	xtc_rcu_read_lock();
	(void)__xtc_mtx_lock(&s->wlock);

	victim = __cskip_find_ge(s, key, preds, succs);
	if (victim == NULL || s->cmp(victim->key, key) != 0) {
		(void)__xtc_mtx_unlock(&s->wlock);
		xtc_rcu_read_unlock();
		return XTC_E_NOTFOUND;
	}
	/* Unlink TOP-DOWN: at every level where preds[i]'s successor is
	 * the victim, splice preds[i] past it (release store of the
	 * victim's own unchanged successor).  Only touch levels that
	 * actually pointed at the victim. */
	top = xtc_atomic_load_i32(&s->level, xtc_mo_relaxed);
	for (i = top - 1; i >= 0; i--) {
		if (i < victim->level && preds[i] != NULL &&
		    xtc_atomic_load_ptr(&preds[i]->forward[i],
		        xtc_mo_relaxed) == victim) {
			struct cskip_node *after =
			    xtc_atomic_load_ptr(&victim->forward[i],
			        xtc_mo_relaxed);
			xtc_atomic_store_ptr(&preds[i]->forward[i], after,
			    xtc_mo_release);
		}
	}
	/* Lower s->level while the top levels are now empty. */
	while (top > 1 &&
	    xtc_atomic_load_ptr(&s->head->forward[top - 1], xtc_mo_relaxed) ==
	        NULL) {
		top--;
	}
	xtc_atomic_store_i32(&s->level, top, xtc_mo_release);

	val = xtc_atomic_load_ptr(&victim->value, xtc_mo_relaxed);
	xtc_atomic_fetch_sub_size(&s->count, 1, xtc_mo_relaxed);
	(void)__xtc_mtx_unlock(&s->wlock);
	xtc_rcu_read_unlock();

	/* Retire, never free inline: a reader parked on the victim keeps
	 * walking its untouched forward pointers until it drains. */
	xtc_rcu_retire(victim, __cskip_node_free_cb);
	if (out_value != NULL) *out_value = val;
	return XTC_OK;
}

size_t
xtc_cskip_size(const xtc_cskip_t *s)
{
	if (s == NULL)
		return 0;
	return xtc_atomic_load_size(&((struct xtc_cskip *)s)->count,
	    xtc_mo_relaxed);
}
