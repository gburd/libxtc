/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/btnode.c
 *	Slotted-page B-tree node with prefix compression.  See btnode.h
 *	for the layout and the public contract.
 *
 *	The design follows leanstore's BTreeNode (prefix derived from
 *	the fence keys, per-slot 2-byte "head" for fast ordering, key
 *	suffixes + values packed from the back of the page) and the
 *	classic slotted page (slot array grows
 *	from the front, cells from the back).
 */

#include "btnode.h"
#include "btovf.h"      /* out-of-line ref shape, for btnode_check */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(struct btnode_slot) == 8,
    "btnode_slot is on disk: 8 bytes (format XBT3)");
_Static_assert(sizeof(struct btnode_hdr) == 48,
    "btnode_hdr is on disk: the insert history must stay in the padding");
_Static_assert(offsetof(struct btnode_hdr, page_cksum) == BTNODE_CKSUM_OFF,
    "the buffer manager writes the page checksum at BTNODE_CKSUM_OFF");
_Static_assert(BT_OVF_PGHDR >= BTNODE_CKSUM_OFF + 8,
    "an out-of-line value page keeps its payload past the checksum");

#define HDR(p) ((struct btnode_hdr *)(p))
/* A slot's val_len carries a flag bit (BTNODE_VFLAG, see btnode.h); the
 * stored byte count is the low 15 bits.  Every size computation masks. */
#define VLEN(v) ((uint16_t)((v) & 0x7FFFu))
#define VFLAG(v) ((uint16_t)((v) & BTNODE_VFLAG))
#define CHDR(p) ((const struct btnode_hdr *)(p))

static struct btnode_slot *
slots_of(void *page)
{
	return (struct btnode_slot *)((uint8_t *)page +
	    sizeof(struct btnode_hdr));
}

static const struct btnode_slot *
cslots_of(const void *page)
{
	return (const struct btnode_slot *)((const uint8_t *)page +
	    sizeof(struct btnode_hdr));
}

/* First two bytes of `k` packed big-endian, zero padded for short keys. */
#define HEAD_LEN 2
static uint16_t
suffix_head(const uint8_t *k, uint16_t len)
{
	uint16_t h = 0;

	if (len > 0)
		h = (uint16_t)(k[0] << 8);
	if (len > 1)
		h |= k[1];
	return h;
}

/* Lexicographic compare of two byte strings.  Returns -1, 0 or 1. */
static int
cmp_bytes(const uint8_t *a, uint16_t alen, const uint8_t *b, uint16_t blen)
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

/* Byte offset of the end of the slot array (start of free space). */
static uint16_t
slots_end(const struct btnode_hdr *h)
{
	return (uint16_t)(sizeof(struct btnode_hdr) +
	    (size_t)h->count * sizeof(struct btnode_slot));
}

/* Contiguous free bytes between the slot array and the cell heap. */
static uint16_t
free_contig(const struct btnode_hdr *h)
{
	return (uint16_t)(h->data_offset - slots_end(h));
}

/* Free bytes that would exist after squeezing out deleted-cell holes. */
static uint16_t
free_compact(const struct btnode_hdr *h)
{
	return (uint16_t)(h->page_size - slots_end(h) - h->space_used);
}

/*
 * Write the key SUFFIX `skey` (the full key minus the node prefix) plus
 * `val` into slot `idx`'s cell and fill in the slot fields.  Does NOT
 * touch count or shift slots -- the caller positions the slot first.
 */
static void
node_store_f(void *page, uint16_t idx, const uint8_t *skey, uint16_t sklen,
    const uint8_t *val, uint16_t val_len, uint16_t vflag)
{
	struct btnode_hdr *h = HDR(page);
	struct btnode_slot *s = slots_of(page);
	uint8_t *p = page;
	uint16_t space = (uint16_t)(sklen + val_len);

	h->data_offset = (uint16_t)(h->data_offset - space);
	h->space_used = (uint16_t)(h->space_used + space);
	s[idx].offset = h->data_offset;
	s[idx].key_len = sklen;
	s[idx].val_len = (uint16_t)(val_len | vflag);
	s[idx].head = suffix_head(skey, sklen);
	memcpy(p + h->data_offset, skey, sklen);
	memcpy(p + h->data_offset + sklen, val, val_len);
}


/*
 * Reclaim deleted-cell holes by rebuilding the heap into a scratch copy
 * of the page and copying it back.  The prefix is unchanged (same
 * fences) so suffixes are copied verbatim.  Uses a stack buffer; no
 * heap allocation.
 */
static void
node_compact(void *page)
{
	struct btnode_hdr *h = HDR(page);
	uint8_t *p = page;
	uint32_t ps = h->page_size;
	uint8_t scratch[ps];
	struct btnode_hdr *th = HDR(scratch);
	struct btnode_slot *src = slots_of(p);
	struct btnode_slot *dst;
	const uint8_t *lo, *hi;
	uint16_t i;

	btnode_init(scratch, ps, h->is_leaf);
	lo = h->lo_fence_len ? p + h->lo_fence_off : NULL;
	hi = h->hi_fence_len ? p + h->hi_fence_off : NULL;
	btnode_set_fences(scratch, lo, h->lo_fence_len, hi, h->hi_fence_len);
	th->right_sibling = h->right_sibling;

	dst = slots_of(scratch);
	for (i = 0; i < h->count; i++) {
		uint16_t kv = (uint16_t)(src[i].key_len + VLEN(src[i].val_len));

		th->data_offset = (uint16_t)(th->data_offset - kv);
		th->space_used = (uint16_t)(th->space_used + kv);
		dst[i].offset = th->data_offset;
		dst[i].key_len = src[i].key_len;
		dst[i].val_len = src[i].val_len;
		dst[i].head = src[i].head;
		memcpy(scratch + th->data_offset, p + src[i].offset, kv);
	}
	th->count = h->count;
	memcpy(p, scratch, ps);
}

/*
 * Binary search.  Returns the insertion point in [0, count]; sets
 * *found (if non-NULL) to 1 on an exact match.  Compares the key prefix
 * against the node prefix first, then orders suffixes by `head`,
 * falling back to a full byte compare only when heads tie on a key
 * longer than HEAD_LEN bytes.
 */
