/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/sql_parse_lime.h
 *	Internal header shared between the Lime grammar (sql_parse.lime),
 *	the tokenizer (sql_parse_lex.c) and the driver (sql_parse_drv.c).
 *	The Lime-generated file includes this; do not move types around
 *	without regenerating sql_parse_gen.c.
 */

#ifndef SQLXTC_SQL_PARSE_LIME_H
#define SQLXTC_SQL_PARSE_LIME_H

#include <stddef.h>

#include "sql_parse.h"
#include "sql_ast.h"

typedef struct sql_token {
	const char *p;
	int         len;
} sql_token_t;

typedef struct sql_parse_state {
	sql_kind_t   kind;        /* kind of the FIRST statement (routing) */
	int          readonly;    /* readonly-ness of the first statement */
	int          error;
	const char  *err_msg;
	sql_arena_t *arena;       /* AST allocation arena (caller-owned) */
	sql_stmt_t  *stmts;       /* parsed statement list (the AST root) */
} sql_parse_state_t;

#endif /* SQLXTC_SQL_PARSE_LIME_H */
