/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/qsel.h
 *	QuickSelect (Hoare's FIND) over an array of element ids, with a
 *	caller comparator: rank selection in O(n) expected comparisons.
 *	sqlxtc's bounded ORDER BY ... LIMIT K keeps up to 2K rows and cuts
 *	them back to the K best with qsel_nth whenever the buffer fills,
 *	so a scan of N rows costs O(N + K log K) whatever the arrival
 *	order (incremental QuickSelect: Paredes & Navarro, "Optimal
 *	Incremental Sorting", ALENEX 2006).
 *
 *	Header-only (static inline): the one caller is vexec.c, and a new
 *	.c would have to join every object list that builds the engine.
 */
#ifndef SQLXTC_QSEL_H
#define SQLXTC_QSEL_H

#include <stdint.h>

/* <0, 0, >0 as element a sorts before, with, after element b. */
typedef int (*qsel_cmp_fn)(int a, int b, void *ctx);

/*
 * Permute v[0..n) so that v[k-1] holds the element of rank k (1-based)
 * under cmp, every element of v[0..k-1) sorts at or before it, and
 * every element of v[k..n) at or after it.  Requires 1 <= k <= n.
 *
 * Pivots are the median of three positions drawn from *rng (caller
 * state, any nonzero seed): no global RNG, so the result is a pure
 * function of the input and the seed (replayable under the simulator).
 * After 2 log2(n) + 8 rounds that do not finish (an adversarial input
 * for the pivot rule) the remaining range is heap-sorted, so the worst
 * case is O(n log n) comparisons.  Allocates nothing; not stable
 * (supply a total order, e.g. break ties on an arrival number, when the
 * caller needs one).
 */
#define QSEL_SMALL 16   /* insertion-sort ranges at or below this */

static inline uint32_t
qs_rnd(uint64_t *s, uint32_t n)
{
	uint64_t x = *s;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	*s = x;
	return (uint32_t)((x * 0x2545F4914F6CDD1DULL) >> 32) % n;
}

static inline void
qs_swap(int *v, int a, int b)
{
	int t = v[a];

	v[a] = v[b];
	v[b] = t;
}

static inline void
qs_isort(int *v, int lo, int hi, qsel_cmp_fn cmp, void *ctx)
{
	int i, j, x;

	for (i = lo + 1; i <= hi; i++) {
		x = v[i];
		for (j = i; j > lo && cmp(x, v[j - 1], ctx) < 0; j--)
			v[j] = v[j - 1];
		v[j] = x;
	}
}

/* Max-heap sift-down over v[lo..lo+n), the fallback's heapsort. */
static inline void
qs_sift(int *v, int lo, int i, int n, qsel_cmp_fn cmp, void *ctx)
{
	int x = v[lo + i], c;

	while ((c = 2 * i + 1) < n) {
		if (c + 1 < n && cmp(v[lo + c + 1], v[lo + c], ctx) > 0)
			c++;
		if (cmp(v[lo + c], x, ctx) <= 0)
			break;
		v[lo + i] = v[lo + c];
		i = c;
	}
	v[lo + i] = x;
}

static inline void
qs_heapsort(int *v, int lo, int hi, qsel_cmp_fn cmp, void *ctx)
{
	int n = hi - lo + 1, i;

	for (i = n / 2 - 1; i >= 0; i--)
		qs_sift(v, lo, i, n, cmp, ctx);
	for (i = n - 1; i > 0; i--) {
		qs_swap(v, lo, lo + i);
		qs_sift(v, lo, 0, i, cmp, ctx);
	}
}

static inline void
qsel_nth(int *v, int n, int k, qsel_cmp_fn cmp, void *ctx, uint64_t *rng)
{
	int lo = 0, hi = n - 1, t = k - 1, budget = 8, m;
	int a, b, c, p, i, j, pv;

	if (n <= 1 || k < 1 || k > n)
		return;
	for (m = n; m > 1; m >>= 1)
		budget += 2;
	if (*rng == 0)
		*rng = 0x9E3779B97F4A7C15ULL;
	while (hi - lo > QSEL_SMALL) {
		if (budget-- == 0) {
			qs_heapsort(v, lo, hi, cmp, ctx);
			return;
		}
		/* Median of three random positions. */
		a = lo + (int)qs_rnd(rng, (uint32_t)(hi - lo + 1));
		b = lo + (int)qs_rnd(rng, (uint32_t)(hi - lo + 1));
		c = lo + (int)qs_rnd(rng, (uint32_t)(hi - lo + 1));
		if (cmp(v[a], v[b], ctx) < 0)
			p = cmp(v[b], v[c], ctx) < 0 ? b :
			    cmp(v[a], v[c], ctx) < 0 ? c : a;
		else
			p = cmp(v[a], v[c], ctx) < 0 ? a :
			    cmp(v[b], v[c], ctx) < 0 ? c : b;
		/* Hoare partition around v[lo]: elements equal to the pivot
		 * stop both scans, so runs of equal keys still split evenly. */
		qs_swap(v, lo, p);
		pv = v[lo];
		i = lo;
		j = hi + 1;
		for (;;) {
			while (cmp(v[++i], pv, ctx) < 0)
				if (i == hi)
					break;
			while (cmp(pv, v[--j], ctx) < 0)
				if (j == lo)
					break;
			if (i >= j)
				break;
			qs_swap(v, i, j);
		}
		qs_swap(v, lo, j);
		if (t < j)
			hi = j - 1;
		else if (t > j)
			lo = j + 1;
		else
			return;
	}
	qs_isort(v, lo, hi, cmp, ctx);
}

#endif /* SQLXTC_QSEL_H */
