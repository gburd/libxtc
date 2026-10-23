/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/pbt/pbt_qsel.c
 *	Property-based tests for sqlxtc's QuickSelect (examples/06_sqlxtc/
 *	qsel.h, header-only), the cut behind ORDER BY ... LIMIT.
 *
 *	1. select_is_rank: for any array (sizes 1..400, values drawn from
 *	   a small or a wide domain, so ties are common), any rank k and any
 *	   seed, qsel_nth leaves the k-th smallest at v[k-1], nothing larger
 *	   before it, nothing smaller after it, and the multiset unchanged.
 *	2. topn_stream_is_prefix: the sqlxtc top-N scheme (a buffer of
 *	   K + max(K, 32), cut to the K best of (key, arrival) when full,
 *	   later rows admitted only if strictly smaller than the K-th key)
 *	   over any stream returns exactly the first K of a stable sort of
 *	   the stream: SQL's LIMIT-is-a-prefix rule.
 *	3. seed_replay: the same seed gives the same permutation (the cut
 *	   is replayable; no global RNG).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pbt_common.h"
#include "../../examples/06_sqlxtc/qsel.h"

#if defined(XTC_HAVE_HEGEL)

struct kv { int64_t k, s; };

static int
by_key(int a, int b, void *ctx)
{
	const int64_t *k = ctx;
	return k[a] < k[b] ? -1 : k[a] > k[b];
}

static int
by_kv(int a, int b, void *ctx)
{
	const struct kv *v = ctx;
	if (v[a].k != v[b].k) return v[a].k < v[b].k ? -1 : 1;
	return v[a].s < v[b].s ? -1 : v[a].s > v[b].s;
}

static int
i64cmp(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
	return x < y ? -1 : x > y;
}

static int
kvcmp(const void *a, const void *b)
{
	const struct kv *x = a, *y = b;
	if (x->k != y->k) return x->k < y->k ? -1 : 1;
	return x->s < y->s ? -1 : x->s > y->s;
}

/* Draw n keys: a narrow domain (many ties) or a wide one. */
static void
draw_keys(hegel_test_case *tc, int64_t *k, int n)
{
	int64_t dom = hegel_draw_int(tc, hegel_integers(0, 1)) ? 6 :
	    INT64_C(1) << 40;
	int shape = (int)hegel_draw_int(tc, hegel_integers(0, 3)), i;
	for (i = 0; i < n; i++) {
		switch (shape) {
		case 0: k[i] = hegel_draw_int(tc, hegel_integers(-dom, dom)); break;
		case 1: k[i] = i; break;              /* ascending */
		case 2: k[i] = n - i; break;          /* descending */
		default: k[i] = i % 7; break;         /* sawtooth */
		}
	}
}

static void
prop_select_is_rank(hegel_test_case *tc, void *u)
{
	int n = (int)hegel_draw_int(tc, hegel_integers(1, 400));
	int k = (int)hegel_draw_int(tc, hegel_integers(1, n));
	uint64_t seed = (uint64_t)hegel_draw_int(tc, hegel_integers(1, INT64_MAX));
	int64_t *key = malloc(sizeof *key * (size_t)n);
	int64_t *srt = malloc(sizeof *srt * (size_t)n);
	int64_t *got = malloc(sizeof *got * (size_t)n);
	int *v = malloc(sizeof *v * (size_t)n), i;
	(void)u;
	PBT_ASSERT(key && srt && got && v);
	draw_keys(tc, key, n);
	for (i = 0; i < n; i++) { v[i] = i; srt[i] = key[i]; }
	qsort(srt, (size_t)n, sizeof *srt, i64cmp);
	qsel_nth(v, n, k, by_key, key, &seed);
	PBT_ASSERT(key[v[k - 1]] == srt[k - 1]);
	for (i = 0; i < k - 1; i++) PBT_ASSERT(key[v[i]] <= key[v[k - 1]]);
	for (i = k; i < n; i++) PBT_ASSERT(key[v[i]] >= key[v[k - 1]]);
	for (i = 0; i < n; i++) got[i] = key[v[i]];
	qsort(got, (size_t)n, sizeof *got, i64cmp);
	PBT_ASSERT(memcmp(got, srt, sizeof *got * (size_t)n) == 0);
	free(key); free(srt); free(got); free(v);
}

