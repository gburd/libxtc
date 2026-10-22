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
#include "btovf.h"
#include "xtc_sim.h"    /* XTC_SIM_BUGGIFY -- DST eager-split (no-op in prod) */

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

/* Test-only seam: force a descent's root-level stale check to fire.
 * Defined by test_btree_stale_descent.c before it includes this file;
 * production builds compile it to 0. */
#ifndef BT_STALE_TEST_HOOK
#define BT_STALE_TEST_HOOK() 0
#define BT_STALE_TEST_HOOK_PEEK() 0
#endif

/*
 * The optimistic (seqlock) readers -- bt_lookup_optimistic and
 * descend_optimistic -- read pages a writer may be changing and discard
 * the result if the frame version moved.  ThreadSanitizer reports every
 * such read as a race (it cannot see the version check), which buried
 * the real ones (26 reports in test_btree_mt alone).  Under TSan they
 * are compiled out, so every read takes the latched path and TSan checks
 * the latch protocol, which must be race-free.  The optimistic paths are
 * covered by the multi-thread tests and DST in the other builds.
 */
#if defined(__SANITIZE_THREAD__)
#define BT_OLC_ENABLED 0
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define BT_OLC_ENABLED 0
#endif
#endif
#ifndef BT_OLC_ENABLED
#define BT_OLC_ENABLED 1
#endif
#ifndef BT_STALE_TEST_HOOK_PEEK
/* The stale-descent test forces the LATCHED path's checks: skip the
 * optimistic descent while it has a forced stale hit pending. */
#define BT_STALE_TEST_HOOK_PEEK() (g_force_stale > 0)
#endif

/*
 * In-debug node validator (RethinkDB runs leaf::validate after every
 * mutation in debug builds).  With -DBT_CHECK -- and always in a DST
 * build (the sim I/O backend), where a seed then pins the failure --
 * every node a mutation leaves behind is checked with btnode_check
 * while still latched, and a malformed node aborts at the call that
 * made it, with the reason.  A no-op otherwise.
 */
#if defined(XTC_IO_BACKEND_SIM) && !defined(BT_CHECK)
#define BT_CHECK 1
#endif
#ifdef BT_CHECK
#include <stdio.h>
static void
bt_check_node(const void *pg, const char *fn, int line)
{
	char why[200];

	if (btnode_check(pg, why, sizeof why) != 0) {
		fprintf(stderr, "BT_CHECK: %s:%d: malformed node: %s\n", fn,
		    line, why);
		abort();
	}
}
#define BT_CHECK_NODE(pg) bt_check_node((pg), __func__, __LINE__)
#else
#define BT_CHECK_NODE(pg) ((void)0)
#endif

struct bt {
	bm_t             *bm;
	uint32_t          page_size;
	_Atomic bm_pid_t  root_pid;    /* read by every descent */
	/* Keep the statistics below off root_pid's cache line: every reader
	 * loads root_pid, and a counter write by any thread on the same line
	 * would invalidate it in every other core's cache.  (Padding, not
	 * _Alignas: the struct comes from calloc.) */
	char              pad_root[64];

	_Atomic uint64_t  st_inserts;
	_Atomic uint64_t  st_lookups;
	_Atomic uint64_t  st_splits;
	_Atomic uint64_t  st_merges;   /* node merges (right sibling pulled left) */
	_Atomic uint64_t  st_reclaimed;/* pages returned to the bufmgr freelist */
	_Atomic uint64_t  st_height;   /* number of levels (1 == root leaf) */
	_Atomic uint64_t  st_descents; /* full root->leaf cursor descents */
	_Atomic uint64_t  st_resumes;  /* parked-cursor O(1) resumes */
	_Atomic uint64_t  st_ovf_writes; /* values stored out of line */
	_Atomic uint64_t  st_ovf_reads;  /* out-of-line values read back */
	_Atomic uint64_t  st_ovf_pages;  /* data pages written for them */
	_Atomic uint64_t  st_ovf_freed;  /* data pages freed (delete/replace) */
	_Atomic uint64_t  st_sample_pages; /* nodes read by bt_sample_keys */
	uint64_t          meta_clean;  /* superblock clean flag (open/close only) */
	uint64_t          meta_clock;  /* superblock commit clock (open/close only) */
	uint8_t          *smo_scratch; /* one page of scratch for internal split /
	                               * merge rebuilds; used only under the SMO
	                               * write lock (so one per tree suffices) --
	                               * replaces an 8 KiB stack array that made
	                               * every internal split fail on > 8 KiB
	                               * pages */
	xtc_arwlock_t    *smo;         /* serializes structure-modification (splits
	                               * and root growth).  Parking-safe so a
	                               * splitter may park on page I/O without
	                               * wedging the loop; non-splitting inserts
	                               * never take it, so the common path stays
	                               * fully parallel. */
};

static int bt_merge(bt_t *bt, const void *key, uint16_t klen);
static void smo_log_page(bt_t *bt, bm_pid_t pid, void *image);

/*
 * Entry-size bounds (RethinkDB's rule: every leaf entry is bounded by a
 * fixed fraction of the page -- leaf_epsilon, leaf_node.cc:523-535 --
 * so every split and merge guarantee holds by construction).
 *
 * BT_ENTRY_DIV: an entry (slot + full key + value) may use at most
 * 1/BT_ENTRY_DIV of the usable page.  With 4, a node at the split point
 * holds >= 4 such entries' worth of bytes, so a byte-balanced split
 * always finds a cut that leaves room for one more max entry on the
 * side that receives it, even after the separator (<= one key) is
 * added to both halves as a fence.  Larger values belong out of line.
 */
/*
 * The guarantee (proof sketch; cap = page - header, K = max key, M = max
 * leaf entry cost incl. its 10-byte slot):
 *   A byte-balanced cut leaves each half <= T/2 + M (entries; T <= cap)
 *   + 2K (two fences: the old one and the separator).  The pending entry
 *   adds <= M on its side.  So a valid cut exists whenever
 *       cap/2 + 2M + 2K <= cap   <=>   M + K <= cap/4.
 *   Internal entries are a separator plus an 8-byte pid (<= K + 18), and
 *   the same argument needs 4K + 36 <= cap/2, i.e. K <= cap/8 - 9.
 * So K = min(BT_MAX_KEY, cap/8 - 9) and M = cap/4 - K.  For 4 KiB pages
 * that is K = 498 and M = 516 (an inline key+value of up to 506 bytes);
 * for 16 KiB K = 1024 and M = 3062.  This is RethinkDB's leaf_epsilon
 * (512 bytes on its 4 KiB block, MAX_KEY_SIZE 250) derived for our node.
 * Larger values must be stored out of line.
 */
static uint32_t
bt_cap(const bt_t *bt)
{
	return bt->page_size - (uint32_t)sizeof(struct btnode_hdr);
}

static uint32_t
bt_key_max(const bt_t *bt)
{
	uint32_t k = bt_cap(bt) / 8 - 9;

	return k < BT_MAX_KEY ? k : BT_MAX_KEY;
}

static uint32_t
bt_entry_max(const bt_t *bt)
{
	return bt_cap(bt) / 4 - bt_key_max(bt);
}

uint16_t
bt_max_key(const bt_t *bt)
{
	return (uint16_t)bt_key_max(bt);
}

uint16_t
bt_max_inline(const bt_t *bt)
{
	return (uint16_t)(bt_entry_max(bt) - 10u);
}

/* ---- out-of-line values (RethinkDB blob, one level; btovf.h) ---------- */

static uint32_t
ovf_payload(const bt_t *bt)
{
	return bt->page_size - BT_OVF_PGHDR;
}

/* Largest value bt_insert accepts for a key of `klen` bytes. */
static uint32_t
bt_value_max(const bt_t *bt, uint16_t klen)
{
	uint32_t pages, cap, inl;

	/* The ref must itself fit as an inline entry. */
	pages = BT_OVF_MAXPAGES;
	while (pages > 0 && btnode_entry_cost(klen,
	    (uint16_t)bt_ovf_ref_len(pages)) > bt_entry_max(bt))
		pages--;
	cap = pages * ovf_payload(bt);
	if (cap > 0xFFFFu)
		cap = 0xFFFFu;          /* the bt_* API's 16-bit value length */
	inl = bt_entry_max(bt) - 10u - klen;
	return cap > inl ? cap : inl;
}

uint32_t
bt_max_value(const bt_t *bt, uint16_t klen)
{
	if (bt == NULL || klen == 0 || klen > bt_key_max(bt))
		return 0;
	return bt_value_max(bt, klen);
}

/*
 * Write `vlen` bytes of `val` to fresh data pages and build the ref in
 * `ref` (capacity bt_ovf_ref_len(BT_OVF_MAXPAGES)).  The pages are left
 * DIRTY in the pool before the caller publishes the ref, so a reader that
 * finds the ref always finds the data (it is in memory or on disk).  On
 * failure every page allocated so far is freed again.  Returns the ref
 * length, or 0 on failure.
 */
static uint32_t
ovf_write(bt_t *bt, const void *val, uint32_t vlen, uint8_t *ref)
{
	bm_t *bm = bt->bm;
	uint32_t per = ovf_payload(bt);
	uint32_t n = (vlen + per - 1) / per, i, off = 0;
	bm_pid_t pids[BT_OVF_MAXPAGES];

	if (n == 0 || n > BT_OVF_MAXPAGES)
		return 0;
	for (i = 0; i < n; i++) {
		bm_frame_t *f;
		uint8_t *pg;
		uint32_t take = vlen - off < per ? vlen - off : per;

		if (bm_alloc_pid(bm, &f, &pids[i]) != XTC_OK) {
			while (i > 0)
				(void)bm_free_pid(bm, pids[--i]);
			return 0;
		}
		pg = bm_page(f);
		memset(pg, 0, BT_OVF_PGHDR);
		bt_ovf_put32(pg + 8, BT_OVF_MAGIC);
		bt_ovf_put32(pg + 12, take);
		memcpy(pg + BT_OVF_PGHDR, (const uint8_t *)val + off, take);
		if (take < per)
			memset(pg + BT_OVF_PGHDR + take, 0, per - take);
		off += take;
		bm_predirty(bm, f);
		smo_log_page(bt, pids[i], pg);   /* physiological image, if logging */
		bm_unfix(bm, f, 1);
	}
	ref[0] = BT_OVF_TAG;
	ref[1] = (uint8_t)n;
	ref[2] = 0;
	ref[3] = 0;
	bt_ovf_put32(ref + 4, vlen);
	bt_ovf_put32(ref + 8, bt_ovf_fnv(val, vlen));
	for (i = 0; i < n; i++)
		bt_ovf_put64(ref + BT_OVF_HDR + 8u * i, (uint64_t)pids[i]);
	atomic_fetch_add(&bt->st_ovf_writes, 1);
	atomic_fetch_add(&bt->st_ovf_pages, n);
	return bt_ovf_ref_len(n);
}

