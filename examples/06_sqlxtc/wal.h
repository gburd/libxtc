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
 *	See the design notes (stage 1).
 */

#ifndef SQLXTC_WAL_H
#define SQLXTC_WAL_H

#include <stdint.h>
#include <stddef.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

typedef struct wal wal_t;

#define WAL_HAVE_FAILSTOP 1   /* wal_failed(); commits fail-stop on I/O error */
#define WAL_HAVE_TAIL 1       /* wal_tail_t, wal_scan_ex, wal_get_tail */

/*
 * Where a log ends: the offset just past its last complete frame, the
 * offset of that frame (last_off == end when the log is empty) and the
 * highest LSN in the log.  wal_scan_ex fills one; wal_open (append) takes
 * one so it need not scan the whole log again.  valid is 1 only when the
 * scan reached the end of the log -- a torn tail and the zero-filled
 * preallocated region both count as the end; a callback that stopped the
 * scan early, a missing file or an error leave it 0.
 */
typedef struct wal_tail {
	int      valid;
	uint64_t end;
	uint64_t last_off;
	uint64_t max_lsn;
} wal_tail_t;

typedef struct wal_opts {
	const char *path;          /* log file */
	int64_t     window_ns;     /* group-commit gather window (e.g. 500us) */
	uint32_t    max_batch;     /* cap on records per fsync (e.g. 256) */
	int         append;        /* 1: preserve an existing log and append
	                            * (resume LSNs, drop a torn tail); 0:
	                            * truncate to a fresh empty log */
	const wal_tail_t *tail;    /* append only, optional: the end of THIS
	                            * file as a scan just found it.  Verified
	                            * (the frame at last_off checksums, ends at
	                            * end and carries max_lsn; no frame starts
	                            * at end), else the full scan runs.  NULL
	                            * or !valid: scan, as before. */
} wal_opts_t;

typedef struct wal_stats {
	uint64_t commits;          /* records made durable */
	uint64_t batches;          /* fsync calls (commits/batches == group factor) */
	uint64_t bytes;            /* payload bytes logged */
	uint64_t max_batch_seen;   /* largest batch coalesced */
	uint64_t durable_lsn;      /* highest LSN on stable storage */
	uint64_t failed;           /* commits refused because a log write failed */
} wal_stats_t;

/* Create the log and its backing file (does not spawn the writer). */
int  wal_open(const wal_opts_t *opts, wal_t **out);
void wal_close(wal_t *w);

/* Spawn the group-commit writer process on `loop`.  Returns its pid in
 * *pid (also retrievable via wal_writer_pid). */
int  wal_writer_spawn(wal_t *w, xtc_loop_t *loop, xtc_pid_t *pid);
xtc_pid_t wal_writer_pid(const wal_t *w);

/*
 * Append `record` to the log and block until it is durable: a fiber
 * parks, a plain thread blocks.  Callable from a proc or off a loop.
 * Returns XTC_OK with the assigned durable LSN in *lsn, or an error.
 * Many callers calling concurrently coalesce into one fsync (group
 * commit); the copy into the log buffer runs in parallel.
 *
 * Fail-stop: if a log write or fdatasync fails (or is short), every
 * commit in that batch returns XTC_E_IO, durable_lsn does not advance,
 * and the log latches failed -- every later wal_commit/wal_commit_sync
 * returns XTC_E_IO without writing, until the log is reopened.  A commit
 * that got XTC_E_IO may or may not be on disk after a restart (its bytes
 * may have reached the file before the fsync failed); a commit that got
 * XTC_OK is durable.
 */
int  wal_commit(wal_t *w, const void *record, uint32_t len, uint64_t *lsn);

/* The same as wal_commit (kept as a name: the two paths used to differ). */
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
/* wal_scan that also reports where the log ended (tail may be NULL). */
int  wal_scan_ex(const char *path, wal_replay_cb cb, void *user,
         wal_tail_t *tail);

