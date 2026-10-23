/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_qsel.c
 *	qsel_nth (QuickSelect, qsel.h) against a full sort, on the inputs
 *	that break selection: random, sorted, reverse, all-equal, organ
 *	pipe, few distinct values, sawtooth, and the median-of-3 killer;
 *	n from 1 to 20000 and every boundary rank.  Checked: the element at
 *	rank k is the sorted array's k-th, everything before it sorts at or
 *	before it and everything after at or after it, the multiset is
 *	unchanged, the comparison count stays O(n) on average and under the
 *	O(n log n) fallback bound on every input, and the same seed gives
 *	the same permutation (replayable).  McIlroy's adaptive adversary
 *	("A Killer Adversary for Quicksort", 1999), which makes ANY pivot
 *	rule pick a near-extreme pivot, drives the heapsort fallback: the
 *	compare count must stay within the O(n log n) bound.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qsel.h"

static long g_ncmp;
static int g_fail;

#define CK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	g_fail = 1; } } while (0)

static int
by_key(int a, int b, void *ctx)
{
	const int64_t *k = ctx;
	g_ncmp++;
	return k[a] < k[b] ? -1 : k[a] > k[b];
}

/* McIlroy's adversary: values are decided lazily ("gas" until frozen);
 * whenever two gas elements meet, the one that is not the current pivot
 * candidate is frozen to the next smallest solid value. */
static int *g_val, g_gas, g_nsolid, g_cand;

static int
adversary(int a, int b, void *ctx)
{
	(void)ctx;
	g_ncmp++;
	if (g_val[a] == g_gas && g_val[b] == g_gas) {
		if (a == g_cand) g_val[a] = g_nsolid++;
		else g_val[b] = g_nsolid++;
	}
	if (g_val[a] == g_gas) g_cand = a;
	else if (g_val[b] == g_gas) g_cand = b;
	return g_val[a] < g_val[b] ? -1 : g_val[a] > g_val[b];
}

static int
i64cmp(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
	return x < y ? -1 : x > y;
}

static uint64_t g_rng = 88172645463325252ULL;

static uint64_t
rnd(void)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 7;
	g_rng ^= g_rng << 17;
	return g_rng;
}

enum { RANDOM, SORTED, REVERSE, EQUAL, ORGAN, FEW, SAW, KILLER, NSHAPE };
static const char *const shape_name[] = { "random", "sorted", "reverse",
    "equal", "organ", "few", "saw", "m3killer" };

static void
fill(int64_t *k, int n, int shape)
{
	int i;
	for (i = 0; i < n; i++) {
		switch (shape) {
		case RANDOM:  k[i] = (int64_t)(rnd() >> 1); break;
		case SORTED:  k[i] = i; break;
		case REVERSE: k[i] = n - i; break;
		case EQUAL:   k[i] = 7; break;
		case ORGAN:   k[i] = i < n / 2 ? i : n - i; break;
		case FEW:     k[i] = (int64_t)(rnd() % 4); break;
		case SAW:     k[i] = i % 17; break;
		default:
			/* Musser's median-of-3 killer for a fixed-position
			 * pivot rule; the random-position rule must not care. */
			k[i] = (i & 1) ? n / 2 + i : i;
			break;
		}
	}
}

/* One selection: check rank, partition, multiset.  Returns compares. */
static long
check(const int64_t *k, int n, int rank, uint64_t seed, int *v)
{
	int64_t *sorted = malloc(sizeof *sorted * (size_t)n);
	int64_t *got = malloc(sizeof *got * (size_t)n);
	uint64_t s = seed;
	int i;
	long c0 = g_ncmp;

	if (sorted == NULL || got == NULL) abort();
	for (i = 0; i < n; i++) { v[i] = i; sorted[i] = k[i]; }
	qsort(sorted, (size_t)n, sizeof *sorted, i64cmp);
	qsel_nth(v, n, rank, by_key, (void *)k, &s);
	c0 = g_ncmp - c0;
	CK(k[v[rank - 1]] == sorted[rank - 1]);
	for (i = 0; i < rank - 1; i++) CK(k[v[i]] <= k[v[rank - 1]]);
	for (i = rank; i < n; i++) CK(k[v[i]] >= k[v[rank - 1]]);
	for (i = 0; i < n; i++) got[i] = k[v[i]];
	qsort(got, (size_t)n, sizeof *got, i64cmp);
	CK(memcmp(got, sorted, sizeof *got * (size_t)n) == 0);
	free(sorted);
	free(got);
	return c0;
}

