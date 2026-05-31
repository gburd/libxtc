/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/btnode.h
 *	A self-contained, slotted-page B-tree node with prefix
 *	compression.
 *
 *	The module is pure logic over a caller-provided, fixed-size
 *	page buffer (the buffer manager owns the memory).  It does not
 *	allocate, does not depend on any xtc/sqlite header, and never
 *	calls malloc inside a node operation.
 *
 *	LAYOUT
 *	------
 *	A page is a fixed buffer of `page_size` bytes (e.g. 4096 or
 *	16384; must be <= 65535).  At the front sits a `struct
 *	btnode_hdr`, immediately followed by a packed array of `struct
 *	btnode_slot` that grows toward the end of the page.  Variable
 *	length cells -- the key suffixes, the values, and the two fence
 *	keys -- live at the back of the page and grow downward toward
 *	the slot array.  `data_offset` is the lowest byte currently
 *	occupied by a cell; free space is the gap between the end of the
 *	slot array and `data_offset`.
 *
 *	    +----------------------------------------------------------+
 *	    | btnode_hdr | slot[0] slot[1] ... ->     free      <- ... |
 *	    |            |                                  cells/keys |
 *	    +----------------------------------------------------------+
 *	    0          hdr                         data_offset   page_size
 *
 *	PREFIX COMPRESSION (the headline feature)
 *	-----------------------------------------
 *	Every key routed into a node is bracketed by the node's lower
 *	and upper fence keys.  The bytes the two fences share are, by
 *	construction, shared by every key in the node, so they are
 *	stored exactly once: `prefix_len` records how many leading bytes
 *	are common, and the prefix bytes themselves are taken from the
 *	stored lower fence key.  Each slot then stores only the key
 *	SUFFIX (the bytes after the common prefix).  The slot also caches
 *	the first four bytes of that suffix, big-endian, in `head`, so a
 *	binary search can usually order two keys with a single 32-bit
 *	compare before touching the cell bytes.
 *
 *	USAGE ORDER
 *	-----------
 *	  1. btnode_init(page, page_size, is_leaf)
 *	  2. btnode_set_fences(page, lo, lo_len, hi, hi_len)  -- once,
 *	     before any insert; this fixes prefix_len.  A node with no
 *	     fences (infinity on both sides) simply has prefix_len 0.
 *	  3. btnode_insert / btnode_search / btnode_get / ...
 *
 *	All keys handed to btnode_insert MUST share the first
 *	`prefix_len` bytes with the fences; the caller (the B-tree)
 *	guarantees this by construction.
 */

#ifndef SQLXTC_BTNODE_H
#define SQLXTC_BTNODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * On-page header.  Plain little C struct overlaid on the front of the
 * page buffer.  Offsets are page-relative.  `right_sibling` is a page
 * id assigned by the buffer manager (0 means "no sibling").
 */
struct btnode_hdr {
	uint32_t page_size;     /* size of the whole page buffer */
	uint32_t right_sibling; /* next leaf page id, 0 = none */
	uint16_t count;         /* number of live slots */
	uint8_t is_leaf;        /* 1 = leaf, 0 = inner */
	uint8_t reserved;       /* pad; keep struct layout explicit */
	uint16_t space_used;    /* heap bytes in use (suffixes+vals+fences) */
	uint16_t data_offset;   /* lowest occupied cell byte; heap grows down */
	uint16_t prefix_len;    /* shared-prefix length (compressed away) */
	uint16_t lo_fence_off;  /* lower fence key offset (0 = -infinity) */
	uint16_t lo_fence_len;  /* lower fence key length */
	uint16_t hi_fence_off;  /* upper fence key offset (0 = +infinity) */
	uint16_t hi_fence_len;  /* upper fence key length */
};

/*
 * One slot.  Packed so the on-page layout is exactly 10 bytes
 * regardless of the host's alignment rules.  `offset` points at the
 * key suffix; the value immediately follows the suffix in the cell.
 */
struct __attribute__((packed)) btnode_slot {
	uint16_t offset;  /* page offset of the key suffix */
	uint16_t key_len; /* length of the key SUFFIX (full key - prefix) */
	uint16_t val_len; /* length of the value */
	uint32_t head;    /* first 4 bytes of the suffix, big-endian */
};

/* Initialize an empty node in `page`.  page_size must be <= 65535. */
void btnode_init(void *page, uint32_t page_size, int is_leaf);

/*
 * Record the lower/upper fence keys and derive prefix_len from the
 * bytes they share.  Pass lo == NULL (or lo_len == 0) for a -infinity
 * lower bound and hi == NULL for a +infinity upper bound.  Call once,
 * right after btnode_init and before inserting anything.
 */
void btnode_set_fences(void *page, const void *lo, uint16_t lo_len,
    const void *hi, uint16_t hi_len);

/*
 * Binary search for `key`.  Returns the insertion point (the index of
 * the first slot whose full key is >= `key`, in [0, count]) and sets
 * *found to 1 if an exact match exists, else 0.  `found` may be NULL.
 */
int btnode_search(const void *page, const void *key, uint16_t key_len,
    int *found);

/*
 * Insert key/value, keeping slots sorted.  `key` is the FULL key; only
 * its suffix (key_len - prefix_len bytes) is stored.  Returns 0 on
 * success, -1 if the node lacks room even after compaction (the caller
 * must split).
 */
int btnode_insert(void *page, const void *key, uint16_t key_len,
    const void *val, uint16_t val_len);

/*
 * Borrow the stored suffix and value for `slot` (no copy).  The
 * returned `key_len` is the SUFFIX length, not the full key length.
 * Returns 0 on success, -1 if slot is out of range.
 */
int btnode_get(const void *page, int slot, const void **key_suffix,
    uint16_t *key_len, const void **val, uint16_t *val_len);

/*
 * Reconstruct the full key (prefix + suffix) for `slot` into `out`.
 * Returns 0 on success; -1 if slot is out of range or out_cap is too
 * small (in which case *out_len is still set to the required length).
 */
int btnode_full_key(const void *page, int slot, void *out, uint16_t out_cap,
    uint16_t *out_len);

/* Remove `slot`.  Returns 0 on success, -1 if slot is out of range. */
int btnode_remove(void *page, int slot);

/*
 * Split a full node: move the upper half of the slots into `right_page`
 * (which must already be btnode_init'd with the same page size and
 * leaf-ness), keeping the lower half in `page`.  Recomputes both nodes'
 * prefixes and fences, writes the separator key (the largest key kept
 * in the left node == the right node's lower fence) into `sep_key_out`
 * (which must be large enough for one full key) and its length into
 * *sep_len_out.
 *
 * The sibling chain is maintained at the node level: `right_page`
 * inherits the old node's right_sibling.  Because the node module does
 * not know page ids, the caller must wire the left node's right_sibling
 * to the right page's id via btnode_set_right_sibling().
 *
 * Returns 0 on success, -1 if the node has fewer than two slots or the
 * halves do not fit.
 */
int btnode_split(void *page, void *right_page, void *sep_key_out,
    uint16_t *sep_len_out);

/* Accessors. */
uint16_t btnode_count(const void *page);
uint16_t btnode_free_space(const void *page); /* contiguous free bytes */
int btnode_is_leaf(const void *page);
uint16_t btnode_prefix_len(const void *page);
uint32_t btnode_right_sibling(const void *page);
void btnode_set_right_sibling(void *page, uint32_t id);

#ifdef __cplusplus
}
#endif

#endif /* SQLXTC_BTNODE_H */
