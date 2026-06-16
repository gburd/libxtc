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

#include "sqlite3.h"   /* xsql_* via the force-included rename header */

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

/* A DataChunk: ncol columns by up to VEXEC_VECTOR_SIZE rows, stored
 * row-major in V0 for simplicity (V1 switches to columnar vectors with
 * a selection vector + NULL bitmask). */
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
int vx_try_prepare(sqlite3 *db, const char *sql, vx_stmt_t **out,
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
 * Run a recognized P1/P2 query in parallel over N worker procs on a
 * libxtc executor (one worker per loop).  The table's rowid space is
 * sliced into morsels handed out by an atomic cursor; each worker opens
 * its OWN connection to `db_path` (so the cursors are independent),
 * runs the compiled plan over its morsels, and appends surviving rows
 * to its own buffer.  A final combine concatenates the per-worker
 * buffers (multiset semantics -- row order is unspecified, as the query
 * has no top-level ORDER BY).
 *
 * The collected result is returned as a flat array of cells, row-major:
 * *out_cells has (*out_nrow * *out_ncol) entries, owned by the returned
 * vx_result and freed by vx_result_free.  TEXT/BLOB bytes are owned by
 * the result.
 *
 * Returns 1 if the query was recognized and run in parallel (*res set),
 * 0 if not recognized (caller falls back to the VDBE), <0 on error.
 */
typedef struct vx_result vx_result_t;

int vx_run_parallel(const char *db_path, const char *sql, int n_workers,
                    vx_result_t **res, char **errmsg);

int            vx_result_nrow(const vx_result_t *r);
int            vx_result_ncol(const vx_result_t *r);
vx_type_t      vx_result_type(const vx_result_t *r, int row, int col);
int64_t        vx_result_int64(const vx_result_t *r, int row, int col);
double         vx_result_double(const vx_result_t *r, int row, int col);
const char    *vx_result_text(const vx_result_t *r, int row, int col);
int            vx_result_bytes(const vx_result_t *r, int row, int col);
/* Number of executor loops the run actually used (for the scaling gate). */
int            vx_result_nworkers(const vx_result_t *r);

void vx_result_free(vx_result_t *r);

#endif /* SQLXTC_VEXEC_H */
