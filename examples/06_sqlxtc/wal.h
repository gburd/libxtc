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
	const char *path;          /* log file (created/truncated) */
	int64_t     window_ns;     /* group-commit gather window (e.g. 500us) */
	uint32_t    max_batch;     /* cap on records per fsync (e.g. 256) */
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
int  wal_commit(wal_t *w, const void *record, uint16_t len, uint64_t *lsn);

/* Ask the writer to flush any pending batch and exit.  Call once all
 * committers are done so the loop can drain. */
int  wal_writer_stop(wal_t *w);

void wal_get_stats(wal_t *w, wal_stats_t *out);

#endif /* SQLXTC_WAL_H */
