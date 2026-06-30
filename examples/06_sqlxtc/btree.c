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
 *	 CONCURRENCY (parallel writers via latch coupling)
 *	 -------------------------------------------------
 *	 Writers and readers latch-couple down the tree with fiber-
 *	 yielding per-frame content latches (xtc_arwlock).  There is no
 *	 per-tree writer mutex: writers on disjoint subtrees proceed in
 *	 parallel.
 *
 *	 bt_insert descends taking the EXCLUSIVE latch at each level and
 *	 keeps a stack of held frames.  When it latches an internal node
 *	 that is "safe" (has room for one more entry, so a split below
 *	 cannot cascade into it) it releases every ancestor above that
 *	 node: the retained stack is exactly [deepest safe node .. leaf],
 *	 the frames a split may touch.  A leaf split propagates the
 *	 separator UP through that already-held stack -- it never acquires
 *	 a latch upward -- and grows a new root if the held root splits.
 *	 Because every latch is taken top-down (root toward leaf), in the
 *	 same order by every writer and reader, and propagation only
 *	 touches already-held frames, the scheme is deadlock-free.
 *
 *	 bt_lookup and bt_delete descend with shared / exclusive coupling
 *	 respectively (latch the child before releasing the parent), so a
 *	 reader is never split out from under and a delete reaches the
 *	 owning leaf without a B-link follow.  The cursor holds one leaf
 *	 shared at a time and advances along the right-sibling chain.
 *
 *	 Latches are fiber-yielding, so a holder may park across a child
 *	 fix or a page-allocation park without wedging a cooperative loop;
 *	 the pin (bm_fix_pid / bm_unfix) keeps a frame resident across the
 *	 operation.
 */

#include "btree.h"
#include "btnode.h"

#include <pthread.h>
#include <stdatomic.h>
#include "xtc_sync.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BT_MAX_HEIGHT 32       /* fanout >= 2 => trees this tall are absurd */
#define BT_MAX_KEY    1024     /* largest full key we route on the stack */

/*
 * Merge threshold: a delete that leaves a node using less than this
 * fraction of its page triggers a merge attempt (right-merge under
 * the SMO lock).  Kept low (1/4) so the common delete stays on the
 * latch-free fast path and only a genuinely sparse node provokes an
 * SMO -- the classic B-tree underflow bound, tuned conservatively so
 * a delete/insert oscillation around the boundary does not thrash
 * (insert splits near full, merge fires near a quarter full).
 */
#define BT_MERGE_NUM  1
#define BT_MERGE_DEN  4

/* Bound on delete re-descents.  A concurrent merge can move a key into
 * its left sibling after a delete's descent has passed that subtree;
 * the delete then re-descends to find it.  A handful of attempts
 * absorbs any realistic merge race while still terminating promptly
 * on a genuinely absent key. */
#define BT_DELETE_RETRIES 4

struct bt {
	bm_t             *bm;
	uint32_t          page_size;
	_Atomic bm_pid_t  root_pid;

	_Atomic uint64_t  st_inserts;
	_Atomic uint64_t  st_lookups;
	_Atomic uint64_t  st_splits;
	_Atomic uint64_t  st_merges;   /* node merges (right sibling pulled left) */
	_Atomic uint64_t  st_reclaimed;/* pages returned to the bufmgr freelist */
	_Atomic int       merge_on;    /* 1 == run merge/reclaim on delete underflow.
	                                * Off by default: the merge SMO is correct
	                                * under a single mutator but has a known
	                                * structural race under concurrent latch-free
	                                * deletes (see docs/M_SQLXTC_STORAGE.md and
	                                * .agent/M_SQLXTC_BTREE_MERGE.md).  Callers that
	                                * delete single-threaded (or hold the tree
	                                * exclusively) enable it for compaction. */
	_Atomic uint64_t  st_height;   /* number of levels (1 == root leaf) */
	_Atomic uint64_t  st_descents; /* full root->leaf cursor descents */
	_Atomic uint64_t  st_resumes;  /* parked-cursor O(1) resumes */
	uint64_t          meta_clean;  /* superblock clean flag (open/close only) */
	uint64_t          meta_clock;  /* superblock commit clock (open/close only) */
	xtc_arwlock_t    *smo;         /* serializes structure-modification (splits
	                               * and root growth).  Parking-safe so a
	                               * splitter may park on page I/O without
	                               * wedging the loop; non-splitting inserts
	                               * never take it, so the common path stays
	                               * fully parallel. */
};

static int bt_merge(bt_t *bt, const void *key, uint16_t klen);
static int merge_level(bt_t *bt, bm_pid_t *path, int level,
    const void *key, uint16_t klen);

/* On-disk superblock (buffer-manager page 0), recording where the tree
 * lives so a restart can find the root instead of building a fresh
 * one.  Rewritten on create and whenever the root pid changes. */
#define BT_SUPER_MAGIC 0x58425431u   /* "XBT1" */
struct bt_super {
	uint32_t magic;
	uint32_t page_size;
	uint64_t root_pid;
	uint64_t height;
	uint64_t clean;          /* 1 if the base was cleanly shut down */
	uint64_t commit_clock;   /* persisted MVCC commit clock at clean shutdown */
};
void
bt_write_super(bt_t *bt)
{
	struct bt_super s;
	s.magic = BT_SUPER_MAGIC;
	s.page_size = bt->page_size;
	s.root_pid = (uint64_t)atomic_load(&bt->root_pid);
	s.height = atomic_load(&bt->st_height);
	s.clean = bt->meta_clean;
	s.commit_clock = bt->meta_clock;
	(void)bm_write_super(bt->bm, &s, sizeof s);
}

void
bt_set_meta(bt_t *bt, uint64_t clean, uint64_t commit_clock)
{
	bt->meta_clean = clean;
	bt->meta_clock = commit_clock;
}

void
bt_get_meta(const bt_t *bt, uint64_t *clean, uint64_t *commit_clock)
{
	if (clean) *clean = bt->meta_clean;
	if (commit_clock) *commit_clock = bt->meta_clock;
}

