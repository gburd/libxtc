/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/sql_ast.c
 *	Arena allocator for the SQL AST.  A bump allocator over a list
 *	of fixed blocks; one destroy frees the whole tree, so the parser
 *	leaks nothing on an early syntax error.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "sql_ast.h"

#define SQL_ARENA_BLOCK (32 * 1024)

struct sql_arena_block {
	struct sql_arena_block *next;
	size_t                  used;
	size_t                  cap;
	/* payload follows */
	unsigned char           data[];
};

struct sql_arena {
	struct sql_arena_block *head;   /* current block (most recent) */
	int                     oom;    /* a block allocation failed */
};

sql_arena_t *
sql_arena_create(void)
{
	sql_arena_t *a = (sql_arena_t *)calloc(1, sizeof *a);
	return a;
}

void
sql_arena_destroy(sql_arena_t *a)
{
	struct sql_arena_block *b, *n;
	if (a == NULL) return;
	for (b = a->head; b != NULL; b = n) {
		n = b->next;
		free(b);
	}
	free(a);
}

static struct sql_arena_block *
arena_new_block(size_t need)
{
	size_t cap = SQL_ARENA_BLOCK;
	struct sql_arena_block *b;
	if (need > cap) cap = need;
	b = (struct sql_arena_block *)malloc(sizeof *b + cap);
	if (b == NULL) return NULL;
	b->next = NULL;
	b->used = 0;
	b->cap = cap;
	return b;
}

void *
sql_arena_alloc(sql_arena_t *a, size_t n)
{
	struct sql_arena_block *b;
	void *p;

	if (a == NULL || a->oom) return NULL;
	/* Round up to pointer alignment so any struct is aligned. */
	n = (n + (sizeof(void *) - 1)) & ~(size_t)(sizeof(void *) - 1);

	b = a->head;
	if (b == NULL || b->used + n > b->cap) {
		struct sql_arena_block *nb = arena_new_block(n);
		if (nb == NULL) { a->oom = 1; return NULL; }
		nb->next = a->head;
		a->head = nb;
		b = nb;
	}
	p = b->data + b->used;
	b->used += n;
	memset(p, 0, n);
	return p;
}

/* Accumulate the min start / max end over the span-bearing leaves of an
 * expression tree.  All spans point into the same source buffer, so the
 * union is just [min(start), max(end)). */
static void
expr_span_acc(const sql_expr_t *e, const char **lo, const char **hi)
{
	const sql_case_arm_t *arm;
	const sql_exprlist_item_t *it;
	if (e == NULL) return;
	if (e->src != NULL && e->srclen > 0) {
		const char *s = e->src, *t = e->src + e->srclen;
		if (*lo == NULL || s < *lo) *lo = s;
		if (*hi == NULL || t > *hi) *hi = t;
	}
	expr_span_acc(e->a, lo, hi);
	expr_span_acc(e->b, lo, hi);
	expr_span_acc(e->c, lo, hi);
	if (e->list)
		for (it = e->list->head; it; it = it->next)
			expr_span_acc(it->expr, lo, hi);
	for (arm = e->arms; arm; arm = arm->next) {
		expr_span_acc(arm->when, lo, hi);
		expr_span_acc(arm->then, lo, hi);
	}
	expr_span_acc(e->els, lo, hi);
}

int
sql_expr_span(const sql_expr_t *e, const char **p, int *len)
{
	const char *lo = NULL, *hi = NULL;
	expr_span_acc(e, &lo, &hi);
	if (lo == NULL || hi == NULL || hi <= lo) return 0;
	if (p) *p = lo;
	if (len) *len = (int)(hi - lo);
	return 1;
}
