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
 *	The design leans on libxtc's actor model.  Each partition is
 *	owned by exactly one xtc_proc; every PRODUCE for that partition
 *	is a message to that proc.  Two consequences fall out for free:
 *
 *	  1. Ordering.  The mailbox serializes appends, so records get
 *	     monotonic offsets with no per-partition lock on the write
 *	     path.  This is the BEAM single-owner insight applied to a
 *	     Kafka partition.
 *
 *	  2. Bounded in-flight writes.  The mailbox cap is the natural
 *	     limit on un-acked produce requests; credit-based flow
 *	     control (Phase 3) builds on it to throttle producers
 *	     instead of dropping.
 *
 *	The high-water mark (next offset) is published through an
 *	xtc_lrlock so consumers read it wait-free, never contending
 *	with the appending writer.
 *
 *	Phase 1 keeps records in memory.  Phase 2 swaps the backing
 *	store for Bitcask-framed segments without changing this API.
 *
 *	The log core below (plog_*) is plain data structure, unit-tested
 *	without any proc or socket.  partition_spawn wraps it in an
 *	xtc_proc.
 */

#ifndef KAKA_PARTITION_H
#define KAKA_PARTITION_H

#include <stddef.h>
#include <stdint.h>

#include "frame.h"

/* ---- in-memory partition log (the testable core) ---- */

typedef struct plog plog_t;

/* Create/destroy an in-memory log starting at base offset 0. */
int   plog_create(plog_t **out);

/*
 * Create a log backed by segmented, CRC-framed files in `dir`
 * (Phase 2).  Records are appended to a segment that rolls to a new
 * file once it exceeds `seg_roll_bytes` (0 selects a 1 MiB default);
 * segment files are named <20-digit-base-offset>.log, Kafka-style.
 * On open, existing segments in `dir` are scanned and replayed so
 * the log resumes at its previous high-water mark.  `dir` must
 * already exist.  Passing dir == NULL is equivalent to plog_create
 * (pure in-memory).  Reads remain O(1) in memory; the segments give
 * durability and crash recovery.
 */
int   plog_create_ex(const char *dir, size_t seg_roll_bytes, plog_t **out);

void  plog_destroy(plog_t *log);

/* Append one record; returns the offset assigned (>= 0) or -1 on OOM.
 * The record bytes are copied into the log. */
int64_t plog_append(plog_t *log, const kaka_record_t *rec);

/* Current high-water mark: the offset the next append will receive,
 * i.e. one past the last stored record. */
uint64_t plog_high_water(const plog_t *log);

/* Fetch the record at `offset` into *rec (borrowed pointers into the
 * log's storage, valid until the next append/destroy).  Returns 1 on
 * hit, 0 if offset >= high-water, -1 on a freed/compacted offset. */
int   plog_read(plog_t *log, uint64_t offset, kaka_record_t *rec);

/* Number of live records. */
uint64_t plog_count(const plog_t *log);

#endif /* KAKA_PARTITION_H */
