/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/wal.h
 *	A write-ahead log with a dedicated group-commit writer process.
 *
 *	The WAL writer is a single xtc_proc that exclusively owns the log
 *	file (durability is inherently serial -- one append-only writer,
 *	exactly the shape kaka gives a partition).  Committing fibers do
 *	NOT write the file; each sends its record to the writer and parks
 *	on the acknowledgement.
 *
 *	Group commit: the writer drains its mailbox, batching every
 *	commit that arrives within a short window (or until a batch cap),
 *	writes the whole batch with ONE write(2) and amortizes ONE
 *	fsync(2) across all of them, then acks every committer in the
 *	batch.  The file write and the fsync are offloaded via
 *	xtc_blocking_run so the loop keeps serving peers while the writer
 *	is parked on disk.  This is the canonical place a database trades
 *	one fsync per commit for one fsync per batch.
 *
 *	See docs/M_SQLXTC_SCALEOUT.md (stage 1).
 */

#ifndef SQLXTC_WAL_H
#define SQLXTC_WAL_H

#include <stdint.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

typedef struct wal wal_t;

typedef struct wal_opts {
	const char *path;          /* log file */
	int64_t     window_ns;     /* group-commit gather window (e.g. 500us) */
	uint32_t    max_batch;     /* cap on records per fsync (e.g. 256) */
	int         append;        /* 1: preserve an existing log and append
	                            * (resume LSNs, drop a torn tail); 0:
	                            * truncate to a fresh empty log */
} wal_opts_t;

typedef struct wal_stats {
	uint64_t commits;          /* records made durable */
	uint64_t batches;          /* fsync calls (commits/batches == group factor) */
	uint64_t bytes;            /* payload bytes logged */
	uint64_t max_batch_seen;   /* largest batch coalesced */
	uint64_t durable_lsn;      /* highest LSN on stable storage */
} wal_stats_t;

/* Create the log and its backing file (does not spawn the writer). */
int  wal_open(const wal_opts_t *opts, wal_t **out);
void wal_close(wal_t *w);

/* Spawn the group-commit writer process on `loop`.  Returns its pid in
 * *pid (also retrievable via wal_writer_pid). */
int  wal_writer_spawn(wal_t *w, xtc_loop_t *loop, xtc_pid_t *pid);
xtc_pid_t wal_writer_pid(const wal_t *w);

/*
 * Append `record` to the log and block (park the fiber) until it is
 * durable.  Must be called from a process (it parks on an ack message
 * from the writer).  Returns XTC_OK with the assigned durable LSN in
 * *lsn, or an error.  Many callers calling concurrently coalesce into
 * one fsync.
 */
int  wal_commit(wal_t *w, const void *record, uint32_t len, uint64_t *lsn);

/*
 * Synchronous durable append, usable OFF a loop (no writer process):
 * appends one record and fdatasyncs before returning, serialized by an
 * internal mutex.  Same on-disk format as wal_commit, so wal_scan
 * replays records written by either path.  Use when the committer is
 * not running on an xtc loop; on a loop, prefer wal_commit (group
 * commit).  Do not mix with a spawned writer on the same log.
 */
int  wal_commit_sync(wal_t *w, const void *record, uint32_t len, uint64_t *lsn);

/*
 * Replay: scan the log file at `path` in LSN order, invoking `cb` for
 * each complete record.  A torn trailing record (a partial write at
 * crash) is detected by a short read and ends the scan -- everything
 * before it is intact.  Reads the raw file, so it runs at startup
 * before any writer is spawned.  cb returns 0 to continue, non-zero to
 * stop.
 */
typedef int (*wal_replay_cb)(uint64_t lsn, const void *rec, uint32_t len,
    void *user);
int  wal_scan(const char *path, wal_replay_cb cb, void *user);

/*
 * Truncate the log to empty: discard all records and reset the append
 * offset (the LSN counter keeps advancing, so LSNs never repeat).
 * Safe ONLY when the data the log describes is already durable on the
 * data file (after bm_checkpoint) AND no commit is in flight -- a
 * checkpoint quiesces commits, truncates, then resumes.  Returns XTC_OK.
 */
int  wal_truncate(wal_t *w);

/* The highest LSN on stable storage (fsync'd).  Monotonic. */
uint64_t wal_durable_lsn(const wal_t *w);

/* The underlying WAL file descriptor.  Exposed for the DST crash tests,
 * which arm the sim write-back model on it (a crash loses bytes past the
 * last fsync) to verify durability is tied to fsync, not just to the
 * writer's self-reported LSN.  Returns -1 if the WAL is closed. */
int wal_fd(const wal_t *w);

/* Ensure the log is durable through `lsn`: XTC_OK if it already is,
 * XTC_E_AGAIN if not yet (the buffer manager then defers writing a page
 * whose LSN is past the durable point -- the write-ahead rule). */
int  wal_flush_through(wal_t *w, uint64_t lsn);

/*
 * In-WAL checkpoint (log-compaction).  Atomically rewrites the log as a
 * fresh CHECKPOINT record (carrying `clock`) followed by whatever
 * `dump` emits -- the live database state as redo records.  This is the
 * checkpoint: it lives IN the log, not in a side file, and it bounds
 * the log because every record before it is superseded by the dump and
 * discarded.  Recovery replays the compacted log onto a fresh tree.
 *
 * The rewrite goes to `path`.compact, is fsync'd, atomically renamed
 * over `path`, and the handle is rebound to it (LSNs resume past the
 * dump).  Crash-atomic: a crash before the rename leaves the previous
 * (complete) log intact; after it, the compacted log.  Call ONLY when
 * commits are quiesced.  Returns XTC_OK or an error (log unchanged).
 *
 * `dump(emit, emit_ctx, user)` calls emit(emit_ctx, payload, len) once
 * per record of the compacted log, in the WAL payload format wal_scan
 * delivers.  The dump emits the leading checkpoint record itself; this
 * layer does not interpret the bytes.
 */
typedef void (*wal_emit_fn)(void *emit_ctx, const void *payload, uint32_t len);
int  wal_checkpoint(wal_t *w, const char *path,
         void (*dump)(wal_emit_fn emit, void *emit_ctx, void *user),
         void *user);

/* Ask the writer to flush any pending batch and exit.  Call once all
 * committers are done so the loop can drain. */
int  wal_writer_stop(wal_t *w);

void wal_get_stats(wal_t *w, wal_stats_t *out);

#endif /* SQLXTC_WAL_H */
