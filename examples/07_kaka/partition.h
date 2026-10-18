/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/07_kaka/partition.h
 *
 *	A kaka partition: an ordered, append-only log of records.
 *
 *	Each partition is owned by exactly one xtc_proc; every PRODUCE for
 *	that partition is a message to that proc, so appends need no lock
 *	and offsets come out monotonic (the BEAM single-owner insight
 *	applied to a Kafka partition).
 *
 *	Persistence (plog_create_ex with a directory): records are appended
 *	to CRC-framed segment files that roll at a size threshold.  Each
 *	segment carries an in-memory sparse offset index (one entry per
 *	~4 KiB of log, like Kafka's .index), so a read below the in-RAM
 *	window seeks straight to the record instead of scanning.  Cold
 *	reads use xtc_aio_pread, so the owning proc parks and the loop
 *	keeps serving other connections.
 *
 *	Durability is explicit: plog_append makes a record visible to the
 *	writer but NOT durable; plog_sync (fdatasync via xtc_aio_fdatasync)
 *	makes every appended record durable.  The broker acks a PRODUCE
 *	only after plog_sync succeeds (group commit: one sync covers every
 *	request batched behind it).  A failed write or sync puts the log in
 *	a failed state (fail-stop): every later append fails.
 *
 *	The log core is plain data structure, unit-tested without any proc
 *	or socket (the aio calls run synchronously off a loop).
 */

#ifndef KAKA_PARTITION_H
#define KAKA_PARTITION_H

#include <stddef.h>
#include <stdint.h>

#include "frame.h"

typedef struct plog plog_t;

/* In-memory log starting at offset 0 (no persistence). */
int   plog_create(plog_t **out);

/*
 * Log backed by segment files in `dir` (which must exist).  A segment
 * rolls once it exceeds `seg_roll_bytes` (0 = 1 MiB); files are named
 * <20-digit-base-offset>.log.  On open the segments are scanned to
 * rebuild the offset index and high-water mark; a torn or CRC-failing
 * tail of the newest segment is truncated (a crash mid-append).  An
 * inconsistency in an older segment fails the open rather than
 * silently dropping or renumbering records.  dir == NULL is
 * plog_create.
 */
int   plog_create_ex(const char *dir, size_t seg_roll_bytes, plog_t **out);

/* Size-based retention: after a roll, delete the oldest closed
 * segments while the log exceeds `bytes` (0 = keep everything, the
 * default).  The active segment is never deleted. */
void  plog_set_retention(plog_t *log, uint64_t bytes);

/* Sync, close and free. */
void  plog_destroy(plog_t *log);

/* Append one record (bytes are copied); returns its offset, or -1 on
 * OOM, an oversized record, an I/O error, or a failed log.  Not
 * durable until plog_sync. */
int64_t plog_append(plog_t *log, const kaka_record_t *rec);

/* Make every appended record durable.  0 on success; -1 on a sync
 * failure, after which the log is failed.  No-op in memory mode. */
int   plog_sync(plog_t *log);

/* write(2) every appended record (no fsync): after this a process
 * crash loses none of them.  0 on success; -1 on a write failure,
 * after which the log is failed.  No-op in memory mode. */
int   plog_flush(plog_t *log);

/* 1 once a write or sync has failed (no further appends accepted). */
int   plog_failed(const plog_t *log);

/* High-water mark: the offset the next append receives. */
uint64_t plog_high_water(const plog_t *log);

/* Durable high-water: every offset below it survived a plog_sync (in
 * memory mode, equal to plog_high_water). */
uint64_t plog_synced(const plog_t *log);

/* Oldest offset still stored (advances with retention). */
uint64_t plog_start(const plog_t *log);

/* Read the record at `offset` into *rec (borrowed pointers, valid
 * until the next call on the log).  1 hit, 0 if offset >= high-water
 * or below plog_start, -1 on I/O error. */
int   plog_read(plog_t *log, uint64_t offset, kaka_record_t *rec);

/*
 * Copy records [*offset, end) in FETCH wire format (u64 offset | u32
 * key_len | u32 value_len | key | value) into dst, stopping before the
 * first record that does not fit in `cap` bytes.  An *offset below
 * plog_start starts at plog_start.  Returns the bytes written (>= 0),
 * sets *n to the records copied and advances *offset past them; -1 on
 * an I/O error or a CRC mismatch.  When no record fit, *need (if
 * non-NULL) is set to the next record's wire size.
 */
long  plog_fetch(plog_t *log, uint64_t *offset, uint64_t end, uint8_t *dst,
    size_t cap, uint32_t *n, size_t *need);

/* Total records ever appended (== high-water). */
uint64_t plog_count(const plog_t *log);

/* Records / payload bytes currently resident in RAM.  In persistence
 * mode this is a bounded window (~2 segments) because records of
 * closed segments are evicted (they stay readable from disk). */
uint64_t plog_mem_records(const plog_t *log);
uint64_t plog_mem_bytes(const plog_t *log);

/* Evict every RAM-resident record (persistence mode only; the records
 * stay readable from disk).  Used under memory-budget pressure. */
void  plog_evict_all(plog_t *log);

/* Number of segment files currently on disk (0 in memory mode). */
uint64_t plog_segments(const plog_t *log);

/* Test hook: replace the data-file sync (default xtc_aio_fdatasync).
 * fn returns 0 or a negative errno.  NULL restores the default. */
void  plog_set_sync_hook(int (*fn)(int fd));

#endif /* KAKA_PARTITION_H */