/*
 * Truncate the log to empty: discard all records and reset the append
 * offset (the LSN counter keeps advancing, so LSNs never repeat).
 * Safe ONLY when the data the log describes is already durable on the
 * data file (after bm_checkpoint) AND no commit is in flight -- a
 * checkpoint quiesces commits, truncates, then resumes.  Returns XTC_OK.
 */
int  wal_truncate(wal_t *w);

/* 1 once a log write/fsync has failed (the log is fail-stopped). */
int  wal_failed(const wal_t *w);

/* Fail-stop the log as a failed write does: every later commit is
 * refused and no compaction replaces the log (the engine calls it when a
 * page of the page file fails its checksum, so the base is not trusted
 * again and the next open rebuilds it from the log). */
void wal_fail_stop(wal_t *w);

/* CRC32C (Castagnoli), hardware-accelerated where the host has it: the
 * log's frame checksum, for the page checksum too.  crc = 0 starts a sum;
 * passing a returned value continues it over the next bytes. */
uint32_t wal_crc32c(uint32_t crc, const void *p, size_t n);

/* Bytes of complete records in the log (the append offset). */
uint64_t wal_size(const wal_t *w);

/*
 * Where an open, quiesced log ends, in the form wal_scan_ex reports and
 * wal_opts_t.tail takes, so a clean shutdown can record it and the next
 * open need not scan.  XTC_OK with out->valid = 1 when nothing is in
 * flight (every reserved byte durable, no direct write, not failed) and
 * the handle knows where its last frame starts: after an append or a
 * compaction on it, an open of an empty log or an open with a verified
 * hint -- not after an open that scanned a non-empty log.  Otherwise
 * XTC_E_AGAIN with valid = 0.
 */
int  wal_get_tail(wal_t *w, wal_tail_t *out);

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
 * layer does not interpret the bytes. A NULL callback or a dump emitting
 * zero records is rejected with XTC_E_INVAL without replacing the log:
 * an empty file cannot preserve its LSN high-water mark across reopen.
 */
typedef void (*wal_emit_fn)(void *emit_ctx, const void *payload, uint32_t len);
int  wal_checkpoint(wal_t *w, const char *path,
         void (*dump)(wal_emit_fn emit, void *emit_ctx, void *user),
         void *user);

/*
 * Online checkpoint: compact the log WITHOUT holding commits for the
 * whole rewrite.  wal_checkpoint must run quiesced from start to end;
 * this splits it so only the two ends need the quiesce:
 *
 *   wal_ockpt_begin   (quiesced)  open path.ocompact; note the live log's
 *                                 durable end -- the copy start point.
 *   emit via wal_ockpt_emitter    write the compacted image (the caller's
 *                                 dump, as of a snapshot taken at begin);
 *                                 commits continue meanwhile.
 *   wal_ockpt_catchup (any time)  append the live log's durable frames
 *                                 past the start point, re-numbered;
 *                                 *left = durable bytes still uncopied.
 *   wal_ockpt_finish  (quiesced)  copy the rest, fsync, rename over path,
 *                                 rebind; frees the handle.
 *   wal_ockpt_abort               drop it (old log untouched).
 *
 * The caller owns correctness of the image (it must be the state as of
 * the start point).  A quiesced rewrite or truncate of the log between
 * begin and finish makes catchup/finish return XTC_E_AGAIN (and finish
 * frees the handle).  LSNs keep rising across it (see wal_checkpoint).
 */
typedef struct wal_ock wal_ock_t;
int  wal_ockpt_begin(wal_t *w, const char *path, wal_ock_t **out);
void wal_ockpt_emitter(wal_ock_t *k, wal_emit_fn *fn, void **ctx);
int  wal_ockpt_catchup(wal_ock_t *k, uint64_t *left);
int  wal_ockpt_finish(wal_ock_t *k);
void wal_ockpt_abort(wal_ock_t *k);
uint64_t wal_ockpt_frames(const wal_ock_t *k);   /* live frames copied */

/* Ask the writer to flush any pending batch and exit.  Call once all
 * committers are done so the loop can drain. */
int  wal_writer_stop(wal_t *w);

void wal_get_stats(wal_t *w, wal_stats_t *out);

#endif /* SQLXTC_WAL_H */
