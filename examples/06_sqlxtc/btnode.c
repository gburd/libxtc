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
 *	the fence keys, per-slot 4-byte "head" for fast ordering, key
 *	suffixes + values packed from the back of the page) and the
 *	classic slotted page of the threadskv B-tree (slot array grows
 *	from the front, cells from the back).
 */

#include "btnode.h"

#include <string.h>

_Static_assert(sizeof(struct btnode_slot) == 10,
    "btnode_slot must pack to 10 bytes");

#define HDR(p) ((struct btnode_hdr *)(p))
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

/* First four bytes of `k` packed big-endian, zero padded for short keys. */
static uint32_t
suffix_head(const uint8_t *k, uint16_t len)
{
	uint32_t h = 0;

	if (len > 0)
		h |= (uint32_t)k[0] << 24;
	if (len > 1)
		h |= (uint32_t)k[1] << 16;
	if (len > 2)
		h |= (uint32_t)k[2] << 8;
	if (len > 3)
		h |= (uint32_t)k[3];
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
 * Write the suffix of `key` plus `val` into slot `idx`'s cell and fill
 * in the slot fields.  Does NOT touch count or shift slots -- the
 * caller positions the slot first.  `key` is the full key; prefix_len
 * leading bytes are dropped.
 */
static void
node_store(void *page, uint16_t idx, const uint8_t *key, uint16_t key_len,
    const uint8_t *val, uint16_t val_len)
{
	struct btnode_hdr *h = HDR(page);
	struct btnode_slot *s = slots_of(page);
	uint8_t *p = page;
	uint16_t pl = h->prefix_len;
	const uint8_t *skey = key + pl;
	uint16_t sklen = (uint16_t)(key_len - pl);
	uint16_t space = (uint16_t)(sklen + val_len);

	h->data_offset = (uint16_t)(h->data_offset - space);
	h->space_used = (uint16_t)(h->space_used + space);
	s[idx].offset = h->data_offset;
	s[idx].key_len = sklen;
	s[idx].val_len = val_len;
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
		uint16_t kv = (uint16_t)(src[i].key_len + src[i].val_len);

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
 * longer than four bytes.
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
	uint32_t khead;
	uint16_t lo = 0;
	uint16_t hi = h->count;

	if (found != NULL)
		*found = 0;

	if (pl > 0) {
		const uint8_t *pref = p + h->lo_fence_off;
		uint16_t cl = key_len < pl ? key_len : pl;
		int c = cl ? memcmp(key, pref, cl) : 0;

		if (c < 0)
			return 0;
		if (c > 0)
			return (int)h->count;
		if (key_len < pl)
			return 0; /* key is a strict prefix; sorts first */
	}

	skey = key + pl;
	sklen = (uint16_t)(key_len - pl);
	khead = suffix_head(skey, sklen);

	while (lo < hi) {
		uint16_t mid = (uint16_t)(lo + (hi - lo) / 2);
		uint32_t mhead = s[mid].head;

		if (khead < mhead) {
			hi = mid;
		} else if (khead > mhead) {
			lo = (uint16_t)(mid + 1);
		} else if (s[mid].key_len <= 4) {
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
			int c = cmp_bytes(skey, sklen, mkey, s[mid].key_len);

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
	struct btnode_hdr *h = HDR(page);
	struct btnode_slot *s;
	uint16_t pl = h->prefix_len;
	uint16_t sklen = (uint16_t)(key_len - pl);
	uint16_t need =
	    (uint16_t)(sizeof(struct btnode_slot) + sklen + val_len);
	int idx;

	if (need > free_contig(h)) {
		if (need > free_compact(h))
			return -1; /* caller must split */
		node_compact(page);
	}

	idx = node_lower_bound(page, key, key_len, NULL);
	s = slots_of(page);
	memmove(&s[idx + 1], &s[idx],
	    sizeof(struct btnode_slot) * (size_t)(h->count - idx));
	node_store(page, (uint16_t)idx, key, key_len, val, val_len);
	h->count++;
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
	if (key_suffix != NULL)
		*key_suffix = p + s[slot].offset;
	if (key_len != NULL)
		*key_len = s[slot].key_len;
	if (val != NULL)
		*val = p + s[slot].offset + s[slot].key_len;
	if (val_len != NULL)
		*val_len = s[slot].val_len;
	return 0;
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
	h->space_used =
	    (uint16_t)(h->space_used - (s[slot].key_len + s[slot].val_len));
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
	uint16_t vlen = s[i].val_len;
	uint16_t dsk = (uint16_t)(full - dh->prefix_len);
	uint16_t need = (uint16_t)(sizeof(struct btnode_slot) + dsk + vlen);

	if (need > free_contig(dh))
		return -1;
	if (pl > 0)
		memcpy(keybuf, p + h->lo_fence_off, pl);
	memcpy(keybuf + pl, p + s[i].offset, s[i].key_len);
	node_store(dst, dst_slot, keybuf, full, v, vlen);
	return 0;
}

int
btnode_split(void *page, void *right_page, void *sep_key_out,
    uint16_t *sep_len_out)
{
	struct btnode_hdr *h = HDR(page);
	struct btnode_slot *s = slots_of(page);
	uint8_t *p = page;
	uint8_t *sep = sep_key_out;
	uint16_t pl = h->prefix_len;
	uint32_t ps = h->page_size;
	uint16_t sep_slot, sep_len;
	const uint8_t *lo, *hi;
	uint16_t lo_len, hi_len;
	uint16_t i, j;

	if (h->count < 2)
		return -1;

	sep_slot = (uint16_t)((h->count - 1) / 2);

	/* Separator = full key of the last slot kept in the left node. */
	sep_len = (uint16_t)(pl + s[sep_slot].key_len);
	if (pl > 0)
		memcpy(sep, p + h->lo_fence_off, pl);
	memcpy(sep + pl, p + s[sep_slot].offset, s[sep_slot].key_len);
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