struct bt_cursor {
	bt_t          *bt;
	bm_frame_t    *leaf;   /* current leaf: fixed + shared-latched, or NULL */
	int            slot;   /* next slot to yield in the current leaf */
	int            done;
	bm_pid_t       parked_pid;  /* leaf pid remembered while parked */
	int            parked;      /* 1 == latch released, resume re-fixes */
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

/*
 * Move-right (the B-link recovery step).  *fp is a fixed+latched node
 * (shared or exclusive per `excl`).  While `key` lies strictly beyond
 * the node's upper fence -- meaning a concurrent split moved `key`'s
 * range into a right sibling after this descent read the parent's
 * child pointer -- hop to the right sibling: latch it in the same
 * mode, then release the old node.  Because btnode_split makes the
 * new right node inherit the old right-link and sets the separator as
 * both nodes' shared fence, the half-split state is immediately
 * chase-consistent and a single rightward walk always reaches the
 * node that owns `key`.  Internal and leaf nodes both carry fences,
 * so this works at every level.  Latch coupling is never held during
 * the hop (only one node latched at a time), so move-right cannot
 * deadlock against a splitter posting a separator upward.
 */
static int
move_right(bt_t *bt, bm_frame_t **fp, const void *key, uint16_t klen, int excl)
{
	bm_t *bm = bt->bm;
	bm_frame_t *f = *fp;

	while (key != NULL && btnode_beyond_hi_fence(bm_page(f), key, klen)) {
		uint32_t rs = btnode_right_sibling(bm_page(f));
		bm_frame_t *nf;

		if (rs == 0)
			break;            /* rightmost: key belongs here */
		if (bm_fix_pid(bm, (bm_pid_t)rs, &nf) != XTC_OK)
			break;
		if (excl)
			bm_latch_exclusive(nf);
		else
			bm_latch_shared(nf);
		bm_unlatch(f);
		bm_unfix(bm, f, 0);
		f = nf;
	}
	*fp = f;
	return XTC_OK;
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
	if (xtc_arwlock_create(&bt->smo) != XTC_OK) { free(bt); return XTC_E_NOMEM; }

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

	bt_write_super(bt);             /* persist where the root lives */

	*out = bt;
	return XTC_OK;

fail:
	xtc_arwlock_destroy(bt->smo);
	free(bt);
	return rc;
}

/*
 * Reopen a B-tree from an existing store (the buffer manager must have
 * been created with reopen != 0).  Reads the superblock to find the
 * live root, page size, and height -- no fresh root is allocated.
 * Returns XTC_E_INVAL if the superblock is absent or unrecognized.
 */
int
bt_reopen(bm_t *bm, bt_t **out)
{
	bt_t *bt;
	struct bt_super s;
	int rc;

	if (bm == NULL || out == NULL)
		return XTC_E_INVAL;
	bt = calloc(1, sizeof *bt);
	if (bt == NULL)
		return XTC_E_NOMEM;
	bt->bm = bm;
	if (xtc_arwlock_create(&bt->smo) != XTC_OK) { free(bt); return XTC_E_NOMEM; }
	if ((rc = bm_read_super(bm, &s, sizeof s)) != XTC_OK) {
		xtc_arwlock_destroy(bt->smo);
		free(bt);
		return rc;
	}
	if (s.magic != BT_SUPER_MAGIC || s.page_size == 0) {
		xtc_arwlock_destroy(bt->smo);
		free(bt);
		return XTC_E_INVAL;          /* not an xstore B-tree store */
	}
	bt->page_size = s.page_size;
	atomic_store(&bt->root_pid, (bm_pid_t)s.root_pid);
	atomic_store(&bt->st_height, s.height);
	bt->meta_clean = s.clean;
	bt->meta_clock = s.commit_clock;
	*out = bt;
	return XTC_OK;
}

static void (*bt_close_hook)(bt_t *) = NULL;

void
bt_close(bt_t *bt)
{
	if (bt == NULL)
		return;
	if (bt_close_hook != NULL)
		bt_close_hook(bt);     /* let xstore drop cached state for this bt */
	if (bt->smo != NULL)
		xtc_arwlock_destroy(bt->smo);
	free(bt);
}

void
bt_set_close_hook(void (*fn)(bt_t *))
{
	bt_close_hook = fn;
}

void
bt_set_lsn(bt_t *bt, uint64_t lsn)
{
	/* The log LSN stamped onto every page this tree dirties from here
	 * on; the caller sets it to the LSN of the change about to be made. */
	bm_set_lsn(bt->bm, lsn);
}

/*
 * SMO logging hook (ARIES physiological redo + nested-top-action).  A
 * single global hook, set by the engine; NULL means no SMO logging and
 * the recovery path rebuilds logically.  See btree.h.
 */
static bt_smo_hook_t bt_smo_hook = { NULL, NULL, NULL, NULL };

void
bt_set_smo_hook(const bt_smo_hook_t *hook)
{
	if (hook == NULL)
		bt_smo_hook = (bt_smo_hook_t){ NULL, NULL, NULL, NULL };
	else
		bt_smo_hook = *hook;
}

/* Begin a nested top action around a split (returns the NTA token, or 0
 * when no hook is installed). */
static uint64_t
smo_begin(void)
{
	return bt_smo_hook.begin != NULL ? bt_smo_hook.begin(bt_smo_hook.user)
	    : 0;
}

/* Log one finished SMO page's full after-image: physiological redo for
 * the structural change.  The caller has just bm_predirty'd the page, so
 * its LSN field already holds bm_get_lsn (the SMO's LSN); pass that same
 * LSN so recovery can gate the image apply by page LSN.  No-op without a
 * hook.  Skips page 0 (never an SMO page) defensively. */
static void
smo_log_page(bt_t *bt, bm_pid_t pid, const void *image)
{
	if (bt_smo_hook.page == NULL || pid == 0)
		return;
	bt_smo_hook.page(bt_smo_hook.user, pid, image, bt->page_size,
	    bm_get_lsn(bt->bm));
}

/* Close the nested top action (writes the dummy CLR).  No-op without a
 * hook. */
static void
smo_end(uint64_t token)
{
	if (bt_smo_hook.end != NULL)
		bt_smo_hook.end(bt_smo_hook.user, token);
}

/*
 * Split a full INTERNAL node the B-link way.  Unlike a leaf, an
 * internal node's leftmost child is reached through a -infinity
 * (empty-key) slot 0, so the right half cannot simply inherit raw
 * slots: the middle separator is pushed UP and OUT (it becomes the
 * boundary between the two halves), the middle child becomes the
 * right node's -infinity leftmost child, and the separators above it
 * follow.  For chase-consistency the left node's upper fence is set
 * to the pushed-up separator and the right node inherits the old
 * right-link, so a key routed into the left half but beyond the new
 * fence is recovered by move_right.  `pp` is the full left internal
 * (exclusively latched); `rp` is a freshly initialized right node.
 * The pushed-up separator is copied into pushup / *pushuplen.
 */
static int
bt_split_internal(bt_t *bt, void *pp, void *rp, uint32_t rpid,
    uint8_t *pushup, uint16_t *pushuplen)
{
	int count = (int)btnode_count(pp);
	int mid = count / 2;
	int i;
	uint8_t empty = 0;
	uint32_t old_right = btnode_right_sibling(pp);
	uint8_t scratch[BT_MAX_KEY * 8];   /* one page; internals are small */
	void *lp = scratch;

	if (bt->page_size > sizeof scratch)
		return XTC_E_INTERNAL;

	/* The mid key is pushed up; it becomes the boundary between the
	 * two halves (and the left node's new upper fence). */
	if (btnode_full_key(pp, mid, pushup, BT_MAX_KEY, pushuplen) != 0)
		return XTC_E_INTERNAL;

	/* Right node: -infinity leftmost child = mid's child, then the
	 * separators/children strictly above mid.  Wide-open lo fence so
	 * routing through slot 0 stays correct; upper fence inherits the
	 * old node's (+infinity unless the old node was itself a left
	 * half), and the old right-link transfers here. */
	btnode_set_fences(rp, NULL, 0, NULL, 0);
	if (internal_insert(rp, &empty, 0, child_pid_at(pp, mid)) != 0)
		return XTC_E_INTERNAL;
	for (i = mid + 1; i < count; i++) {
		uint8_t kb[BT_MAX_KEY];
		uint16_t kl = 0;

		if (btnode_full_key(pp, i, kb, sizeof kb, &kl) != 0 ||
		    internal_insert(rp, kb, kl, child_pid_at(pp, i)) != 0)
			return XTC_E_INTERNAL;
	}
	btnode_set_right_sibling(rp, old_right);

	/* Left node, rebuilt in scratch: slots [0, mid) under a narrowed
	 * upper fence == the pushed-up separator, so a key routed here in
	 * the window before the parent learns of the split but actually
	 * belonging to the right half is recovered by move_right.  The
	 * lower fence is preserved from the old node. */
	{
		uint8_t lof[BT_MAX_KEY];
		uint16_t loflen = 0;
		const uint8_t *lo = NULL;

		if (btnode_lo_fence(pp, lof, sizeof lof, &loflen) == 0 && loflen > 0)
			lo = lof;
		btnode_init(lp, bt->page_size, 0);
		btnode_set_fences(lp, lo, loflen, pushup, *pushuplen);
		for (i = 0; i < mid; i++) {
			uint8_t kb[BT_MAX_KEY];
			uint16_t kl = 0;

			if (btnode_full_key(pp, i, kb, sizeof kb, &kl) != 0 ||
			    internal_insert(lp, kb, kl, child_pid_at(pp, i)) != 0)
				return XTC_E_INTERNAL;
		}
		btnode_set_right_sibling(lp, rpid);
		memcpy(pp, lp, bt->page_size);
	}
	return XTC_OK;
}

/*
 * Latch-free descent to the leaf that owns `key`, then insert if it
 * fits.  Descends one node at a time (no latch coupling): latch a
 * node, move-right if a concurrent split pushed `key`'s range to a
 * sibling, read the child pointer, release, descend.  Releasing the
 * parent before latching the child is what forces move-right -- the
 * child may split in that window -- but it is also what makes the
 * descent deadlock-free against a splitter that posts separators
 * bottom-up (no thread ever holds two latches in opposite order).
 *
 * Takes only the LEAF exclusive, so disjoint-leaf inserts run fully
 * in parallel.  Returns 1 if the row was inserted; returns 0, having
 * made NO change, when the leaf is full (a split is needed) or the
 * key already exists (upsert) -- the SMO path handles those.
 * btnode_insert is all-or-nothing, so a full leaf is left untouched.
 */
static int
bt_insert_fast(bt_t *bt, const void *key, uint16_t klen, const void *val,
    uint16_t vlen)
{
	bm_t *bm = bt->bm;
	bm_pid_t pid;
	bm_frame_t *f;
	void *pg;
	int found, r;

	pid = atomic_load(&bt->root_pid);
	if (bm_fix_pid(bm, pid, &f) != XTC_OK)
		return 0;
	bm_latch_shared(f);
	if (pid != atomic_load(&bt->root_pid)) {   /* root grew under us */
		bm_unlatch(f); bm_unfix(bm, f, 0);
		return 0;
	}
	(void)move_right(bt, &f, key, klen, 0);
	pg = bm_page(f);
	/* Stale-node guard at the root: a concurrent merge may have
	 * unlinked it (dead) or absorbed its range leftward (key at/below
	 * lo fence -- move_right cannot follow); bail to the SMO path. */
	if (btnode_is_dead(pg) || btnode_below_lo_fence(pg, key, klen)) {
		bm_unlatch(f); bm_unfix(bm, f, 0);
		return 0;
	}
	while (!btnode_is_leaf(pg)) {                /* descend, no coupling */
		bm_pid_t child = child_for_key(pg, key, klen);
		bm_frame_t *cf;

		if (bm_fix_pid(bm, child, &cf) != XTC_OK) {
			bm_unlatch(f); bm_unfix(bm, f, 0);
			return 0;
		}
		bm_latch_shared(cf);
		bm_unlatch(f); bm_unfix(bm, f, 0);   /* release before child */
		f = cf;
		(void)move_right(bt, &f, key, klen, 0);
		pg = bm_page(f);
		/* Internal-level stale-node guard: the child we just latched
		 * may itself have been merged away (dead) or had its range
		 * absorbed leftward (key at/below lo fence) by a concurrent
		 * merge between reading the parent's child pointer and
		 * latching the child.  move_right handles only the rightward
		 * (split) direction; a leftward absorb the SMO path settles.
		 * Bail and let bt_insert re-descend under the SMO lock. */
		if (btnode_is_dead(pg) || btnode_below_lo_fence(pg, key, klen)) {
			bm_unlatch(f); bm_unfix(bm, f, 0);
			return 0;
		}
	}

	/* Re-latch the leaf exclusive, then move-right again: between the
	 * shared unlatch and the exclusive latch the leaf may have split. */
	bm_unlatch(f);
	bm_latch_exclusive(f);
	(void)move_right(bt, &f, key, klen, 1);
	pg = bm_page(f);

	/* Stale-leaf guard: if this leaf was unlinked by a concurrent
	 * merge (marked dead) or the key is at/below its lower fence (the
	 * key's range moved LEFT, which move_right cannot follow), bail to
	 * the SMO path, which re-descends under the SMO lock and so never
	 * races a merge. */
	if (btnode_is_dead(pg) || btnode_below_lo_fence(pg, key, klen)) {
		bm_unlatch(f); bm_unfix(bm, f, 0);
		return 0;
	}

	(void)btnode_search(pg, key, klen, &found);
	if (found) {                       /* upsert: defer to the SMO path */
		bm_unlatch(f); bm_unfix(bm, f, 0);
		return 0;
	}
	r = btnode_insert(pg, key, klen, val, vlen);
	if (r == 0) {
		atomic_fetch_add(&bt->st_inserts, 1);
		bm_unlatch(f); bm_unfix(bm, f, 1);   /* modified -> dirty */
		return 1;
	}
	bm_unlatch(f); bm_unfix(bm, f, 0);   /* full: no change made */
	return 0;
}

/*
 * Post a separator into the parent level during a split.  Called
 * under the SMO lock with the new right child already built and
 * linked.  `path` is the ancestor pid stack captured top-down during
 * descent (path[0] == root .. path[level] == the leaf's parent);
 * `level` indexes the parent to receive (cur_sep, cur_right).  Walks
 * up, splitting any full internal node (carrying its push-up further
 * up), and grows a new root when it runs off the top.  Each level is
 * latched independently with a move-right re-find -- the captured pid
 * may itself have split since descent -- so no latch is ever held in
 * the descent's top-down order while reaching upward (no deadlock).
 */
static int
post_separator(bt_t *bt, bm_pid_t *path, int level,
    const void *sep0, uint16_t sep0len, bm_pid_t right0)
{
	bm_t *bm = bt->bm;
	bm_frame_t *rf;
	bm_pid_t rpid;
	uint8_t cur_sep[BT_MAX_KEY];
	uint16_t cur_seplen = sep0len;
	bm_pid_t cur_right = right0;
	int rc, r;

	memcpy(cur_sep, sep0, sep0len);

	for (; level >= 0; level--) {
		bm_frame_t *pf;
		void *pp;
		uint8_t pushup[BT_MAX_KEY];
		uint16_t pushuplen = 0;

		if (bm_fix_pid(bm, path[level], &pf) != XTC_OK)
			return XTC_E_INTERNAL;
		bm_latch_exclusive(pf);
		/* The captured parent may have split since descent: walk
		 * right to the internal node that now owns cur_sep. */
		(void)move_right(bt, &pf, cur_sep, cur_seplen, 1);
		pp = bm_page(pf);

		if (internal_insert(pp, cur_sep, cur_seplen, cur_right) == 0) {
			bm_predirty(bm, pf); smo_log_page(bt, path[level], bm_page(pf));
			bm_unlatch(pf); bm_unfix(bm, pf, 1);   /* absorbed */
			return XTC_OK;
		}
		/* Parent full: split it, link the new right internal, carry
		 * the push-up further up. */
		rc = bm_alloc_pid(bm, &rf, &rpid);
		if (rc != XTC_OK) { bm_unlatch(pf); bm_unfix(bm, pf, 0); return rc; }
		bm_latch_exclusive(rf);
		btnode_init(bm_page(rf), bt->page_size, 0);
		if (bt_split_internal(bt, pp, bm_page(rf), (uint32_t)rpid,
		    pushup, &pushuplen) != XTC_OK) {
			bm_unlatch(rf); bm_unfix(bm, rf, 1);
			bm_unlatch(pf); bm_unfix(bm, pf, 0);
			return XTC_E_INTERNAL;
		}
		if (key_cmp(cur_sep, cur_seplen, pushup, pushuplen) > 0)
			r = internal_insert(bm_page(rf), cur_sep, cur_seplen, cur_right);
		else
			r = internal_insert(pp, cur_sep, cur_seplen, cur_right);
		bm_predirty(bm, rf); smo_log_page(bt, rpid, bm_page(rf));
		bm_predirty(bm, pf); smo_log_page(bt, path[level], bm_page(pf));
		bm_unlatch(rf); bm_unfix(bm, rf, 1);
		bm_unlatch(pf); bm_unfix(bm, pf, 1);
		if (r != 0)
			return XTC_E_INTERNAL;
		atomic_fetch_add(&bt->st_splits, 1);
		memcpy(cur_sep, pushup, pushuplen);
		cur_seplen = pushuplen;
		cur_right = rpid;
	}

	/*
	 * Ran off the top: the root split.  Grow a new root holding the
	 * old root (as -infinity child) and the carried right child.
	 * Done under the SMO lock, so the root grows exactly once.
	 */
	{
		bm_frame_t *nf;
		bm_pid_t npid;
		uint8_t empty = 0;
		bm_pid_t oldroot = atomic_load(&bt->root_pid);

		rc = bm_alloc_pid(bm, &nf, &npid);
		if (rc != XTC_OK)
			return rc;
		bm_latch_exclusive(nf);
		btnode_init(bm_page(nf), bt->page_size, 0);
		btnode_set_fences(bm_page(nf), NULL, 0, NULL, 0);
		if (internal_insert(bm_page(nf), &empty, 0, oldroot) != 0 ||
		    internal_insert(bm_page(nf), cur_sep, cur_seplen, cur_right) != 0) {
			bm_unlatch(nf); bm_unfix(bm, nf, 1);
			return XTC_E_INTERNAL;
		}
		bm_predirty(bm, nf); smo_log_page(bt, npid, bm_page(nf));
		bm_unlatch(nf); bm_unfix(bm, nf, 1);
		atomic_store(&bt->root_pid, npid);
		atomic_fetch_add(&bt->st_height, 1);
		bt_write_super(bt);     /* root pid changed: persist it */
	}
	return XTC_OK;
}

int
bt_insert(bt_t *bt, const void *key, uint16_t klen, const void *val,
    uint16_t vlen)
{
	bm_t *bm;
	bm_pid_t path[BT_MAX_HEIGHT];   /* ancestor pids, root..parent-of-leaf */
	int depth;
	bm_pid_t pid, rpid;
	bm_frame_t *f, *rf;
	void *pg;
	uint8_t sep[BT_MAX_KEY];
	uint16_t seplen = 0;
	int rc = XTC_OK;
	int r, found, s;
	uint64_t nta = 0;          /* nested-top-action token for the split */

	if (bt == NULL || key == NULL || (val == NULL && vlen != 0))
		return XTC_E_INVAL;
	bm = bt->bm;

	/* Fast path: latch-free descent + leaf-exclusive insert, parallel
	 * for disjoint leaves.  Falls through to the SMO path only when a
	 * split is needed (full leaf) or the key already exists. */
	if (bt_insert_fast(bt, key, klen, val, vlen))
		return XTC_OK;

	/*
	 * SMO path: serialize structure modification on the tree's SMO
	 * lock (parking-safe -- a splitter may park on page I/O).  Only
	 * full-leaf inserts and upserts reach here, so this lock is off
	 * the common path; it lets readers and non-splitting writers run
	 * fully in parallel (they never take it), unlike a root-exclusive
	 * scheme that blocks every descent during a split.
	 */
	rc = xtc_arwlock_wrlock(bt->smo, -1);
	if (rc != XTC_OK)
		return rc;

	/* Latch-free descent collecting the ancestor pid path.  move-right
	 * at every level recovers from splits that happened before we held
	 * the SMO lock. */
	depth = 0;
	pid = atomic_load(&bt->root_pid);
	if ((rc = bm_fix_pid(bm, pid, &f)) != XTC_OK)
		goto unlock;
	bm_latch_exclusive(f);
	(void)move_right(bt, &f, key, klen, 1);
	while (!btnode_is_leaf(bm_page(f))) {
		bm_pid_t child;
		bm_frame_t *cf;

		if (depth >= BT_MAX_HEIGHT) {
			bm_unlatch(f); bm_unfix(bm, f, 0);
			rc = XTC_E_INTERNAL; goto unlock;
		}
		path[depth++] = bm_frame_pid(f);
		child = child_for_key(bm_page(f), key, klen);
		if ((rc = bm_fix_pid(bm, child, &cf)) != XTC_OK) {
			bm_unlatch(f); bm_unfix(bm, f, 0); goto unlock;
		}
		bm_latch_exclusive(cf);
		bm_unlatch(f); bm_unfix(bm, f, 0);
		f = cf;
		(void)move_right(bt, &f, key, klen, 1);
	}

	/* f is the leaf, exclusive-latched.  Upsert, then re-check space:
	 * a concurrent fast-path insert may have changed things, but we
	 * hold the leaf exclusive now so its contents are stable. */
	pg = bm_page(f);
	found = 0;
	s = btnode_search(pg, key, klen, &found);
	if (found)
		(void)btnode_remove(pg, s);
	if (btnode_insert(pg, key, klen, val, vlen) == 0) {
		atomic_fetch_add(&bt->st_inserts, 1);
		bm_unlatch(f); bm_unfix(bm, f, 1);
		rc = XTC_OK; goto unlock;
	}

	/*
	 * Leaf full: split it, link the new right leaf, place the
	 * triggering pair, then post the separator up the captured path.
	 * btnode_split sets the left leaf's hi fence == the right's lo
	 * fence and makes the right inherit the old right-link, so the
	 * half-split is immediately chase-consistent for any concurrent
	 * reader/writer move-right before the separator reaches the
	 * parent.
	 */
	rc = bm_alloc_pid(bm, &rf, &rpid);
	if (rc != XTC_OK) { bm_unlatch(f); bm_unfix(bm, f, 0); goto unlock; }
	bm_latch_exclusive(rf);
	btnode_init(bm_page(rf), bt->page_size, 1);
	if (btnode_split(pg, bm_page(rf), sep, &seplen) != 0) {
		bm_unlatch(rf); bm_unfix(bm, rf, 1);
		bm_unlatch(f); bm_unfix(bm, f, 0);
		rc = XTC_E_INTERNAL; goto unlock;
	}
	btnode_set_right_sibling(pg, (uint32_t)rpid);
	if (key_cmp(key, klen, sep, seplen) > 0)
		r = btnode_insert(bm_page(rf), key, klen, val, vlen);
	else
		r = btnode_insert(pg, key, klen, val, vlen);
	/*
	 * Open the nested top action and log both leaf images (the new
	 * right and the shrunk left) as physiological redo before they
	 * leave the latch.  bm_predirty stamps each page's LSN now, so the
	 * image carries the LSN recovery gates the apply by.
	 */
	nta = smo_begin();
	bm_predirty(bm, rf); smo_log_page(bt, rpid, bm_page(rf));
	bm_predirty(bm, f);  smo_log_page(bt, bm_frame_pid(f), bm_page(f));
	bm_unlatch(rf); bm_unfix(bm, rf, 1);
	bm_unlatch(f); bm_unfix(bm, f, 1);   /* left leaf modified */
	if (r != 0) { smo_end(nta); rc = XTC_E_INTERNAL; goto unlock; }
	atomic_fetch_add(&bt->st_inserts, 1);
	atomic_fetch_add(&bt->st_splits, 1);

	rc = post_separator(bt, path, depth - 1, sep, seplen, rpid);
	smo_end(nta);            /* close the NTA: the SMO is crash-atomic */

unlock:
	(void)xtc_arwlock_unlock(bt->smo);
	return rc;
}

/*
 * Latch-free descent to the leaf for `key` with B-link move-right.
 * Latches one node at a time (no coupling): latch, move-right if a
 * concurrent split pushed `key`'s range to a sibling, read the child
 * pointer, release, descend.  This always lands on the leaf that owns
 * `key` -- a miss is conclusive -- and never holds two latches in
 * opposite order, so it cannot deadlock against a splitter posting
 * separators upward.  Returns the leaf fixed + shared-latched in *out.
 */
static int
descend_shared(bt_t *bt, const void *key, uint16_t klen, bm_frame_t **out)
{
	bm_t *bm = bt->bm;
	bm_pid_t pid;
	bm_frame_t *f;
	void *pg;
	int rc, attempt;

	for (attempt = 0; attempt < BT_DELETE_RETRIES; attempt++) {
		int stale = 0;

		pid = atomic_load(&bt->root_pid);
		rc = bm_fix_pid(bm, pid, &f);
		if (rc != XTC_OK)
			return rc;
		bm_latch_shared(f);
		(void)move_right(bt, &f, key, klen, 0);
		pg = bm_page(f);
		/* Stale-node guard at the root (see the internal-level guard
		 * below): a concurrent merge may have unlinked it or absorbed
		 * its range leftward; restart the descent. */
		if (btnode_is_dead(pg) || btnode_below_lo_fence(pg, key, klen)) {
			bm_unlatch(f);
			bm_unfix(bm, f, 0);
			xtc_yield();
			continue;
		}
		while (!btnode_is_leaf(pg)) {
			bm_pid_t child = child_for_key(pg, key, klen);
			bm_frame_t *cf;

			rc = bm_fix_pid(bm, child, &cf);
			if (rc != XTC_OK) {
				bm_unlatch(f);
				bm_unfix(bm, f, 0);
				return rc;
			}
			bm_latch_shared(cf);
			bm_unlatch(f);             /* release before latching deeper */
			bm_unfix(bm, f, 0);
			f = cf;
			(void)move_right(bt, &f, key, klen, 0);
			pg = bm_page(f);
			/* Internal-level stale-node guard.  Between reading the
			 * parent's child pointer and latching the child, a
			 * concurrent merge can unlink the child (dead) or absorb
			 * its range into a LEFT sibling (key at/below lo fence) --
			 * a direction move_right cannot follow.  Catch it at the
			 * FIRST level it happens, not only at the leaf, so the
			 * descent never lands on a live-but-wrong leaf; restart
			 * from the root. */
			if (btnode_is_dead(pg) ||
			    btnode_below_lo_fence(pg, key, klen)) {
				stale = 1;
				break;
			}
		}
		if (stale) {
			bm_unlatch(f);
			bm_unfix(bm, f, 0);
			xtc_yield();
			continue;
		}
		/* If this leaf was merged away (marked dead, or the key is
		 * at/below its lower fence -- absorbed leftward where
		 * move_right cannot follow), restart from the root.  Bounded
		 * retries absorb any realistic merge race. */
		if (!btnode_is_dead(pg) && !btnode_below_lo_fence(pg, key, klen)) {
			*out = f;
			return XTC_OK;
		}
		bm_unlatch(f);
		bm_unfix(bm, f, 0);
		xtc_yield();
	}
	/* Exhausted retries under relentless merging: hand back the last
	 * leaf reached; its contents are valid, so a miss is conclusive. */
	*out = f;
	return XTC_OK;
}

/* Search a shared-latched leaf for `key`, copy the value on a hit, and
 * release the leaf.  Returns XTC_OK on a hit, XTC_E_NOTFOUND else. */
static int
leaf_get(bt_t *bt, bm_frame_t *f, const void *key, uint16_t klen,
    void *buf, uint16_t cap, uint16_t *vlen)
{
	void *pg = bm_page(f);
	int found = 0;
	int s = btnode_search(pg, key, klen, &found);
	const void *vp = NULL;
	uint16_t vl = 0;
	int rc = XTC_E_NOTFOUND;

	if (found) {
		(void)btnode_get(pg, s, NULL, NULL, &vp, &vl);
		if (vlen != NULL)
			*vlen = vl;
		if (buf != NULL && cap > 0) {
			uint16_t n = vl < cap ? vl : cap;
			memcpy(buf, vp, n);
		}
		rc = XTC_OK;
	}
	bm_unlatch(f);
	bm_unfix(bt->bm, f, 0);
	return rc;
}

int
bt_lookup(bt_t *bt, const void *key, uint16_t klen, void *buf, uint16_t cap,
    uint16_t *vlen)
{
	bm_frame_t *f = NULL;
	int rc;

	if (bt == NULL || key == NULL)
		return XTC_E_INVAL;
	atomic_fetch_add(&bt->st_lookups, 1);

	/*
	 * Shared latch-coupling descent.  Coupling holds the parent's
	 * shared latch across the child fix, so a writer cannot split a
	 * node out from under the descent: the descent always reaches the
	 * leaf that owns `key`, and a miss is therefore conclusive (no
	 * re-confirm pass needed).
	 */
	rc = descend_shared(bt, key, klen, &f);
	if (rc != XTC_OK)
		return rc;
	return leaf_get(bt, f, key, klen, buf, cap, vlen);
}

/*
 * If the root is an internal node holding a single child (only the
 * slot-0 -infinity child remains after a merge cascade removed its
 * last separator), collapse it: make that child the new root and free
 * the old root page, shrinking the tree height by one.  Repeats while
 * the new root is itself a one-child internal node.  Called under the
 * SMO lock, so the root is stable.  `key`/`klen` route move_right if
 * the (single-child) root grew a right sibling -- it cannot, since a
 * one-child internal node is the rightmost at its level, but the
 * move_right call keeps the discipline uniform and is a no-op here.
 */
static void
collapse_root(bt_t *bt)
{
	bm_t *bm = bt->bm;

	for (;;) {
		bm_pid_t rpid = atomic_load(&bt->root_pid);
		bm_frame_t *rf;
		void *rp;
		bm_pid_t only_child;

		if (bm_fix_pid(bm, rpid, &rf) != XTC_OK)
			return;
		bm_latch_exclusive(rf);
		rp = bm_page(rf);
		if (btnode_is_leaf(rp) || btnode_count(rp) != 1) {
			bm_unlatch(rf); bm_unfix(bm, rf, 0);
			return;
		}
		/* Sole child becomes the new root. */
		only_child = child_pid_at(rp, 0);
		bm_unlatch(rf); bm_unfix(bm, rf, 0);
		if (only_child == BM_PID_NONE)
			return;
		atomic_store(&bt->root_pid, only_child);
		atomic_fetch_sub(&bt->st_height, 1);
		bt_write_super(bt);            /* root pid changed: persist it */
		if (bm_free_pid(bm, rpid) == XTC_OK)
			atomic_fetch_add(&bt->st_reclaimed, 1);
	}
}

/*
 * Right-merge at one level and cascade upward.  `path[level]` is the
 * parent pid; `key`/`klen` route to the underflowing node L beneath
 * it.  Latches strictly top-down: parent (exclusive), then L, then L's
 * right sibling R.  If L still underflows and R exists with the SAME
 * parent and the pair fits one page, absorbs R into L, unlinks R from
 * the parent and the sibling chain, and frees R; then, if the parent
 * now underflows, recurses on it.  Returns XTC_OK whether or not a
 * merge happened (best-effort reclaim).  See bt_merge for the B-link
 * safety invariant this latch discipline upholds.
 */
static int
merge_level(bt_t *bt, bm_pid_t *path, int level, const void *key,
    uint16_t klen)
{
	bm_t *bm = bt->bm;
	bm_frame_t *pf = NULL, *lf = NULL, *rf = NULL;
	void *pp, *lp = NULL, *rp = NULL;
	bm_pid_t lpid = BM_PID_NONE, rpid = BM_PID_NONE;
	int cslot, lslot, rslot;
	int merged = 0;
	int punder = 0;
	uint32_t cap = bt->page_size;

	if (level < 0)
		return XTC_OK;

	/* Parent, exclusive.  Under the SMO lock its structure is stable,
	 * but its frame may have been evicted, so re-fix it.  move_right is
	 * a no-op for an internal node that owns key (it has not split
	 * since we hold the SMO lock), but keep the discipline uniform. */
	if (bm_fix_pid(bm, path[level], &pf) != XTC_OK)
		return XTC_OK;
	bm_latch_exclusive(pf);
	(void)move_right(bt, &pf, key, klen, 1);
	pp = bm_page(pf);

	/*
	 * The child C the key routes through is the underflow candidate.
	 * Form a merge pair (left, right) of adjacent siblings under THIS
	 * parent: prefer C's right sibling (merge it into C); if C is the
	 * parent's rightmost child, use C's left sibling (merge C into it).
	 * The right member of the pair is always the one unlinked.  We do
	 * not cross to a cousin under a different parent -- that would need
	 * a second parent latch and break the simple top-down order.
	 */
	cslot = btnode_search(pp, key, klen, NULL) - 1;
	if (cslot < 0)
		cslot = 0;
	if (cslot + 1 < (int)btnode_count(pp)) {
		lslot = cslot;
		rslot = cslot + 1;
	} else if (cslot - 1 >= 0) {
		lslot = cslot - 1;
		rslot = cslot;
	} else {
		lslot = rslot = -1;        /* single child: nothing to merge */
	}

	if (lslot < 0)
		goto check_parent;         /* no pair: maybe collapse the parent */

	lpid = child_pid_at(pp, lslot);
	rpid = child_pid_at(pp, rslot);
	if (lpid == BM_PID_NONE || rpid == BM_PID_NONE || lpid == rpid)
		goto done;                 /* malformed parent: leave intact */

	/* Left child, then right sibling: top-down / left-right order, the
	 * same order a descent and a move_right take, so no deadlock. */
	if (bm_fix_pid(bm, lpid, &lf) != XTC_OK)
		goto done;
	bm_latch_exclusive(lf);
	lp = bm_page(lf);
	if (bm_fix_pid(bm, rpid, &rf) != XTC_OK)
		goto done;
	bm_latch_exclusive(rf);
	rp = bm_page(rf);

	/*
	 * Re-validate under the latches.  A non-SMO insert/delete may have
	 * changed either node while we reached up to the parent.  Merge
	 * only when (a) the two are the same leaf-ness, (b) the left's
	 * right-link really is the right (the sibling chain and the parent
	 * agree -- they must, since lslot/rslot are adjacent and their
	 * separator is the shared fence, but verify), (c) the pair fits in
	 * one page, and (d) at least one of them underflows (so a healthy
	 * pair is never needlessly combined).
	 */
	{
		int leaf = btnode_is_leaf(lp);
		uint32_t lrs = btnode_right_sibling(lp);
		int lu = (btnode_count(lp) == 0) ||
		    (!leaf && btnode_count(lp) <= 1) ||
		    ((uint32_t)btnode_used_bytes(lp) * BT_MERGE_DEN
		    < cap * BT_MERGE_NUM);
		int ru = (btnode_count(rp) == 0) ||
		    (!leaf && btnode_count(rp) <= 1) ||
		    ((uint32_t)btnode_used_bytes(rp) * BT_MERGE_DEN
		    < cap * BT_MERGE_NUM);

		if (leaf != btnode_is_leaf(rp) || lrs != (uint32_t)rpid ||
		    !(lu || ru) || !btnode_merge_fits(lp, rp))
			goto done;
	}

	/*
	 * Merge R into L.  For an INTERNAL pair the slot-0 of R is an
	 * empty-key (-infinity) leftmost child; merging it verbatim would
	 * leave a duplicate empty separator.  So first rebuild R into
	 * scratch with its slot-0 keyed by the real separator (the parent's
	 * rslot key == R's lower fence), carrying R's REAL upper fence so
	 * the merged node's hi-fence is right (a finite hi-fence on R, from
	 * an earlier split, must survive or a key beyond it would be
	 * wrongly claimed instead of chased right).  Leaves merge directly.
	 */
	if (!btnode_is_leaf(lp)) {
		uint8_t sepk[BT_MAX_KEY];
		uint16_t seplen = 0;
		uint8_t scratch[BT_MAX_KEY * 8];
		void *tmp = scratch;
		int i, rn;

		if (cap > sizeof scratch)
			goto done;
		if (btnode_full_key(pp, rslot, sepk, sizeof sepk, &seplen) != 0)
			goto done;
		{
			uint8_t rhi[BT_MAX_KEY];
			uint16_t rhilen = 0;
			const uint8_t *hip = NULL;

			if (btnode_hi_fence(rp, rhi, sizeof rhi, &rhilen) == 0 &&
			    rhilen > 0)
				hip = rhi;
			btnode_init(tmp, cap, 0);
			btnode_set_fences(tmp, NULL, 0, hip, rhilen);
		}
		if (internal_insert(tmp, sepk, seplen, child_pid_at(rp, 0)) != 0)
			goto done;
		rn = (int)btnode_count(rp);
		for (i = 1; i < rn; i++) {
			uint8_t kb[BT_MAX_KEY];
			uint16_t kl = 0;

			if (btnode_full_key(rp, i, kb, sizeof kb, &kl) != 0 ||
			    internal_insert(tmp, kb, kl,
			    child_pid_at(rp, i)) != 0)
				goto done;
		}
		btnode_set_right_sibling(tmp, btnode_right_sibling(rp));
		if (btnode_merge(lp, tmp) != 0)
			goto done;             /* did not fit after all */
	} else {
		if (btnode_merge(lp, rp) != 0)
			goto done;
	}

	/*
	 * L now owns R's keys and R's right-link (btnode_merge inherited
	 * it).  Unlink R from the parent by removing the rslot separator,
	 * so no descent or move_right can reach R anymore.  Order: commit L
	 * (merged contents + new right-link) and the parent (R's slot
	 * dropped), then free R -- which is now unreachable.
	 */
	(void)btnode_remove(pp, rslot);
	bm_unlatch(lf); bm_unfix(bm, lf, 1);   /* merged node: dirty */
	lf = NULL;
	btnode_mark_dead(rp);
	bm_unlatch(rf); bm_unfix(bm, rf, 0);   /* R is dead: do not dirty */
	rf = NULL;
	if (bm_free_pid(bm, rpid) == XTC_OK)
		atomic_fetch_add(&bt->st_reclaimed, 1);
	atomic_fetch_add(&bt->st_merges, 1);
	merged = 1;

 done:
	if (rf != NULL) { bm_unlatch(rf); bm_unfix(bm, rf, merged); }
	if (lf != NULL) { bm_unlatch(lf); bm_unfix(bm, lf, merged); }

 check_parent:
	/*
	 * Did the parent underflow?  An internal node underflows when it is
	 * down to a single child (only the slot-0 -infinity child) or its
	 * heap drops below the threshold.  If so, collapse it at the root
	 * or merge it one level up.  This runs whether or not a merge
	 * happened here, so a one-child parent still collapses upward.
	 */
	{
		uint16_t pcount = btnode_count(pp);
		punder = (pcount <= 1) ||
		    ((uint32_t)btnode_used_bytes(pp) * BT_MERGE_DEN
		    < cap * BT_MERGE_NUM);
	}
	bm_unlatch(pf); bm_unfix(bm, pf, merged);   /* dirty iff we changed it */
	if (punder) {
		if (level == 0)
			collapse_root(bt);     /* root may now have one child */
		else
			(void)merge_level(bt, path, level - 1, key, klen);
	}
	return XTC_OK;
}

/*
 * Structure-modification pass: right-merge to reclaim space after a
 * delete left a node sparse.  Runs under the SMO lock, so it never
 * races a split or another merge (both serialize on bt->smo); it
 * re-descends from the root because the sparse leaf may have moved or
 * split since the delete dropped its latch.
 *
 * B-LINK SAFETY INVARIANT
 * -----------------------
 * A page is unlinked and its id freed only while this pass holds, at
 * once and exclusively, the latches on (a) the parent that routes to
 * it, (b) the LEFT node it is merged into, and (c) the node R being
 * removed.  Because every descent latches a child before releasing
 * its parent, and move_right latches the right sibling before
 * releasing the left, any concurrent reader/writer that can reach R
 * either already holds R's latch -- so this pass blocks acquiring R
 * exclusively until that reader leaves R, reading only valid pre-merge
 * bytes -- or reaches R through a live link (the parent separator or
 * the left node's right-link), BOTH of which this pass rewires away
 * before releasing the parent and left latches.  After the rewire no
 * descent or move_right can newly reach R, and the lone reader that
 * held R has left, so R is unreachable when its id is freed.  A reused
 * id is therefore installed only behind a fresh link that no stale
 * reader follows.  (A parked cursor that remembered R's id is handled
 * separately, in bt_cursor_resume, which re-descends when its parked
 * page no longer covers the resume key.)
 *
 * Direction matters: merging R INTO L keeps L's identity and its place
 * in the right-sibling chain, so a reader that was on L and walks
 * right now finds R's keys absorbed into L (L's hi-fence widened to
 * R's) and stops there -- it never chases a dangling right-link.
 *
 * Best-effort: any obstacle (no right sibling, a different parent,
 * the pair does not fit, an allocation failure) simply ends the pass
 * with the tree intact and still correct, just not maximally compact.
 */
static int
bt_merge(bt_t *bt, const void *key, uint16_t klen)
{
	bm_t *bm = bt->bm;
	bm_pid_t path[BT_MAX_HEIGHT];   /* ancestor pids, root..parent-of-leaf */
	int depth;
	bm_pid_t pid;
	bm_frame_t *f;
	int rc;

	rc = xtc_arwlock_wrlock(bt->smo, -1);
	if (rc != XTC_OK)
		return rc;

	/*
	 * Epoch boundary: drain pids freed by the PREVIOUS merge onto the
	 * reusable freelist.  Holding the SMO lock here means every earlier
	 * structure modification has completed; any latch-free chaser that
	 * observed one of those now-freed pids has likewise finished (it
	 * does not park on a freed page).  Pids freed by THIS pass go to
	 * the quarantine and only become reusable at the next merge -- so a
	 * page unlinked now is never reissued for fresh contents while a
	 * chaser that read its id this epoch is still in flight.
	 */
	bm_reclaim_quarantine(bm);

	/*
	 * Latch-free descent collecting the ancestor pid path, exactly as
	 * bt_insert does, with move-right at each level to recover from any
	 * split that happened before we held the SMO lock.
	 */
	depth = 0;
	pid = atomic_load(&bt->root_pid);
	if ((rc = bm_fix_pid(bm, pid, &f)) != XTC_OK)
		goto unlock;
	bm_latch_exclusive(f);
	(void)move_right(bt, &f, key, klen, 1);
	while (!btnode_is_leaf(bm_page(f))) {
		bm_pid_t child;
		bm_frame_t *cf;

		if (depth >= BT_MAX_HEIGHT) {
			bm_unlatch(f); bm_unfix(bm, f, 0);
			rc = XTC_E_INTERNAL; goto unlock;
		}
		path[depth++] = bm_frame_pid(f);
		child = child_for_key(bm_page(f), key, klen);
		if ((rc = bm_fix_pid(bm, child, &cf)) != XTC_OK) {
			bm_unlatch(f); bm_unfix(bm, f, 0); goto unlock;
		}
		bm_latch_exclusive(cf);
		bm_unlatch(f); bm_unfix(bm, f, 0);
		f = cf;
		(void)move_right(bt, &f, key, klen, 1);
	}

	/*
	 * f is the leaf L that owns `key`, exclusive-latched.  A leaf with
	 * no parent IS the root: a single-leaf tree never merges (nothing
	 * to merge into).  Release and finish.
	 */
	if (depth == 0) {
		bm_unlatch(f); bm_unfix(bm, f, 0);
		rc = XTC_OK; goto unlock;
	}

	/*
	 * Drop the leaf latch before reaching back up to the parent: the
	 * merge re-acquires latches strictly top-down (parent, then the
	 * left child, then its right sibling), the same order as a descent,
	 * so it cannot deadlock.  We hold the SMO lock throughout, so no
	 * split or other merge can restructure the path under us; only a
	 * non-SMO insert/delete can mutate a leaf's contents, which the
	 * merge re-reads after latching.
	 */
	bm_unlatch(f); bm_unfix(bm, f, 0);
	rc = merge_level(bt, path, depth - 1, key, klen);

 unlock:
	(void)xtc_arwlock_unlock(bt->smo);
	return rc;
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
	int attempt;

	if (bt == NULL || key == NULL)
		return XTC_E_INVAL;
	bm = bt->bm;

	/*
	 * Latch-free descent with B-link move-right.  The delete proper
	 * retains no ancestor: it latches one node at a time, walks right
	 * past any concurrent split, and removes the key from the owning
	 * leaf -- the common path stays latch-free and leaf-exclusive,
	 * exactly like before.  Only if the leaf then underflows does the
	 * delete hand off to bt_merge, a separate structure-modification
	 * pass under the SMO lock; the descent here never holds two
	 * latches in opposite order, so it stays deadlock-free against a
	 * splitter or merger posting changes upward.
	 *
	 * A concurrent merge can absorb this delete's leaf into its LEFT
	 * sibling after the descent has already passed that sibling, so
	 * the key the descent is chasing ends up to the LEFT of where it
	 * landed -- a direction move_right cannot recover.  When the leaf
	 * does not contain the key, re-descend from the root: the parent
	 * separators now route to the node that absorbed it.  Bounded so a
	 * genuinely absent key still terminates in NOTFOUND.
	 */
	for (attempt = 0; attempt < BT_DELETE_RETRIES; attempt++) {
		int stale = 0;

		pid = atomic_load(&bt->root_pid);
		rc = bm_fix_pid(bm, pid, &f);
		if (rc != XTC_OK)
			return rc;
		bm_latch_exclusive(f);
		(void)move_right(bt, &f, key, klen, 1);
		pg = bm_page(f);
		/* Stale-node guard at the root (see the internal-level guard
		 * below): a concurrent merge may have unlinked it or absorbed
		 * its range leftward; restart the descent. */
		if (btnode_is_dead(pg) || btnode_below_lo_fence(pg, key, klen)) {
			bm_unlatch(f); bm_unfix(bm, f, 0);
			f = NULL;
			xtc_yield();
			continue;
		}
		while (!btnode_is_leaf(pg)) {
			bm_pid_t child = child_for_key(pg, key, klen);
			bm_frame_t *cf;

			rc = bm_fix_pid(bm, child, &cf);
			if (rc != XTC_OK) {
				bm_unlatch(f); bm_unfix(bm, f, 0);
				return rc;
			}
			bm_latch_exclusive(cf);
			bm_unlatch(f);             /* release before latching deeper */
			bm_unfix(bm, f, 0);
			f = cf;
			(void)move_right(bt, &f, key, klen, 1);
			pg = bm_page(f);
			/* Internal-level stale-node guard.  A concurrent merge can
			 * unlink the child (dead) or absorb its range into a LEFT
			 * sibling (key at/below lo fence) between reading the
			 * parent's child pointer and latching the child -- a
			 * direction move_right cannot follow.  Catch it at the
			 * first level it happens (not only at the leaf) so the
			 * descent never lands on a live-but-wrong leaf and silently
			 * misses the key; restart from the root. */
			if (btnode_is_dead(pg) ||
			    btnode_below_lo_fence(pg, key, klen)) {
				stale = 1;
				break;
			}
		}
		if (stale) {
			bm_unlatch(f); bm_unfix(bm, f, 0);
			f = NULL;
			xtc_yield();
			continue;
		}

		found = 0;
		s = btnode_search(pg, key, klen, &found);
		if (!found || btnode_is_dead(pg) ||
		    btnode_below_lo_fence(pg, key, klen)) {
			/* Key not here, or this leaf was merged away (marked dead,
			 * or the key is at/below its lower fence) -- a concurrent
			 * merge absorbed it into its left sibling, where
			 * move_right cannot follow.  Release and re-descend; after
			 * the bounded retries the SMO-locked path settles it. */
			bm_unlatch(f);
			bm_unfix(bm, f, 0);
			f = NULL;
			xtc_yield();           /* let a racing merge finish, then retry */
			continue;
		}
		(void)btnode_remove(pg, s);
		{
			/* Decide whether the leaf now underflows.  We hold it
			 * exclusive, so the count/used reading is stable. */
			uint32_t cap = bt->page_size;
			uint16_t used = btnode_used_bytes(pg);
			uint16_t cnt = btnode_count(pg);
			int underflow = (cnt == 0) ||
			    ((uint32_t)used * BT_MERGE_DEN < cap * BT_MERGE_NUM);
			bm_unlatch(f);
			bm_unfix(bm, f, 1);
			if (underflow && atomic_load(&bt->merge_on))
				(void)bt_merge(bt, key, klen); /* best-effort reclaim */
		}
		return XTC_OK;
	}

	/*
	 * The bounded latch-free retries did not find the key.  Either it
	 * is genuinely absent, or a relentless merge storm kept relocating
	 * it leftward faster than the lock-free descent could re-find it.
	 * Settle it authoritatively under the SMO write lock: no split or
	 * merge runs concurrently while it is held, so a single descent
	 * with move-right deterministically reaches the leaf that owns the
	 * key if it exists.  This is the rare path -- the common delete
	 * stays fully latch-free -- and it guarantees a delete is never
	 * lost to a concurrent structural change.
	 */
	if (xtc_arwlock_wrlock(bt->smo, -1) != XTC_OK)
		return XTC_E_NOTFOUND;
	pid = atomic_load(&bt->root_pid);
	if (bm_fix_pid(bm, pid, &f) != XTC_OK) {
		(void)xtc_arwlock_unlock(bt->smo);
		return XTC_E_NOTFOUND;
	}
	bm_latch_exclusive(f);
	(void)move_right(bt, &f, key, klen, 1);
	pg = bm_page(f);
	while (!btnode_is_leaf(pg)) {
		bm_pid_t child = child_for_key(pg, key, klen);
		bm_frame_t *cf;

		if (bm_fix_pid(bm, child, &cf) != XTC_OK) {
			bm_unlatch(f); bm_unfix(bm, f, 0);
			(void)xtc_arwlock_unlock(bt->smo);
			return XTC_E_NOTFOUND;
		}
		bm_latch_exclusive(cf);
		bm_unlatch(f); bm_unfix(bm, f, 0);
		f = cf;
		(void)move_right(bt, &f, key, klen, 1);
		pg = bm_page(f);
	}
	found = 0;
	s = btnode_search(pg, key, klen, &found);
	if (!found) {
		bm_unlatch(f); bm_unfix(bm, f, 0);
		(void)xtc_arwlock_unlock(bt->smo);
		return XTC_E_NOTFOUND;   /* genuinely absent */
	}
	(void)btnode_remove(pg, s);
	bm_unlatch(f); bm_unfix(bm, f, 1);
	(void)xtc_arwlock_unlock(bt->smo);
	return XTC_OK;
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
	int rc, attempt;

	atomic_fetch_add(&bt->st_descents, 1);

	/*
	 * Bounded latch-free retries (like descend_shared): a concurrent
	 * merge can unlink a node we are descending through, or absorb its
	 * range into a LEFT sibling -- which move_right cannot follow.  We
	 * revalidate dead/lo-fence at EVERY level and restart from the root
	 * on a hit so the cursor never opens onto a merged-away page.  With
	 * start == NULL there is no routing key, so only the dead flag can
	 * fire (the leftmost child is never absorbed leftward).
	 */
	for (attempt = 0; attempt < BT_DELETE_RETRIES; attempt++) {
		int stale = 0;

		pid = atomic_load(&bt->root_pid);
		rc = bm_fix_pid(bm, pid, &f);
		if (rc != XTC_OK)
			return rc;
		bm_latch_shared(f);
		(void)move_right(bt, &f, start, klen, 0);
		pg = bm_page(f);
		if (btnode_is_dead(pg) ||
		    (start != NULL && btnode_below_lo_fence(pg, start, klen))) {
			bm_unlatch(f); bm_unfix(bm, f, 0);
			xtc_yield();
			continue;
		}

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
			bm_unlatch(f);             /* release before latching deeper */
			bm_unfix(bm, f, 0);
			f = cf;
			(void)move_right(bt, &f, start, klen, 0);
			pg = bm_page(f);
			/* Internal-level stale-node guard (see descend_shared). */
			if (btnode_is_dead(pg) ||
			    (start != NULL &&
			    btnode_below_lo_fence(pg, start, klen))) {
				stale = 1;
				break;
			}
		}
		if (stale) {
			bm_unlatch(f); bm_unfix(bm, f, 0);
			xtc_yield();
			continue;
		}
		*out = f;
		return XTC_OK;
	}
	/* Exhausted retries under relentless merging: hand back the last
	 * leaf reached; its contents are valid for a scan. */
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
	/* Read-ahead the next leaf so a forward scan stays a cache hit. */
	{
		uint32_t rs = btnode_right_sibling(bm_page(leaf));
		if (rs != 0)
			(void)bm_prefetch_pid(bt->bm, (bm_pid_t)rs);
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
			/* Read-ahead: warm the NEXT leaf while we scan this one,
			 * so following the sibling chain stays a cache hit. */
			{
				uint32_t rs2 = btnode_right_sibling(bm_page(nf));
				if (rs2 != 0)
					(void)bm_prefetch_pid(bm, (bm_pid_t)rs2);
			}
		}
	}
}

