/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/btree.c
 *	A B+-tree key/value store layered on the xtc-native buffer
 *	manager (bufmgr.h, page-table path) and the prefix-compressed
 *	slotted node (btnode.h).  See btree.h for the public contract.
 *
 *	NODE CONVENTIONS
 *	----------------
 *	Every page IS a btnode (btnode_init'd on the raw frame bytes).
 *
 *	  o A LEAF node (btnode is_leaf == 1) maps the full user key to
 *	    the user value bytes.
 *
 *	  o An INTERNAL node (is_leaf == 0) maps a separator key to an
 *	    8-byte child PAGE ID, stored as the btnode "value".  The 8
 *	    value bytes are NOT guaranteed aligned, so child ids are
 *	    always moved in/out with memcpy of a uint64_t -- never by
 *	    casting the value pointer to uint64_t*.
 *
 *	    An internal node with C children stores them as:
 *	        slot 0 : (EMPTY key, klen 0)  -> leftmost child pid
 *	        slot i : (separator_i)        -> child_i pid     (i >= 1)
 *	    Separators use "exclusive lower bound of the child to their
 *	    right" semantics: a slot (k_i, c_i) means c_i holds keys in
 *	    (k_i, k_{i+1}], with the slot-0 empty key acting as -inf.
 *	    Equivalently k_i is the *largest* key of the subtree to its
 *	    left.  Child selection for a search key K:
 *
 *	        s = btnode_search(K, NULL)   // first slot key >= K
 *	        child_slot = max(0, s - 1)   // rightmost key strictly < K
 *
 *	    so K descends through c_i where k_i < K <= k_{i+1}.  The
 *	    empty key (klen 0) sorts before every non-empty key, so a
 *	    real K never needs the clamp; it only guards the degenerate
 *	    empty-key probe.  test_btree.c exercises this rule directly,
 *	    including keys that sit exactly on a separator boundary.
 *
 *	    Internal nodes are kept with wide-open (-inf/+inf) fences,
 *	    hence prefix_len 0: separators are stored verbatim and are
 *	    never constrained by a fence.  That is essential -- a node's
 *	    fence (which bounds its stored separators) is generally
 *	    tighter than its subtree, so a key routed *through* the node
 *	    can exceed the fence; with prefix_len 0 that is harmless,
 *	    and storing a later separator that exceeds the old fence is
 *	    likewise safe.  The slot-0 empty key (zero length) is also
 *	    only well-defined with prefix_len 0.
 *
 *	SPLIT / SEPARATOR
 *	-----------------
 *	LEAF split uses btnode_split(): it keeps the lower half of the
 *	slots in the original page, moves the upper half into a freshly
 *	allocated right page, narrows both fences (left's hi fence ==
 *	right's lo fence == the left's largest key, "esep"), and
 *	maintains the sibling chain (right inherits the old
 *	right_sibling; the caller rewires left -> right pid).  We push
 *	esep UP as the separator and route the triggering key by esep:
 *	keys <= esep stay left, keys > esep go right.  Because esep is
 *	exactly the left's hi fence, the parent's routing boundary
 *	coincides with the child fences -- no key can land in an unowned
 *	gap, and the buggy alternative of pushing the right page's first
 *	key (its lo fence is esep, but its first key is larger) is
 *	avoided.
 *
 *	INTERNAL split is done manually (split_internal): the middle
 *	separator is pushed up and removed from the node, the left node
 *	keeps the children to its left, and the right node takes the
 *	middle child as its empty-key leftmost child plus everything to
 *	the right.  btnode_split is NOT used for inner nodes because it
 *	would narrow their fences to the left's max separator while the
 *	left subtree extends past it, breaking the prefix invariant for
 *	later separator inserts.
 *
 *	When the root splits, a new root is grown whose empty-key slot 0
 *	points at the old root and whose one separator points at the new
 *	right page; the tree height grows by one.
 *
 *	CONCURRENCY (correctness first)
 *	-------------------------------
 *	A single per-tree mutex (bt->struct_mu) serializes the whole of
 *	bt_insert and bt_delete: writers run one at a time.  Readers
 *	(bt_lookup, the cursor) take NO mutex and proceed concurrently;
 *	they latch-couple with shared per-frame content latches.  To
 *	exclude readers from a node it is mutating, a writer takes the
 *	exclusive content latch on that node before the mutation and
 *	drops it after.  Thus struct_mu serializes writers against each
 *	other and the per-node latch serializes a writer against readers.
 *	The pin (bm_fix_pid / bm_unfix) keeps a frame resident across an
 *	operation; the content latch is only held for brief in-memory
 *	node work, never across page I/O.
 *
 *	This is the simplest scheme that is correct for one writer and
 *	many readers.  Finer-grained latch coupling for writers (releasing
 *	safe ancestors during the descent and splitting bottom-up under
 *	per-node latches, B-link style) is the next step; here a writer
 *	records the root-to-leaf pid path and re-fixes ancestors by pid
 *	when a split must propagate (safe because struct_mu keeps the
 *	structure stable for the duration).
 */

#include "btree.h"
#include "btnode.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BT_MAX_HEIGHT 32       /* fanout >= 2 => trees this tall are absurd */
#define BT_MAX_KEY    1024     /* largest full key we route on the stack */

struct bt {
	bm_t             *bm;
	uint32_t          page_size;
	_Atomic bm_pid_t  root_pid;
	pthread_mutex_t   struct_mu;   /* serializes writers */

	_Atomic uint64_t  st_inserts;
	_Atomic uint64_t  st_lookups;
	_Atomic uint64_t  st_splits;
	_Atomic uint64_t  st_height;   /* number of levels (1 == root leaf) */
};

struct bt_cursor {
	bt_t          *bt;
	bm_frame_t    *leaf;   /* current leaf: fixed + shared-latched, or NULL */
	int            slot;   /* next slot to yield in the current leaf */
	int            done;
	uint8_t        keybuf[BT_MAX_KEY];
	uint16_t       keylen;
};

/* Lexicographic compare of two byte strings as unsigned bytes. */
static int
key_cmp(const void *a, uint16_t alen, const void *b, uint16_t blen)
{
	uint16_t lim = alen < blen ? alen : blen;
	int c = lim ? memcmp(a, b, lim) : 0;

	if (c != 0)
		return c < 0 ? -1 : 1;
	if (alen < blen)
		return -1;
	if (alen > blen)
		return 1;
	return 0;
}

/* Read the 8-byte child page id stored as the value of `slot`. */
static bm_pid_t
child_pid_at(const void *page, int slot)
{
	const void *vp = NULL;
	uint16_t vl = 0;
	bm_pid_t pid = BM_PID_NONE;

	if (btnode_get(page, slot, NULL, NULL, &vp, &vl) != 0 || vp == NULL)
		return BM_PID_NONE;
	/* Value bytes are unaligned; copy out a whole uint64_t. */
	memcpy(&pid, vp, sizeof pid < (size_t)vl ? sizeof pid : (size_t)vl);
	return pid;
}

/* Child to descend into from an internal node for search key K.
 *
 * Separators carry "exclusive lower bound of the child to their right"
 * semantics: a slot (k_i, c_i) means c_i holds keys in (k_i, k_{i+1}],
 * with the slot-0 empty key acting as -infinity.  The child for K is
 * therefore the rightmost slot whose key is strictly less than K, i.e.
 * (lower_bound(K) - 1); descend through c_i where k_i < K <= k_{i+1}.
 * This pairs with pushing the splitting node's largest key (its hi
 * fence) up as the separator, so the routing boundary always coincides
 * with a node fence and no key can fall into an unowned gap. */
static bm_pid_t
child_for_key(const void *page, const void *key, uint16_t klen)
{
	int s = btnode_search(page, key, klen, NULL);
	int idx = s - 1;

	if (idx < 0)
		idx = 0;
	return child_pid_at(page, idx);
}

/* Store an 8-byte child pid value into an internal slot via insert. */
static int
internal_insert(void *page, const void *sep, uint16_t seplen, bm_pid_t child)
{
	uint64_t v = (uint64_t)child;

	return btnode_insert(page, sep, seplen, &v, (uint16_t)sizeof v);
}

int
bt_open(bm_t *bm, bt_t **out)
{
	bt_t *bt;
	bm_frame_t *rf = NULL, *sf = NULL;
	bm_pid_t rpid = BM_PID_NONE, spid = BM_PID_NONE;
	uint32_t ps;
	int rc;

	if (bm == NULL || out == NULL)
		return XTC_E_INVAL;

	bt = calloc(1, sizeof *bt);
	if (bt == NULL)
		return XTC_E_NOMEM;
	bt->bm = bm;
	(void)pthread_mutex_init(&bt->struct_mu, NULL);

	/*
	 * The buffer manager exposes no page-size accessor and its
	 * struct is opaque, so discover the page size from the geometry
	 * of the (contiguous) frame pool: two freshly allocated frames
	 * are adjacent, so their page pointers differ by exactly one
	 * page.  The scratch page is left as a never-referenced orphan
	 * (cheap, and freed with the pool at bm_destroy).
	 */
	rc = bm_alloc_pid(bm, &rf, &rpid);
	if (rc != XTC_OK)
		goto fail;
	rc = bm_alloc_pid(bm, &sf, &spid);
	if (rc != XTC_OK) {
		bm_unfix(bm, rf, 0);
		goto fail;
	}
	{
		uintptr_t a = (uintptr_t)bm_page(rf);
		uintptr_t b = (uintptr_t)bm_page(sf);
		uintptr_t d = a > b ? a - b : b - a;

		ps = (uint32_t)d;
		if (ps < 64 || ps > 65535)
			ps = 4096;   /* fall back to the common default */
	}
	bm_unfix(bm, sf, 0);             /* scratch becomes an orphan page */
	bt->page_size = ps;

	/* Root starts as a single leaf with -inf/+inf fences (prefix 0). */
	btnode_init(bm_page(rf), ps, 1);
	btnode_set_fences(bm_page(rf), NULL, 0, NULL, 0);
	atomic_store(&bt->root_pid, rpid);
	atomic_store(&bt->st_height, 1);
	bm_unfix(bm, rf, 1);

	*out = bt;
	return XTC_OK;

fail:
	(void)pthread_mutex_destroy(&bt->struct_mu);
	free(bt);
	return rc;
}

void
bt_close(bt_t *bt)
{
	if (bt == NULL)
		return;
	(void)pthread_mutex_destroy(&bt->struct_mu);
	free(bt);
}

/*
 * Split a full INTERNAL node `pp` (exclusively latched) the standard
 * B-tree way: the middle separator is pushed up and removed from the
 * node, pp keeps the children to its left, and a freshly allocated
 * right node rp receives the middle child as its empty-key leftmost
 * child followed by every separator/child to the right.  Internal
 * nodes always carry wide-open fences (prefix_len 0), so a separator
 * is stored verbatim and is never constrained by a fence -- which is
 * what makes routing a key through a node whose fence is tighter than
 * its subtree safe, and what btnode_split (which narrows fences to the
 * left node's max key) cannot give us for inner nodes.
 *
 * On success returns XTC_OK with rp fixed + exclusively latched in
 * *rf_out, its pid in *rpid_out, and the pushed-up separator copied
 * into pushup / *pushuplen.
 */
static int
split_internal(bt_t *bt, void *pp, bm_frame_t **rf_out, bm_pid_t *rpid_out,
    uint8_t *pushup, uint16_t *pushuplen)
{
	bm_t *bm = bt->bm;
	bm_frame_t *rf;
	bm_pid_t rpid;
	void *rp;
	int count = (int)btnode_count(pp);
	int mid = count / 2;
	int i;
	uint8_t empty = 0;
	int rc;

	rc = bm_alloc_pid(bm, &rf, &rpid);
	if (rc != XTC_OK)
		return rc;
	rp = bm_page(rf);
	bm_latch_exclusive(rf);
	btnode_init(rp, bt->page_size, 0);
	btnode_set_fences(rp, NULL, 0, NULL, 0);

	/* Read everything from pp before trimming it. */
	if (btnode_full_key(pp, mid, pushup, BT_MAX_KEY, pushuplen) != 0 ||
	    internal_insert(rp, &empty, 0, child_pid_at(pp, mid)) != 0) {
		bm_unlatch(rf);
		bm_unfix(bm, rf, 1);
		return XTC_E_INTERNAL;
	}
	for (i = mid + 1; i < count; i++) {
		uint8_t kb[BT_MAX_KEY];
		uint16_t kl = 0;

		if (btnode_full_key(pp, i, kb, sizeof kb, &kl) != 0 ||
		    internal_insert(rp, kb, kl, child_pid_at(pp, i)) != 0) {
			bm_unlatch(rf);
			bm_unfix(bm, rf, 1);
			return XTC_E_INTERNAL;
		}
	}
	/* Drop slots mid..count-1 from pp (back to front keeps indices). */
	for (i = count - 1; i >= mid; i--)
		(void)btnode_remove(pp, i);

	*rf_out = rf;
	*rpid_out = rpid;
	return XTC_OK;
}

/*
 * Insert (sep, right_pid) into the parent that previously pointed at
 * left_pid, splitting parents and growing the root as needed.  Called
 * with struct_mu held; `path` holds the root-to-leaf pid chain and
 * `level` is the index in `path` of the node that just split.
 */
static int
propagate_split(bt_t *bt, const bm_pid_t *path, int level,
    const void *sep_in, uint16_t seplen_in, bm_pid_t left_pid,
    bm_pid_t right_pid)
{
	bm_t *bm = bt->bm;
	uint8_t sep[BT_MAX_KEY];
	uint16_t seplen = seplen_in;
	int rc = XTC_OK;

	if (seplen > sizeof sep)
		return XTC_E_INTERNAL;
	memcpy(sep, sep_in, seplen);

	for (;;) {
		if (level == 0) {
			/* The node that split was the root: grow a new one. */
			bm_frame_t *nf;
			bm_pid_t npid;
			void *np;
			uint8_t empty = 0;

			rc = bm_alloc_pid(bm, &nf, &npid);
			if (rc != XTC_OK)
				return rc;
			np = bm_page(nf);
			bm_latch_exclusive(nf);
			btnode_init(np, bt->page_size, 0);
			btnode_set_fences(np, NULL, 0, NULL, 0);
			if (internal_insert(np, &empty, 0, left_pid) != 0 ||
			    internal_insert(np, sep, seplen, right_pid) != 0) {
				bm_unlatch(nf);
				bm_unfix(bm, nf, 1);
				return XTC_E_INTERNAL;
			}
			bm_unlatch(nf);
			bm_unfix(bm, nf, 1);
			atomic_store(&bt->root_pid, npid);
			atomic_fetch_add(&bt->st_height, 1);
			return XTC_OK;
		}

		{
			int pidx = level - 1;
			bm_pid_t parent_pid = path[pidx];
			bm_frame_t *pf;
			void *pp;

			rc = bm_fix_pid(bm, parent_pid, &pf);
			if (rc != XTC_OK)
				return rc;
			pp = bm_page(pf);
			bm_latch_exclusive(pf);

			if (internal_insert(pp, sep, seplen, right_pid) == 0) {
				bm_unlatch(pf);
				bm_unfix(bm, pf, 1);
				return XTC_OK;
			}

			/* Parent is full: split it and carry the split up. */
			{
				bm_frame_t *rf;
				bm_pid_t rpid;
				void *rp;
				uint8_t pushup[BT_MAX_KEY];
				uint16_t pushuplen = 0;

				rc = split_internal(bt, pp, &rf, &rpid, pushup,
				    &pushuplen);
				if (rc != XTC_OK) {
					bm_unlatch(pf);
					bm_unfix(bm, pf, 1);
					return rc;
				}
				rp = bm_page(rf);

				/*
				 * Place the pending (sep,right_pid): a separator
				 * strictly below the pushed-up key stays in pp,
				 * otherwise it belongs in rp.  Inner nodes have
				 * no prefix, so either store is unconditionally
				 * valid; the comparison only fixes routing.
				 */
				if (key_cmp(sep, seplen, pushup, pushuplen) < 0)
					rc = internal_insert(pp, sep, seplen,
					    right_pid);
				else
					rc = internal_insert(rp, sep, seplen,
					    right_pid);
				if (rc != 0) {
					bm_unlatch(rf);
					bm_unfix(bm, rf, 1);
					bm_unlatch(pf);
					bm_unfix(bm, pf, 1);
					return XTC_E_INTERNAL;
				}
				bm_unlatch(rf);
				bm_unfix(bm, rf, 1);
				bm_unlatch(pf);
				bm_unfix(bm, pf, 1);
				atomic_fetch_add(&bt->st_splits, 1);

				/* Carry the pushed-up separator to the grandparent. */
				memcpy(sep, pushup, pushuplen);
				seplen = pushuplen;
				left_pid = parent_pid;
				right_pid = rpid;
				level = pidx;
				continue;
			}
		}
	}
}

int
bt_insert(bt_t *bt, const void *key, uint16_t klen, const void *val,
    uint16_t vlen)
{
	bm_t *bm;
	bm_pid_t path[BT_MAX_HEIGHT];
	int depth = 0;
	bm_pid_t pid;
	bm_frame_t *f = NULL;
	void *leaf;
	int rc = XTC_OK;
	int r;
	int found;
	int s;

	if (bt == NULL || key == NULL || (val == NULL && vlen != 0))
		return XTC_E_INVAL;

	bm = bt->bm;
	(void)pthread_mutex_lock(&bt->struct_mu);

	/* Descend to the target leaf, recording the pid path. */
	pid = atomic_load(&bt->root_pid);
	for (;;) {
		void *pg;

		if (depth >= BT_MAX_HEIGHT) {
			rc = XTC_E_INTERNAL;
			goto out;
		}
		path[depth++] = pid;
		rc = bm_fix_pid(bm, pid, &f);
		if (rc != XTC_OK)
			goto out;
		bm_latch_shared(f);
		pg = bm_page(f);
		if (btnode_is_leaf(pg)) {
			/* Swap to the exclusive latch for mutation.  The pin
			 * keeps the frame resident; struct_mu keeps the node
			 * stable, so it is still a leaf afterward. */
			bm_unlatch(f);
			bm_latch_exclusive(f);
			break;
		}
		pid = child_for_key(pg, key, klen);
		bm_unlatch(f);
		bm_unfix(bm, f, 0);
		f = NULL;
	}

	leaf = bm_page(f);

	/* Upsert: drop any existing entry for an exact key match. */
	found = 0;
	s = btnode_search(leaf, key, klen, &found);
	if (found)
		(void)btnode_remove(leaf, s);

	r = btnode_insert(leaf, key, klen, val, vlen);
	if (r == 0) {
		bm_unlatch(f);
		bm_unfix(bm, f, 1);
		f = NULL;
		atomic_fetch_add(&bt->st_inserts, 1);
		goto out;
	}

	/* Leaf is full: split it, place the new pair, push up a separator. */
	{
		bm_frame_t *rf;
		bm_pid_t rpid;
		void *rp;
		uint8_t esep[BT_MAX_KEY];
		uint16_t eseplen = 0;

		rc = bm_alloc_pid(bm, &rf, &rpid);
		if (rc != XTC_OK)
			goto out;
		rp = bm_page(rf);
		bm_latch_exclusive(rf);
		btnode_init(rp, bt->page_size, btnode_is_leaf(leaf));
		if (btnode_split(leaf, rp, esep, &eseplen) != 0) {
			bm_unlatch(rf);
			bm_unfix(bm, rf, 1);
			rc = XTC_E_INTERNAL;
			goto out;
		}
		btnode_set_right_sibling(leaf, (uint32_t)rpid);

		/*
		 * Place the triggering pair, then push esep up as the
		 * separator.  esep is the left node's largest key == its hi
		 * fence == the right's lo fence, so the routing boundary
		 * coincides with a fence: keys <= esep belong (and route) to
		 * the left, keys > esep to the right.  A new key > esep goes
		 * right even when it is smaller than the right's current first
		 * key (it falls in the gap between the halves); putting it in
		 * the left would breach the left's hi fence and corrupt prefix
		 * compression.
		 */
		if (key_cmp(key, klen, esep, eseplen) > 0)
			r = btnode_insert(rp, key, klen, val, vlen);
		else
			r = btnode_insert(leaf, key, klen, val, vlen);
		if (r != 0) {
			bm_unlatch(rf);
			bm_unfix(bm, rf, 1);
			rc = XTC_E_INTERNAL;
			goto out;
		}
		bm_unlatch(rf);
		bm_unfix(bm, rf, 1);
		bm_unlatch(f);
		bm_unfix(bm, f, 1);
		f = NULL;

		atomic_fetch_add(&bt->st_inserts, 1);
		atomic_fetch_add(&bt->st_splits, 1);

		rc = propagate_split(bt, path, depth - 1, esep, eseplen,
		    path[depth - 1], rpid);
	}

out:
	if (f != NULL) {
		bm_unlatch(f);
		bm_unfix(bm, f, 0);
	}
	(void)pthread_mutex_unlock(&bt->struct_mu);
	return rc;
}

int
bt_lookup(bt_t *bt, const void *key, uint16_t klen, void *buf, uint16_t cap,
    uint16_t *vlen)
{
	bm_t *bm;
	bm_pid_t pid;
	bm_frame_t *f;
	void *pg;
	int rc;
	int found;
	int s;
	const void *vp = NULL;
	uint16_t vl = 0;

	if (bt == NULL || key == NULL)
		return XTC_E_INVAL;
	bm = bt->bm;
	atomic_fetch_add(&bt->st_lookups, 1);

retry:
	pid = atomic_load(&bt->root_pid);
	rc = bm_fix_pid(bm, pid, &f);
	if (rc != XTC_OK)
		return rc;
	bm_latch_shared(f);
	/* If the root changed between the load and the latch, restart. */
	if (pid != atomic_load(&bt->root_pid)) {
		bm_unlatch(f);
		bm_unfix(bm, f, 0);
		goto retry;
	}
	pg = bm_page(f);

	while (!btnode_is_leaf(pg)) {
		bm_pid_t child = child_for_key(pg, key, klen);
		bm_frame_t *cf;

		rc = bm_fix_pid(bm, child, &cf);
		if (rc != XTC_OK) {
			bm_unlatch(f);
			bm_unfix(bm, f, 0);
			return rc;
		}
		bm_latch_shared(cf);     /* couple: hold child before parent */
		bm_unlatch(f);
		bm_unfix(bm, f, 0);
		f = cf;
		pg = bm_page(f);
	}

	found = 0;
	s = btnode_search(pg, key, klen, &found);
	if (!found) {
		bm_unlatch(f);
		bm_unfix(bm, f, 0);
		return XTC_E_NOTFOUND;
	}
	(void)btnode_get(pg, s, NULL, NULL, &vp, &vl);
	if (vlen != NULL)
		*vlen = vl;
	if (buf != NULL && cap > 0) {
		uint16_t n = vl < cap ? vl : cap;

		memcpy(buf, vp, n);
	}
	bm_unlatch(f);
	bm_unfix(bm, f, 0);
	return XTC_OK;
}

int
bt_delete(bt_t *bt, const void *key, uint16_t klen)
{
	bm_t *bm;
	bm_pid_t pid;
	bm_frame_t *f = NULL;
	void *pg;
	int rc = XTC_OK;
	int found;
	int s;

	if (bt == NULL || key == NULL)
		return XTC_E_INVAL;
	bm = bt->bm;
	(void)pthread_mutex_lock(&bt->struct_mu);

	pid = atomic_load(&bt->root_pid);
	for (;;) {
		rc = bm_fix_pid(bm, pid, &f);
		if (rc != XTC_OK)
			goto out;
		bm_latch_shared(f);
		pg = bm_page(f);
		if (btnode_is_leaf(pg)) {
			bm_unlatch(f);
			bm_latch_exclusive(f);
			break;
		}
		pid = child_for_key(pg, key, klen);
		bm_unlatch(f);
		bm_unfix(bm, f, 0);
		f = NULL;
	}

	pg = bm_page(f);
	found = 0;
	s = btnode_search(pg, key, klen, &found);
	if (!found) {
		rc = XTC_E_NOTFOUND;
		bm_unlatch(f);
		bm_unfix(bm, f, 0);
		f = NULL;
		goto out;
	}
	(void)btnode_remove(pg, s);
	bm_unlatch(f);
	bm_unfix(bm, f, 1);
	f = NULL;
	/* Leaves may underflow without merging in this version. */

out:
	if (f != NULL) {
		bm_unlatch(f);
		bm_unfix(bm, f, 0);
	}
	(void)pthread_mutex_unlock(&bt->struct_mu);
	return rc;
}

/* Shared-latched descent for the cursor.  If start == NULL, take the
 * leftmost child at every level; otherwise route toward `start`.  On
 * success returns the leaf fixed + shared-latched in *out. */
static int
cursor_descend(bt_t *bt, const void *start, uint16_t klen, bm_frame_t **out)
{
	bm_t *bm = bt->bm;
	bm_pid_t pid;
	bm_frame_t *f;
	void *pg;
	int rc;

retry:
	pid = atomic_load(&bt->root_pid);
	rc = bm_fix_pid(bm, pid, &f);
	if (rc != XTC_OK)
		return rc;
	bm_latch_shared(f);
	if (pid != atomic_load(&bt->root_pid)) {
		bm_unlatch(f);
		bm_unfix(bm, f, 0);
		goto retry;
	}
	pg = bm_page(f);

	while (!btnode_is_leaf(pg)) {
		bm_pid_t child;
		bm_frame_t *cf;

		if (start == NULL)
			child = child_pid_at(pg, 0);     /* leftmost child */
		else
			child = child_for_key(pg, start, klen);
		rc = bm_fix_pid(bm, child, &cf);
		if (rc != XTC_OK) {
			bm_unlatch(f);
			bm_unfix(bm, f, 0);
			return rc;
		}
		bm_latch_shared(cf);
		bm_unlatch(f);
		bm_unfix(bm, f, 0);
		f = cf;
		pg = bm_page(f);
	}
	*out = f;
	return XTC_OK;
}

int
bt_cursor_open(bt_t *bt, const void *start, uint16_t klen, bt_cursor_t **out)
{
	bt_cursor_t *c;
	bm_frame_t *leaf = NULL;
	int rc;

	if (bt == NULL || out == NULL)
		return XTC_E_INVAL;

	c = calloc(1, sizeof *c);
	if (c == NULL)
		return XTC_E_NOMEM;
	c->bt = bt;

	rc = cursor_descend(bt, start, klen, &leaf);
	if (rc != XTC_OK) {
		free(c);
		return rc;
	}
	c->leaf = leaf;
	if (start == NULL)
		c->slot = 0;
	else {
		int found = 0;

		/* First slot with key >= start. */
		c->slot = btnode_search(bm_page(leaf), start, klen, &found);
	}
	*out = c;
	return XTC_OK;
}

int
bt_cursor_next(bt_cursor_t *c, const void **key, uint16_t *klen,
    const void **val, uint16_t *vlen)
{
	bm_t *bm;

	if (c == NULL)
		return XTC_E_INVAL;
	bm = c->bt->bm;

	for (;;) {
		void *pg;

		if (c->done || c->leaf == NULL) {
			c->done = 1;
			return XTC_E_NOTFOUND;
		}
		pg = bm_page(c->leaf);

		if (c->slot < (int)btnode_count(pg)) {
			const void *vp = NULL;
			uint16_t vl = 0;

			if (btnode_full_key(pg, c->slot, c->keybuf,
			    sizeof c->keybuf, &c->keylen) != 0) {
				/* Key longer than our buffer: should not
				 * happen for sane keys. */
				return XTC_E_INTERNAL;
			}
			(void)btnode_get(pg, c->slot, NULL, NULL, &vp, &vl);
			c->slot++;
			if (key != NULL)
				*key = c->keybuf;
			if (klen != NULL)
				*klen = c->keylen;
			if (val != NULL)
				*val = vp;
			if (vlen != NULL)
				*vlen = vl;
			return XTC_OK;
		}

		/* Current leaf exhausted: follow the right-sibling chain. */
		{
			uint32_t rs = btnode_right_sibling(pg);
			bm_frame_t *nf;

			bm_unlatch(c->leaf);
			bm_unfix(bm, c->leaf, 0);
			c->leaf = NULL;
			if (rs == 0) {
				c->done = 1;
				return XTC_E_NOTFOUND;
			}
			if (bm_fix_pid(bm, (bm_pid_t)rs, &nf) != XTC_OK) {
				c->done = 1;
				return XTC_E_INTERNAL;
			}
			bm_latch_shared(nf);
			c->leaf = nf;
			c->slot = 0;
		}
	}
}

void
bt_cursor_close(bt_cursor_t *c)
{
	if (c == NULL)
		return;
	if (c->leaf != NULL) {
		bm_unlatch(c->leaf);
		bm_unfix(c->bt->bm, c->leaf, 0);
		c->leaf = NULL;
	}
	free(c);
}

void
bt_get_stats(bt_t *bt, bt_stats_t *out)
{
	if (bt == NULL || out == NULL)
		return;
	out->inserts = atomic_load(&bt->st_inserts);
	out->lookups = atomic_load(&bt->st_lookups);
	out->splits = atomic_load(&bt->st_splits);
	out->height = atomic_load(&bt->st_height);
}