static int
node_lower_bound(const void *page, const uint8_t *key, uint16_t key_len,
    int *found)
{
	const struct btnode_hdr *h = CHDR(page);
	const struct btnode_slot *s = cslots_of(page);
	const uint8_t *p = page;
	uint16_t pl = h->prefix_len;
	const uint8_t *skey;
	uint16_t sklen;
	uint16_t khead;
	uint16_t lo = 0;
	uint16_t hi = h->count;

	if (found != NULL)
		*found = 0;

	/* OLC bounds guards.  Under optimistic reads this may run on a page
	 * a writer is mid-mutating, so no header field can be trusted to be
	 * in-range.  Clamp the slot count so the binary search never reads
	 * a slot past the page, and skip the prefix compare if the lo-fence
	 * bytes would fall outside the page.  A torn read produces a
	 * meaningless (but in-bounds) result; the OLC caller re-validates
	 * the frame version and retries.  Well-formed pages are unaffected. */
	{
		size_t max_slots = ((size_t)h->page_size > sizeof(*h))
		    ? ((size_t)h->page_size - sizeof(*h)) /
		      sizeof(struct btnode_slot)
		    : 0;
		if ((size_t)hi > max_slots)
			hi = (uint16_t)max_slots;
	}

	if (pl > 0 &&
	    (size_t)h->lo_fence_off + pl <= (size_t)h->page_size) {
		const uint8_t *pref = p + h->lo_fence_off;
		uint16_t cl = key_len < pl ? key_len : pl;
		int c = cl ? memcmp(key, pref, cl) : 0;

		if (c < 0)
			return 0;
		if (c > 0)
			return (int)hi;
		if (key_len < pl)
			return 0; /* key is a strict prefix; sorts first */
	}

	skey = key + pl;
	sklen = (uint16_t)(key_len - pl);
	khead = suffix_head(skey, sklen);

	while (lo < hi) {
		uint16_t mid = (uint16_t)(lo + (hi - lo) / 2);
		uint16_t mhead = s[mid].head;

		if (khead < mhead) {
			hi = mid;
		} else if (khead > mhead) {
			lo = (uint16_t)(mid + 1);
		} else if (s[mid].key_len <= HEAD_LEN) {
			/* head holds the whole stored suffix */
			if (sklen < s[mid].key_len) {
				hi = mid;
			} else if (sklen > s[mid].key_len) {
				lo = (uint16_t)(mid + 1);
			} else {
				if (found != NULL)
					*found = 1;
				return mid;
			}
		} else {
			const uint8_t *mkey = p + s[mid].offset;
			int c;
			/* OLC bounds guard: on a torn page a stale offset/key_len
			 * could put this key's bytes outside the page buffer; a
			 * mis-sized memcmp would read OOB.  Treat an out-of-page
			 * cell as "greater" so the search stays in-bounds and
			 * terminates; the OLC caller re-validates the version and
			 * retries.  A well-formed slot always lies in-page. */
			if ((size_t)s[mid].offset + s[mid].key_len >
			    (size_t)h->page_size) {
				hi = mid;
				continue;
			}
			c = cmp_bytes(skey, sklen, mkey, s[mid].key_len);

			if (c < 0) {
				hi = mid;
			} else if (c > 0) {
				lo = (uint16_t)(mid + 1);
			} else {
				if (found != NULL)
					*found = 1;
				return mid;
			}
		}
	}
	return lo;
}

void
btnode_init(void *page, uint32_t page_size, int is_leaf)
{
	struct btnode_hdr *h = HDR(page);

	memset(h, 0, sizeof(*h));
	h->page_size = page_size;
	h->right_sibling = 0;
	h->count = 0;
	h->is_leaf = (uint8_t)(is_leaf ? 1 : 0);
	h->space_used = 0;
	h->data_offset = (uint16_t)page_size;
	h->prefix_len = 0;
}

void
btnode_set_fences(void *page, const void *lo, uint16_t lo_len, const void *hi,
    uint16_t hi_len)
{
	struct btnode_hdr *h = HDR(page);
	uint8_t *p = page;
	const uint8_t *L = lo;
	const uint8_t *H = hi;
	uint16_t pl = 0;

	if (L != NULL && lo_len > 0) {
		h->data_offset = (uint16_t)(h->data_offset - lo_len);
		h->space_used = (uint16_t)(h->space_used + lo_len);
		h->lo_fence_off = h->data_offset;
		h->lo_fence_len = lo_len;
		memcpy(p + h->data_offset, L, lo_len);
	} else {
		h->lo_fence_off = 0;
		h->lo_fence_len = 0;
	}

	if (H != NULL && hi_len > 0) {
		h->data_offset = (uint16_t)(h->data_offset - hi_len);
		h->space_used = (uint16_t)(h->space_used + hi_len);
		h->hi_fence_off = h->data_offset;
		h->hi_fence_len = hi_len;
		memcpy(p + h->data_offset, H, hi_len);
	} else {
		h->hi_fence_off = 0;
		h->hi_fence_len = 0;
	}

	if (L != NULL && H != NULL) {
		uint16_t lim = lo_len < hi_len ? lo_len : hi_len;

		while (pl < lim && L[pl] == H[pl])
			pl++;
	}
	h->prefix_len = pl;
}

int
btnode_search(const void *page, const void *key, uint16_t key_len, int *found)
{
	return node_lower_bound(page, key, key_len, found);
}

int
btnode_insert(void *page, const void *key, uint16_t key_len, const void *val,
    uint16_t val_len)
{
	return btnode_insert_f(page, key, key_len, val, val_len, 0);
}

int
btnode_insert_f(void *page, const void *key, uint16_t key_len, const void *val,
    uint16_t val_len, uint16_t vflag)
{
	struct btnode_hdr *h = HDR(page);
	struct btnode_slot *s;
	uint16_t pl = h->prefix_len;
	uint16_t sklen;
	uint16_t need;
	int idx;

	/* The empty key (an inner node's -infinity slot 0) is stored with
	 * an empty suffix whatever the prefix; any other key must carry the
	 * node prefix (the caller routes only in-range keys here). */
	if (key_len != 0 && (key_len < pl ||
	    (pl > 0 && memcmp(key, (const uint8_t *)page + h->lo_fence_off,
	    pl) != 0)))
		return -2;
	sklen = key_len == 0 ? 0 : (uint16_t)(key_len - pl);
	need = (uint16_t)(sizeof(struct btnode_slot) + sklen + val_len);
	if (need > free_contig(h)) {
		if (need > free_compact(h))
			return -1; /* caller must split */
		node_compact(page);
	}

	idx = key_len == 0 ? 0 : node_lower_bound(page, key, key_len, NULL);
	s = slots_of(page);
	memmove(&s[idx + 1], &s[idx],
	    sizeof(struct btnode_slot) * (size_t)(h->count - idx));
	node_store_f(page, (uint16_t)idx, (const uint8_t *)key + (key_len ?
	    pl : 0), sklen, val, val_len, VFLAG(vflag));
	h->count++;
	/* insert history: did this one land just after the previous one? */
	if (h->last_ins1 != 0 && idx == (int)h->last_ins1) {
		if (h->ins_run < 255) h->ins_run++;
	} else {
		h->ins_run = 0;
	}
	h->last_ins1 = (uint16_t)(idx + 1);
	return 0;
}

int
btnode_get(const void *page, int slot, const void **key_suffix,
    uint16_t *key_len, const void **val, uint16_t *val_len)
{
	const struct btnode_hdr *h = CHDR(page);
	const struct btnode_slot *s = cslots_of(page);
	const uint8_t *p = page;

	if (slot < 0 || slot >= (int)h->count)
		return -1;
	/* Bounds guard.  Under OLC (optimistic latch coupling) a reader may
	 * call this on a page a writer is concurrently mutating -- a torn
	 * slot could carry a stale/garbage offset that would make the
	 * key/val pointers below point OUTSIDE the page buffer (offset,
	 * key_len, val_len are each uint16_t, so their sum can exceed a
	 * 4 KB page).  Dereferencing that is an out-of-bounds read.  Reject
	 * a cell that does not lie wholly within the page; the OLC caller
	 * then sees a version conflict on re-validate and retries (or the
	 * latched caller never hits this on a well-formed page).  A
	 * correctly-formed slot always passes. */
	if ((size_t)s[slot].offset + s[slot].key_len + VLEN(s[slot].val_len) >
	    (size_t)h->page_size)
		return -1;
	if (key_suffix != NULL)
		*key_suffix = p + s[slot].offset;
	if (key_len != NULL)
		*key_len = s[slot].key_len;
	if (val != NULL)
		*val = p + s[slot].offset + s[slot].key_len;
	if (val_len != NULL)
		*val_len = VLEN(s[slot].val_len);
	return 0;
}