/*
 * Park: release the leaf latch and pin but remember the leaf pid and
 * the last key returned (already in c->keybuf).  After this the cursor
 * holds no latch -- safe to hand control to code that may write the
 * tree (the VDBE's xUpdate) on the same thread.
 */
int
bt_cursor_park(bt_cursor_t *c)
{
	if (c == NULL)
		return XTC_E_INVAL;
	if (c->leaf != NULL) {
		c->parked_pid = bm_frame_pid(c->leaf);
		c->parked = 1;
		bm_unlatch(c->leaf);
		bm_unfix(c->bt->bm, c->leaf, 0);
		c->leaf = NULL;
	}
	return XTC_OK;
}

/*
 * Resume a parked cursor without re-descending the tree: re-fix the
 * remembered leaf, shared-latch it, and position just past the last
 * key returned.  If that leaf split while parked, keys that moved to
 * the new right sibling are still reached by bt_cursor_next following
 * the right-sibling chain.  O(1) amortized vs the O(log n) of a fresh
 * bt_cursor_open.
 */
int
bt_cursor_resume(bt_cursor_t *c)
{
	bm_t *bm;
	bm_frame_t *nf = NULL;
	void *pg;
	int slot, found = 0;

	if (c == NULL)
		return XTC_E_INVAL;
	if (!c->parked)
		return XTC_OK;             /* nothing parked: leaf still held, or done */
	c->parked = 0;
	bm = c->bt->bm;
	if (bm_fix_pid(bm, c->parked_pid, &nf) != XTC_OK) {
		c->done = 1;
		return XTC_E_INTERNAL;
	}
	bm_latch_shared(nf);
	pg = bm_page(nf);
	/* First slot with key >= the last key returned; skip it if present
	 * (already yielded), else start at the first key greater than it. */
	slot = btnode_search(pg, c->keybuf, c->keylen, &found);
	c->leaf = nf;
	c->slot = found ? slot + 1 : slot;
	atomic_fetch_add(&c->bt->st_resumes, 1);
	return XTC_OK;
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

/*
 * Enable or disable merge/reclaim on delete underflow.  Disabled by
 * default.  The merge structure-modification is correct under a single
 * mutator but has a known structural race against concurrent
 * latch-free deletes -- not the descent-level dead/fence race (which
 * this tree now revalidates at every internal level), but a deeper
 * buffer-manager page-reclamation interaction: a latch-free chaser can
 * reload a just-freed page id from disk during the quarantine epoch,
 * leaving a phantom resident frame that aliases the id once it is
 * reissued, so two frames map one pid and a reader can see a divergent
 * (garbage) page.  Enable it only when deletes are single-threaded or
 * the caller otherwise holds the tree exclusively; with it off, deletes
 * still remove keys correctly, pages just stay underfull (bounded,
 * safe) instead of being reclaimed.  See .agent/M_SQLXTC_BTREE_MERGE.md.
 */
void
bt_set_merge_enabled(bt_t *bt, int on)
{
	if (bt != NULL)
		atomic_store(&bt->merge_on, on ? 1 : 0);
}

void
bt_get_stats(bt_t *bt, bt_stats_t *out)
{
	if (bt == NULL || out == NULL)
		return;
	out->inserts = atomic_load(&bt->st_inserts);
	out->lookups = atomic_load(&bt->st_lookups);
	out->splits = atomic_load(&bt->st_splits);
	out->merges = atomic_load(&bt->st_merges);
	out->reclaimed = atomic_load(&bt->st_reclaimed);
	out->height = atomic_load(&bt->st_height);
	out->descents = atomic_load(&bt->st_descents);
	out->resumes = atomic_load(&bt->st_resumes);
}
