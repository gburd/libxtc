/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/mvcc.h
 *	Stage 4 of the scale-out plan (docs/M_SQLXTC_STAGE4.md): snapshot-
 *	isolation MVCC over the share-nothing shards, with cross-shard
 *	transactions committed by a two-phase-commit coordinator.
 *
 *	Each SHARD is one xtc_svr that exclusively owns a versioned
 *	key/value store and its own hybrid logical clock (HLC): a key
 *	maps to a short chain of versions, each tagged with the commit
 *	HLC of the transaction that wrote it.  A read at snapshot
 *	timestamp ts returns the newest committed version with
 *	commit_ts <= ts -- wait-free against other readers.
 *
 *	The COORDINATOR is one xtc_svr that runs transactions.  Commit is
 *	2PC built on the gen_server DEFERRED REPLY (xtc_svr_call_save):
 *	the client's commit call is parked, PREPARE is fanned out to the
 *	participant shards, their votes arrive asynchronously, and the
 *	client is answered only once every vote is in.  A per-shard HLC
 *	gives the commit timestamp without a central allocator.
 *
 *	This is the demonstrator KV, not the SQL engine; it exists to
 *	prove the hardest coordination on libxtc's primitives.
 */

#ifndef SQLXTC_MVCC_H
#define SQLXTC_MVCC_H

#include <stdint.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

#define MVCC_MAX_SHARDS   8
#define MVCC_MAX_WRITES   8     /* keys a single transaction may write */

/* A buffered write in a transaction. */
typedef struct mvcc_write {
	uint32_t key;
	uint32_t value;
} mvcc_write_t;

/*
 * Bring up `n_shards` shard servers (one per loop in shard_loops[]) and
 * one coordinator on coord_loop.  Returns XTC_OK or a negative XTC_E_*.
 */
int  mvcc_start(xtc_loop_t **shard_loops, int n_shards, xtc_loop_t *coord_loop);

/* Stop every server (call once all client procs are done so the loop /
 * executor can drain). */
void mvcc_stop(void);

/* Join the stopped servers and free their state.  Call after the loop /
 * executor has returned (mvcc_stop was issued during the run). */
void mvcc_fini(void);

/* Which shard owns a key (stable hash). */
int  mvcc_shard_of(uint32_t key);

/* ---- client operations (call from inside a proc) ---- */

/* Open a read snapshot: a timestamp at which reads are consistent. */
uint64_t mvcc_begin(void);

/* Read `key` as of snapshot `snap_ts`.  XTC_OK + *out on a hit,
 * XTC_E_NOTFOUND if no version is visible at that snapshot. */
int  mvcc_read(uint32_t key, uint64_t snap_ts, uint32_t *out);

/* Release a snapshot taken with mvcc_begin once no further reads will
 * use it.  Until released, a snapshot pins every version it can see
 * against garbage collection; releasing it lets the coordinator's
 * low-water mark advance so shards can reclaim obsolete versions. */
void mvcc_snapshot_release(uint64_t snap_ts);

/* Total live versions held across all shards (observability: with GC
 * working, a hot key's chain stays short instead of growing). */
int  mvcc_total_versions(void);

/*
 * Atomically commit `n` writes that were decided against snapshot
 * `snap_ts`.  Runs 2PC across the participant shards.  On success
 * returns XTC_OK and, if commit_ts != NULL, the commit timestamp; on a
 * conflict (another transaction committed/locked an overlapping key
 * since `snap_ts`) returns XTC_E_AGAIN (the transaction aborted and
 * may be retried).
 */
int  mvcc_commit(uint64_t snap_ts, const mvcc_write_t *writes, int n,
                 uint64_t *commit_ts);

#endif /* SQLXTC_MVCC_H */
