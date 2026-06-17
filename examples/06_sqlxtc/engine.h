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

/* Configure the page store's I/O for the next sx_storage_open:
 * direct = cache-bypass (direct I/O), adaptive = GA-tuned writeback. */
void sx_storage_set_io(int direct, int adaptive);
/* Start the background storage procs (WAL group-commit writer, page
 * provider, trickler) on `loop`.  Call after the loop exists. */
int  sx_storage_run(struct xtc_loop *loop);
/* Flush all dirty pages durable (a running checkpoint).  Does not
 * truncate the log -- see engine.c. */
int  sx_storage_checkpoint(void);
void sx_storage_quiesce(void);
void sx_storage_close(void);
/* Drop all in-memory engine state WITHOUT a checkpoint or clean marker,
 * simulating a crash (dirty pages lost, the log left intact and
 * durable).  The next sx_storage_open rebuilds the tree from the full
 * log.  Intended for crash-recovery tests. */
void sx_storage_abandon(void);

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

/*
 * Vectorized-executor fast path.  sx_vexec_try recognizes a read-only
 * query and, when it can run it on the libxtc-native vectorized executor
 * (vexec) over the xstore B-tree, materializes the full result and
 * returns 1 with *out set (caller must sx_vexec_free it).  It returns 0
 * when the query is not recognized (the caller runs the VDBE), or a
 * negative XTC_E_* on error.  n_workers > 1 requests morsel-parallel
 * execution where the plan supports it.  This is correct-by-fallback:
 * anything vexec cannot reproduce exactly is left to the VDBE.
 *
 * The result carries typed cells but NOT column names; the caller
 * supplies names from the parallel VDBE prepare (so a client sees
 * identical headers whichever path served the rows).
 */
typedef struct sx_vx_result sx_vx_result;
int              sx_vexec_try(sx_db *h, const char *sql, int n_workers,
                             sx_vx_result **out);
void             sx_vexec_free(sx_vx_result *r);int              sx_vexec_nrow(const sx_vx_result *r);
int              sx_vexec_ncol(const sx_vx_result *r);
/* Column cell type: returns one of SX_INTEGER/SX_FLOAT/SX_TEXT/SX_BLOB/SX_NULL. */
int              sx_vexec_type(const sx_vx_result *r, int row, int col);
int64_t          sx_vexec_int64(const sx_vx_result *r, int row, int col);
double           sx_vexec_double(const sx_vx_result *r, int row, int col);
const char      *sx_vexec_text(const sx_vx_result *r, int row, int col);
const void      *sx_vexec_blob(const sx_vx_result *r, int row, int col);
int              sx_vexec_bytes(const sx_vx_result *r, int row, int col);
/* Output column name from the plan, or NULL when the caller should use
 * the VDBE-prepared name (an expression column whose name is its source
 * text). */
const char      *sx_vexec_name(const sx_vx_result *r, int col);

/*
 * Native write fast path.  Recognizes a simple literal-row INSERT into
 * an xstore table and applies it directly to the B-tree (no VDBE / no
 * vtab round-trip), returning 1 with *nchanges set.  Returns 0 when the
 * statement is not a recognized native write (the caller runs the
 * VDBE), or <0 on a storage error.  All-or-nothing on a 0 return.
 */
int              sx_vexec_write(sx_db *h, const char *sql, int64_t *nchanges);

#ifdef __cplusplus
}
#endif

#endif /* SQLXTC_ENGINE_H */
