/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * examples/05_rexis/lhash.h
 *	Linear hashing (W. Litwin, 1980) over intrusive chains: the bucket
 *	table behind both the keyspace (db.c) and the bitcask index
 *	(bitcask.c).
 *
 *	The table grows by ONE bucket per insert past load factor 1: a
 *	split rehashes a single chain into itself and one new bucket, so no
 *	operation ever rehashes the whole table and there is no latency
 *	spike when the key count doubles (DragonFly's Dash table makes the
 *	same choice with whole segments; Redis/Valkey rehash incrementally
 *	between two tables instead).  Buckets live in fixed segments of
 *	LH_SEG chain heads reached through a small directory, so growth
 *	never copies the bucket array either; only the directory (one
 *	pointer per segment) is reallocated, doubling.
 *
 *	Addressing: with `low` = 2^level buckets at the start of a round
 *	and `split` buckets already split in it, bucket b = h mod low, or
 *	h mod 2*low when b < split.  n_buckets = low + split.
 *
 *	Memory comes from the caller's allocator (lh_alloc_fn returns
 *	zeroed memory or NULL) so the keyspace can charge it to its budget.
 *	A refused allocation only stops the table from growing: inserts
 *	keep working, with longer chains.
 */

#ifndef REXIS_LHASH_H
#define REXIS_LHASH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define LH_SEG_SHIFT 10
#define LH_SEG       ((size_t)1 << LH_SEG_SHIFT)   /* bucket heads per segment */

typedef struct lh_node {
	struct lh_node *next;
	uint64_t        hash;
} lh_node_t;

typedef void *(*lh_alloc_fn)(void *ctx, size_t sz);         /* zeroed */
typedef void  (*lh_free_fn)(void *ctx, void *p, size_t sz);

typedef struct lh {
	lh_node_t ***dir;     /* dir[i] = segment i (LH_SEG chain heads) */
	size_t       ndir;    /* segments allocated */
	size_t       dircap;  /* directory slots */
	size_t       low;     /* 2^level */
	size_t       split;   /* next bucket to split this round */
} lh_t;

static inline size_t
lh_buckets(const lh_t *t)
{
	return t->low + t->split;
}

/* Head of bucket b (b < lh_buckets). */
static inline lh_node_t **
lh_bucket(const lh_t *t, size_t b)
{
	return &t->dir[b >> LH_SEG_SHIFT][b & (LH_SEG - 1)];
}

/* Head of the bucket that holds hash h. */
static inline lh_node_t **
lh_slot(const lh_t *t, uint64_t h)
{
	size_t b = (size_t)h & (t->low - 1);
	if (b < t->split)
		b = (size_t)h & (2 * t->low - 1);
	return lh_bucket(t, b);
}

static inline int
lh_add_segment(lh_t *t, lh_alloc_fn a, lh_free_fn f, void *ctx)
{
	lh_node_t ***nd;
	lh_node_t **seg;

	/* Segment first: a refused growth must leave nothing allocated. */
	if ((seg = a(ctx, LH_SEG * sizeof *seg)) == NULL)
		return -1;
	if (t->ndir == t->dircap) {
		if ((nd = a(ctx, 2 * t->dircap * sizeof *nd)) == NULL) {
			f(ctx, seg, LH_SEG * sizeof *seg);
			return -1;
		}
		memcpy(nd, t->dir, t->ndir * sizeof *nd);
		f(ctx, t->dir, t->dircap * sizeof *nd);
		t->dir = nd;
		t->dircap *= 2;
	}
	t->dir[t->ndir++] = seg;
	return 0;
}

/* At least min_buckets (rounded up to a power of two, >= LH_SEG). */
static inline int
lh_init(lh_t *t, size_t min_buckets, lh_alloc_fn a, lh_free_fn f, void *ctx)
{
	size_t low = LH_SEG, i;

	while (low < min_buckets)
		low *= 2;
	memset(t, 0, sizeof *t);
	t->dircap = low / LH_SEG;
	if ((t->dir = a(ctx, t->dircap * sizeof *t->dir)) == NULL)
		return -1;
	for (i = 0; i < low / LH_SEG; i++) {
		if (lh_add_segment(t, a, f, ctx) != 0) {
			while (t->ndir > 0)
				f(ctx, t->dir[--t->ndir], LH_SEG * sizeof **t->dir);
			f(ctx, t->dir, t->dircap * sizeof *t->dir);
			t->dir = NULL;
			return -1;
		}
	}
	t->low = low;
	return 0;
}

/* Split one bucket.  -1 (table unchanged) if a segment was refused. */
static inline int
lh_grow(lh_t *t, lh_alloc_fn a, lh_free_fn f, void *ctx)
{
	size_t nb = t->low + t->split, mask = 2 * t->low - 1;
	lh_node_t **from, **to, *n;

	if ((nb >> LH_SEG_SHIFT) >= t->ndir && lh_add_segment(t, a, f, ctx) != 0)
		return -1;
	from = lh_bucket(t, t->split);
	to = lh_bucket(t, nb);
	while ((n = *from) != NULL) {
		if (((size_t)n->hash & mask) != t->split) {
			*from = n->next;
			n->next = *to;
			*to = n;
		} else {
			from = &n->next;
		}
	}
	if (++t->split == t->low) {
		t->low *= 2;
		t->split = 0;
	}
	return 0;
}

/* Free the directory and segments (the chains are the caller's). */
static inline void
lh_destroy(lh_t *t, lh_free_fn f, void *ctx)
{
	if (t->dir == NULL)
		return;
	while (t->ndir > 0)
		f(ctx, t->dir[--t->ndir], LH_SEG * sizeof **t->dir);
	f(ctx, t->dir, t->dircap * sizeof *t->dir);
	t->dir = NULL;
}

#endif /* REXIS_LHASH_H */
