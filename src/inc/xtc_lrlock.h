/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_lrlock.h
 *	Left-Right concurrency primitive: wait-free reads, single-writer
 *	with cooperative replay.  Ported from PostgreSQL's lrlck branch
 *	(src/backend/storage/lmgr/lrlock.c) — see Pedro Ramalhete &
 *	Andreia Correia, "Left-Right: A Concurrency Control Technique
 *	with Wait-Free Population Oblivious Reads" (2014).
 *
 *	Two copies of the protected data live side-by-side.  An atomic
 *	read_idx says which is currently the "read" copy.
 *
 *	Reader path (wait-free):
 *	  1. set bit[me] in active_readers_mask          (fetch_or)
 *	  2. epochs[me].epoch += 1                       (becomes odd)
 *	  3. SeqCst fence
 *	  4. idx = atomic_read(read_idx)
 *	  5. read data[idx]
 *	  6. epochs[me].epoch += 1                       (becomes even)
 *	  7. clear bit[me]
 *
 *	Writer path:
 *	  - acquire writer mutex
 *	  - mutate data[1 - read_idx] either directly or via apply_op
 *	  - publish:
 *	      atomic_exchange(read_idx, 1 - read_idx)
 *	      SeqCst fence
 *	      snapshot epochs (skipping bits=0 in the active mask)
 *	      wait until each snapshotted reader has advanced
 *	      hybrid sync the now-stale copy:
 *	        if oplog_count*256 <= data_size: replay oplog
 *	        else:                            full sync
 *	  - release writer mutex
 *
 *	Trade-offs vs an RCU pointer:
 *	  + No allocation in the publish path (the two copies are
 *	    pre-allocated).
 *	  + Deterministic reclamation (a single pointer swap).
 *	  + Cache-locality preserved across mutations.
 *	  - Two copies of the data; ~2× memory by default.
 *	  - Writes are slower (applied twice; replay must be
 *	    deterministic).
 *	  - Single writer.
 *
 *	xtc-specific extensions:
 *	  - XTC_LRLOCK_COW: lazy second-copy allocation + MADV_FREE
 *	    after publish.  Idle steady state ≈ 1× memory; first write
 *	    after idle pays an mmap+memcpy.  See docs/M_LRLOCK_COW.md.
 */

#ifndef XTC_LRLOCK_H
#define XTC_LRLOCK_H

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"

typedef struct xtc_lrlock xtc_lrlock_t;

/* Apply a single operation to one copy.  Must be deterministic. */
typedef void (*xtc_lrlock_apply_fn)(void *data, const void *op, size_t op_size);

/* Synchronize destination from source — used during first publish
 * and during full-sync publishes. */
typedef void (*xtc_lrlock_sync_fn)(void *dst, const void *src, size_t data_size);

/* Flags for xtc_lrlock_create_ex(). */
#define XTC_LRLOCK_COW            (1u << 0)   /* lazy data[1], MADV_FREE after publish */

typedef struct xtc_lrlock_opts {
	const char           *name;
	size_t                data_size;
	xtc_lrlock_apply_fn   apply_fn;
	xtc_lrlock_sync_fn    sync_fn;
	int                   max_readers;     /* slot count; 0 → 64 default */
	size_t                oplog_capacity;  /* initial oplog bytes; 0 → 4096 */
	unsigned              flags;
} xtc_lrlock_opts_t;

/*
 * PUBLIC: int   xtc_lrlock_create __P((size_t, xtc_lrlock_apply_fn, xtc_lrlock_sync_fn, const char *, xtc_lrlock_t **));
 * PUBLIC: int   xtc_lrlock_create_ex __P((const xtc_lrlock_opts_t *, xtc_lrlock_t **));
 * PUBLIC: void  xtc_lrlock_destroy __P((xtc_lrlock_t *));
 *
 * PUBLIC: const void *xtc_lrlock_read_begin __P((xtc_lrlock_t *));
 * PUBLIC: void        xtc_lrlock_read_end __P((xtc_lrlock_t *));
 *
 * PUBLIC: void *xtc_lrlock_write_begin __P((xtc_lrlock_t *));
 * PUBLIC: void  xtc_lrlock_apply_op __P((xtc_lrlock_t *, const void *, size_t));
 * PUBLIC: void  xtc_lrlock_publish __P((xtc_lrlock_t *));
 * PUBLIC: void  xtc_lrlock_publish_full_sync __P((xtc_lrlock_t *));
 * PUBLIC: void  xtc_lrlock_write_end __P((xtc_lrlock_t *));
 *
 * PUBLIC: const void *xtc_lrlock_read_data __P((xtc_lrlock_t *));
 * PUBLIC: void       *xtc_lrlock_write_data __P((xtc_lrlock_t *));
 * PUBLIC: void        xtc_lrlock_mark_ready __P((xtc_lrlock_t *));
 */

/* Convenience wrapper: max_readers=64, oplog_capacity=4096, no flags. */
int   xtc_lrlock_create(size_t data_size,
                        xtc_lrlock_apply_fn apply_fn,
                        xtc_lrlock_sync_fn  sync_fn,
                        const char *name,
                        xtc_lrlock_t **out);

int   xtc_lrlock_create_ex(const xtc_lrlock_opts_t *opts,
                           xtc_lrlock_t **out);

void  xtc_lrlock_destroy(xtc_lrlock_t *lr);

/* ---- reader (wait-free) ---- */
const void *xtc_lrlock_read_begin(xtc_lrlock_t *lr);
void        xtc_lrlock_read_end(xtc_lrlock_t *lr);

/* ---- writer (mutex-serialized) ---- */
void *xtc_lrlock_write_begin(xtc_lrlock_t *lr);
void  xtc_lrlock_apply_op(xtc_lrlock_t *lr, const void *op, size_t op_size);
void  xtc_lrlock_publish(xtc_lrlock_t *lr);

/* Like publish, but unconditionally full-syncs (use when the writer
 * mutated data directly, bypassing apply_op). */
void  xtc_lrlock_publish_full_sync(xtc_lrlock_t *lr);

void  xtc_lrlock_write_end(xtc_lrlock_t *lr);

/* Direct accessors (only safe during writer ownership). */
const void *xtc_lrlock_read_data(xtc_lrlock_t *lr);
void       *xtc_lrlock_write_data(xtc_lrlock_t *lr);

/* Tell the lock both copies are pre-initialized to the same state.
 * Skips the first-publish full-sync. */
void  xtc_lrlock_mark_ready(xtc_lrlock_t *lr);

#endif /* XTC_LRLOCK_H */
