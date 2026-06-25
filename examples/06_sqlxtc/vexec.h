/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/vexec.h
 *	Vectorized execution engine -- V0/V1 (recognizer + fallback + a
 *	single-pipeline scan/filter/project executor with a compiled,
 *	vectorized scalar expression evaluator).  See
 *	docs/M_SQLXTC_VEXEC.md.
 *
 *	The engine intercepts a query: a RECOGNIZER inspects the Lime AST
 *	and, if it matches the P1/P2 template (single-table scan,
 *	projection of scalar expressions, optional WHERE of a scalar
 *	boolean expression), builds a vectorized plan that produces
 *	results in DataChunks.  Any query the recognizer does not match --
 *	including any expression whose SQLite affinity/coercion semantics
 *	the engine cannot faithfully reproduce -- is left to run on the
 *	VDBE unchanged, so vexec is always a strict superset and a
 *	fallback is never wrong.
 *
 *	The row source is the reference engine's own cursor (so MVCC
 *	visibility, version decoding, and type/affinity are identical to
 *	the VDBE); vexec compiles the projection and filter from the AST
 *	and evaluates them per row.  The functional gate is the
 *	differential oracle: vexec results equal VDBE results (as a
 *	multiset when the query has no top-level ORDER BY).
 */

#ifndef SQLXTC_VEXEC_H
#define SQLXTC_VEXEC_H

#include <stddef.h>
#include <stdint.h>

/* The connection is an opaque key (the engine's struct xsql). */
struct xsql;

#define VEXEC_VECTOR_SIZE 2048   /* values per vector in a DataChunk */

/* ---- value + chunk model ----------------------------------------- */

typedef enum vx_type {
	VX_NULL = 0, VX_INT, VX_REAL, VX_TEXT, VX_BLOB
} vx_type_t;

/* One materialized cell in a chunk.  TEXT/BLOB bytes are owned by the
 * chunk's arena (copied out of the source row), so a cell stays valid
 * for the chunk's lifetime regardless of cursor movement. */
typedef struct vx_cell {
	vx_type_t type;
	int64_t   i;            /* VX_INT */
	double    r;            /* VX_REAL */
	const uint8_t *bytes;   /* VX_TEXT / VX_BLOB (into the chunk arena) */
	uint32_t  nbytes;
} vx_cell_t;

/* A DataChunk: a batch of up to VEXEC_VECTOR_SIZE rows, stored
 * row-major (ncol cells per row).  sqlxtc is a row store, so chunks
 * are row batches, not column vectors -- there is no selection vector
 * or NULL bitmask; each cell self-describes its storage class. */
typedef struct vx_chunk vx_chunk_t;

/* ---- the vexec statement ----------------------------------------- */

typedef struct vx_stmt vx_stmt_t;

/*
 * Try to build a vexec plan for `sql` against db.  Returns:
 *    1  -- recognized: *out holds a vexec statement; use vx_step/...
 *    0  -- NOT recognized: caller must run `sql` on the VDBE (fallback)
 *   <0  -- error (e.g. prepare failed); *errmsg may be set
 * On a return of 1 the caller owns *out and must vx_finalize it.
 */
int vx_try_prepare(struct xsql *db, const char *sql, vx_stmt_t **out,
                   char **errmsg);

/* Step a vexec statement: returns SQLITE_ROW with a row available
 * (read via vx_column_*), SQLITE_DONE at end, or an SQLITE_ error. */
int vx_step(vx_stmt_t *st);

int          vx_column_count(vx_stmt_t *st);
vx_type_t    vx_column_type(vx_stmt_t *st, int i);
int64_t      vx_column_int64(vx_stmt_t *st, int i);
double       vx_column_double(vx_stmt_t *st, int i);
const char  *vx_column_text(vx_stmt_t *st, int i);
const void  *vx_column_blob(vx_stmt_t *st, int i);
int          vx_column_bytes(vx_stmt_t *st, int i);

void vx_finalize(vx_stmt_t *st);

/* ---- V2: morsel-parallel execution ------------------------------- *
 *
 * Run a recognized P1/P2 query in parallel over N worker tasks on a
 * libxtc executor (one per loop).  The table's rowid space is sliced
 * into morsels handed out by an atomic cursor; each worker opens its
 * OWN storage scan (xstore_scan) over a disjoint rowid range on the
 * connection's shared B-tree, so the cursors are independent and no
 * VDBE is on the hot path.  A final combine concatenates the per-worker
 * buffers (multiset semantics -- row order is unspecified, as the query
 * has no top-level ORDER BY).
 *
 * `db` must be a connection with xstore_register'd tables (the workers
 * scan its B-tree directly).  An ordered, limited, joined, or
 * non-storage plan returns 0 (caller runs it single-threaded / VDBE).
 * On 1, *res is the row-major result owned by the caller (vx_result_free).
 */