/* Parse a ref; returns the page count (0 if malformed). */
static uint32_t
ovf_parse(const uint8_t *ref, uint32_t rlen, uint32_t *total)
{
	uint32_t n;

	if (rlen < BT_OVF_HDR || ref[0] != BT_OVF_TAG)
		return 0;
	n = ref[1];
	if (n == 0 || n > BT_OVF_MAXPAGES || rlen != bt_ovf_ref_len(n))
		return 0;
	*total = bt_ovf_get32(ref + 4);
	return n;
}

/*
 * Read an out-of-line value into `buf` (capacity `cap`); *vlen gets the
 * true length.  The ref was copied out of the leaf by the caller while
 * it held the leaf (latched or version-validated), so the data pages it
 * names are live: a concurrent replace/delete frees them only after
 * removing the ref under the leaf's EXCLUSIVE latch, and bm_free_pid
 * quarantines a freed id so a straggling fix gets XTC_E_AGAIN rather than
 * another value's bytes.  The checksum in the ref is verified when the
 * whole value was read, so a value is never returned torn.
 * Returns XTC_OK, XTC_E_AGAIN (a page was freed under us: re-read the
 * leaf), or XTC_E_INTERNAL (corrupt).
 */
static int
ovf_read(bt_t *bt, const uint8_t *ref, uint32_t rlen, void *buf,
    uint16_t cap, uint16_t *vlen)
{
	bm_t *bm = bt->bm;
	uint32_t total = 0, n, i, off = 0, per = ovf_payload(bt);
	uint32_t sum = 2166136261u;
	int rc;

	n = ovf_parse(ref, rlen, &total);
	if (n == 0)
		return XTC_E_INTERNAL;
	for (i = 0; i < n && off < total; i++) {
		bm_frame_t *f;
		const uint8_t *pg;
		uint32_t len, j;
		bm_pid_t pid = (bm_pid_t)bt_ovf_get64(ref + BT_OVF_HDR + 8u * i);

		rc = bm_fix_pid(bm, pid, &f);
		if (rc != XTC_OK)
			return rc == XTC_E_AGAIN ? XTC_E_AGAIN : XTC_E_INTERNAL;
		bm_latch_shared(f);
		pg = bm_page(f);
		len = bt_ovf_get32(pg + 12);
		if (bt_ovf_get32(pg + 8) != BT_OVF_MAGIC || len > per ||
		    off + len > total) {
			bm_unlatch(f); bm_unfix(bm, f, 0);
			return XTC_E_AGAIN;   /* page reused: the ref is stale */
		}
		for (j = 0; j < len; j++) {
			sum ^= pg[BT_OVF_PGHDR + j];
			sum *= 16777619u;
		}
		if (buf != NULL && off < cap)
			memcpy((uint8_t *)buf + off, pg + BT_OVF_PGHDR,
			    off + len <= cap ? len : cap - off);
		off += len;
		bm_unlatch(f);
		bm_unfix(bm, f, 0);
	}
	if (off != total || sum != bt_ovf_get32(ref + 8))
		return XTC_E_AGAIN;           /* a page changed mid-read */
	if (vlen != NULL)
		*vlen = (uint16_t)total;
	atomic_fetch_add(&bt->st_ovf_reads, 1);
	return XTC_OK;
}

/* If slot `s` of the (exclusively latched) leaf holds an out-of-line
 * ref, copy it into `out` and return its length; else 0. */
static uint32_t
take_ovf_ref(const void *pg, int s, uint8_t *out)
{
	const void *ov = NULL;
	uint16_t ol = 0;

	if (!btnode_vflag(pg, s) ||
	    btnode_get(pg, s, NULL, NULL, &ov, &ol) != 0 ||
	    ol > BT_OVF_HDR + 8u * BT_OVF_MAXPAGES)
		return 0;
	memcpy(out, ov, ol);
	return ol;
}

/* Free the data pages a ref names (the ref itself is already gone from
 * every reachable leaf). */
static void
ovf_free(bt_t *bt, const uint8_t *ref, uint32_t rlen)
{
	uint32_t total = 0, n, i;

	n = ovf_parse(ref, rlen, &total);
	for (i = 0; i < n; i++) {
		bm_pid_t pid = (bm_pid_t)bt_ovf_get64(ref + BT_OVF_HDR + 8u * i);

		if (bm_free_pid(bt->bm, pid) == XTC_OK)
			atomic_fetch_add(&bt->st_ovf_freed, 1);
	}
}
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
int
bt_write_super(bt_t *bt)
{
	struct bt_super s;
	s.magic = BT_SUPER_MAGIC;
	s.page_size = bt->page_size;
	s.root_pid = (uint64_t)atomic_load(&bt->root_pid);
	s.height = atomic_load(&bt->st_height);
	s.clean = bt->meta_clean;
	s.commit_clock = bt->meta_clock;
	return bm_write_super(bt->bm, &s, sizeof s);
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
	uint8_t        keybuf[BT_MAX_KEY];  /* scan position: last key returned, */
	uint16_t       keylen;              /* or the start key before the first */
	int            have_key;            /* 1 == keybuf was returned: resume after it */
	uint8_t       *vbuf;   /* out-of-line values are assembled here (lazily
	                        * allocated, 64 KiB); valid until the next
	                        * next/close, like a leaf pointer */
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
	if ((bt->smo_scratch = malloc(ps)) == NULL) {
		bm_unfix(bm, rf, 0);
		rc = XTC_E_NOMEM;
		goto fail;
	}

	/* Root starts as a single leaf with -inf/+inf fences (prefix 0). */
	btnode_init(bm_page(rf), ps, 1);
	btnode_set_fences(bm_page(rf), NULL, 0, NULL, 0);
	BT_CHECK_NODE(bm_page(rf));
	atomic_store(&bt->root_pid, rpid);
	atomic_store(&bt->st_height, 1);
	bm_unfix(bm, rf, 1);

	(void)bt_write_super(bt);       /* persist where the root lives */

	*out = bt;
	return XTC_OK;

fail:
	xtc_arwlock_destroy(bt->smo);
	free(bt->smo_scratch);
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
	if ((bt->smo_scratch = malloc(s.page_size)) == NULL) {
		xtc_arwlock_destroy(bt->smo);
		free(bt);
		return XTC_E_NOMEM;
	}
	atomic_store(&bt->root_pid, (bm_pid_t)s.root_pid);
	atomic_store(&bt->st_height, s.height);
	bt->meta_clean = s.clean;
	bt->meta_clock = s.commit_clock;
	*out = bt;
	return XTC_OK;
}

/* Set by every connection registration (always to the same function),
 * possibly from several loops at once: atomic so that is not a race. */
typedef void (*bt_close_hook_fn)(bt_t *);
static _Atomic(bt_close_hook_fn) bt_close_hook = NULL;

void
bt_close(bt_t *bt)
{
	bt_close_hook_fn hook;
	if (bt == NULL)
		return;
	hook = atomic_load_explicit(&bt_close_hook, memory_order_acquire);
	if (hook != NULL)
		hook(bt);              /* let xstore drop cached state for this bt */
	if (bt->smo != NULL)
		xtc_arwlock_destroy(bt->smo);
	free(bt->smo_scratch);
	free(bt);
}

