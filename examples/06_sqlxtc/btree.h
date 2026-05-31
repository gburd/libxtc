/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/btree.h
 *	A B-tree key/value store built on the xtc-native buffer manager
 *	(bufmgr.h) and the prefix-compressed slotted node (btnode.h).
 *
 *	Pages are fixed through the buffer manager's page-table path
 *	(bm_fix_pid), so they are evicted and reloaded by the
 *	cooling-stage machinery transparently -- a tree larger than the
 *	resident pool works.  Leaf nodes map key -> user value; internal
 *	nodes map separator -> child page id (an 8-byte value).  Reads
 *	latch-couple with shared latches; inserts couple with exclusive
 *	latches and release safe ancestors.
 *
 *	Keys and values are caller-supplied byte strings (keys up to a
 *	few hundred bytes; one key+value pair must fit comfortably in a
 *	page).  Keys are compared lexicographically as unsigned bytes.
 */

#ifndef SQLXTC_BTREE_H
#define SQLXTC_BTREE_H

#include <stddef.h>
#include <stdint.h>

#include "bufmgr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bt        bt_t;
typedef struct bt_cursor bt_cursor_t;

/* Create an empty tree (a single root leaf) on the buffer manager.
 * The tree's root page id lives in the handle for its lifetime. */
int  bt_open(bm_t *bm, bt_t **out);
void bt_close(bt_t *bt);

/* Insert or replace key -> val.  Returns XTC_OK, or an error. */
int  bt_insert(bt_t *bt, const void *key, uint16_t klen,
               const void *val, uint16_t vlen);

/* Look up key.  On a hit copies up to cap bytes of the value into buf,
 * sets *vlen to the true value length, and returns XTC_OK.  Returns
 * XTC_E_NOTFOUND if the key is absent. */
int  bt_lookup(bt_t *bt, const void *key, uint16_t klen,
               void *buf, uint16_t cap, uint16_t *vlen);

/* Remove key.  Returns XTC_OK if removed, XTC_E_NOTFOUND if absent.
 * (Leaves may underflow without merging in this version.) */
int  bt_delete(bt_t *bt, const void *key, uint16_t klen);

/* Forward range cursor.  start == NULL positions at the first key;
 * otherwise at the first key >= start.  bt_cursor_next yields each
 * (key, val) in ascending order and returns XTC_OK, or XTC_E_NOTFOUND
 * when exhausted.  The returned pointers are valid until the next
 * bt_cursor_next / bt_cursor_close. */
int  bt_cursor_open(bt_t *bt, const void *start, uint16_t klen,
                    bt_cursor_t **out);
int  bt_cursor_next(bt_cursor_t *c, const void **key, uint16_t *klen,
                    const void **val, uint16_t *vlen);
void bt_cursor_close(bt_cursor_t *c);

/* Observability. */
typedef struct bt_stats {
	uint64_t inserts;
	uint64_t lookups;
	uint64_t splits;
	uint64_t height;        /* number of levels (1 == root leaf) */
} bt_stats_t;
void bt_get_stats(bt_t *bt, bt_stats_t *out);

#ifndef XTC_E_NOTFOUND
#define XTC_E_NOTFOUND (-11)   /* local: key absent (does not collide) */
#endif

#ifdef __cplusplus
}
#endif

#endif /* SQLXTC_BTREE_H */
