/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/sqlxtc/db.h
 *	SQLite handle management.  In Phase 1 each connection has its
 *	own sqlite3*; in Phase 3 a single shared sqlite3* is used with
 *	the xtc_lwlock-backed mutex methods.
 */

#ifndef SQLXTC_DB_H
#define SQLXTC_DB_H

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_lrlock.h"
#include "xtc_res.h"

#include "quack.h"
#include "sqlite/sqlite3.h"

/* Catalog cache content (read-mostly state).  Just a coarse
 * `n_tables` counter today; placeholder for a fuller schema cache. */
typedef struct db_catalog {
	uint64_t n_tables;
	uint64_t generation;
} db_catalog_t;

typedef struct db {
	sqlite3   *sdb;          /* shared SQLite handle (Phase 3) */
	xtc_res_t *res;
	const char *path;
	int         shared;      /* 1 = single shared handle for all conns */

	xtc_lrlock_t *cat;       /* read-mostly db_catalog_t */
} db_t;

typedef struct db_opts {
	const char *path;        /* ":memory:" by default */
	int         shared;      /* 1 to share one sqlite3* across conns */
	xtc_res_t  *res;
} db_opts_t;

#define DB_OPTS_DEFAULT { .path = ":memory:", .shared = 1, .res = NULL }

int  db_create(const db_opts_t *opts, db_t **out);
void db_destroy(db_t *db);

/* Open a connection-private sqlite3 handle (Phase 1 path).
 * Phase 3 may return the shared handle when db->shared is set. */
int  db_handle_get(db_t *db, sqlite3 **out, int *out_owned);
void db_handle_put(db_t *db, sqlite3 *h, int owned);

/* Execute SQL and stream rows into out_buf via Quack encoder.
 * Returns 0 on success (sets *n_rows); -1 on failure (sets *err
 * to a malloc'd message; caller frees). */
int  db_exec(sqlite3 *h, const char *sql, int64_t limit,
             quack_buf_t *out_buf, int64_t *n_rows, char **err);

#endif /* SQLXTC_DB_H */
