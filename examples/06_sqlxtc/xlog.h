/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/xlog.h
 *	ARIES log-record vocabulary and codec.
 *
 *	The write-ahead log (wal.c) is a generic append-only log of
 *	opaque byte records: it assigns each record an LSN and frames it
 *	with a length, but does not interpret the bytes.  This module
 *	defines those bytes -- the record format recovery reads.
 *
 *	Every record begins with a common header: a one-byte type, the
 *	owning transaction id, and prev_lsn -- the LSN of the previous
 *	record written by the same transaction.  prev_lsn chains a
 *	transaction's records backward so the undo pass can walk them
 *	from newest to oldest.  The record's own LSN is the one the log
 *	assigns at append time (returned by wal_commit, passed to the
 *	replay callback); it is not stored in the payload.
 *
 *	Record types:
 *	  XL_BEGIN       a transaction's first record (header only).
 *	  XL_UPDATE      one change: redo (after-image) and undo
 *	                 (before-image).  Redo re-applies it during the
 *	                 redo pass; undo reverses it during the undo
 *	                 pass.  Carries the page it touches so redo can
 *	                 be gated by that page's LSN.
 *	  XL_COMMIT      the transaction committed (header only).
 *	  XL_ABORT       the transaction aborted (header only).
 *	  XL_CLR         a compensation log record: the redo-only record
 *	                 written while undoing an XL_UPDATE.  Carries
 *	                 undo_next_lsn -- the next record of the loser
 *	                 still to undo -- so a crash during recovery
 *	                 resumes without re-undoing compensated work.  A
 *	                 CLR is never itself undone.
 *	  XL_CHECKPOINT  carries the persisted commit clock; the live
 *	                 state dump (a run of XL_UPDATE records) follows
 *	                 it in the compacted log.
 *	  XL_END         a loser's undo is complete (header only).
 *
 *	Field order within a record is fixed and host-endian (the log is
 *	a single-host artifact, like the page file); all integer reads
 *	and writes go through memcpy, matching the rest of the engine.
 *
 *	This is the format the redo/undo recovery driver consumes; see
 *	docs/M_SQLXTC_BDB.md stages S2 (logging) and S3 (recovery).
 */

#ifndef SQLXTC_XLOG_H
#define SQLXTC_XLOG_H

#include <stddef.h>
#include <stdint.h>

typedef enum xl_type {
	XL_BEGIN      = 1,
	XL_UPDATE     = 2,
	XL_COMMIT     = 3,
	XL_ABORT      = 4,
	XL_CLR        = 5,
	XL_CHECKPOINT = 6,
	XL_END        = 7
} xl_type_t;

/* Common record header: type:1 + txn_id:8 + prev_lsn:8. */
#define XL_HDR_LEN 17

typedef struct xl_hdr {
	uint8_t  type;       /* xl_type_t */
	uint64_t txn_id;     /* owning transaction (0 for XL_CHECKPOINT) */
	uint64_t prev_lsn;   /* previous LSN of this transaction (0 if first) */
} xl_hdr_t;

/*
 * Body of an XL_UPDATE or XL_CLR.  For the multi-version store a change
 * is located by its key (tableid, rowid, commit_ts); redo is the
 * version's value (after-image) and undo is the prior value
 * (before-image).  An insert of a brand-new version has undo_len == 0:
 * its inverse is to remove the key, not restore an older value.  page_id
 * is the B-tree page the change lands on, used to gate redo by page LSN
 * (0 when not yet known).  undo_next_lsn is meaningful only for XL_CLR.
 */
typedef struct xl_body {
	uint32_t    page_id;
	uint64_t    undo_next_lsn;   /* XL_CLR only */
	uint32_t    tableid;
	int64_t     rowid;
	uint64_t    commit_ts;
	uint8_t     flags;           /* tombstone, etc. (matches the value flag byte) */
	const void *redo;            /* after-image (NULL if redo_len == 0) */
	uint16_t    redo_len;
	const void *undo;            /* before-image (NULL if undo_len == 0); XL_UPDATE only */
	uint16_t    undo_len;
} xl_body_t;

/*
 * Encoders.  Each serializes one record into buf (capacity cap) and
 * returns the number of bytes written, or a negative XTC_E_* code if
 * the buffer is too small or an argument is invalid.  xl_enc_simple
 * encodes the header-only records (XL_BEGIN/COMMIT/ABORT/END).
 */
int xl_enc_simple(uint8_t *buf, size_t cap, const xl_hdr_t *h);
int xl_enc_update(uint8_t *buf, size_t cap, const xl_hdr_t *h, const xl_body_t *b);
int xl_enc_clr(uint8_t *buf, size_t cap, const xl_hdr_t *h, const xl_body_t *b);
int xl_enc_checkpoint(uint8_t *buf, size_t cap, uint64_t commit_clock);

/* The exact encoded size of an UPDATE/CLR body for a given payload, so
 * a caller can size its buffer (XL_HDR_LEN + this). */
size_t xl_update_size(uint16_t redo_len, uint16_t undo_len);
size_t xl_clr_size(uint16_t redo_len);

/*
 * Decoders.  xl_parse_hdr fills *h from the first XL_HDR_LEN bytes and
 * validates len.  xl_parse_update / xl_parse_clr additionally fill *b
 * (its redo/undo pointers alias into rec, valid for the record's
 * lifetime).  xl_parse_checkpoint extracts the commit clock.  Each
 * returns XTC_OK, or a negative XTC_E_* code for a malformed or
 * truncated record (the caller treats that as the torn tail and stops).
 */
int xl_parse_hdr(const void *rec, uint32_t len, xl_hdr_t *h);
int xl_parse_update(const void *rec, uint32_t len, xl_hdr_t *h, xl_body_t *b);
int xl_parse_clr(const void *rec, uint32_t len, xl_hdr_t *h, xl_body_t *b);
int xl_parse_checkpoint(const void *rec, uint32_t len, uint64_t *commit_clock);

/*
 * Total encoded length of the record at the front of rec, given avail
 * readable bytes.  Lets a reader walk a buffer holding several records
 * back to back (one WAL frame can carry a whole transaction's records).
 * Returns the record length, or a negative XTC_E_* code if avail does
 * not hold a complete record (a torn tail) or the type is unknown.
 */
int xl_record_len(const void *rec, uint32_t avail);

#endif /* SQLXTC_XLOG_H */
