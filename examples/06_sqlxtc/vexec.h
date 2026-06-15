/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/vexec.h
 *	Vectorized execution engine -- V0 (recognizer + fallback + a
 *	single-pipeline scan/filter/project executor).  See
 *	docs/M_SQLXTC_VEXEC.md.
 *
 *	The engine intercepts a prepared statement: a RECOGNIZER inspects
 *	the planner's opcode program (via EXPLAIN, the stable public
 *	surface) and, if it matches the P1 template (single-table scan,
 *	optional WHERE filter, projection of columns/rowid), builds a
 *	vectorized plan that produces results in DataChunks.  Any query
 *	the recognizer does not match is left to run on the VDBE unchanged
 *	-- vexec is always a strict superset, so a fallback is never wrong.
 *
 *	V0 is single-worker and reads rows through the reference engine's
 *	own cursor (so MVCC visibility, version decoding, and type/affinity
 *	are identical to the VDBE); the vectorization is the chunked,
 *	push-based shape that V1+ build parallelism and a native expression
 *	evaluator on top of.  The functional gate is the differential
 *	oracle: vexec results equal VDBE results (as a multiset when the
 *	query has no top-level ORDER BY).
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

#endif /* SQLXTC_VEXEC_H */
