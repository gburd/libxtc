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
/* Reopen an existing tree from its superblock (bm created with
 * reopen != 0); finds the live root rather than building a fresh one. */
int  bt_reopen(bm_t *bm, bt_t **out);
void bt_close(bt_t *bt);
/* Register a callback invoked by bt_close just before the tree is freed.
 * Used by higher layers (xstore) to drop any per-bt cached state keyed
 * by the bt pointer, so a freed pointer reused by a new tree cannot
 * alias stale entries.  A single global hook; the last setter wins. */
void bt_set_close_hook(void (*fn)(bt_t *));
/* Persist the superblock (root pid, height, clean flag, commit clock).
 * The clean flag and commit clock are opaque metadata set by bt_set_meta:
 * the engine marks the base clean at shutdown and reads it back to decide
 * whether to trust the base on restart or rebuild from the log. */
void bt_write_super(bt_t *bt);
void bt_set_meta(bt_t *bt, uint64_t clean, uint64_t commit_clock);
void bt_get_meta(const bt_t *bt, uint64_t *clean, uint64_t *commit_clock);
/* Stamp `lsn` (the log LSN of the change about to be made) onto every
 * page this tree dirties from now on -- the ARIES page LSN. */
void bt_set_lsn(bt_t *bt, uint64_t lsn);

/*
 * Structure-modification (SMO) logging hook -- the ARIES physiological
 * redo + nested-top-action (NTA) bracket for splits and root growth.
 *
 * A split touches several pages (the splitting node, its new right
 * sibling, a posted-into parent, sometimes a fresh root); a crash that
 * flushed some but not all of them leaves the on-disk tree structurally
 * torn.  ARIES repairs this in place by logging each touched page's
 * full after-image (physiological redo) inside an NTA: a begin marker,
 * the page images, and a closing dummy compensation record whose
 * presence makes a completed SMO redo to completion and never half-undo
 * (Stasis TbeginNestedTopAction / TendNestedTopAction).
 *
 * btree.c stays WAL-agnostic: it calls these hooks, and the engine
 * (xstore.c) implements them over the log (XL_PAGE images + a dummy
 * XL_CLR).  All three may be NULL (the default: no SMO logging, the
 * recovery path rebuilds logically instead of repairing in place).
 *
 *   begin(user)             -> opaque NTA token (e.g. the begin LSN);
 *                              called under the SMO lock before the
 *                              first page of a split is finalized.
 *   page(user, pid, image,   -> log one finished page's full after-image
 *        page_size, lsn)        (the bytes about to be unfixed dirty,
 *                              with `lsn` already stamped at lsn_off).
 *                              Returns the image record's own WAL LSN
 *                              (0 if not logged); btree.c stamps that
 *                              onto the live page so the on-disk page
 *                              LSN matches the LSN recovery gates on.
 *   end(user, token)        -> close the NTA (dummy CLR); the SMO is now
 *                              crash-atomic with respect to recovery.
 *   leaf(user, pid, image,   -> log a PLAIN (non-split) leaf's full
 *        page_size, lsn)        after-image.  Not an SMO: a single
 *                              in-leaf insert dirties one leaf, no NTA.
 *                              This is what lets in-place recovery repair
 *                              a torn NON-split leaf from its image
 *                              (record-LSN gated) instead of losing its
 *                              whole key range to a logical redo that
 *                              descends the torn page.  Returns the image
 *                              record's WAL LSN (0 if not logged); may be
 *                              NULL.
 *
 * A single global hook (the engine is process-global); the last setter
 * wins, matching bt_set_close_hook.
 */
typedef struct bt_smo_hook {
	void    *user;
	uint64_t (*begin)(void *user);
	uint64_t (*page)(void *user, bm_pid_t pid, const void *image,
	                 uint32_t page_size, uint64_t lsn);
	void     (*end)(void *user, uint64_t token);
	uint64_t (*leaf)(void *user, bm_pid_t pid, const void *image,
	                 uint32_t page_size, uint64_t lsn);
} bt_smo_hook_t;
void bt_set_smo_hook(const bt_smo_hook_t *hook);

/* Insert or replace key -> val.  Returns XTC_OK, or an error. */
int  bt_insert(bt_t *bt, const void *key, uint16_t klen,
               const void *val, uint16_t vlen);

/* Look up key.  On a hit copies up to cap bytes of the value into buf,
 * sets *vlen to the true value length, and returns XTC_OK.  Returns
 * XTC_E_NOTFOUND if the key is absent. */
int  bt_lookup(bt_t *bt, const void *key, uint16_t klen,
               void *buf, uint16_t cap, uint16_t *vlen);

/* Remove key.  Returns XTC_OK if removed, XTC_E_NOTFOUND if absent.
 * When the owning leaf falls below the merge threshold the delete
 * triggers a structure-modification pass that merges the leaf's right
 * sibling into it (cascading up and shrinking the tree height when an
 * internal level collapses) and returns the emptied pages to the
 * buffer-manager freelist, so a delete-heavy workload reclaims space
 * instead of bloating forever.  The non-merging common case stays on
 * the latch-free, leaf-exclusive fast path. */
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
	uint64_t merges;        /* node merges (right sibling pulled into left) */
	uint64_t reclaimed;     /* pages returned to the buffer-manager freelist */
	uint64_t height;        /* number of levels (1 == root leaf) */
	uint64_t descents;      /* full root->leaf cursor descents */
	uint64_t resumes;       /* O(1) parked-cursor resumes (no descent) */
} bt_stats_t;
void bt_get_stats(bt_t *bt, bt_stats_t *out);

/*
 * Latch-releasing, position-revalidating cursor.  A caller that must
 * not hold a page latch across an external boundary (the SQLite VDBE
 * calls xUpdate between xNext calls) parks the cursor -- releasing its
 * leaf latch and pin while remembering the leaf and the last key
 * returned -- and later resumes it.  Resume re-fixes the SAME leaf and
 * continues from just past the last key, an O(1) amortized scan step
 * instead of an O(log n) re-descent.  Correct across concurrent splits
 * (page ids are stable for a live leaf and the B-link right-sibling
 * chain reaches any keys that moved right after the park) and across
 * concurrent merges: resume re-fixes the parked leaf and checks it
 * still covers the resume key (it is a leaf whose fence range brackets
 * the last key returned).  A merge that reclaimed the parked page --
 * so the re-fixed page fails that check -- falls back to a fresh
 * root-to-leaf descent from the remembered key, so a reclaimed-and-
 * reused page id can never feed the scan stale rows.
 */
int  bt_cursor_park(bt_cursor_t *c);
int  bt_cursor_resume(bt_cursor_t *c);

/* XTC_E_NOTFOUND (key absent) now comes from the core <xtc.h> enum,
 * pulled in via bufmgr.h. */

#ifdef __cplusplus
}
#endif

#endif /* SQLXTC_BTREE_H */