void
bt_set_close_hook(void (*fn)(bt_t *))
{
	atomic_store_explicit(&bt_close_hook, fn, memory_order_release);
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
static bt_smo_hook_t bt_smo_hook = { NULL, NULL, NULL, NULL, NULL };

void
bt_set_smo_hook(const bt_smo_hook_t *hook)
{
	if (hook == NULL)
		bt_smo_hook = (bt_smo_hook_t){ NULL, NULL, NULL, NULL, NULL };
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
 * LSN so the hook can log it.  The hook returns the image record's own
 * WAL LSN; stamp that back onto the live page so its on-disk page LSN
 * equals the LSN recovery gates the image apply by (each image is its
 * own monotonically-numbered record, so the last image of a page wins).
 * No-op without a hook.  Skips page 0 (never an SMO page) defensively. */
static void
smo_log_page(bt_t *bt, bm_pid_t pid, void *image)
{
	uint64_t rlsn;
	if (bt_smo_hook.page == NULL || pid == 0)
		return;
	rlsn = bt_smo_hook.page(bt_smo_hook.user, pid, image, bt->page_size,
	    bm_get_lsn(bt->bm));
	if (rlsn != 0)
		bm_stamp_lsn(bt->bm, image, rlsn);
}

/* Close the nested top action (writes the dummy CLR).  No-op without a
 * hook. */
static void
smo_end(uint64_t token)
{
	if (bt_smo_hook.end != NULL)
		bt_smo_hook.end(bt_smo_hook.user, token);
}

/* Log a PLAIN (non-split) leaf's full after-image: physiological redo
 * for an in-leaf insert that did NOT split.  The caller has just
 * bm_predirty'd the leaf, so its LSN field holds the change's LSN.  The
 * hook returns the image record's own WAL LSN; stamp that back onto the
 * live leaf so its on-disk page LSN equals the LSN recovery gates on --
 * so the LAST image of a leaf (the one after the final in-leaf insert)
 * wins even when many inserts in one group-committed transaction share
 * a commit LSN.  Not bracketed by a nested top action (a single leaf
 * change is atomic under the double-write buffer); it just gives
 * in-place recovery an after-image to repair a torn non-split leaf from.
 * No-op without a hook.  Skips page 0 defensively. */
static void
smo_log_leaf(bt_t *bt, bm_pid_t pid, void *image)
{
	uint64_t rlsn;
	if (bt_smo_hook.leaf == NULL || pid == 0)
		return;
	rlsn = bt_smo_hook.leaf(bt_smo_hook.user, pid, image, bt->page_size,
	    bm_get_lsn(bt->bm));
	if (rlsn != 0)
		bm_stamp_lsn(bt->bm, image, rlsn);
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
    uint8_t *pushup, uint16_t *pushuplen, int append)
{
	int count = (int)btnode_count(pp);
	/* An appended separator (the child split was an append split; see
	 * btnode split_point) pushes up the LAST key: the left keeps all but
	 * one child, the right starts with that child and the new one.  A
	 * midpoint cut left every internal node of an ascending key stream
	 * half empty, as the leaves were. */
	int mid = (append && count >= 3) ? count - 1 : count / 2;
	int i;
	uint8_t empty = 0;
	uint32_t old_right = btnode_right_sibling(pp);
	void *lp = bt->smo_scratch;   /* one page; caller holds the SMO lock */

	/* The mid key is pushed up; it becomes the boundary between the
	 * two halves (and the left node's new upper fence). */
	if (btnode_full_key(pp, mid, pushup, BT_MAX_KEY, pushuplen) != 0)
		return XTC_E_INTERNAL;

	/* Right node: -infinity leftmost child = mid's child, then the
	 * separators/children strictly above mid.  Wide-open lo fence so
	 * routing through slot 0 stays correct; the upper fence INHERITS
	 * the old node's (which is the old node's routed upper bound --
	 * +infinity only if the old node was itself rightmost/root), so the
	 * right half's hi-fence equals what the parent will route to it and
	 * the split maintains the routing invariant hi_fence(child) ==
	 * parent's routed upper bound.  Setting it to +infinity here would
	 * make the right half claim a wider range than its parent routes,
	 * which is harmless for descent but becomes a lost-delete bug when
	 * this node is later the RIGHT member of a merge (the merged node
	 * inherits the bogus +infinity hi-fence).  The old right-link
	 * transfers here. */
	{
		uint8_t ohi[BT_MAX_KEY];
		uint16_t ohilen = 0;
		const uint8_t *ohip = NULL;

		if (btnode_hi_fence(pp, ohi, sizeof ohi, &ohilen) == 0 &&
		    ohilen > 0)
			ohip = ohi;
		btnode_set_fences(rp, NULL, 0, ohip, ohilen);
	}
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
    uint16_t vlen, uint16_t vflag)
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
	r = btnode_insert_f(pg, key, klen, val, vlen, vflag);
	if (r == 0) {
		BT_CHECK_NODE(pg);
		atomic_fetch_add(&bt->st_inserts, 1);
		/* Physiological redo for a plain (non-split) in-leaf insert:
		 * log the leaf's after-image so in-place recovery can repair a
		 * torn NON-split leaf from its image (page-LSN gated) instead of
		 * losing its key range to a logical redo that descends the torn
		 * page.  bm_predirty stamps the leaf LSN now so the image and the
		 * on-disk page agree.  No-op unless the leaf hook is installed. */
		bm_predirty(bm, f); smo_log_leaf(bt, bm_frame_pid(f), pg);
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
		int append;

		if (bm_fix_pid(bm, path[level], &pf) != XTC_OK)
			return XTC_E_INTERNAL;
		bm_latch_exclusive(pf);
		/* The captured parent may have split since descent: walk
		 * right to the internal node that now owns cur_sep. */
		(void)move_right(bt, &pf, cur_sep, cur_seplen, 1);
		pp = bm_page(pf);

		if (internal_insert(pp, cur_sep, cur_seplen, cur_right) == 0) {
			BT_CHECK_NODE(pp);
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
		{
			uint8_t lk[BT_MAX_KEY];
			uint16_t lkl = 0;
			int cnt = (int)btnode_count(pp);
			append = cnt >= 3 &&
			    btnode_full_key(pp, cnt - 1, lk, BT_MAX_KEY, &lkl) == 0 &&
			    key_cmp(cur_sep, cur_seplen, lk, lkl) > 0;
		}
		if (bt_split_internal(bt, pp, bm_page(rf), (uint32_t)rpid,
		    pushup, &pushuplen, append) != XTC_OK) {
			bm_unlatch(rf); bm_unfix(bm, rf, 1);
			bm_unlatch(pf); bm_unfix(bm, pf, 0);
			return XTC_E_INTERNAL;
		}
		if (key_cmp(cur_sep, cur_seplen, pushup, pushuplen) > 0)
			r = internal_insert(bm_page(rf), cur_sep, cur_seplen, cur_right);
		else
			r = internal_insert(pp, cur_sep, cur_seplen, cur_right);
		BT_CHECK_NODE(bm_page(rf));
		BT_CHECK_NODE(pp);
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
		BT_CHECK_NODE(bm_page(nf));
		bm_predirty(bm, nf); smo_log_page(bt, npid, bm_page(nf));
		bm_unlatch(nf); bm_unfix(bm, nf, 1);
		atomic_store(&bt->root_pid, npid);
		atomic_fetch_add(&bt->st_height, 1);
		(void)bt_write_super(bt);   /* root pid changed: persist it */
	}
	return XTC_OK;
}

static int bt_insert_leaf(bt_t *bt, const void *key, uint16_t klen,
    const void *val, uint16_t vlen, uint16_t vflag, uint8_t *oldref,
    uint32_t *oldref_len);

/*
 * Insert or replace.  A value that fits the inline entry bound is stored
 * in the leaf; a larger one (up to bt_max_value) is written out of line
 * first and the leaf stores a bounded reference (RethinkDB blob).  When
 * the key already held an out-of-line value, its data pages are freed
 * only AFTER the new entry is in place, so a concurrent reader sees
 * either the old value or the new one, never freed pages.
 */
int
bt_insert(bt_t *bt, const void *key, uint16_t klen, const void *val,
    uint16_t vlen)
{
	uint8_t ref[BT_OVF_HDR + 8u * BT_OVF_MAXPAGES];
	uint8_t oldref[BT_OVF_HDR + 8u * BT_OVF_MAXPAGES];
	uint32_t rlen, oldlen = 0;
	int rc;

	if (bt == NULL || key == NULL || (val == NULL && vlen != 0))
		return XTC_E_INVAL;
	if (klen == 0 || klen > bt_key_max(bt))
		return XTC_E_INVAL;
	if (btnode_entry_cost(klen, vlen) <= bt_entry_max(bt)) {
		rc = bt_insert_leaf(bt, key, klen, val, vlen, 0, oldref, &oldlen);
	} else {
		if ((uint32_t)vlen > bt_value_max(bt, klen))
			return XTC_E_INVAL;
		rlen = ovf_write(bt, val, vlen, ref);
		if (rlen == 0)
			return XTC_E_NOMEM;
		rc = bt_insert_leaf(bt, key, klen, ref, (uint16_t)rlen,
		    BTNODE_VFLAG, oldref, &oldlen);
		if (rc != XTC_OK)
			ovf_free(bt, ref, rlen);   /* never published */
	}
	if (rc == XTC_OK && oldlen > 0)
		ovf_free(bt, oldref, oldlen);   /* replaced: old pages unreachable */
	return rc;
}

static int
bt_insert_leaf(bt_t *bt, const void *key, uint16_t klen, const void *val,
    uint16_t vlen, uint16_t vflag, uint8_t *oldref, uint32_t *oldref_len)
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

	*oldref_len = 0;
	/* Entry-size bound (see bt_entry_max): an entry that could make a
	 * split impossible is refused here, before anything is touched. */
	if (btnode_entry_cost(klen, vlen) > bt_entry_max(bt))
		return XTC_E_INVAL;
	bm = bt->bm;

	/* Fast path: latch-free descent + leaf-exclusive insert, parallel
	 * for disjoint leaves.  Falls through to the SMO path only when a
	 * split is needed (full leaf) or the key already exists. */
	if (bt_insert_fast(bt, key, klen, val, vlen, vflag))
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
	/* Upsert: drop the old version first.  Safe now: with the entry
	 * bound, the replacement always fits -- in place, or on its side of
	 * the byte-balanced split below (which accounts for it as the
	 * pending entry) -- so the old value can no longer be lost to an
	 * insert that then fails. */
	if (found) {
		/* Remember an out-of-line old value: bt_insert frees its pages
		 * once the replacement is published. */
		*oldref_len = take_ovf_ref(pg, s, oldref);
		(void)btnode_remove(pg, s);
	}
	/* Buggify: force a SPLIT even when the pair would fit in place.  A
	 * B-tree may legally split a non-full node (it only wastes space --
	 * the tree stays ordered and correct), so this deterministically
	 * drives the SMO / separator-post / latch-coupling path far more
	 * often than a natural fill would, where the subtle split bugs live.
	 * Only when the node already holds >= 2 live entries, since
	 * btnode_split needs two slots to divide.  Per-call BUGGIFY coin
	 * (fixed per site per run, replays); a no-op in production. */
	if (!(btnode_count(pg) >= 2 &&
	      XTC_SIM_BUGGIFY("btree.split.eager") &&
	      xtc_sim_buggify_fault(200)) &&
	    btnode_insert_f(pg, key, klen, val, vlen, vflag) == 0) {
		BT_CHECK_NODE(pg);
		atomic_fetch_add(&bt->st_inserts, 1);
		/* Non-split in-leaf insert on the SMO path (an upsert, or a
		 * fast-path miss re-run under the lock): log the leaf image too,
		 * same physiological-redo reason as the fast path above. */
		bm_predirty(bm, f); smo_log_leaf(bt, bm_frame_pid(f), pg);
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
	if (btnode_split_ex(pg, bm_page(rf), sep, (uint16_t)sizeof sep,
	    &seplen, key, klen, vlen) != 0) {
		/* Unreachable for a bounded entry (see bt_entry_max).  The page
		 * is unchanged; return the fresh right page to the freelist. */
		bm_unlatch(rf); bm_unfix(bm, rf, 0);
		(void)bm_free_pid(bm, rpid);
		bm_unlatch(f); bm_unfix(bm, f, 0);
		rc = XTC_E_INTERNAL; goto unlock;
	}
	btnode_set_right_sibling(pg, (uint32_t)rpid);
	if (key_cmp(key, klen, sep, seplen) > 0)
		r = btnode_insert_f(bm_page(rf), key, klen, val, vlen, vflag);
	else
		r = btnode_insert_f(pg, key, klen, val, vlen, vflag);
	BT_CHECK_NODE(pg);
	BT_CHECK_NODE(bm_page(rf));
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
 * Optimistic descent to the leaf for `key` (start == NULL: the leftmost
 * leaf): the internal levels are read WITHOUT their content latch, under
 * each frame's version seqlock (optimistic lock coupling, Leis et al.;
 * LeanStore), and only the leaf is latched (shared), so concurrent
 * readers write no shared cache line above the leaves.  Every descent
 * used to take the root's and each internal node's arwlock: at 8
 * threads a secondary-index read mix spent 36% of its time in futex
 * wait/wake on those few latches.  The races are the latched path's
 * (it too drops the parent before latching the child): a split moved
 * the key right (follow the right link), a merge freed the node or
 * absorbed its range leftward (restart).  A version change under the
 * read -- a writer in the node -- or anything unexpected returns 0 and
 * the caller takes the latched descent.  On 1 the leaf is fixed and
 * shared-latched in *out and owns `key` (fence-checked under the latch).
 */
static int
descend_optimistic(bt_t *bt, const void *key, uint16_t klen, bm_frame_t **out)
{
	bm_t *bm = bt->bm;
	bm_frame_t *f;
	int hops;

	if (!BT_OLC_ENABLED)
		return 0;
	if (bm_fix_pid(bm, atomic_load(&bt->root_pid), &f) != XTC_OK)
		return 0;
	for (hops = 0; hops < 4 * BT_MAX_HEIGHT; hops++) {
		void *pg = bm_page(f);
		uint64_t v;
		bm_pid_t next;
		int leaf, beyond, bad;

		if (!bm_read_begin(f, &v))
			break;                       /* a writer holds it */
		leaf = btnode_is_leaf(pg);
		bad = btnode_is_dead(pg) ||
		    (key != NULL && btnode_below_lo_fence(pg, key, klen));
		beyond = key != NULL && btnode_beyond_hi_fence(pg, key, klen);
		if (beyond)
			next = (bm_pid_t)btnode_right_sibling(pg);
		else if (leaf)
			next = BM_PID_NONE;
		else
			next = key == NULL ? child_pid_at(pg, 0)
			    : child_for_key(pg, key, klen);
		if (!bm_read_valid(f, v) || bad || (beyond && next == 0))
			break;
		if (leaf && !beyond) {
			/* The leaf: latch it and re-check under the latch. */
			bm_latch_shared(f);
			(void)move_right(bt, &f, key, klen, 0);
			pg = bm_page(f);
			if (!btnode_is_leaf(pg) || btnode_is_dead(pg) ||
			    (key != NULL && btnode_below_lo_fence(pg, key, klen))) {
				bm_unlatch(f);
				bm_unfix(bm, f, 0);
				return 0;
			}
			*out = f;
			return 1;
		}
		{
			bm_frame_t *nf;
			if (next == BM_PID_NONE || bm_fix_pid(bm, next, &nf) != XTC_OK)
				break;
			bm_unfix(bm, f, 0);
			f = nf;
		}
	}
	bm_unfix(bm, f, 0);
	return 0;
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
	int rc;

	if (!BT_STALE_TEST_HOOK_PEEK() && descend_optimistic(bt, key, klen, out))
		return XTC_OK;
	/* Retry until a descent lands on a live leaf.  Each stale hit means
	 * a concurrent merge changed the path; it finishes and the next
	 * descent routes past it.  (This was bounded at BT_DELETE_RETRIES
	 * and then returned the last frame -- which the stale branch had
	 * already unlatched and unfixed, so the caller released it twice.) */
	for (;;) {
		int stale = 0;

		pid = atomic_load(&bt->root_pid);
		rc = bm_fix_pid(bm, pid, &f);
		if (rc == XTC_E_AGAIN) {   /* root pid quarantined: retry */
			xtc_yield();
			continue;
		}
		if (rc != XTC_OK)
			return rc;
		bm_latch_shared(f);
		(void)move_right(bt, &f, key, klen, 0);
		pg = bm_page(f);
		/* Stale-node guard at the root (see the internal-level guard
		 * below): a concurrent merge may have unlinked it or absorbed
		 * its range leftward; restart the descent. */
		if (BT_STALE_TEST_HOOK() || btnode_is_dead(pg) ||
		    btnode_below_lo_fence(pg, key, klen)) {
			bm_unlatch(f);
			bm_unfix(bm, f, 0);
			xtc_yield();
			continue;
		}
		while (!btnode_is_leaf(pg)) {
			bm_pid_t child = child_for_key(pg, key, klen);
			bm_frame_t *cf;

			rc = bm_fix_pid(bm, child, &cf);
			if (rc == XTC_E_AGAIN) {
				/* Child pid quarantined by a concurrent merge:
				 * restart from the root. */
				stale = 1;
				break;
			}
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
		 * move_right cannot follow), restart from the root. */
		if (!btnode_is_dead(pg) && !btnode_below_lo_fence(pg, key, klen)) {
			*out = f;
			return XTC_OK;
		}
		bm_unlatch(f);
		bm_unfix(bm, f, 0);
		xtc_yield();
	}
}

/*
 * OLC (optimistic latch coupling) read path for bt_lookup.
 *
 * The measured read-scalability wall (bench_btree_concurrent) is the
 * SHARED content latch taken at every frame during descend_shared: a
 * shared-latch acquire/release is a contended WRITE to the latch word,
 * so pure reads bounce that cache line across cores and do not scale.
 * This path descends taking NO content latch -- it reads each node
 * under the frame's version seqlock (bm_read_begin/bm_read_valid) and
 * retries if a writer intervened -- so concurrent reads issue no shared
 * writes on the descent.  The node reads are OOB-safe on a torn page
 * (btnode_get / node_lower_bound bounds-guard every offset), so an
 * optimistic read of a page a writer is mutating produces a garbage
 * (but in-bounds) result the version re-check then discards.
 *
 * Frames are still PINNED across the read (OLC removes the latch, not
 * the pin -- a pinned frame cannot be evicted).  On repeated version
 * conflict, or any structural ambiguity the optimistic path cannot
 * resolve, the caller falls back to the latched descend_shared, which
 * is unconditionally correct -- OLC is a fast path, never a correctness
 * dependency.
 *
 * Returns XTC_OK (found, value copied), XTC_E_NOTFOUND (conclusive
 * miss), or XTC_E_AGAIN (optimism failed -- caller should fall back).
 */
#define BT_OLC_RETRIES 3
static int
bt_lookup_optimistic(bt_t *bt, const void *key, uint16_t klen,
    void *buf, uint16_t cap, uint16_t *vlen)
{
	bm_t *bm = bt->bm;
	int attempt;

	if (!BT_OLC_ENABLED)
		return XTC_E_AGAIN;              /* take the latched lookup */
	for (attempt = 0; attempt < BT_OLC_RETRIES; attempt++) {
		bm_pid_t pid = atomic_load(&bt->root_pid);
		bm_frame_t *f;
		int depth = 0;
		int conflict = 0;
		int ovf_retry = 0;   /* leaf released; ovf page freed under us */

#ifdef BM_EPOCH_FASTPATH
		/* Pin-free epoch descent: NO pin CAS on any hop -- zero shared
		 * write on the read path (Invariant 1).  Safety is by the
		 * version bracket + a pid recheck after each node read (a reload
		 * bumps version).  The whole descent is CPU-bound with no fiber
		 * yield (no content latch, no await), so no pin is needed to
		 * hold residency across a park.  A miss (NULL) or any ambiguity
		 * falls through to XTC_E_AGAIN and the pinned latched descent. */
		f = bm_fix_pid_nopin(bm, pid);
		if (f == NULL)
			return XTC_E_AGAIN;
#else
		if (bm_fix_pid(bm, pid, &f) != XTC_OK)
			return XTC_E_AGAIN;
#endif

#ifdef BM_EPOCH_FASTPATH
		bm_pid_t cur_pid = pid;   /* pid of the frame `f` currently held */
#endif

		for (;;) {
			void *pg = bm_page(f);
			uint64_t v;
			uint32_t rsib = 0;
			int is_leaf, beyond_hi, dead, below_lo;
			bm_pid_t child = BM_PID_NONE;
			int found = 0;
			uint16_t vl = 0;
			uint16_t vfl = 0;
			uint8_t oref[BT_OVF_HDR + 8u * BT_OVF_MAXPAGES];

			if (++depth > BT_MAX_HEIGHT + 4) { conflict = 1; break; }

			if (!bm_read_begin(f, &v)) {   /* writer active */
				conflict = 1; break;
			}
			/* read the node under the version bracket */
			dead = btnode_is_dead(pg);
			below_lo = btnode_below_lo_fence(pg, key, klen);
			beyond_hi = btnode_beyond_hi_fence(pg, key, klen);
			if (beyond_hi)
				rsib = btnode_right_sibling(pg);
			is_leaf = btnode_is_leaf(pg);
			if (!beyond_hi && !dead && !below_lo) {
				if (is_leaf) {
					int s = btnode_search(pg, key, klen,
					    &found);
					if (found) {
						const void *vp = NULL;
						uint16_t gl = 0;
						if (btnode_get(pg, s, NULL,
						    NULL, &vp, &gl) == 0 &&
						    vp != NULL) {
							/* Copy straight into
							 * the caller's buffer
							 * (bounded by cap and
							 * the page).  A torn
							 * copy is discarded by
							 * the version check
							 * below -- the caller
							 * sees a result only
							 * after it validates.
							 * (1.56 copied through a
							 * 1024-byte stack buffer
							 * and silently cut
							 * larger values.) */
							vl = gl;
							vfl = btnode_vflag(pg, s);
							if (vfl) {
								/* an out-of-line ref:
								 * copy it; the data is
								 * read after validate */
								if (gl <= sizeof oref)
									memcpy(oref, vp, gl);
								else
									vfl = 0, vl = 0,
									found = 0;
							} else if (buf != NULL &&
							    cap > 0)
								memcpy(buf, vp,
								    gl < cap ? gl : cap);
						}
					}
				} else {
					child = child_for_key(pg, key, klen);
				}
			}
			/* validate: did a writer touch the frame? */
			if (!bm_read_valid(f, v)) { conflict = 1; break; }
#ifdef BM_EPOCH_FASTPATH
			/* Pin-free: also confirm the frame still names the pid we
			 * descended to.  A reload bumps version (caught above), but
			 * recheck pid too as belt-and-suspenders against a version
			 * wrap -- a mismatch means the frame was reissued under us. */
			if (bm_frame_pid(f) != cur_pid) { conflict = 1; break; }
#endif

			if (dead || below_lo) { conflict = 1; break; }
			if (beyond_hi) {
				bm_frame_t *nf;
				if (rsib == 0) { conflict = 1; break; }
#ifdef BM_EPOCH_FASTPATH
				nf = bm_fix_pid_nopin(bm, (bm_pid_t)rsib);
				if (nf == NULL) { conflict = 1; break; }
				cur_pid = (bm_pid_t)rsib;
#else
				if (bm_fix_pid(bm, (bm_pid_t)rsib, &nf)
				    != XTC_OK) { conflict = 1; break; }
				bm_unfix(bm, f, 0);
#endif
				f = nf;
				continue;              /* B-link hop */
			}
			if (is_leaf) {
#ifndef BM_EPOCH_FASTPATH
				bm_unfix(bm, f, 0);
#endif
				if (!found)
					return XTC_E_NOTFOUND;
				if (vfl) {
					/* Validated ref: read its pages.  A
					 * concurrent replace may have freed them
					 * (AGAIN): retry the descent. */
					int orc = ovf_read(bt, oref, vl, buf,
					    cap, vlen);
					if (orc == XTC_E_AGAIN) {
						/* frame already released */
						ovf_retry = 1;
						break;
					}
					return orc;
				}
				if (vlen != NULL) *vlen = vl;
				return XTC_OK;
			}
			if (child == BM_PID_NONE) { conflict = 1; break; }
			{
				bm_frame_t *cf;
#ifdef BM_EPOCH_FASTPATH
				cf = bm_fix_pid_nopin(bm, child);
				if (cf == NULL) { conflict = 1; break; }
				cur_pid = child;
#else
				if (bm_fix_pid(bm, child, &cf) != XTC_OK) {
					conflict = 1; break;
				}
				bm_unfix(bm, f, 0);
#endif
				f = cf;
			}
		}
		if (ovf_retry)
			continue;          /* the leaf frame was already unfixed */
#ifndef BM_EPOCH_FASTPATH
		bm_unfix(bm, f, 0);
#endif
		if (conflict)
			continue;
	}
	return XTC_E_AGAIN;       /* optimism exhausted: caller falls back */
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
		if (btnode_vflag(pg, s)) {
			/* Out-of-line: read the data pages while still holding
			 * the leaf shared, so no replace/delete can free them
			 * (those need the leaf exclusive). */
			rc = ovf_read(bt, vp, vl, buf, cap, vlen);
			if (rc == XTC_E_AGAIN)
				rc = XTC_E_INTERNAL;   /* cannot happen under the latch */
		} else {
			if (vlen != NULL)
				*vlen = vl;
			if (buf != NULL && cap > 0) {
				uint16_t n = vl < cap ? vl : cap;
				memcpy(buf, vp, n);
			}
			rc = XTC_OK;
		}
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

	if (bt == NULL || key == NULL || klen > bt_key_max(bt))
		return XTC_E_INVAL;
	atomic_fetch_add(&bt->st_lookups, 1);

	/*
	 * OLC fast path: descend + read the leaf with no content latch,
	 * validated by the per-frame version seqlock.  This is the
	 * read-scalability path (no shared-latch cache-line bounce).  On
	 * XTC_E_AGAIN (a writer kept conflicting, or a structural
	 * ambiguity) fall back to the unconditionally-correct latched
	 * descent below -- so correctness never depends on the optimism.
	 */
	rc = bt_lookup_optimistic(bt, key, klen, buf, cap, vlen);
	if (rc != XTC_E_AGAIN)
		return rc;

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
 * Copy slot `slot`'s full key (node prefix + stored suffix) out of a page
 * read optimistically: every offset is bounds-checked against the tree's
 * page size, since a torn header can carry any value (btnode_full_key
 * trusts the header).  0, or -1 on an out-of-page cell or a short buffer.
 */
static int
olc_full_key(const bt_t *bt, const void *pg, int slot, uint8_t *out,
    uint16_t cap, uint16_t *olen)
{
	const struct btnode_hdr *h = pg;
	const void *suf = NULL;
	uint16_t sl = 0, pl = h->prefix_len;

	if (btnode_get(pg, slot, &suf, &sl, NULL, NULL) != 0 ||
	    (uint32_t)h->lo_fence_off + pl > bt->page_size ||
	    (uint32_t)pl + sl > cap)
		return -1;
	memcpy(out, (const uint8_t *)pg + h->lo_fence_off, pl);
	memcpy(out + pl, suf, sl);
	*olen = (uint16_t)(pl + sl);
	return 0;
}

/*
 * First key >= `key` with its value, read the way bt_lookup_optimistic
 * reads (no content latch: each node under its version seqlock).  The
 * leaf for `key` holds every key up to its upper fence; when none of
 * them is >= `key`, the answer is the first key of the next non-empty
 * leaf to the right, reached by the right link.  That neighbour is
 * accepted only if its lower fence equals the upper fence just left
 * (the B-link invariant: a split or a merge keeps the two equal), so a
 * link that went stale under the read is never followed.  XTC_OK,
 * XTC_E_NOTFOUND (no such key), XTC_E_INVAL (key buffer too small), or
 * XTC_E_AGAIN (optimism failed: take the latched path).
 */
static int
seek_ge_optimistic(bt_t *bt, const void *key, uint16_t klen, void *kout,
    uint16_t kcap, uint16_t *kolen, void *val, uint16_t vcap, uint16_t *vlen)
{
	bm_t *bm = bt->bm;
	uint8_t hf[BT_MAX_KEY];   /* upper fence of the leaf just left */
	uint16_t hfl = 0;
	int attempt;

	if (!BT_OLC_ENABLED)
		return XTC_E_AGAIN;
	for (attempt = 0; attempt < BT_OLC_RETRIES; attempt++) {
		bm_frame_t *f;
		int depth = 0, first = 0, orc = XTC_E_AGAIN;

		if (bm_fix_pid(bm, atomic_load(&bt->root_pid), &f) != XTC_OK)
			return XTC_E_AGAIN;
		for (;;) {
			const struct btnode_hdr *h = bm_page(f);
			const void *pg = h, *vp = NULL;
			uint64_t v;
			bm_pid_t next = BM_PID_NONE;
			int leaf, s, bad, beyond = 0, hit = 0, kbad = 0;
			uint16_t vl = 0, vfl = 0, kl = 0;
			uint8_t oref[BT_OVF_HDR + 8u * BT_OVF_MAXPAGES];

			if (++depth > 4 * BT_MAX_HEIGHT || !bm_read_begin(f, &v))
				break;
			leaf = btnode_is_leaf(pg);
			if (first)   /* the right neighbour of the leaf just left */
				bad = !leaf || h->lo_fence_len != hfl ||
				    (uint32_t)h->lo_fence_off + hfl > bt->page_size ||
				    memcmp((const uint8_t *)pg + h->lo_fence_off, hf,
				    hfl) != 0;
			else
				bad = btnode_below_lo_fence(pg, key, klen);
			bad |= btnode_is_dead(pg);
			if (!first && !bad)
				beyond = btnode_beyond_hi_fence(pg, key, klen);
			if (bad) {
				/* nothing to read: retry */
			} else if (beyond) {
				next = (bm_pid_t)btnode_right_sibling(pg);
			} else if (!leaf) {
				next = child_for_key(pg, key, klen);
			} else {
				s = first ? 0 : btnode_search(pg, key, klen, NULL);
				if (s < (int)btnode_count(pg) &&
				    btnode_get(pg, s, NULL, NULL, &vp, &vl) == 0) {
					hit = 1;
					kbad = olc_full_key(bt, pg, s, kout, kcap, &kl);
					vfl = btnode_vflag(pg, s);
					if (vfl && vl <= sizeof oref)
						memcpy(oref, vp, vl);
					else if (vfl)
						bad = 1;
					else if (val != NULL && vcap > 0)
						memcpy(val, vp, vl < vcap ? vl : vcap);
				} else {
					/* no key >= `key` here: the next leaf's first */
					next = (bm_pid_t)btnode_right_sibling(pg);
					hfl = h->hi_fence_len;
					if (next != BM_PID_NONE && (hfl > sizeof hf ||
					    (uint32_t)h->hi_fence_off + hfl > bt->page_size))
						bad = 1;
					else if (next != BM_PID_NONE)
						memcpy(hf, (const uint8_t *)pg + h->hi_fence_off,
						    hfl);
				}
			}
			if (!bm_read_valid(f, v) || bad)
				break;
			if (hit) {
				bm_unfix(bm, f, 0);
				f = NULL;
				if (kbad)
					return XTC_E_INVAL;
				if (kolen != NULL)
					*kolen = kl;
				if (vfl) {
					/* a validated ref: a concurrent replace may
					 * have freed its pages (AGAIN): retry */
					orc = ovf_read(bt, oref, vl, val, vcap, vlen);
					break;
				}
				if (vlen != NULL)
					*vlen = vl;
				return XTC_OK;
			}
			if (leaf && !beyond) {
				if (next == BM_PID_NONE) {   /* the rightmost leaf */
					bm_unfix(bm, f, 0);
					return XTC_E_NOTFOUND;
				}
				first = 1;
			}
			if (next == BM_PID_NONE)
				break;
			{
				bm_frame_t *nf;
				if (bm_fix_pid(bm, next, &nf) != XTC_OK)
					break;
				bm_unfix(bm, f, 0);
				f = nf;
			}
		}
		if (f != NULL)
			bm_unfix(bm, f, 0);
		if (orc != XTC_E_AGAIN)
			return orc;
	}
	return XTC_E_AGAIN;
}

int
bt_seek_ge(bt_t *bt, const void *key, uint16_t klen, void *kout,
    uint16_t kcap, uint16_t *kolen, void *val, uint16_t vcap, uint16_t *vlen)
{
	bt_cursor_t *c = NULL;
	const void *k = NULL, *vp = NULL;
	uint16_t kl = 0, vl = 0;
	int rc;

	if (bt == NULL || key == NULL || kout == NULL || klen > bt_key_max(bt))
		return XTC_E_INVAL;
	/* Not counted in st_lookups (as bt_seek_lt is not): one shared
	 * counter write per point read was 2% of a busy 8-reader profile. */
	rc = seek_ge_optimistic(bt, key, klen, kout, kcap, kolen, val, vcap,
	    vlen);
	if (rc != XTC_E_AGAIN)
		return rc;
	/* The latched path: a cursor positioned at the first key >= key. */
	if ((rc = bt_cursor_open(bt, key, klen, &c)) != XTC_OK)
		return rc;
	rc = bt_cursor_next(c, &k, &kl, &vp, &vl);
	if (rc == XTC_OK && kl > kcap)
		rc = XTC_E_INVAL;
	if (rc == XTC_OK) {
		memcpy(kout, k, kl);
		if (kolen != NULL)
			*kolen = kl;
		if (val != NULL && vcap > 0)
			memcpy(val, vp, vl < vcap ? vl : vcap);
		if (vlen != NULL)
			*vlen = vl;
	}
	bt_cursor_close(c);
	return rc;
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
		(void)bt_write_super(bt);      /* root pid changed: persist it */
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
	/*
	 * If C and its right sibling do not fit in one page, try C with its
	 * LEFT sibling instead.  Deleting keys in ascending order (a queue:
	 * the oldest rows go first) empties C while its right sibling is
	 * still untouched; once append splits fill pages, that sibling is
	 * full, the right pair never fits, and the space was never given
	 * back.  The left sibling is the one the stream already emptied.
	 */
	if (lslot == cslot && cslot - 1 >= 0) {
		bm_frame_t *tf = NULL, *tr = NULL;
		int fits = 1;
		if (bm_fix_pid(bm, child_pid_at(pp, cslot), &tf) == XTC_OK) {
			bm_latch_shared(tf);
			if (bm_fix_pid(bm, child_pid_at(pp, cslot + 1), &tr) == XTC_OK) {
				bm_latch_shared(tr);
				fits = btnode_merge_fits(bm_page(tf), bm_page(tr));
				bm_unlatch(tr); bm_unfix(bm, tr, 0);
			}
			bm_unlatch(tf); bm_unfix(bm, tf, 0);
		}
		if (!fits) {
			lslot = cslot - 1;
			rslot = cslot;
		}
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
		void *tmp = bt->smo_scratch;   /* under the SMO lock */
		int i, rn;
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
	BT_CHECK_NODE(lp);
	BT_CHECK_NODE(pp);
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

	/* Transfer pids from earlier merges to the reusable stack. The
	 * buffer manager defers this while old pinned/writeback owners
	 * remain. Holding the SMO lock serializes structure changes, not
	 * arbitrary parked descents: retired membership still rejects
	 * disk reloads until an allocator publishes a replacement. */
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
	uint8_t oldref[BT_OVF_HDR + 8u * BT_OVF_MAXPAGES];
	uint32_t oldlen = 0;

	if (bt == NULL || key == NULL || klen > bt_key_max(bt))
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
		if (rc == XTC_E_AGAIN) {   /* root pid quarantined: retry */
			f = NULL;
			xtc_yield();
			continue;
		}
		if (rc != XTC_OK)
			return rc;
		/* Internal levels are read SHARED: a delete only modifies the
		 * leaf.  (1.56 took every level EXCLUSIVE, root included, which
		 * made each in-flight delete fail every optimistic lookup at the
		 * root and park every other loop's descent on the root latch --
		 * the RethinkDB root-write-lock pattern; bench_btree_mixed.) */
		bm_latch_shared(f);
		(void)move_right(bt, &f, key, klen, 0);
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
			if (rc == XTC_E_AGAIN) {
				/* Child pid quarantined by a concurrent merge: the
				 * page was freed under us; restart from the root. */
				stale = 1;
				break;
			}
			if (rc != XTC_OK) {
				bm_unlatch(f); bm_unfix(bm, f, 0);
				return rc;
			}
			bm_latch_shared(cf);
			bm_unlatch(f);             /* release before latching deeper */
			bm_unfix(bm, f, 0);
			f = cf;
			(void)move_right(bt, &f, key, klen, 0);
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
		/* f is the leaf, held SHARED.  Upgrade: release and re-latch it
		 * exclusive (an arwlock cannot upgrade in place), then walk right
		 * again -- the leaf may have split in the window.  The stale
		 * guards below re-check dead / below-lo-fence under the
		 * exclusive latch, so a merge in the window restarts. */
		bm_unlatch(f);
		bm_latch_exclusive(f);
		(void)move_right(bt, &f, key, klen, 1);
		pg = bm_page(f);

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
		oldlen = take_ovf_ref(pg, s, oldref);
		(void)btnode_remove(pg, s);
		BT_CHECK_NODE(pg);
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
			if (oldlen > 0)
				ovf_free(bt, oldref, oldlen);   /* ref is gone */
			/* Merge/reclaim on underflow is always on: the merge SMO
			 * is concurrency-safe (the internal-split hi-fence fix
			 * closed the last race; see the churn-gone gate in
			 * test_btree_delete_merge and .agent/M_SQLXTC_BTREE_MERGE
			 * .md). */
			if (underflow)
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
	oldlen = take_ovf_ref(pg, s, oldref);
	(void)btnode_remove(pg, s);
	BT_CHECK_NODE(pg);
	bm_unlatch(f); bm_unfix(bm, f, 1);
	(void)xtc_arwlock_unlock(bt->smo);
	if (oldlen > 0)
		ovf_free(bt, oldref, oldlen);
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
	int rc;

	atomic_fetch_add(&bt->st_descents, 1);
	if (!BT_STALE_TEST_HOOK_PEEK() && descend_optimistic(bt, start, klen, out))
		return XTC_OK;

	/*
	 * Latch-free retries until a live leaf (like descend_shared): a concurrent
	 * merge can unlink a node we are descending through, or absorb its
	 * range into a LEFT sibling -- which move_right cannot follow.  We
	 * revalidate dead/lo-fence at EVERY level and restart from the root
	 * on a hit so the cursor never opens onto a merged-away page.  With
	 * start == NULL there is no routing key, so only the dead flag can
	 * fire (the leftmost child is never absorbed leftward).  Unbounded
	 * for the reason given in descend_shared: the frame handed back must
	 * be the one this descent still holds.
	 */
	for (;;) {
		int stale = 0;

		pid = atomic_load(&bt->root_pid);
		rc = bm_fix_pid(bm, pid, &f);
		if (rc == XTC_E_AGAIN) {   /* root pid quarantined: retry */
			xtc_yield();
			continue;
		}
		if (rc != XTC_OK)
			return rc;
		bm_latch_shared(f);
		(void)move_right(bt, &f, start, klen, 0);
		pg = bm_page(f);
		if (BT_STALE_TEST_HOOK() || btnode_is_dead(pg) ||
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
			if (rc == XTC_E_AGAIN) {
				/* Child pid quarantined by a concurrent merge:
				 * restart from the root. */
				stale = 1;
				break;
			}
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
}

/*
 * Largest key strictly less than `hi` (O(log n); there is no reverse
 * cursor).  Descends to the leaf that would hold `hi` and takes the slot
 * before hi's insertion point.  When that leaf has no smaller key (hi is
 * at its front, or the leaf is empty), every smaller key lives in a leaf
 * to the LEFT, whose keys are all <= this leaf's (exclusive) lower fence.
 * So retry with the fence as an INCLUSIVE bound: the fence routes left
 * (keys <= a separator descend left), and the fence itself may be a live
 * key (the left sibling's largest key at split time), which a strict
 * "< fence" would skip.  A leftmost leaf (lower fence -infinity) with
 * nothing below the bound means there is no such key.  The key is copied to `out` (cap bytes); the
 * value (if `val` != NULL) to `val` (vcap bytes), whole for out-of-line
 * values.  Returns XTC_OK, XTC_E_NOTFOUND, or XTC_E_INVAL.
 *
 * RethinkDB keeps a per-table population block for this kind of
 * question; we answer it from the tree in one or two descents.
 */
int
bt_seek_lt(bt_t *bt, const void *hi, uint16_t hlen, void *out,
    uint16_t cap, uint16_t *olen, void *val, uint16_t vcap, uint16_t *vlen)
{
	uint8_t bound[BT_MAX_KEY + 1];
	uint16_t blen;
	int guard, incl = 0;   /* 0: keys < bound; 1: keys <= bound */

	if (bt == NULL || hi == NULL || out == NULL || hlen > bt_key_max(bt))
		return XTC_E_INVAL;
	memcpy(bound, hi, hlen);
	blen = hlen;
	for (guard = 0; guard < 1 << 20; guard++) {
		bm_frame_t *f = NULL;
		void *pg;
		int s, found = 0, rc;

		rc = descend_shared(bt, bound, blen, &f);
		if (rc != XTC_OK)
			return rc;
		pg = bm_page(f);
		s = btnode_search(pg, bound, blen, &found);
		if (incl && found)
			s++;                    /* the bound itself qualifies */
		if (s > 0) {
			const void *vp = NULL;
			uint16_t vl = 0, kl = 0;

			if (btnode_full_key(pg, s - 1, out, cap, &kl) != 0) {
				bm_unlatch(f); bm_unfix(bt->bm, f, 0);
				return XTC_E_INVAL;     /* caller's buffer too small */
			}
			if (olen != NULL)
				*olen = kl;
			rc = XTC_OK;
			if (val != NULL || vlen != NULL) {
				(void)btnode_get(pg, s - 1, NULL, NULL, &vp, &vl);
				if (btnode_vflag(pg, s - 1)) {
					rc = ovf_read(bt, vp, vl, val, vcap, vlen);
				} else {
					if (vlen != NULL)
						*vlen = vl;
					if (val != NULL && vcap > 0)
						memcpy(val, vp, vl < vcap ? vl : vcap);
				}
			}
			bm_unlatch(f); bm_unfix(bt->bm, f, 0);
			return rc;
		}
		/* Nothing below `bound` in this leaf: move the bound to just
		 * above this leaf's lower fence and look left. */
		{
			uint16_t ll = 0;

			if (btnode_lo_fence(pg, bound, BT_MAX_KEY, &ll) != 0) {
				bm_unlatch(f); bm_unfix(bt->bm, f, 0);
				return XTC_E_INTERNAL;
			}
			bm_unlatch(f); bm_unfix(bt->bm, f, 0);
			if (ll == 0)
				return XTC_E_NOTFOUND;  /* leftmost leaf: none */
			blen = ll;
			incl = 1;
			/* Progress: the next leaf's fence is strictly smaller, so
			 * the loop terminates at the leftmost leaf. */
		}
	}
	return XTC_E_INTERNAL;
}

/*
 * Up to `max` keys strictly below `hi`, largest first, from ONE descent
 * (bt_seek_lt answers one key per descent; a descending range walk paid
 * a root-to-leaf descent per entry).  Copies the leaf's keys below the
 * bound, walking left only as bt_seek_lt does when the leaf holds none.
 * Key i goes to buf + off[i], its length to lens[i]; the copies stop
 * when `bcap` bytes would be exceeded.  Values are not returned (index
 * entries carry none).  *n gets the count (0: no key below `hi`).
 * XTC_OK, or an error.
 */
int
bt_seek_lt_n(bt_t *bt, const void *hi, uint16_t hlen, void *buf,
    uint32_t bcap, uint32_t *off, uint16_t *lens, int max, int *n)
{
	uint8_t bound[BT_MAX_KEY + 1];
	uint16_t blen;
	int guard, incl = 0;

	if (bt == NULL || hi == NULL || buf == NULL || n == NULL || max < 1 ||
	    hlen > bt_key_max(bt))
		return XTC_E_INVAL;
	*n = 0;
	memcpy(bound, hi, hlen);
	blen = hlen;
	for (guard = 0; guard < 1 << 20; guard++) {
		bm_frame_t *f = NULL;
		void *pg;
		int s, found = 0, rc;
		uint16_t ll = 0;
		uint32_t used = 0;

		rc = descend_shared(bt, bound, blen, &f);
		if (rc != XTC_OK)
			return rc;
		pg = bm_page(f);
		s = btnode_search(pg, bound, blen, &found);
		if (incl && found)
			s++;                    /* the bound itself qualifies */
		for (; s > 0 && *n < max; s--) {
			uint16_t kl = 0;
			if (btnode_full_key(pg, s - 1, (uint8_t *)buf + used,
			    (uint16_t)(bcap - used > 0xFFFF ? 0xFFFF : bcap - used),
			    &kl) != 0)
				break;          /* buffer full */
			off[*n] = used;
			lens[*n] = kl;
			used += kl;
			(*n)++;
		}
		if (*n > 0 || s > 0) {
			bm_unlatch(f); bm_unfix(bt->bm, f, 0);
			return *n > 0 ? XTC_OK : XTC_E_INVAL;
		}
		/* Nothing below `bound` in this leaf: look left (bt_seek_lt). */
		if (btnode_lo_fence(pg, bound, BT_MAX_KEY, &ll) != 0) {
			bm_unlatch(f); bm_unfix(bt->bm, f, 0);
			return XTC_E_INTERNAL;
		}
		bm_unlatch(f); bm_unfix(bt->bm, f, 0);
		if (ll == 0)
			return XTC_OK;          /* leftmost leaf: none (*n == 0) */
		blen = ll;
		incl = 1;
	}
	return XTC_E_INTERNAL;
}

/* ---- key-distribution sample (RethinkDB get_distribution) ---------- */

struct bt_sample {
	bt_t          *bt;
	const uint8_t *lo, *hi;    /* [lo, hi); NULL = unbounded */
	uint16_t       lolen, hilen;
	int            depth;
	uint8_t       *buf;        /* found keys, concatenated */
	size_t         bused, bcap;
	uint32_t      *off;        /* key i starts at buf + off[i] */
	uint16_t      *len;
	uint32_t       n, ncap;
	int            leaf_below; /* the level being emitted is the leaves'
	                            * parent level (all leaves are equally deep) */
};

static int
sample_in_range(const struct bt_sample *sm, const void *k, uint16_t kl)
{
	return (sm->lo == NULL || key_cmp(k, kl, sm->lo, sm->lolen) >= 0) &&
	    (sm->hi == NULL || key_cmp(k, kl, sm->hi, sm->hilen) < 0);
}

static int
sample_add(struct bt_sample *sm, const void *k, uint16_t kl)
{
	void *nb;

	if (!sample_in_range(sm, k, kl))
		return XTC_OK;
	if (sm->n == sm->ncap) {
		uint32_t nc = sm->ncap ? sm->ncap * 2 : 256;

		if ((nb = realloc(sm->off, nc * sizeof *sm->off)) == NULL)
			return XTC_E_NOMEM;
		sm->off = nb;
		if ((nb = realloc(sm->len, nc * sizeof *sm->len)) == NULL)
			return XTC_E_NOMEM;
		sm->len = nb;
		sm->ncap = nc;
	}
	if (sm->bused + kl > sm->bcap) {
		size_t nc = sm->bcap ? sm->bcap * 2 : 16384;

		while (nc < sm->bused + kl)
			nc *= 2;
		if ((nb = realloc(sm->buf, nc)) == NULL)
			return XTC_E_NOMEM;
		sm->buf = nb;
		sm->bcap = nc;
	}
	memcpy(sm->buf + sm->bused, k, kl);
	sm->off[sm->n] = (uint32_t)sm->bused;
	sm->len[sm->n] = kl;
	sm->bused += kl;
	sm->n++;
	return XTC_OK;
}

/* A root leaf has no separators: its keys are the sample. */
static int
sample_leaf(struct bt_sample *sm, const void *pg)
{
	uint8_t k[BT_MAX_KEY];
	uint16_t kl = 0;
	int i, cnt = (int)btnode_count(pg), rc = XTC_OK;

	for (i = 0; i < cnt && rc == XTC_OK; i++)
		if (btnode_full_key(pg, i, k, sizeof k, &kl) == 0)
			rc = sample_add(sm, k, kl);
	return rc;
}

/* Add separators 1..cnt-1 (slot 0 is the -infinity key) of internal
 * node `pg` that lie in range. */
static int
sample_seps(struct bt_sample *sm, const void *pg)
{
	uint8_t k[BT_MAX_KEY];
	uint16_t kl = 0;
	int i, cnt = (int)btnode_count(pg), rc = XTC_OK;

	for (i = 1; i < cnt && rc == XTC_OK; i++)
		if (btnode_full_key(pg, i, k, sizeof k, &kl) == 0)
			rc = sample_add(sm, k, kl);
	return rc;
}

/*
 * Visit the shared-latched internal node `pg` at `level` (0 = root).  At
 * the depth limit, or once its children are known to be leaves, it
 * contributes its own separators.  Otherwise each child whose range
 * (sep_i, sep_i+1] meets [lo, hi) is visited in order, and the separator
 * between two visited children -- a boundary of the deeper level too --
 * is emitted between them, so the output is strictly ascending.  Leaf
 * keys are never emitted (a parent separator can equal a live leaf key):
 * the first leaf child met only teaches the walk where the leaf level is,
 * which costs one page read per call.  The child is latched before its
 * parent is released and every latch is taken top-down (the order every
 * descent uses), so a merge -- which needs the parent exclusive -- cannot
 * free a child under the walk.  Returns XTC_OK, XTC_E_AGAIN (a dead
 * node: restart), or an error.
 */
static int
sample_node(struct bt_sample *sm, void *pg, int level)
{
	bm_t *bm = sm->bt->bm;
	uint8_t k[BT_MAX_KEY], nk[BT_MAX_KEY];
	uint16_t kl = 0, nkl = 0;
	int i, cnt = (int)btnode_count(pg), rc = XTC_OK;

	if (level >= sm->depth || sm->leaf_below)
		return sample_seps(sm, pg);
	for (i = 0; i < cnt && rc == XTC_OK; i++) {
		bm_frame_t *cf;
		void *cp;

		/* k = sep_i (child i's exclusive lower bound), nk = sep_i+1. */
		if (i > 0) {
			memcpy(k, nk, nkl);
			kl = nkl;
		}
		if (i + 1 < cnt &&
		    btnode_full_key(pg, i + 1, nk, sizeof nk, &nkl) != 0)
			return XTC_E_INTERNAL;
		if (i > 0 && sm->hi != NULL &&
		    key_cmp(k, kl, sm->hi, sm->hilen) >= 0)
			break;                  /* this and later children >= hi */
		if (i + 1 < cnt && sm->lo != NULL &&
		    key_cmp(nk, nkl, sm->lo, sm->lolen) < 0)
			continue;               /* child entirely below lo */
		rc = bm_fix_pid(bm, child_pid_at(pg, i), &cf);
		if (rc != XTC_OK)
			return rc;
		bm_latch_shared(cf);
		atomic_fetch_add(&sm->bt->st_sample_pages, 1);
		cp = bm_page(cf);
		if (btnode_is_dead(cp)) {
			rc = XTC_E_AGAIN;
		} else if (btnode_is_leaf(cp)) {
			/* The first child this node visits: nothing of this
			 * node was emitted yet, so emit its separators. */
			sm->leaf_below = 1;
			rc = sample_seps(sm, pg);
			i = cnt;
		} else {
			if (i > 0)
				rc = sample_add(sm, k, kl);
			if (rc == XTC_OK)
				rc = sample_node(sm, cp, level + 1);
		}
		bm_unlatch(cf);
		bm_unfix(bm, cf, 0);
	}
	return rc;
}

int
bt_sample_keys(bt_t *bt, const void *lo, uint16_t lolen, const void *hi,
    uint16_t hilen, int depth, void *keys_out, uint16_t key_cap,
    uint16_t *lens_out, int max_keys, uint32_t *n_out)
{
	struct bt_sample sm;
	uint32_t i, m = 0;
	int rc;

	if (bt == NULL || n_out == NULL || depth < 0 || max_keys < 0 ||
	    (max_keys > 0 && (keys_out == NULL || lens_out == NULL)))
		return XTC_E_INVAL;
	*n_out = 0;
	memset(&sm, 0, sizeof sm);
	sm.bt = bt;
	sm.lo = lolen ? lo : NULL;
	sm.lolen = lolen;
	sm.hi = hilen ? hi : NULL;
	sm.hilen = hilen;
	sm.depth = depth < BT_MAX_HEIGHT ? depth : BT_MAX_HEIGHT;
	for (;;) {
		bm_pid_t pid = atomic_load(&bt->root_pid);
		bm_frame_t *f;

		sm.n = 0;
		sm.bused = 0;
		sm.leaf_below = 0;
		rc = bm_fix_pid(bt->bm, pid, &f);
		if (rc == XTC_E_AGAIN) {   /* root collapsed + quarantined */
			xtc_yield();
			continue;
		}
		if (rc != XTC_OK)
			break;
		bm_latch_shared(f);
		atomic_fetch_add(&bt->st_sample_pages, 1);
		/* Dead, or no longer the root (it grew: its right half would
		 * be missed): restart. */
		if (btnode_is_dead(bm_page(f)) || pid != atomic_load(&bt->root_pid))
			rc = XTC_E_AGAIN;
		else if (btnode_is_leaf(bm_page(f)))
			rc = sample_leaf(&sm, bm_page(f));   /* no separators yet */
		else
			rc = sample_node(&sm, bm_page(f), 0);
		bm_unlatch(f);
		bm_unfix(bt->bm, f, 0);
		if (rc != XTC_E_AGAIN)
			break;
		xtc_yield();           /* a merge raced the walk: restart */
	}
	if (rc == XTC_OK) {
		/* Evenly thin n found keys to at most max_keys, skipping keys
		 * the caller cannot hold. */
		for (i = 0; i < (uint32_t)max_keys && i < sm.n; i++) {
			uint32_t j = sm.n <= (uint32_t)max_keys ? i :
			    (uint32_t)(((uint64_t)(i + 1) * sm.n) /
			    ((uint64_t)max_keys + 1));

			if (sm.len[j] > key_cap)
				continue;
			memcpy((uint8_t *)keys_out + (size_t)m * key_cap,
			    sm.buf + sm.off[j], sm.len[j]);
			lens_out[m++] = sm.len[j];
		}
		*n_out = m;
	}
	free(sm.buf);
	free(sm.off);
	free(sm.len);
	return rc;
}

int
bt_cursor_open(bt_t *bt, const void *start, uint16_t klen, bt_cursor_t **out)
{
	bt_cursor_t *c;
	bm_frame_t *leaf = NULL;
	int rc;

	if (bt == NULL || out == NULL ||
	    (start != NULL && klen > bt_key_max(bt)))
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
		/* Remember the start as the scan position, so a park or a
		 * stale-link re-descent before the first key resumes here. */
		if (klen <= sizeof c->keybuf) {
			memcpy(c->keybuf, start, klen);
			c->keylen = klen;
		}
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

static int cursor_seek(bt_cursor_t *c, bm_frame_t *f);

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
			if (btnode_vflag(pg, c->slot)) {
				/* Out-of-line value: assemble it while the leaf
				 * is held shared (no replace/delete can free its
				 * pages until we release). */
				uint16_t tl = 0;
				int orc;

				if (c->vbuf == NULL &&
				    (c->vbuf = malloc(0xFFFFu)) == NULL)
					return XTC_E_NOMEM;
				orc = ovf_read(c->bt, vp, vl, c->vbuf, 0xFFFFu, &tl);
				if (orc != XTC_OK)
					return XTC_E_INTERNAL;
				vp = c->vbuf;
				vl = tl;
			}
			c->slot++;
			c->have_key = 1;
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
			int frc;

			bm_unlatch(c->leaf);
			bm_unfix(bm, c->leaf, 0);
			c->leaf = NULL;
			if (rs == 0) {
				c->done = 1;
				return XTC_E_NOTFOUND;
			}
			frc = bm_fix_pid(bm, (bm_pid_t)rs, &nf);
			if (frc == XTC_E_AGAIN) {
				/* The right sibling pid was quarantined by a
				 * concurrent merge: the link is stale.  Re-descend
				 * from the scan position to find the leaf that now
				 * owns the next key, then continue the scan. */
				bm_frame_t *rf = NULL;

				if (cursor_descend(c->bt,
				    c->keylen ? c->keybuf : NULL, c->keylen,
				    &rf) != XTC_OK || cursor_seek(c, rf) != XTC_OK) {
					c->done = 1;
					return XTC_E_NOTFOUND;
				}
				continue;
			}
			if (frc != XTC_OK) {
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
 * Position the cursor in `f` (fixed + shared-latched) just past the
 * scan position (at it, if no key has been returned yet).  `f` is only
 * a candidate -- the parked leaf, or a leaf reached by a descent -- and
 * may no longer own the position:
 *   - a split while parked moves the upper half of the leaf, possibly
 *     including the position key, to a new right sibling.  The parked
 *     pid still names the left half; searching it for the key returns
 *     its slot count, and the scan would then restart at the right
 *     sibling's slot 0, yielding again every key between the split
 *     separator and the position.  Walk right (move_right) instead.
 *   - a merge may have freed the page, and its pid been reissued as any
 *     node.  If it is not a live leaf, or the position is at/below its
 *     lower fence (its range moved left), re-descend from the root.
 * Takes ownership of `f`.  Returns XTC_OK, or XTC_E_NOTFOUND (done) if
 * a needed re-descent fails.
 */
static int
cursor_seek(bt_cursor_t *c, bm_frame_t *f)
{
	bm_t *bm = c->bt->bm;
	const void *k = c->keylen ? c->keybuf : NULL;
	void *pg = bm_page(f);
	int slot, found = 0;

	if (!btnode_is_leaf(pg) || btnode_is_dead(pg) ||
	    (k != NULL && btnode_below_lo_fence(pg, k, c->keylen))) {
		bm_unlatch(f);
		bm_unfix(bm, f, 0);
		if (cursor_descend(c->bt, k, c->keylen, &f) != XTC_OK) {
			c->done = 1;
			return XTC_E_NOTFOUND;
		}
	}
	(void)move_right(c->bt, &f, k, c->keylen, 0);
	if (k != NULL && btnode_beyond_hi_fence(bm_page(f), k, c->keylen) &&
	    btnode_right_sibling(bm_page(f)) != 0) {
		/* move_right could not fix the sibling (merged away):
		 * re-descend, which retries past the merge. */
		bm_unlatch(f);
		bm_unfix(bm, f, 0);
		if (cursor_descend(c->bt, k, c->keylen, &f) != XTC_OK) {
			c->done = 1;
			return XTC_E_NOTFOUND;
		}
	}
	slot = btnode_search(bm_page(f), c->keybuf, c->keylen, &found);
	c->leaf = f;
	c->slot = (found && c->have_key) ? slot + 1 : slot;
	return XTC_OK;
}

/*
 * Resume a parked cursor without re-descending the tree: re-fix the
 * remembered leaf, shared-latch it, and position just past the last
 * key returned (cursor_seek: walks right if a split moved that key to a
 * new sibling, re-descends if the page was merged away).  O(1)
 * amortized vs the O(log n) of a fresh bt_cursor_open.
 */
int
bt_cursor_resume(bt_cursor_t *c)
{
	bm_t *bm;
	bm_frame_t *nf = NULL;
	int rc;

	if (c == NULL)
		return XTC_E_INVAL;
	if (!c->parked)
		return XTC_OK;             /* nothing parked: leaf still held, or done */
	c->parked = 0;
	bm = c->bt->bm;
	if (bm_fix_pid(bm, c->parked_pid, &nf) != XTC_OK) {
		/* The parked leaf was reclaimed (its pid quarantined) by a
		 * concurrent merge, or could not be re-fixed: fall back to a
		 * fresh root-to-leaf descent from the scan position. */
		if (cursor_descend(c->bt, c->keylen ? c->keybuf : NULL,
		    c->keylen, &nf) != XTC_OK) {
			c->done = 1;
			return XTC_E_NOTFOUND;
		}
	} else
		bm_latch_shared(nf);
	rc = cursor_seek(c, nf);
	atomic_fetch_add(&c->bt->st_resumes, 1);
	return rc;
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
	free(c->vbuf);
	free(c);
}

/*
 * Merge/reclaim on delete underflow is ALWAYS ON.  The merge
 * structure-modification is concurrency-safe: the descent-level
 * dead/fence revalidation and the internal-split hi-fence fix closed
 * the last races (see the churn-gone gate in test_btree_delete_merge
 * and .agent/M_SQLXTC_BTREE_MERGE.md), so there is no opt-out -- a
 * delete that empties a node merges and reclaims it.
 */
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
	out->ovf_writes = atomic_load(&bt->st_ovf_writes);
	out->ovf_reads = atomic_load(&bt->st_ovf_reads);
	out->ovf_pages = atomic_load(&bt->st_ovf_pages);
	out->ovf_freed = atomic_load(&bt->st_ovf_freed);
	out->sample_pages = atomic_load(&bt->st_sample_pages);
}
