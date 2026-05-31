/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/db.h
 *	Engine-handle management.  File-backed databases use a private
 *	handle per connection (concurrent executions; WAL readers run
 *	concurrently with a writer); an in-memory database uses one
 *	shared handle.  All engine access is via the sx_ API (engine.h);
 *	this layer names no vendored-engine symbols.
 */

#ifndef SQLXTC_DB_H
#define SQLXTC_DB_H

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_lrlock.h"
#include "xtc_res.h"

#include "quack.h"
#include "engine.h"

/* Catalog cache content (read-mostly state).  Just a coarse
 * `n_tables` counter today; placeholder for a fuller schema cache. */
typedef struct db_catalog {
	uint64_t n_tables;
	uint64_t generation;
} db_catalog_t;

typedef struct db {
	sx_db     *sdb;          /* shared handle (in-memory databases) */
	xtc_res_t *res;
	const char *path;
	int         shared;      /* 1 = single shared handle for all conns */

	xtc_lrlock_t *cat;       /* read-mostly db_catalog_t */
} db_t;

typedef struct db_opts {
	const char *path;        /* ":memory:" by default */
	int         shared;      /* 1 to share one handle; auto-forced for
	                          * in-memory; file-backed defaults per-conn */
	xtc_res_t  *res;
} db_opts_t;

#define DB_OPTS_DEFAULT { .path = ":memory:", .shared = 1, .res = NULL }

int  db_create(const db_opts_t *opts, db_t **out);
void db_destroy(db_t *db);

/* Acquire an engine handle: the shared one when db->shared is set,
 * otherwise a fresh connection-private handle (*out_owned == 1, the
 * caller releases it via db_handle_put). */
int  db_handle_get(db_t *db, sx_db **out, int *out_owned);
void db_handle_put(db_t *db, sx_db *h, int owned);

/* Execute SQL and stream rows into out_buf via Quack encoder.
 * Returns 0 on success (sets *n_rows); -1 on failure (sets *err
 * to a malloc'd message; caller frees). */
int  db_exec(sx_db *h, const char *sql, int64_t limit,
             quack_buf_t *out_buf, int64_t *n_rows, char **err);

/* Like db_exec but binds parameters (?1..?N) from a request's params
 * array before stepping (see quack.h). */
int  db_exec_params(sx_db *h, const char *sql,
             const struct quack_param *params, int n_params, int64_t limit,
             quack_buf_t *out_buf, int64_t *n_rows, char **err);

#endif /* SQLXTC_DB_H */