uint16_t
btnode_vflag(const void *page, int slot)
{
	const struct btnode_hdr *h = CHDR(page);
	const struct btnode_slot *s = cslots_of(page);

	if (slot < 0 || slot >= (int)h->count)
		return 0;
	return VFLAG(s[slot].val_len);
}

int
btnode_full_key(const void *page, int slot, void *out, uint16_t out_cap,
    uint16_t *out_len)
{
	const struct btnode_hdr *h = CHDR(page);
	const struct btnode_slot *s = cslots_of(page);
	const uint8_t *p = page;
	uint8_t *o = out;
	uint16_t pl = h->prefix_len;
	uint16_t full;

	if (slot < 0 || slot >= (int)h->count)
		return -1;
	full = (uint16_t)(pl + s[slot].key_len);
	if (out_cap < full) {
		if (out_len != NULL)
			*out_len = full;
		return -1;
	}
	if (pl > 0)
		memcpy(o, p + h->lo_fence_off, pl);
	memcpy(o + pl, p + s[slot].offset, s[slot].key_len);
	if (out_len != NULL)
		*out_len = full;
	return 0;
}

int
btnode_remove(void *page, int slot)
{
	struct btnode_hdr *h = HDR(page);
	struct btnode_slot *s = slots_of(page);

	if (slot < 0 || slot >= (int)h->count)
		return -1;
	h->last_ins1 = 0;              /* a delete breaks an insert run */
	h->ins_run = 0;
	h->space_used =
	    (uint16_t)(h->space_used - (s[slot].key_len + VLEN(s[slot].val_len)));
	memmove(&s[slot], &s[slot + 1],
	    sizeof(struct btnode_slot) * (size_t)(h->count - slot - 1));
	h->count--;
	return 0;
}

/*
 * Append the full key/value of source slot `i` (read from `page`) into
 * `dst`, recomputing the suffix against dst's prefix.  Returns -1 if
 * dst has no room.  `keybuf` is scratch for the reconstructed full key.
 */
static int
copy_slot(const void *page, uint16_t i, void *dst, uint16_t dst_slot,
    uint8_t *keybuf)
{
	const struct btnode_hdr *h = CHDR(page);
	const struct btnode_slot *s = cslots_of(page);
	const uint8_t *p = page;
	struct btnode_hdr *dh = HDR(dst);
	uint16_t pl = h->prefix_len;
	uint16_t full = (uint16_t)(pl + s[i].key_len);
	const uint8_t *v = p + s[i].offset + s[i].key_len;
	uint16_t vlen = VLEN(s[i].val_len);
	uint16_t vfl = VFLAG(s[i].val_len);
	/* An inner node's slot 0 (the -infinity child) keeps its empty
	 * suffix under any prefix. */
	int minf = !h->is_leaf && i == 0 && s[i].key_len == 0;
	uint16_t dsk = minf ? 0 : (uint16_t)(full - dh->prefix_len);
	uint16_t need = (uint16_t)(sizeof(struct btnode_slot) + dsk + vlen);

	if (need > free_contig(dh) || (!minf && full < dh->prefix_len))
		return -1;
	if (pl > 0)
		memcpy(keybuf, p + h->lo_fence_off, pl);
	memcpy(keybuf + pl, p + s[i].offset, s[i].key_len);
	node_store_f(dst, dst_slot, keybuf + (full - dsk), dsk, v, vlen, vfl);
	return 0;
}

int
btnode_split(void *page, void *right_page, void *sep_key_out,
    uint16_t *sep_len_out)
{
	return btnode_split_ex(page, right_page, sep_key_out, 0xFFFFu,
	    sep_len_out, NULL, 0, 0);
}

/*
 * Bytes a slot's entry occupies in a node whose prefix is `npl`: the
 * slot itself plus the (re-suffixed) key and the value.  The node's own
 * prefix is `pl`; a caller that passes npl == 0 counts the full key.
 */
static uint32_t
slot_cost(const struct btnode_slot *s, uint16_t pl, uint16_t npl)
{
	uint32_t full = (uint32_t)pl + s->key_len;
	uint32_t suf = full > npl ? full - npl : 0;

	return (uint32_t)sizeof(struct btnode_slot) + suf + VLEN(s->val_len);
}

/* Length of the common prefix of two byte strings. */
static uint16_t
lcp_bytes(const uint8_t *a, uint16_t al, const uint8_t *b, uint16_t bl)
{
	uint16_t n = 0, lim = al < bl ? al : bl;

	while (n < lim && a[n] == b[n])
		n++;
	return n;
}

/*
 * The prefix a split half will have: btnode_set_fences derives it from
 * the half's two fences, one of them `fence` (the node's own lo or hi
 * fence; len 0 = infinite, which gives prefix 0), the other the
 * separator, the node prefix (pl bytes, equal to every fence's first pl
 * bytes) followed by `sfx`.
 */
static uint16_t
half_prefix(const uint8_t *fence, uint16_t flen, uint16_t pl,
    const uint8_t *sfx, uint16_t slen)
{
	if (flen == 0)
		return 0;
	return (uint16_t)(pl + lcp_bytes(fence + pl, (uint16_t)(flen - pl), sfx,
	    slen));
}

/*
 * Byte-balanced split point (RethinkDB leaf::split, leaf_node.cc:
 * 980-1054: split the node's bytes, not its entry count, and take the
 * cut nearest half).  Returns the index of the LAST slot kept in the
 * left half, in [0, count-2] so both halves are non-empty, or -1 if no
 * cut leaves room for the pending entry.
 *
 * `pend` (bytes, slot included; 0 = no pending entry) is the entry the
 * caller will insert right after the split, and `pend_key` its key: the
 * pending entry lands LEFT iff its key <= the separator.  A cut is
 * acceptable only if the side receiving the pending entry still has
 * room for it.  Each half is costed EXACTLY as it will be stored: its
 * keys re-suffixed against the prefix of its own fences (half_prefix,
 * never shorter than the node's), plus both fences; the pending entry
 * is counted with its full key.  Among acceptable cuts the one whose
 * larger side is smallest wins (most balanced).
 *
 * The costs used to count every key in full ("conservatively").  That
 * is not conservative for a node with a long shared prefix: an index
 * leaf ([0xFE][id][elem...][rowid], ~30 shared bytes) holds about twice
 * page / (slot + full key) entries, so its full-key total exceeded two
 * pages, NO cut passed, and the split failed (test_btree_prefix_split:
 * 44,687 of 60,000 scrambled index inserts were refused and the index
 * missed those rows).  With exact costs the total is <= the page, which
 * is what the entry bound's proof (btree.c bt_entry_max) assumes.
 *
 * A slot-count split (the 1.56 behaviour) could leave the receiving
 * half too full for a large pending entry, failing the insert AFTER the
 * split was applied; and it never ran when the leaf held one entry.
 */
