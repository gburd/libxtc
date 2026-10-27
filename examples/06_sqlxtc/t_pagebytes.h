/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/t_pagebytes.h
 *	Test-only helper: the bytes of a B-tree node page that belong to
 *	nothing -- outside the header, the slot array, the fences and the
 *	live cells -- and are not zero.  A page the engine built has none,
 *	so a page image never carries memory it was not meant to.  Reads
 *	only the on-disk layout (btnode.h); used against a live page and
 *	against a closed store's page file.
 */
#ifndef SQLXTC_T_PAGEBYTES_H
#define SQLXTC_T_PAGEBYTES_H

#include <stdint.h>
#include <string.h>

#include "btnode.h"

/* Non-zero unowned bytes of a node page, or -1 if its header is unusable. */
static long
t_dead_bytes(const void *page)
{
	const struct btnode_hdr *h = page;
	const struct btnode_slot *s = (const struct btnode_slot *)
	    ((const uint8_t *)page + sizeof *h);
	const uint8_t *p = page;
	uint8_t own[65536];
	uint32_t ps = h->page_size, send, i, k;
	long n = 0;

	if (ps <= sizeof *h || ps > sizeof own)
		return -1;
	send = (uint32_t)sizeof *h + (uint32_t)h->count *
	    (uint32_t)sizeof(struct btnode_slot);
	if (send > ps)
		return -1;
	memset(own, 0, ps);
	memset(own, 1, send);
	if (h->lo_fence_len && (uint32_t)h->lo_fence_off + h->lo_fence_len <= ps)
		memset(own + h->lo_fence_off, 1, h->lo_fence_len);
	if (h->hi_fence_len && (uint32_t)h->hi_fence_off + h->hi_fence_len <= ps)
		memset(own + h->hi_fence_off, 1, h->hi_fence_len);
	for (i = 0; i < h->count; i++) {
		k = (uint32_t)s[i].key_len + (s[i].val_len & 0x7FFFu);
		if ((uint32_t)s[i].offset + k <= ps)
			memset(own + s[i].offset, 1, k);
	}
	for (i = 0; i < ps; i++)
		if (!own[i] && p[i] != 0)
			n++;
	return n;
}

#endif /* SQLXTC_T_PAGEBYTES_H */
