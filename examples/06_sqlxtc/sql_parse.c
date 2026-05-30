/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/sql_parse.c
 *	Phase 1: keyword-based SQL classifier (no Lime yet).
 *	Phase 2: drives the Lime-generated parser in sql_parse_gen.c
 *	via sql_parse_lime() (compiled in when SQLXTC_HAVE_LIME=1).
 */

#include "sql_parse.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Skip whitespace and SQL line/block comments. */
static void
parse_skip_ws(const char **pp, const char *end)
{
	const char *p = *pp;
	while (p < end) {
		if (isspace((unsigned char)*p)) { p++; continue; }
		if (p + 1 < end && p[0] == '-' && p[1] == '-') {
			while (p < end && *p != '\n') p++;
			continue;
		}
		if (p + 1 < end && p[0] == '/' && p[1] == '*') {
			p += 2;
			while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
			if (p + 1 < end) p += 2;
			continue;
		}
		break;
	}
	*pp = p;
}

static int
parse_keyword(const char *p, const char *end, const char *kw)
{
	size_t n = strlen(kw);
	if ((size_t)(end - p) < n) return 0;
	for (size_t i = 0; i < n; i++) {
		char c = p[i];
		if (c >= 'a' && c <= 'z') c -= 32;
		if (c != kw[i]) return 0;
	}
	if (p + n < end) {
		char c = p[n];
		if (isalnum((unsigned char)c) || c == '_') return 0;
	}
	return (int)n;
}

#ifdef SQLXTC_HAVE_LIME
extern int sql_parse_lime(const char *sql, size_t len, sql_info_t *info);
#endif

int
sql_parse(const char *sql, size_t len, sql_info_t *info)
{
	const char *p = sql;
	const char *end = sql + len;
	int n;

	memset(info, 0, sizeof *info);
	info->kind = SQL_KIND_UNKNOWN;
	info->readonly = 0;

	parse_skip_ws(&p, end);

	if ((n = parse_keyword(p, end, "SELECT")) != 0) {
		info->kind = SQL_KIND_SELECT;
		info->readonly = 1;
	} else if ((n = parse_keyword(p, end, "WITH")) != 0) {
		/* CTE -- treat as SELECT for routing; could be DML in
		 * theory but the SELECT classifier covers the common case. */
		info->kind = SQL_KIND_SELECT;
		info->readonly = 1;
	} else if ((n = parse_keyword(p, end, "INSERT")) != 0) {
		info->kind = SQL_KIND_INSERT;
	} else if ((n = parse_keyword(p, end, "REPLACE")) != 0) {
		info->kind = SQL_KIND_INSERT;     /* REPLACE is INSERT-shaped */
	} else if ((n = parse_keyword(p, end, "UPDATE")) != 0) {
		info->kind = SQL_KIND_UPDATE;
	} else if ((n = parse_keyword(p, end, "DELETE")) != 0) {
		info->kind = SQL_KIND_DELETE;
	} else if ((n = parse_keyword(p, end, "CREATE")) != 0) {
		info->kind = SQL_KIND_CREATE;
	} else if ((n = parse_keyword(p, end, "DROP")) != 0) {
		info->kind = SQL_KIND_DROP;
	} else if ((n = parse_keyword(p, end, "PRAGMA")) != 0) {
		info->kind = SQL_KIND_PRAGMA;
		info->readonly = 1;     /* approximation */
	} else if ((n = parse_keyword(p, end, "BEGIN")) != 0) {
		info->kind = SQL_KIND_BEGIN;
	} else if ((n = parse_keyword(p, end, "COMMIT")) != 0) {
		info->kind = SQL_KIND_COMMIT;
	} else if ((n = parse_keyword(p, end, "END")) != 0) {
		info->kind = SQL_KIND_COMMIT;     /* SQLite's END is COMMIT */
	} else if ((n = parse_keyword(p, end, "ROLLBACK")) != 0) {
		info->kind = SQL_KIND_ROLLBACK;
	} else if ((n = parse_keyword(p, end, "ATTACH")) != 0) {
		info->kind = SQL_KIND_ATTACH;
	} else if ((n = parse_keyword(p, end, "DETACH")) != 0) {
		info->kind = SQL_KIND_DETACH;
	} else if ((n = parse_keyword(p, end, "EXPLAIN")) != 0) {
		info->kind = SQL_KIND_EXPLAIN;
		info->readonly = 1;
	} else if (p == end) {
		info->err = "empty statement";
		return -1;
	} else {
		info->err = "unrecognized statement";
		return -1;
	}
	(void)n;

#ifdef SQLXTC_HAVE_LIME
	{
		sql_info_t lime_info = *info;
		int rc = sql_parse_lime(sql, len, &lime_info);
		if (rc == 0 && lime_info.kind != SQL_KIND_UNKNOWN) {
			info->kind = lime_info.kind;
			info->readonly = lime_info.readonly;
		}
	}
#endif

	return 0;
}

char *
sql_canonicalize(const char *sql, size_t len)
{
	char *out = (char *)malloc(len + 1);
	if (!out) return NULL;
	memcpy(out, sql, len);
	out[len] = '\0';
	return out;
}
