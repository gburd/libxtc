/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/engine.h
 *	The sqlxtc storage-engine API: an "sx_" surface the rest of the
 *	server is written against, so the application code names no
 *	vendored-engine symbols.  engine.c is the single boundary to the
 *	embedded SQL engine; swapping the backend (today SQLite, later a
 *	from-scratch xtc-native engine) touches only that file.  The
 *	storage seams (vfs.c / pcache.c / mutex.c) implement the engine's
 *	own extension interfaces and so name those interface types
 *	directly; everything else speaks sx_.
 *
 *	sx_open applies the concurrency policy: WAL journaling (readers
 *	run concurrently with a writer), a busy timeout (concurrent
 *	writers queue and retry rather than failing), and NORMAL sync.
 *	Each connection opens its own handle, so executions proceed in
 *	parallel.
 */

#ifndef SQLXTC_ENGINE_H
#define SQLXTC_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handles -- the same underlying structs the engine uses, named
 * so the application never references the vendored type names. */
typedef struct xsql      sx_db;
typedef struct xsql_stmt sx_stmt;
struct xtc_loop;                 /* fwd: background storage procs run here */

/* Result codes (values match the engine ABI; engine.c static-asserts). */
#define SX_OK        0
#define SX_ROW     100
#define SX_DONE    101

/* Column datatypes. */
#define SX_INTEGER   1
#define SX_FLOAT     2
#define SX_TEXT      3
#define SX_BLOB      4
#define SX_NULL      5

/* Process-global engine lifecycle. */
int  sx_init(void);
int  sx_shutdown(void);

/* Open the libxtc-native storage engine (an on-disk B-tree over the
 * cooling buffer pool) at `path` with `n_frames` resident pages (0 =
 * default).  Reopens an existing store from its superblock and replays
 * its write-ahead log; otherwise creates a fresh one.  Once open,
 * every connection exposes it as the "xstore" virtual table: SQL
 * against an xstore table runs on this engine, larger-than-RAM and
 * durable, instead of SQLite's built-in B-tree.  Needs no loop. */
int  sx_storage_open(const char *path, unsigned int n_frames);
/* Start the background storage procs (WAL group-commit writer, page
 * provider, trickler) on `loop`.  Call after the loop exists. */
int  sx_storage_run(struct xtc_loop *loop);
/* Flush all dirty pages durable and truncate the log (a checkpoint). */
int  sx_storage_checkpoint(void);
void sx_storage_close(void);

/* 1 if the libxtc-native storage engine is open (so plain CREATE TABLE
 * is routed to it).  0 otherwise. */
int  sx_storage_active(void);

/* Install the xtc_amutex-backed mutex methods (opaque table from
 * mutex_methods()).  Call before sx_init. */
int  sx_config_mutex(const void *methods);

/* Install the xtc-allocator-backed memory methods (opaque table from
 * mem_methods()).  Call before sx_init.  Routes every engine
 * allocation through xtc's allocator. */
int  sx_config_mem(const void *methods);

/* Threading mode (call before sx_init).  sx_config_serialized is the
 * safe default -- the engine guards every handle, so a shared handle
 * and per-connection handles are both correct; the xtc_amutex methods
 * make that guarding yield the fiber rather than block the loop. */
int  sx_config_serialized(void);
int  sx_config_multithread(void);

/* Open a connection on `path` (":memory:" / "" for in-memory).  File-
 * backed databases go through the xtc VFS and get the concurrency
 * policy (WAL + busy timeout).  Each call is an independent handle. */
int  sx_open(const char *path, sx_db **out);
void sx_close(sx_db *h);

/* One-shot statement (no result rows expected), e.g. a PRAGMA. */
int  sx_exec(sx_db *h, const char *sql, char **errmsg);

/* Prepared-statement cursor. */
int  sx_prepare(sx_db *h, const char *sql, int n_bytes, sx_stmt **out,
                const char **tail);
int  sx_step(sx_stmt *st);
int  sx_reset(sx_stmt *st);
int  sx_clear_bindings(sx_stmt *st);
void sx_finalize(sx_stmt *st);

/* Parameter binding (1-based index), for prepared statements. */
int  sx_bind_count(sx_stmt *st);
int  sx_bind_int64(sx_stmt *st, int idx, int64_t v);
int  sx_bind_double(sx_stmt *st, int idx, double v);
int  sx_bind_text(sx_stmt *st, int idx, const char *s, int n);
int  sx_bind_blob(sx_stmt *st, int idx, const void *p, int n);
int  sx_bind_null(sx_stmt *st, int idx);

/* Result-row column accessors. */
int          sx_column_count(sx_stmt *st);
const char  *sx_column_name(sx_stmt *st, int i);
int          sx_column_type(sx_stmt *st, int i);
int64_t      sx_column_int64(sx_stmt *st, int i);
double       sx_column_double(sx_stmt *st, int i);
const char  *sx_column_text(sx_stmt *st, int i);
const void  *sx_column_blob(sx_stmt *st, int i);
int          sx_column_bytes(sx_stmt *st, int i);

/* Diagnostics + DML row count. */
const char  *sx_errmsg(sx_db *h);
int64_t      sx_changes(sx_db *h);

#ifdef __cplusplus
}
#endif

#endif /* SQLXTC_ENGINE_H */
