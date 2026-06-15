/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/sql_parse.h
 *	Lime-driven SQL pre-parser.  Used to validate and route a
 *	statement before handing it to SQLite.
 */

#ifndef SQLXTC_SQL_PARSE_H
#define SQLXTC_SQL_PARSE_H

#include <stddef.h>

typedef enum sql_kind {
	SQL_KIND_UNKNOWN = 0,
	SQL_KIND_SELECT,
	SQL_KIND_INSERT,
	SQL_KIND_UPDATE,
	SQL_KIND_DELETE,
	SQL_KIND_CREATE,
	SQL_KIND_DROP,
	SQL_KIND_PRAGMA,
	SQL_KIND_BEGIN,
	SQL_KIND_COMMIT,
	SQL_KIND_ROLLBACK,
	SQL_KIND_ATTACH,
	SQL_KIND_DETACH,
	SQL_KIND_EXPLAIN
} sql_kind_t;

typedef struct sql_info {
	sql_kind_t kind;
	int        readonly;     /* 1 if the statement does not modify state */
	const char *err;         /* parse error message, if any (static) */
} sql_info_t;

/* Pre-parse `sql` (NUL-terminated, len bytes).  Sets info->kind and
 * info->readonly.  Returns 0 on success, -1 on parse error. */
int sql_parse(const char *sql, size_t len, sql_info_t *info);

/*
 * Full parse to an AST (Lime).  On success returns 0 with *arena_out
 * owning the tree (free it with sql_arena_destroy) and *root_out the
 * statement list.  On a parse error returns -1 with *err_out set to a
 * static message and the arena already freed.  Declared here but
 * defined in sql_parse_drv.c (compiled only when the Lime parser is
 * generated); the opaque types come from sql_ast.h.
 */
struct sql_arena;
struct sql_stmt;
int sql_parse_ast(const char *sql, size_t len,
                  struct sql_arena **arena_out, struct sql_stmt **root_out,
                  const char **err_out);

/* Optional: take a parsed AST and serialize it back to canonical SQL.
 * Phase 2 implementation.  Returns malloc'd string on success; caller
 * frees.  Currently passthrough. */
char *sql_canonicalize(const char *sql, size_t len);

#endif /* SQLXTC_SQL_PARSE_H */