/* The sqlxtc top-N buffer, minus the row payloads. */
static int
topn(const int64_t *in, int n, int K, struct kv *out)
{
	int cap = K + (K > 32 ? K : 32), m = 0, i, j, thr = -1;
	struct kv *b = malloc(sizeof *b * (size_t)cap), *c;
	int *v = malloc(sizeof *v * (size_t)cap);
	uint64_t rng = 0x9E3779B97F4A7C15ULL;
	PBT_ASSERT(b && v);
	for (i = 0; i < n; i++) {
		if (thr >= 0 && in[i] >= b[thr].k) continue;
		if (m == cap) {
			for (j = 0; j < m; j++) v[j] = j;
			qsel_nth(v, m, K, by_kv, b, &rng);
			c = malloc(sizeof *c * (size_t)K);
			PBT_ASSERT(c != NULL);
			for (j = 0; j < K; j++) c[j] = b[v[j]];
			memcpy(b, c, sizeof *c * (size_t)K);
			free(c);
			m = K;
			for (thr = 0, j = 1; j < K; j++)
				if (by_kv(j, thr, b) > 0) thr = j;
			if (in[i] >= b[thr].k) continue;
		}
		b[m].k = in[i];
		b[m].s = i;
		m++;
		if (thr < 0 && m == K)
			for (thr = 0, j = 1; j < K; j++)
				if (by_kv(j, thr, b) > 0) thr = j;
	}
	if (m > K) {
		for (j = 0; j < m; j++) v[j] = j;
		qsel_nth(v, m, K, by_kv, b, &rng);
		for (j = 0; j < K; j++) out[j] = b[v[j]];
		m = K;
	} else
		memcpy(out, b, sizeof *b * (size_t)m);
	qsort(out, (size_t)m, sizeof *out, kvcmp);
	free(b); free(v);
	return m;
}

static void
prop_topn_stream_is_prefix(hegel_test_case *tc, void *u)
{
	int n = (int)hegel_draw_int(tc, hegel_integers(0, 600));
	int K = (int)hegel_draw_int(tc, hegel_integers(1, 120));
	int64_t *key = malloc(sizeof *key * (size_t)(n + 1));
	struct kv *all = malloc(sizeof *all * (size_t)(n + 1));
	struct kv *got = malloc(sizeof *got * (size_t)(n + 1 + K));
	int i, m, want;
	(void)u;
	PBT_ASSERT(key && all && got);
	draw_keys(tc, key, n);
	for (i = 0; i < n; i++) { all[i].k = key[i]; all[i].s = i; }
	qsort(all, (size_t)n, sizeof *all, kvcmp);       /* stable: ties by s */
	m = topn(key, n, K, got);
	want = n < K ? n : K;
	PBT_ASSERT(m == want);
	PBT_ASSERT(memcmp(got, all, sizeof *got * (size_t)want) == 0);
	free(key); free(all); free(got);
}

static void
prop_seed_replay(hegel_test_case *tc, void *u)
{
	int n = (int)hegel_draw_int(tc, hegel_integers(1, 300));
	int k = (int)hegel_draw_int(tc, hegel_integers(1, n));
	uint64_t s1 = (uint64_t)hegel_draw_int(tc, hegel_integers(1, INT64_MAX)), s2 = s1;
	int64_t *key = malloc(sizeof *key * (size_t)n);
	int *a = malloc(sizeof *a * (size_t)n), *b = malloc(sizeof *b * (size_t)n), i;
	(void)u;
	PBT_ASSERT(key && a && b);
	draw_keys(tc, key, n);
	for (i = 0; i < n; i++) a[i] = b[i] = i;
	qsel_nth(a, n, k, by_key, key, &s1);
	qsel_nth(b, n, k, by_key, key, &s2);
	PBT_ASSERT(memcmp(a, b, sizeof *a * (size_t)n) == 0 && s1 == s2);
	free(key); free(a); free(b);
}

static const pbt_entry_t tests[] = {
	{ "select_is_rank", prop_select_is_rank, 300 },
	{ "topn_stream_is_prefix", prop_topn_stream_is_prefix, 300 },
	{ "seed_replay", prop_seed_replay, 100 },
	{ NULL, NULL, 0 }
};
#else
static const pbt_entry_t tests[] = {
	{ "select_is_rank", NULL, 0 },
	{ "topn_stream_is_prefix", NULL, 0 },
	{ "seed_replay", NULL, 0 },
	{ NULL, NULL, 0 }
};
#endif

PBT_MAIN("qsel", tests)
