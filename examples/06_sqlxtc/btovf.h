/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/btovf.h
 *	Out-of-line ("overflow") values for the B-tree.
 *
 *	Borrowed from RethinkDB's blob (src/buffer_cache/blob.{hpp,cc}):
 *	a value small enough to live in a leaf stays inline; a larger one
 *	is written to dedicated pages and the leaf keeps only a small
 *	fixed-shape reference to them -- the leaf entry stays bounded no
 *	matter how big the value is, so every split/merge guarantee of the
 *	leaf holds (RethinkDB's leaf_epsilon rule, see btree.c bt_entry_max).
 *
 *	RethinkDB's ref is [251][int64 size][block_id x k] with up to 30
 *	direct ids (a one-level blob of 30 x 4084 bytes) before it adds an
 *	indirection level.  Ours is the same one-level shape, adapted to a
 *	page-table buffer manager with 32-bit-ish page ids and a 16-bit
 *	value length in the bt_* API:
 *
 *	    ref (in the leaf, as the entry's value):
 *	        u8  tag     = BT_OVF_TAG
 *	        u8  npages
 *	        u16 reserved (0)
 *	        u32 total_len           (bytes of the logical value)
 *	        u32 checksum            (FNV-1a of the logical value)
 *	        u64 pid[npages]         (data pages, in order)
 *	    data page: [u64 page LSN slot][u32 magic][u32 len][len bytes]
 *	    (the buffer manager stamps the page LSN into the first 8 bytes of
 *	    every dirtied page -- bm_opts.lsn_off 0 in production -- so the
 *	    data page leaves them to it, exactly like a btnode header)
 *
 *	The two forms are told apart by the high bit of the slot's 16-bit
 *	value length (BT_OVF_FLAG): an inline value is stored exactly as
 *	before (so existing pages, tests and tools are unchanged), and an
 *	inline value can never reach 32 KiB because the entry bound caps
 *	it near a third of a page (21 KiB on a 64 KiB page).  The flag is
 *	private to btree.c: callers of bt_* always see plain value bytes.
 *
 *	The data pages are written (dirty) BEFORE the leaf entry that
 *	points at them is published, and freed only after the entry that
 *	points at them is gone (delete / replace), never on a split or
 *	merge, which merely MOVE the ref (RethinkDB value_deleter_t vs
 *	detach: types.hpp:11-20, operations.cc:45-64).
 */

#ifndef SQLXTC_BTOVF_H
#define SQLXTC_BTOVF_H

#include <stdint.h>

#define BT_OVF_FLAG     0x8000u       /* slot val_len bit: value is a ref */
#define BT_OVF_TAG      0x5Fu         /* first byte of a ref (sanity check) */
#define BT_OVF_MAGIC    0x4F564631u   /* "OVF1" */
#define BT_OVF_MAXPAGES 64u           /* 64 x (1 KiB - 16) >= 64 KiB - 1 */
#define BT_OVF_HDR      12u           /* tag, npages, rsvd, total, sum */
#define BT_OVF_PGHDR    48u           /* LSN slot, magic, len, checksum (btnode.h) */

static inline uint32_t
bt_ovf_ref_len(uint32_t npages)
{
	return BT_OVF_HDR + 8u * npages;
}

static inline uint32_t
bt_ovf_fnv(const void *p, uint32_t n)
{
	const uint8_t *b = p;
	uint32_t h = 2166136261u, i;

	for (i = 0; i < n; i++) {
		h ^= b[i];
		h *= 16777619u;
	}
	return h;
}

static inline void
bt_ovf_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t
bt_ovf_get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void
bt_ovf_put64(uint8_t *p, uint64_t v)
{
	bt_ovf_put32(p, (uint32_t)v);
	bt_ovf_put32(p + 4, (uint32_t)(v >> 32));
}

static inline uint64_t
bt_ovf_get64(const uint8_t *p)
{
	return (uint64_t)bt_ovf_get32(p) | ((uint64_t)bt_ovf_get32(p + 4) << 32);
}

#endif /* SQLXTC_BTOVF_H */
