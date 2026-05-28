/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/05_rexis/bitcask.h -- a Bitcask-style on-disk KV store.
 *
 * Bitcask (Riak, 2010) is a log-structured key/value store
 * optimized for write throughput and fast lookups:
 *
 *   * One append-only data file accumulates every put/delete.
 *   * An in-memory hash index maps key -> (offset, len, timestamp).
 *   * A get is an index lookup + one pread.
 *   * A put is an append + index update.
 *   * A delete is a tombstone append + index removal.
 *
 * On crash, the index is rebuilt by scanning the data file at
 * startup.  Background merge (not implemented in this v1) reclaims
 * space when superseded entries accumulate.
 *
 * This v1 supports byte-string keys and values (no TTL / list /
 * hash types -- those layer on top in db_bitcask.c if/when wired
 * into rexis's full API).
 */

#ifndef BITCASK_H
#define BITCASK_H

#include <stddef.h>
#include <stdint.h>

typedef struct bitcask bitcask_t;

typedef struct bitcask_stats {
	uint64_t n_keys;
	uint64_t bytes_used;
	uint64_t bytes_dead;     /* superseded entries waiting for merge */
	uint64_t puts;
	uint64_t gets;
	uint64_t dels;
	uint64_t hits;
	uint64_t misses;
} bitcask_stats_t;

/* Open / create a Bitcask at the given directory.  The directory
 * must already exist; the data file is named "bitcask.data" inside. */
int   bitcask_open(const char *dir, bitcask_t **out);

/* Close + flush.  Subsequent opens of the same directory will
 * recover state from the data file. */
void  bitcask_close(bitcask_t *bc);

/* Put.  Returns 0 on success, -1 on I/O error. */
int   bitcask_put(bitcask_t *bc,
                  const void *key, size_t key_len,
                  const void *val, size_t val_len);

/* Get.  Caller-allocated `val_out` of `val_buf_size` bytes.  On
 * success, *val_len is the actual value size; if val_buf_size is
 * smaller than the value, only the first val_buf_size bytes are
 * copied (callers should size their buffer or call get_size first).
 * Returns 0 on hit, 1 on miss, -1 on I/O error. */
int   bitcask_get(bitcask_t *bc,
                  const void *key, size_t key_len,
                  void *val_out, size_t val_buf_size,
                  size_t *val_len);

/* Lookup the size of the value for `key` without reading it.
 * Returns 0 on hit (size in *val_len), 1 on miss. */
int   bitcask_size(bitcask_t *bc,
                   const void *key, size_t key_len,
                   size_t *val_len);

/* Delete.  Returns 0 if the key existed, 1 if not, -1 on I/O. */
int   bitcask_del(bitcask_t *bc,
                  const void *key, size_t key_len);

/* Iterate all live keys.  Callback receives (key, key_len, user)
 * and returns nonzero to stop early. */
typedef int (*bitcask_iter_fn)(const void *key, size_t key_len, void *user);
int   bitcask_iterate(bitcask_t *bc, bitcask_iter_fn fn, void *user);

/* Force fsync of the data file.  Returns 0 on success. */
int   bitcask_sync(bitcask_t *bc);

/* Stats snapshot. */
void  bitcask_stat(bitcask_t *bc, bitcask_stats_t *out);

#endif