/*
 * The tail cut for a sequential stream that ends mid-node at slot ip
 * (see split_point): the left keeps slots [0, ip) and takes the pending
 * entry, the right takes the tail [ip, n).  The separator is the
 * shortest prefix of the tail's first key that sorts above the pending
 * key (Bayer and Unterauer's prefix B-tree separator): a proper prefix
 * of tail[0], so it is < tail[0] and > every left key, and every later
 * key of the stream (below tail[0] in its first differing byte) routes
 * left.  Returns the cut (ip - 1) with the separator in csep, or -1 if
 * no proper prefix separates them or the halves do not fit.
 */
static int
seq_split(const void *page, int ip, const uint8_t *pend_key, uint16_t pend_klen,
    uint32_t pend, uint8_t *csep, uint16_t csep_cap, uint16_t *csep_len)
{
	const struct btnode_hdr *h = CHDR(page);
	const struct btnode_slot *s = cslots_of(page);
	const uint8_t *p = page;
	uint16_t pl = h->prefix_len, tl, cp = 0, plen, lpl, rpl;
	uint32_t cap = h->page_size - (uint32_t)sizeof(struct btnode_hdr);
	uint32_t lneed = 0, rneed = 0;
	uint8_t tail[h->page_size];   /* a full key is smaller than a page */
	int i, n = (int)h->count;

	tl = (uint16_t)(pl + s[ip].key_len);
	if (tl > sizeof tail || tl > csep_cap)
		return -1;
	if (pl > 0) memcpy(tail, p + h->lo_fence_off, pl);
	memcpy(tail + pl, p + s[ip].offset, s[ip].key_len);
	while (cp < tl && cp < pend_klen && tail[cp] == pend_key[cp]) cp++;
	if (cp >= pend_klen || cp >= tl || tail[cp] <= pend_key[cp])
		return -1;                 /* pending is not below tail[0] */
	plen = (uint16_t)(cp + 1);
	if (plen >= tl)
		return -1;                 /* no PROPER prefix: cannot route */
	lpl = h->lo_fence_len ? lcp_bytes(p + h->lo_fence_off, h->lo_fence_len,
	    tail, plen) : 0;
	rpl = h->hi_fence_len ? lcp_bytes(tail, plen, p + h->hi_fence_off,
	    h->hi_fence_len) : 0;
	for (i = 0; i < ip; i++) lneed += slot_cost(&s[i], pl, lpl);
	for (i = ip; i < n; i++) rneed += slot_cost(&s[i], pl, rpl);
	lneed += (uint32_t)h->lo_fence_len + plen + pend;
	rneed += (uint32_t)plen + (uint32_t)h->hi_fence_len;
	if (lneed > cap || rneed > cap)
		return -1;
	memcpy(csep, tail, plen);
	*csep_len = plen;
	return ip - 1;
}

/*
 * The separator of a cut between two adjacent keys of a node (Bayer and
 * Unterauer's simple prefix B-tree: suffix truncation).  `a` and `b` are
 * the stored suffixes of the last key kept left and the first key moved
 * right; `lo`/`lol` the node's lower fence past the node prefix (lol 0
 * with lo NULL: -infinity).  Writes the separator's suffix into `out`
 * (cap bytes) and returns its length, or -1 if it does not fit.
 *
 * Keys <= the separator route left, so it must lie in [a, b).  The
 * shortest such string is the shorter of `a` itself and b's first
 * lcp(a, b) + 1 bytes (a proper prefix of b, above a at the byte where
 * they differ); nothing shorter exists (a shorter string either is a
 * prefix of a, so below it, or differs from a and b where they agree).
 * The separator becomes the left half's upper fence, and the left
 * half's prefix is lcp(lower fence, separator): when the left keys share
 * more with the lower fence (q bytes) than a and b share, b's prefix
 * would cut the left prefix down to lcp(a, b) and cost every left key
 * those bytes.  Then take a's first q bytes and a's next byte plus one
 * (above a, still below b, and q bytes in common with the fence), or
 * `a` when that byte is 0xFF.  Either way neither half's prefix is
 * shorter than with `a` as the separator (for x < y < z, lcp(x, z) =
 * min(lcp(x, y), lcp(y, z))), so a half never costs more than it did
 * under the full-key separator, which the entry bound (btree.c
 * bt_entry_max) relies on.
 */
static int
cut_separator(const uint8_t *lo, uint16_t lol, const uint8_t *a, uint16_t al,
    const uint8_t *b, uint16_t bl, uint8_t *out, uint16_t cap)
{
	uint16_t cp = lcp_bytes(a, al, b, bl);
	uint16_t q = lo != NULL ? lcp_bytes(lo, lol, a, al) : 0;
	uint16_t n;

	if (q > cp) {
		if (q + 1 < al && a[q] != 0xFF) {
			if (q + 1 > cap)
				return -1;
			memcpy(out, a, q);
			out[q] = (uint8_t)(a[q] + 1);
			return q + 1;
		}
		n = al;
	} else if (cp < al && cp + 1 < bl) {
		if (cp + 1 > cap)
			return -1;
		memcpy(out, b, (size_t)cp + 1);
		return cp + 1;
	} else
		n = al;
	if (n > cap)
		return -1;
	memcpy(out, a, n);
	return n;
}