typedef struct vx_result vx_result_t;

int vx_run_parallel(struct xsql *db, const char *sql, int n_workers,
                    vx_result_t **res, char **errmsg);

/* Unified dispatcher: the committed vexec entry point for a query.  It
 * recognizes the query once, then routes it:
 *   - a parallelizable single-table scan/aggregation at n_workers > 1
 *     runs on the morsel-parallel storage scan;
 *   - any other recognized plan (ordered, limited, joined, or one
 *     worker) is collected from the serial vectorized path;
 *   - an unrecognized query returns 0 so the caller runs the VDBE.
 * Returns 1 with *res owned by the caller (vx_result_free), 0 to fall
 * back to the VDBE, or <0 on error.
 */
int vx_run(struct xsql *db, const char *sql, int n_workers,
           vx_result_t **res, char **errmsg);

/* Parametrized form of vx_run: `binds` holds the bound ? parameters
 * (1-based by ordinal, `nbinds` of them) as vx_cell values, which the
 * compiler substitutes for SX_E_PARAM nodes.  A parametrized query runs
 * single-threaded.  vx_run is vx_run_p with no binds. */
int vx_run_p(struct xsql *db, const char *sql,
             const vx_cell_t *binds, int nbinds, int n_workers,
             vx_result_t **res, char **errmsg);

/*
 * Native autocommit write path (step 3 of the greenfield teardown).
 * Recognizes a simple INSERT that can run on the xstore B-tree with no
 * VDBE: a single INSERT INTO t VALUES (...), (...) of literal values
 * (INT/REAL/TEXT/NULL, a leading minus allowed) into all columns of an
 * xstore table whose primary key is declared column 0.  On a match it
 * applies every row natively (one commit timestamp per row, WAL-durable)
 * and returns 1 with *nchanges set.  Returns 0 -- caller runs the VDBE
 * -- for anything else (INSERT...SELECT, REPLACE, DEFAULT VALUES, an
 * explicit column list, non-literal values, a non-integer or absent
 * rowid PK, a non-xstore table, or any parse it cannot handle).  All-or-
 * nothing: a return of 0 has written nothing.  <0 is a storage error.
 */
int vx_run_write(struct xsql *db, const char *sql, int64_t *nchanges,
                 char **errmsg);

/* Parametrized form of vx_run_write: ? values in VALUES / SET / WHERE
 * are taken from `binds` (1-based by ordinal).  vx_run_write is this
 * with no binds. */
int vx_run_write_p(struct xsql *db, const char *sql,
                   const vx_cell_t *binds, int nbinds,
                   int64_t *nchanges, char **errmsg);

int            vx_result_nrow(const vx_result_t *r);
int            vx_result_ncol(const vx_result_t *r);
/* The output column's name as derived from the AST select item (the AS
 * alias, or a bare column's name), or NULL when the name is the
 * expression's verbatim source text -- which the AST does not record, so
 * the caller supplies the VDBE-prepared name for those columns. */
const char    *vx_result_name(const vx_result_t *r, int col);
vx_type_t      vx_result_type(const vx_result_t *r, int row, int col);
int64_t        vx_result_int64(const vx_result_t *r, int row, int col);
double         vx_result_double(const vx_result_t *r, int row, int col);
const char    *vx_result_text(const vx_result_t *r, int row, int col);
int            vx_result_bytes(const vx_result_t *r, int row, int col);
/* Number of executor loops the run actually used (for the scaling gate). */
int            vx_result_nworkers(const vx_result_t *r);

void vx_result_free(vx_result_t *r);

/* Build PRAGMA table_info(<table>) natively from the xstore catalog
 * (columns cid,name,type,notnull,dflt_value,pk).  Returns 1 with *res
 * owned by the caller, 0 if the table has no native schema, <0 on OOM. */
int vx_pragma_table_info(struct xsql *db, const char *table, vx_result_t **res);
/* Name the first FROM base table that is not a catalog table or view
 * (the "no such table" culprit) for a failed native query, or NULL.
 * Returns a static buffer. */
const char *vx_unknown_table(struct xsql *db, const char *sql);

#endif /* SQLXTC_VEXEC_H */