int
main(void)
{
	static const int ns[] = { 1, 2, 3, 15, 16, 17, 18, 33, 100, 1000, 20000 };
	int si, ni, *v, *v2, n, ri, i, nchk = 0;
	int64_t *k;
	long worst_ratio_x100 = 0;
	uint64_t s1 = 99, s2 = 99;

	v = malloc(sizeof *v * 20000);
	v2 = malloc(sizeof *v2 * 20000);
	k = malloc(sizeof *k * 20000);
	if (v == NULL || v2 == NULL || k == NULL) return 1;
	for (si = 0; si < NSHAPE; si++) {
		long tot = 0, nsel = 0;
		for (ni = 0; ni < (int)(sizeof ns / sizeof ns[0]); ni++) {
			int ranks[5];
			n = ns[ni];
			fill(k, n, si);
			ranks[0] = 1; ranks[1] = n; ranks[2] = (n + 1) / 2;
			ranks[3] = 1 + (int)(rnd() % (uint64_t)n);
			ranks[4] = n > 1 ? n - 1 : 1;
			for (ri = 0; ri < 5; ri++) {
				long c = check(k, n, ranks[ri], 0x1234 + (uint64_t)ri, v);
				/* fallback bound: <= ~(2 log2 n + 8) partitions of
				 * n plus a heapsort: generous 4 n log2 n + 64 n */
				long lg = 1;
				while ((1L << lg) < n) lg++;
				CK(c <= 4 * (long)n * lg + 64L * n);
				if (n >= 1000) { tot += c; nsel += n; }
				nchk++;
			}
		}
		if (nsel > 0 && tot * 100 / nsel > worst_ratio_x100)
			worst_ratio_x100 = tot * 100 / nsel;
		printf("  %-9s compares per element (n >= 1000): %.2f\n",
		    shape_name[si], nsel ? (double)tot / (double)nsel : 0.0);
		/* O(n) on average: quickselect's expected compares are
		 * <= 3.39 n for the median (2 + 2 ln 2), more for a skewed
		 * random rank; 8 n is a loose ceiling that a degenerate
		 * pivot rule (O(n^2)) blows through at n = 20000. */
		CK(nsel == 0 || tot <= 8 * nsel);
	}
	/* The adversary, at the median rank of 20000. */
	g_val = malloc(sizeof *g_val * 20000);
	if (g_val == NULL) return 1;
	for (i = 0; i < 20000; i++) { v[i] = i; g_val[i] = 20000; }
	g_gas = 20000; g_nsolid = 0; g_cand = 0;
	g_ncmp = 0;
	qsel_nth(v, 20000, 10000, adversary, NULL, &s1);
	printf("  adversary compares per element (n = 20000): %.2f\n",
	    (double)g_ncmp / 20000.0);
	CK(g_ncmp <= 4L * 20000 * 15 + 64L * 20000);   /* the fallback bound */
	for (i = 0; i < 9999; i++) CK(g_val[v[i]] <= g_val[v[9999]]);
	for (i = 10000; i < 20000; i++) CK(g_val[v[i]] >= g_val[v[9999]]);
	free(g_val);
	s1 = 99;

	/* Replay: the same seed gives the same permutation. */
	fill(k, 20000, RANDOM);
	for (i = 0; i < 20000; i++) v[i] = v2[i] = i;
	qsel_nth(v, 20000, 777, by_key, k, &s1);
	qsel_nth(v2, 20000, 777, by_key, k, &s2);
	CK(memcmp(v, v2, sizeof *v * 20000) == 0 && s1 == s2);
	free(v);
	free(v2);
	free(k);
	printf("test_qsel: %d selections, worst mean compares/elem %.2f, %s\n",
	    nchk, worst_ratio_x100 / 100.0, g_fail ? "FAIL" : "ok");
	return g_fail;
}