static int
split_point(const void *page, const uint8_t *pend_key, uint16_t pend_klen,
    uint32_t pend, uint8_t *csep, uint16_t csep_cap, uint16_t *csep_len)
{
	const struct btnode_hdr *h = CHDR(page);
	const struct btnode_slot *s = cslots_of(page);
	const uint8_t *p = page;
	uint16_t pl = h->prefix_len;
	uint32_t cap = h->page_size - (uint32_t)sizeof(struct btnode_hdr);
	uint32_t total = 0, left = 0, best_max = UINT32_MAX;
	int i, best = -1, n = (int)h->count, append = 0;

	*csep_len = 0;

	if (n < 2)
		return -1;
	for (i = 0; i < n; i++)
		total += slot_cost(&s[i], pl, pl);
	/*
	 * Append split.  When the pending key sorts after every key in the
	 * node, the insert stream is (very likely) ascending -- a table's
	 * newest rowids, a time-ordered id, an index run's newest entry --
	 * and a midpoint cut would leave this left page half empty for
	 * good: every later key lands to the right.  Move as little as
	 * possible instead, keeping the left page full (Berkeley DB
	 * __bam_psplit moves one item for an insert at the end of the last
	 * page; SQLite's balance_quick and InnoDB's insert-point split do the
	 * same).  It is a guess: a wrong one leaves a full left page that
	 * just splits normally on its next miss.
	 */
	if (pend_key != NULL && pend > 0) {
		int ip = node_lower_bound(page, pend_key, pend_klen, NULL);
		append = ip >= n;
		/*
		 * A sequential stream in the MIDDLE of the node: this insert
		 * lands right after the last one, as the ones before it did
		 * (InnoDB's PAGE_LAST_INSERT heuristic) -- one table's newest
		 * rows, followed in the same leaf by the next table's first
		 * keys.  A midpoint cut leaves the stream's page half empty
		 * every time; an insertion-point cut (InnoDB) drags the other
		 * table's keys along to every new page.  Instead (seq_split)
		 * move the TAIL out under a truncated separator that routes the
		 * stream's future keys LEFT: the left page then grows by append
		 * splits and fills.
		 */
		if (!append && ip >= 2 && h->last_ins1 == (uint16_t)ip &&
		    h->ins_run >= 2) {
			int c = seq_split(page, ip, pend_key, pend_klen, pend, csep,
			    csep_cap, csep_len);
			if (c >= 0)
				return c;
		}
	}
	if (csep_cap < pl)
		return -1;
	if (pl > 0)
		memcpy(csep, p + h->lo_fence_off, pl);
	for (i = 0; i < n - 1; i++) {
		uint32_t sep_len, lneed, rneed, lsz, rsz, m;
		uint16_t lpl, rpl;
		int to_left = 1, tl;

		left += slot_cost(&s[i], pl, pl);
		/* The separator (the shortest key between slot i and slot
		 * i + 1, cut_separator) becomes the left's hi fence AND the
		 * right's lo fence; each half's keys are stored as suffixes of
		 * that half's prefix (>= pl). */
		tl = cut_separator(h->lo_fence_len ? p + h->lo_fence_off + pl :
		    NULL, (uint16_t)(h->lo_fence_len - (h->lo_fence_len ? pl : 0)),
		    p + s[i].offset, s[i].key_len, p + s[i + 1].offset,
		    s[i + 1].key_len, csep + pl, (uint16_t)(csep_cap - pl));
		if (tl < 0)
			continue;
		sep_len = (uint32_t)pl + (uint32_t)tl;
		lpl = half_prefix(p + h->lo_fence_off, h->lo_fence_len, pl,
		    csep + pl, (uint16_t)tl);
		rpl = half_prefix(p + h->hi_fence_off, h->hi_fence_len, pl,
		    csep + pl, (uint16_t)tl);
		lsz = left - (uint32_t)(i + 1) * (uint32_t)(lpl - pl) +
		    (uint32_t)h->lo_fence_len + sep_len;
		rsz = (total - left) - (uint32_t)(n - 1 - i) * (uint32_t)(rpl - pl) +
		    sep_len + (uint32_t)h->hi_fence_len;
		if (pend > 0 && pend_key != NULL)
			to_left = cmp_bytes(pend_key, pend_klen, csep,
			    (uint16_t)sep_len) <= 0;
		lneed = lsz + (pend > 0 && to_left ? pend : 0);
		rneed = rsz + (pend > 0 && !to_left ? pend : 0);
		if (lneed > cap || rneed > cap)
			continue;
		if (append) {
			best = i;              /* the highest feasible cut wins */
			continue;
		}
		m = lneed > rneed ? lneed : rneed;
		if (m < best_max) {
			best_max = m;
			best = i;
		}
	}
	if (best < 0)
		return -1;
	/* Rebuild the chosen cut's separator (csep held the last one tried). */
	*csep_len = (uint16_t)(pl + cut_separator(h->lo_fence_len ?
	    p + h->lo_fence_off + pl : NULL,
	    (uint16_t)(h->lo_fence_len - (h->lo_fence_len ? pl : 0)),
	    p + s[best].offset, s[best].key_len, p + s[best + 1].offset,
	    s[best + 1].key_len, csep + pl, (uint16_t)(csep_cap - pl)));
	return best;
}

int
btnode_split_ex(void *page, void *right_page, void *sep_key_out,
    uint16_t sep_cap, uint16_t *sep_len_out, const void *pend_key,
    uint16_t pend_klen, uint16_t pend_vlen)
{
	struct btnode_hdr *h = HDR(page);
	uint8_t *p = page;
	uint8_t *sep = sep_key_out;
	uint32_t ps = h->page_size;
	uint16_t sep_slot, sep_len;
	const uint8_t *lo, *hi;
	uint16_t lo_len, hi_len;
	uint16_t i, j;
	uint32_t pend = 0;
	int sp;

	if (h->count < 2)
		return -1;
	if (pend_key != NULL)
		pend = (uint32_t)sizeof(struct btnode_slot) + pend_klen + pend_vlen;
	sp = split_point(page, pend_key, pend_klen, pend, sep, sep_cap, &sep_len);
	if (sp < 0)
		return -1;
	sep_slot = (uint16_t)sp;

	/* split_point wrote the separator: the shortest key between the
	 * last slot kept left and the first moved right (cut_separator), or
	 * seq_split's.  Both lie in [left's last key, right's first key). */
	if (sep_len_out != NULL)
		*sep_len_out = sep_len;

	lo = h->lo_fence_len ? p + h->lo_fence_off : NULL;
	lo_len = h->lo_fence_len;
	hi = h->hi_fence_len ? p + h->hi_fence_off : NULL;
	hi_len = h->hi_fence_len;

	/*
	 * Build the right node in place: fences [sep, hi], slots
	 * (sep_slot, count).  Reads only from `page`.
	 */
	{
		struct btnode_hdr *rh = HDR(right_page);
		uint32_t rps = rh->page_size;
		uint8_t keybuf[ps];

		btnode_init(right_page, rps, h->is_leaf);
		btnode_set_fences(right_page, sep, sep_len, hi, hi_len);
		j = 0;
		for (i = (uint16_t)(sep_slot + 1); i < h->count; i++) {
			if (copy_slot(page, i, right_page, j, keybuf) != 0)
				return -1;
			j++;
		}
		rh->count = j;
		rh->right_sibling = h->right_sibling;
	}

	/*
	 * Build the left node into scratch (fences [lo, sep], slots
	 * [0, sep_slot]), then commit it over `page`.  All reads from
	 * `page` happen before the final copy.
	 */
	{
		uint8_t scratch[ps];
		struct btnode_hdr *th = HDR(scratch);
		uint8_t keybuf[ps];

		btnode_init(scratch, ps, h->is_leaf);
		btnode_set_fences(scratch, lo, lo_len, sep, sep_len);
		j = 0;
		for (i = 0; i <= sep_slot; i++) {
			if (copy_slot(page, i, scratch, j, keybuf) != 0)
				return -1;
			j++;
		}
		th->count = j;
		th->right_sibling = 0; /* caller wires left -> right id */
		memcpy(p, scratch, ps);
	}
	return 0;
}

unsigned
btnode_ins_run(const void *page)
{
	return CHDR(page)->ins_run;
}

uint32_t
btnode_room(const void *page)
{
	return free_compact(CHDR(page));
}

/* Bytes a node holds, header excluded: slots + cells + fences. */
static uint32_t
node_bytes(const struct btnode_hdr *h)
{
	return (uint32_t)h->count * (uint32_t)sizeof(struct btnode_slot) +
	    h->space_used;
}

uint32_t
btnode_fill(const void *page)
{
	const struct btnode_hdr *h = CHDR(page);

	return node_bytes(h) * 100u / (h->page_size -
	    (uint32_t)sizeof(struct btnode_hdr));
}

/* Entry i of the pair (left's slots, then right's): its node, slot and
 * the node prefix its stored suffix is relative to. */
struct pair {
	const struct btnode_hdr *h[2];
	const struct btnode_slot *s[2];
	uint16_t n0, n;
};

static void
pair_at(const struct pair *pr, int i, const uint8_t **sfx, uint16_t *sl,
    const struct btnode_slot **sp, const uint8_t **pfx, uint16_t *pl)
{
	int side = i >= pr->n0;
	const struct btnode_hdr *h = pr->h[side];
	const struct btnode_slot *s = &pr->s[side][side ? i - pr->n0 : i];

	*sfx = (const uint8_t *)h + s->offset;
	*sl = s->key_len;
	*sp = s;
	*pfx = (const uint8_t *)h + h->lo_fence_off;
	*pl = h->prefix_len;
}

