/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/g_vfs.h
 *	An xtc-native SQLite VFS.
 *
 *	A "shim" VFS named "sqlxtc" that layers over the platform default
 *	VFS.  Path resolution, randomness, sleep, and the byte-range
 *	file locks delegate to the base VFS so behaviour stays exactly
 *	POSIX-correct.  Every byte of database I/O, however, flows
 *	through this layer, which:
 *
 *	  - allocates its per-file state with the xtc allocator
 *	    (__os_malloc / __os_free), and
 *	  - records read/write counts, byte volumes, and per-call
 *	    latencies in xtc_stats counters and histograms.
 *
 *	This is the storage seam of the sqlxtc hard-fork: a single,
 *	instrumented choke point for all page traffic.  It is where an
 *	async, coroutine-yielding pager (read submitted to xtc_io, the
 *	calling fiber parked until completion) plugs in next; see
 *	docs/M_SQLXTC_GREENFIELD.md.
 */

#ifndef SQLXTC_VFS_H
#define SQLXTC_VFS_H

#include <stdint.h>

/*
 * Register the "sqlxtc" VFS.  Idempotent: a second call is a no-op.
 * If make_default is non-zero the xtc VFS becomes the default, so
 * sqlite3_open() uses it without an explicit vfs name.  Returns
 * SQLITE_OK on success.
 */
int vfs_register(int make_default);

/* I/O statistics gathered by the xtc VFS, for the metrics path. */
typedef struct {
	uint64_t reads;          /* xRead calls */
	uint64_t writes;         /* xWrite calls */
	uint64_t bytes_read;
	uint64_t bytes_written;
	uint64_t syncs;          /* xSync calls */
	double   read_p50_us;    /* read latency quantiles (microseconds) */
	double   read_p99_us;
	double   write_p50_us;
	double   write_p99_us;
} vfs_stats_t;

/* Snapshot the VFS I/O counters.  Safe to call from any thread. */
void vfs_get_stats(vfs_stats_t *out);

#endif /* SQLXTC_VFS_H */