/* Full key of pair entry i into out (cap >= a page). */
static uint16_t
pair_key(const struct pair *pr, int i, uint8_t *out)
{
	const uint8_t *sfx, *pfx;
	const struct btnode_slot *sp;
	uint16_t sl, pl;

	pair_at(pr, i, &sfx, &sl, &sp, &pfx, &pl);
	memcpy(out, pfx, pl);
	memcpy(out + pl, sfx, sl);
	return (uint16_t)(pl + sl);
}

/* Build a leaf from pair entries [a, b) under fences lo/hi into dst. */
static int
pair_build(const struct pair *pr, int a, int b, const uint8_t *lo,
    uint16_t lol, const uint8_t *hi, uint16_t hil, void *dst, uint32_t ps,
    uint8_t *keybuf)
{
	struct btnode_hdr *dh = HDR(dst);
	int i, j = 0;

	btnode_init(dst, ps, 1);
	btnode_set_fences(dst, lo, lol, hi, hil);
	for (i = a; i < b; i++, j++) {
		int side = i >= pr->n0;

		if (copy_slot(pr->h[side], (uint16_t)(side ? i - pr->n0 : i), dst,
		    (uint16_t)j, keybuf) != 0)
			return -1;
	}
	dh->count = (uint16_t)j;
	return 0;
}

int
btnode_shift(const void *left, const void *right, const void *pend_key,
    uint16_t pend_klen, uint16_t pend_vlen, void *sep_out, uint16_t sep_cap,
    uint16_t *sep_len_out, void *out)
{
	const struct btnode_hdr *lh = CHDR(left), *rh = CHDR(right);
	uint32_t ps = lh->page_size;
	uint32_t cap = ps - (uint32_t)sizeof(struct btnode_hdr);
	uint32_t pend = (uint32_t)sizeof(struct btnode_slot) + pend_klen +
	    pend_vlen;
	uint32_t total = 0, left_b = 0, best_max = UINT32_MAX;
	struct pair pr;
	const uint8_t *lo, *hi, *sfx, *pfx;
	const struct btnode_slot *sp;
	uint16_t lol, hil, sl, plx, al, bl, lpl, rpl;
	uint8_t a[ps], b[ps];
	uint8_t *sep = sep_out, *sc = out;
	int i, best = -1, n;

	if (rh->page_size != ps || !lh->is_leaf || !rh->is_leaf ||
	    lh->hi_fence_len == 0 || lh->hi_fence_len != rh->lo_fence_len ||
	    memcmp((const uint8_t *)left + lh->hi_fence_off,
	    (const uint8_t *)right + rh->lo_fence_off, lh->hi_fence_len) != 0)
		return -1;
	pr.h[0] = lh; pr.h[1] = rh;
	pr.s[0] = cslots_of(left); pr.s[1] = cslots_of(right);
	pr.n0 = lh->count;
	pr.n = (uint16_t)(lh->count + rh->count);
	n = pr.n;
	if (n < 2)
		return -1;
	lo = lh->lo_fence_len ? (const uint8_t *)left + lh->lo_fence_off : NULL;
	lol = lh->lo_fence_len;
	hi = rh->hi_fence_len ? (const uint8_t *)right + rh->hi_fence_off : NULL;
	hil = rh->hi_fence_len;
	/* Each half is costed exactly as it will be stored: an entry costs
	 * its slot, its full key less the half's prefix (the common prefix
	 * of the half's fences), and its value; plus the two fences.  The
	 * pending entry is counted with its full key. */
	for (i = 0; i < n; i++) {
		pair_at(&pr, i, &sfx, &sl, &sp, &pfx, &plx);
		total += slot_cost(sp, plx, 0);
	}
	for (i = 0; i < n - 1; i++) {
		uint32_t lneed, rneed, m;
		int tl;

		pair_at(&pr, i, &sfx, &sl, &sp, &pfx, &plx);
		left_b += slot_cost(sp, plx, 0);
		al = pair_key(&pr, i, a);
		bl = pair_key(&pr, i + 1, b);
		tl = cut_separator(lo, lol, a, al, b, bl, sep, sep_cap);
		if (tl < 0)
			continue;
		lpl = lo != NULL ? lcp_bytes(lo, lol, sep, (uint16_t)tl) : 0;
		rpl = hi != NULL ? lcp_bytes(sep, (uint16_t)tl, hi, hil) : 0;
		lneed = left_b - (uint32_t)(i + 1) * lpl + lol + (uint32_t)tl;
		rneed = (total - left_b) - (uint32_t)(n - 1 - i) * rpl +
		    (uint32_t)tl + hil;
		if (cmp_bytes(pend_key, pend_klen, sep, (uint16_t)tl) <= 0)
			lneed += pend;      /* keys <= the separator go left */
		else
			rneed += pend;
		if (lneed > cap || rneed > cap)
			continue;
		m = lneed > rneed ? lneed : rneed;
		if (m < best_max) {
			best_max = m;
			best = i;
		}
	}
	/* No feasible cut, or the best one is the current boundary: no
	 * change (the caller splits instead). */
	if (best < 0 || best == pr.n0 - 1)
		return -1;
	al = pair_key(&pr, best, a);
	bl = pair_key(&pr, best + 1, b);
	*sep_len_out = (uint16_t)cut_separator(lo, lol, a, al, b, bl, sep,
	    sep_cap);
	/* Build both into `out` (a is the key scratch now). */
	if (pair_build(&pr, 0, best + 1, lo, lol, sep, *sep_len_out, sc, ps,
	    a) != 0 ||
	    pair_build(&pr, best + 1, n, sep, *sep_len_out, hi, hil, sc + ps, ps,
	    a) != 0)
		return -1;
	HDR(sc)->right_sibling = lh->right_sibling;
	HDR(sc + ps)->right_sibling = rh->right_sibling;
	return 0;
}

uint16_t
btnode_used_bytes(const void *page)
{
	return CHDR(page)->space_used;
}

int
btnode_merge_fits(const void *page, const void *right_page)
{
	const struct btnode_hdr *lh = CHDR(page);
	const struct btnode_hdr *rh = CHDR(right_page);
	uint32_t ps = lh->page_size;
	uint32_t cap, slots, cells;

	/*
	 * After a merge the lower fence is the left's and the upper fence
	 * is the right's, so the prefix can only get SHORTER (a wider
	 * span shares fewer leading bytes), which only expands the stored
	 * suffixes.  Bound the worst case conservatively: assume the
	 * merged prefix is empty, so every key contributes its FULL bytes
	 * (current suffix + the node's own prefix).  space_used already
	 * counts the stored fence keys; the merged node keeps only two
	 * fences (left.lo, right.hi), so drop one fence per side as slack
	 * is unnecessary -- the bound is intentionally pessimistic.
	 */
	cells = (uint32_t)lh->space_used
	    + (uint32_t)lh->count * (uint32_t)lh->prefix_len
	    + (uint32_t)rh->space_used
	    + (uint32_t)rh->count * (uint32_t)rh->prefix_len;
	slots = ((uint32_t)lh->count + (uint32_t)rh->count)
	    * (uint32_t)sizeof(struct btnode_slot);
	cap = ps - (uint32_t)sizeof(struct btnode_hdr);
	return (slots + cells) <= cap ? 1 : 0;
}

int
btnode_merge(void *page, const void *right_page)
{
	struct btnode_hdr *lh = HDR(page);
	const struct btnode_hdr *rh = CHDR(right_page);
	const uint8_t *lp = page;
	uint32_t ps = lh->page_size;
	const uint8_t *lo, *hi;
	uint16_t lo_len, hi_len;
	uint16_t i, j;

	if (rh->page_size != ps || lh->is_leaf != rh->is_leaf)
		return -1;

	/* Merged span: left's lower fence .. right's upper fence. */
	lo = lh->lo_fence_len ? lp + lh->lo_fence_off : NULL;
	lo_len = lh->lo_fence_len;
	hi = rh->hi_fence_len ? (const uint8_t *)right_page + rh->hi_fence_off
	    : NULL;
	hi_len = rh->hi_fence_len;

	/*
	 * Build the merged node in scratch (so reads from `page` and
	 * `right_page` all precede the single write-back), then commit it
	 * over `page`.  left's slots first, then right's, preserving order.
	 */
	{
		uint8_t scratch[ps];
		struct btnode_hdr *th = HDR(scratch);
		uint8_t keybuf[ps];

		btnode_init(scratch, ps, lh->is_leaf);
		btnode_set_fences(scratch, lo, lo_len, hi, hi_len);
		j = 0;
		for (i = 0; i < lh->count; i++) {
			if (copy_slot(page, i, scratch, j, keybuf) != 0)
				return -1;
			j++;
		}
		for (i = 0; i < rh->count; i++) {
			if (copy_slot(right_page, i, scratch, j, keybuf) != 0)
				return -1;
			j++;
		}
		th->count = j;
		th->right_sibling = rh->right_sibling;  /* inherit right's link */
		memcpy(page, scratch, ps);
	}
	return 0;
}

uint16_t
btnode_count(const void *page)
{
	return CHDR(page)->count;
}

uint16_t
btnode_free_space(const void *page)
{
	return free_contig(CHDR(page));
}

int
btnode_is_leaf(const void *page)
{
	return CHDR(page)->is_leaf;
}

uint16_t
btnode_prefix_len(const void *page)
{
	return CHDR(page)->prefix_len;
}

uint32_t
btnode_right_sibling(const void *page)
{
	return CHDR(page)->right_sibling;
}

void
btnode_set_right_sibling(void *page, uint32_t id)
{
	HDR(page)->right_sibling = id;
}

int
btnode_beyond_hi_fence(const void *page, const void *key, uint16_t klen)
{
	const struct btnode_hdr *h = CHDR(page);
	const uint8_t *hf;

	if (h->hi_fence_off == 0 || h->hi_fence_len == 0)
		return 0;        /* +infinity: key always belongs here */
	hf = (const uint8_t *)page + h->hi_fence_off;
	return cmp_bytes(key, klen, hf, h->hi_fence_len) > 0 ? 1 : 0;
}

/* 1 if `key` is at or below this node's lower fence, meaning it does
 * NOT belong here -- the lower fence is the exclusive lower bound, so
 * this node owns (lo, hi].  A concurrent merge that absorbed this node
 * into its LEFT sibling, then freed it, leaves a stale descent holding
 * a page whose lo fence now excludes the key; the caller must restart
 * the descent from the root.  move_right cannot recover this (the key
 * moved left, not right).  A -infinity lower fence excludes nothing. */
int
btnode_below_lo_fence(const void *page, const void *key, uint16_t klen)
{
	const struct btnode_hdr *h = CHDR(page);
	const uint8_t *lf;

	if (h->lo_fence_off == 0 || h->lo_fence_len == 0)
		return 0;        /* -infinity: key is never below it */
	lf = (const uint8_t *)page + h->lo_fence_off;
	return cmp_bytes(key, klen, lf, h->lo_fence_len) <= 0 ? 1 : 0;
}

/* Mark / test the merge-unlinked "dead" flag (see btnode.h). */
void
btnode_mark_dead(void *page)
{
	HDR(page)->dead = 1;
}

int
btnode_is_dead(const void *page)
{
	return CHDR(page)->dead ? 1 : 0;
}

/* Copy this node's lower fence key into `out`.  Returns 0 with *len
 * set on success (0 == -infinity), -1 if the buffer is too small. */
int
btnode_lo_fence(const void *page, void *out, uint16_t cap, uint16_t *len)
{
	const struct btnode_hdr *h = CHDR(page);

	if (h->lo_fence_off == 0 || h->lo_fence_len == 0) {
		if (len != NULL)
			*len = 0;
		return 0;
	}
	if (h->lo_fence_len > cap)
		return -1;
	memcpy(out, (const uint8_t *)page + h->lo_fence_off, h->lo_fence_len);
	if (len != NULL)
		*len = h->lo_fence_len;
	return 0;
}

/* Copy this node's upper fence key into `out`.  Returns 0 with *len
 * set on success (0 == +infinity), -1 if the buffer is too small. */
int
btnode_hi_fence(const void *page, void *out, uint16_t cap, uint16_t *len)
{
	const struct btnode_hdr *h = CHDR(page);

	if (h->hi_fence_off == 0 || h->hi_fence_len == 0) {
		if (len != NULL)
			*len = 0;
		return 0;
	}
	if (h->hi_fence_len > cap)
		return -1;
	memcpy(out, (const uint8_t *)page + h->hi_fence_off, h->hi_fence_len);
	if (len != NULL)
		*len = h->hi_fence_len;
	return 0;
}

/* ---- structural validator (btnode_check) ------------------------------ */

static int
check_fail(char *why, size_t whylen, const char *fmt, ...)
{
	va_list ap;

	if (why != NULL && whylen > 0) {
		va_start(ap, fmt);
		(void)vsnprintf(why, whylen, fmt, ap);
		va_end(ap);
	}
	return -1;
}

/* Mark bytes [off, off+len) in the occupancy bitmap; 1 if any was
 * already marked (two cells overlap). */
static int
mark_bytes(uint8_t *map, uint32_t off, uint32_t len)
{
	uint32_t i;

	for (i = off; i < off + len; i++) {
		if (map[i >> 3] & (1u << (i & 7)))
			return 1;
		map[i >> 3] |= (uint8_t)(1u << (i & 7));
	}
	return 0;
}

/* Is `ref` (len bytes) a well-formed out-of-line value ref for data
 * pages of `ps` bytes?  Mirrors btree.c ovf_write / ovf_parse. */
static int
ovf_ref_ok(const uint8_t *ref, uint32_t len, uint32_t ps)
{
	uint32_t n, total, per, i;

	if (len < BT_OVF_HDR || ref[0] != BT_OVF_TAG || ref[2] != 0 ||
	    ref[3] != 0)
		return 0;
	n = ref[1];
	if (n == 0 || n > BT_OVF_MAXPAGES || len != bt_ovf_ref_len(n) ||
	    ps <= BT_OVF_PGHDR)
		return 0;
	per = ps - BT_OVF_PGHDR;
	total = bt_ovf_get32(ref + 4);
	if (total == 0 || total > n * per || total <= (n - 1) * per)
		return 0;       /* ovf_write uses exactly ceil(total / per) pages */
	for (i = 0; i < n; i++)
		if (bt_ovf_get64(ref + BT_OVF_HDR + 8u * i) == 0)
			return 0;
	return 1;
}

int
btnode_check(const void *page, char *why, size_t whylen)
{
	const struct btnode_hdr *h = CHDR(page);
	const struct btnode_slot *s = cslots_of(page);
	const uint8_t *p = page;
	uint32_t ps = h->page_size, send, sum = 0, i;
	uint16_t pl = h->prefix_len, lim, cp = 0;
	const uint8_t *lo = NULL, *hi = NULL;
	/* Occupancy bitmap, one bit per page byte (512 B for 4 KiB). */
	uint32_t mapn = (ps > sizeof(*h) && ps <= 65535) ? (ps + 7) / 8 : 1;
	uint8_t map[mapn];

	if (ps <= sizeof(*h) || ps > 65535)
		return check_fail(why, whylen, "page_size %u out of range", ps);
	if (h->is_leaf > 1 || h->dead > 1)
		return check_fail(why, whylen, "is_leaf %u / dead %u not 0/1",
		    h->is_leaf, h->dead);
	send = (uint32_t)sizeof(*h) + (uint32_t)h->count *
	    (uint32_t)sizeof(struct btnode_slot);
	if (send > h->data_offset || h->data_offset > ps)
		return check_fail(why, whylen,
		    "slot array end %u > data_offset %u (or > page %u), count %u",
		    send, h->data_offset, ps, h->count);
	memset(map, 0, mapn);

	/* Fences: (off == 0) iff (len == 0), inside the heap, no overlap. */
	if ((h->lo_fence_off == 0) != (h->lo_fence_len == 0) ||
	    (h->hi_fence_off == 0) != (h->hi_fence_len == 0))
		return check_fail(why, whylen, "fence off/len disagree "
		    "(lo %u/%u hi %u/%u)", h->lo_fence_off, h->lo_fence_len,
		    h->hi_fence_off, h->hi_fence_len);
	if (h->lo_fence_len) {
		if (h->lo_fence_off < h->data_offset ||
		    (uint32_t)h->lo_fence_off + h->lo_fence_len > ps)
			return check_fail(why, whylen, "lo fence [%u,+%u) outside "
			    "heap [%u,%u)", h->lo_fence_off, h->lo_fence_len,
			    h->data_offset, ps);
		lo = p + h->lo_fence_off;
		(void)mark_bytes(map, h->lo_fence_off, h->lo_fence_len);
	}
	if (h->hi_fence_len) {
		if (h->hi_fence_off < h->data_offset ||
		    (uint32_t)h->hi_fence_off + h->hi_fence_len > ps)
			return check_fail(why, whylen, "hi fence [%u,+%u) outside "
			    "heap [%u,%u)", h->hi_fence_off, h->hi_fence_len,
			    h->data_offset, ps);
		hi = p + h->hi_fence_off;
		if (mark_bytes(map, h->hi_fence_off, h->hi_fence_len))
			return check_fail(why, whylen, "hi fence overlaps lo fence");
	}
	if (lo != NULL && hi != NULL &&
	    cmp_bytes(lo, h->lo_fence_len, hi, h->hi_fence_len) >= 0)
		return check_fail(why, whylen, "lo fence >= hi fence");

	/* prefix_len is exactly the fences' common prefix (0 unless both). */
	if (lo != NULL && hi != NULL) {
		lim = h->lo_fence_len < h->hi_fence_len ? h->lo_fence_len :
		    h->hi_fence_len;
		while (cp < lim && lo[cp] == hi[cp])
			cp++;
	}
	if (pl != cp)
		return check_fail(why, whylen, "prefix_len %u != fence common "
		    "prefix %u", pl, cp);
	if (!h->is_leaf && h->count == 0)
		return check_fail(why, whylen, "internal node with no children");

	for (i = 0; i < h->count; i++) {
		uint32_t klen = s[i].key_len, vlen = VLEN(s[i].val_len);
		uint32_t off = s[i].offset;
		const uint8_t *k = p + off;

		if (off < h->data_offset || off + klen + vlen > ps)
			return check_fail(why, whylen, "slot %u cell [%u,+%u) "
			    "outside heap [%u,%u)", i, off, klen + vlen,
			    h->data_offset, ps);
		if (mark_bytes(map, off, klen + vlen))
			return check_fail(why, whylen, "slot %u cell [%u,+%u) "
			    "overlaps another cell or a fence", i, off, klen + vlen);
		sum += klen + vlen;
		if (s[i].head != suffix_head(k, (uint16_t)klen))
			return check_fail(why, whylen, "slot %u head %08x != "
			    "suffix head %08x", i, s[i].head,
			    suffix_head(k, (uint16_t)klen));
		/* Strictly ascending.  Every key shares the prefix, so the
		 * suffixes order exactly as the full keys do. */
		if (i > 0 && cmp_bytes(p + s[i - 1].offset, s[i - 1].key_len,
		    k, (uint16_t)klen) >= 0)
			return check_fail(why, whylen, "slot %u key not > slot "
			    "%u key", i, i - 1);
		/* Keys (leaf) and separators (internal, past slot 0) lie in
		 * (lo, hi].  The full key shares the first pl bytes with both
		 * fences, so compare suffix against the fence remainder. */
		if ((h->is_leaf || i > 0) && lo != NULL && cmp_bytes(k,
		    (uint16_t)klen, lo + pl, (uint16_t)(h->lo_fence_len - pl)) <= 0)
			return check_fail(why, whylen, "%s slot %u key <= lo fence",
			    h->is_leaf ? "leaf" : "internal", i);
		if ((h->is_leaf || i > 0) && hi != NULL && cmp_bytes(k,
		    (uint16_t)klen, hi + pl, (uint16_t)(h->hi_fence_len - pl)) > 0)
			return check_fail(why, whylen, "%s slot %u key > hi fence",
			    h->is_leaf ? "leaf" : "internal", i);
		if (h->is_leaf) {
			if (VFLAG(s[i].val_len) &&
			    !ovf_ref_ok(k + klen, vlen, ps))
				return check_fail(why, whylen, "leaf slot %u: "
				    "flagged value (%u B) is not a well-formed "
				    "out-of-line ref", i, vlen);
		} else {
			uint32_t pid;

			/* Internal: slot 0 is the -infinity child, stored with
			 * an empty suffix; every value is an unflagged nonzero
			 * 4-byte child pid (BTNODE_CHILD_LEN). */
			if (i == 0 && klen != 0)
				return check_fail(why, whylen, "internal slot 0 "
				    "key is %u B, not the empty key", klen);
			if (VFLAG(s[i].val_len) || vlen != BTNODE_CHILD_LEN)
				return check_fail(why, whylen, "internal slot %u "
				    "value %u B flag %x (want %u B child pid)", i,
				    vlen, VFLAG(s[i].val_len), BTNODE_CHILD_LEN);
			memcpy(&pid, k + klen, sizeof pid);
			if (pid == 0)
				return check_fail(why, whylen, "internal slot %u "
				    "child pid 0", i);
		}
	}
	sum += (uint32_t)h->lo_fence_len + h->hi_fence_len;
	if (sum != h->space_used)
		return check_fail(why, whylen, "space_used %u != cells+fences %u",
		    h->space_used, sum);
	return 0;
}
