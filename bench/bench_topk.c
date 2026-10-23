/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * bench/bench_topk.c
 *	Bounded top-K selection over a stream of N 64-bit keys: does a SIMD
 *	partition (the SimdQuickHeap idea: compare W keys at once, compress
 *	the lanes that go down) beat a scalar QuickSelect?  Standalone; no
 *	libxtc link.
 *
 *	Every candidate keeps the K smallest (key, seq) of the stream, seq =
 *	arrival index, so ties keep arrival order (SQL's LIMIT rule), and
 *	returns them sorted.  All candidates must produce the same K
 *	(checked by FNV over the output, "bad" column).
 *
 *	  repl  replace-the-worst + O(K) rescan (sqlxtc's top-N before the
 *	        QuickSelect change), on (key, seq) pairs
 *	  bh    bounded binary max-heap of K (the textbook top-K)
 *	  qs    2K buffer + scalar QuickSelect cut, key-only threshold test
 *	        on admission (sqlxtc's current scheme), Hoare partition on
 *	        (key, seq) pairs
 *	  qsv   the same, cut by a branch-free two-output partition (the
 *	        SimdQuickHeap shape, scalar)
 *	  qs2   qsv with an AVX2 compare + 4-lane permute partition
 *	  qs5   qsv with an AVX-512 compare + vpcompressq partition
 *	  qsf   qs5 plus a SIMD admission filter: 8 keys compared to the
 *	        threshold at once, only the survivors are appended
 *	  sqh   SimdQuickHeap (buckets + pivot array, AVX-512 partition):
 *	        push all N, pop K -- the heap the paper is about, for scale
 *	  sort  copy all N, qsort, take K -- the full-sort baseline
 *
 *	Inputs: asc, desc, rnd, eq (all equal), few (4 distinct), organ.
 *	Cost unit: ns per input element.  Usage:
 *	  bench_topk [N] [REPS] [CPU-pin is the caller's job]
 *	CSV: impl,input,n,k,rep,ns_per_elem,bad
 */
#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* -DTK_NO_SIMD builds the scalar paths only (what a non-x86 host runs). */
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)) && \
    !defined(TK_NO_SIMD)
#include <immintrin.h>
#define TK_X86 1
#endif

typedef struct { int64_t k; int64_t s; } kv_t;   /* key, arrival seq */

static int g_avx2, g_avx512;

static double
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void *
xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (p == NULL) { perror("malloc"); exit(1); }
	return p;
}

static inline int
kv_lt(kv_t a, kv_t b)
{
	return a.k < b.k || (a.k == b.k && a.s < b.s);
}

static int
kv_cmp(const void *pa, const void *pb)
{
	const kv_t *a = pa, *b = pb;
	if (a->k != b->k) return a->k < b->k ? -1 : 1;
	return a->s < b->s ? -1 : a->s > b->s;
}

static uint64_t
xs(uint64_t *s)
{
	uint64_t x = *s;
	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	return *s = x;
}

/* ---- repl: replace the worst, rescan ------------------------------- */
static int
topk_repl(const int64_t *in, int n, int k, kv_t *out)
{
	int m = 0, w = -1, i, r;
	for (i = 0; i < n; i++) {
		kv_t x = { in[i], i };
		if (m < k) { out[m++] = x; continue; }
		if (w < 0)
			for (w = 0, r = 1; r < m; r++)
				if (kv_lt(out[w], out[r])) w = r;
		if (x.k >= out[w].k) continue;
		out[w] = x;
		w = -1;
	}
	qsort(out, (size_t)m, sizeof *out, kv_cmp);
	return m;
}

/* ---- bh: bounded max-heap ------------------------------------------ */
static int
topk_bh(const int64_t *in, int n, int k, kv_t *h)
{
	int m = 0, i;
	for (i = 0; i < n; i++) {
		kv_t x = { in[i], i };
		int j, c;
		if (m < k) {
			j = m++;
			while (j > 0 && kv_lt(h[(j - 1) / 2], x)) { h[j] = h[(j - 1) / 2]; j = (j - 1) / 2; }
			h[j] = x;
			continue;
		}
		if (x.k >= h[0].k) continue;    /* later arrival: must be strictly smaller */
		j = 0;
		while ((c = 2 * j + 1) < m) {
			if (c + 1 < m && kv_lt(h[c], h[c + 1])) c++;
			if (!kv_lt(x, h[c])) break;
			h[j] = h[c];
			j = c;
		}
		h[j] = x;
	}
	qsort(h, (size_t)m, sizeof *h, kv_cmp);
	return m;
}

/* ---- QuickSelect cut variants -------------------------------------- */

/* Index of the median of three random positions in b[lo..hi]. */
static int
pick(const kv_t *b, int lo, int hi, uint64_t *rng)
{
	uint32_t m = (uint32_t)(hi - lo + 1);
	int x = lo + (int)(xs(rng) % m), y = lo + (int)(xs(rng) % m),
	    z = lo + (int)(xs(rng) % m);
	if (kv_lt(b[x], b[y])) return kv_lt(b[y], b[z]) ? y : kv_lt(b[x], b[z]) ? z : x;
	return kv_lt(b[x], b[z]) ? x : kv_lt(b[y], b[z]) ? z : y;
}

/* Hoare QuickSelect on pairs: afterwards b[0..k) are the k smallest. */
static void
sel_hoare(kv_t *b, int n, int k, uint64_t *rng)
{
	int lo = 0, hi = n - 1, t = k - 1;
	while (hi > lo) {
		int pi = pick(b, lo, hi, rng), i = lo - 1, j = hi + 1;
		kv_t p, tmp;
		tmp = b[lo]; b[lo] = b[pi]; b[pi] = tmp;
		p = b[lo];
		for (;;) {
			do i++; while (kv_lt(b[i], p));
			do j--; while (kv_lt(p, b[j]));
			if (i >= j) break;
			tmp = b[i]; b[i] = b[j]; b[j] = tmp;
		}
		if (t <= j) hi = j; else lo = j + 1;
	}
}

/*
 * Two-output partitions (SimdQuickHeap's shape): src[0..n) -> lt[] gets
 * the pairs < p, ge[] the rest.  Return the count < p.  ge may alias src
 * (write index never passes read index).
 */
static int
part_scalar(const kv_t *src, int n, kv_t p, kv_t *lt, kv_t *ge)
{
	int i, a = 0, g = 0;
	for (i = 0; i < n; i++) {
		kv_t x = src[i];
		int go = kv_lt(x, p);
		lt[a] = x; ge[g] = x;           /* branch-free: write both */
		a += go; g += !go;
	}
	return a;
}

#ifdef TK_X86
/* Lane masks for (key, seq) < (pk, ps) over 4 pairs in two registers
 * holding keys and seqs de-interleaved. */
__attribute__((target("avx512f,avx512vl"))) static int
part_avx512(const kv_t *src, int n, kv_t p, kv_t *lt, kv_t *ge)
{
	const __m512i pk = _mm512_set1_epi64(p.k), ps = _mm512_set1_epi64(p.s);
	/* gather even (key) and odd (seq) qwords of 8 pairs */
	const __m512i ik = _mm512_set_epi64(14, 12, 10, 8, 6, 4, 2, 0);
	const __m512i is = _mm512_set_epi64(15, 13, 11, 9, 7, 5, 3, 1);
	int i = 0, a = 0, g = 0;
	for (; i + 8 <= n; i += 8) {
		__m512i v0 = _mm512_loadu_si512((const void *)&src[i]);
		__m512i v1 = _mm512_loadu_si512((const void *)&src[i + 4]);
		__m512i k = _mm512_permutex2var_epi64(v0, ik, v1);
		__m512i s = _mm512_permutex2var_epi64(v0, is, v1);
		__mmask8 m = _mm512_cmplt_epi64_mask(k, pk) |
		    (_mm512_cmpeq_epi64_mask(k, pk) & _mm512_cmplt_epi64_mask(s, ps));
		/* compress keys and seqs separately, then re-interleave */
		__m512i ck = _mm512_maskz_compress_epi64(m, k), cs = _mm512_maskz_compress_epi64(m, s);
		__m512i nk = _mm512_maskz_compress_epi64((__mmask8)~m, k), ns = _mm512_maskz_compress_epi64((__mmask8)~m, s);
		const __m512i lo = _mm512_set_epi64(11, 3, 10, 2, 9, 1, 8, 0);
		const __m512i hi = _mm512_set_epi64(15, 7, 14, 6, 13, 5, 12, 4);
		int c = __builtin_popcount(m);
		_mm512_storeu_si512((void *)&lt[a], _mm512_permutex2var_epi64(ck, lo, cs));
		_mm512_storeu_si512((void *)&lt[a + 4], _mm512_permutex2var_epi64(ck, hi, cs));
		/* ge may alias src: both 64-byte halves of this block are
		 * already in registers, and g <= i */
		_mm512_storeu_si512((void *)&ge[g], _mm512_permutex2var_epi64(nk, lo, ns));
		_mm512_storeu_si512((void *)&ge[g + 4], _mm512_permutex2var_epi64(nk, hi, ns));
		a += c;
		g += 8 - c;
	}
	for (; i < n; i++) {
		kv_t x = src[i];
		if (kv_lt(x, p)) lt[a++] = x; else ge[g++] = x;
	}
	return a;
}

/* AVX2: 4 pairs per step; per-lane mask, then scalar scatter by mask
 * (no 128-bit-pair compress in AVX2: vpermd over 8 dwords moves one
 * 64-bit lane per 2 dwords, so 4 pairs = 2 registers of 2 pairs). */
__attribute__((target("avx2"))) static int
part_avx2(const kv_t *src, int n, kv_t p, kv_t *lt, kv_t *ge)
{
	const __m256i pk = _mm256_set1_epi64x(p.k), ps = _mm256_set1_epi64x(p.s);
	int i = 0, a = 0, g = 0, j;
	for (; i + 4 <= n; i += 4) {
		__m256i v0 = _mm256_loadu_si256((const void *)&src[i]);       /* k0 s0 k1 s1 */
		__m256i v1 = _mm256_loadu_si256((const void *)&src[i + 2]);   /* k2 s2 k3 s3 */
		__m256i k = _mm256_permute4x64_epi64(_mm256_unpacklo_epi64(v0, v1), 0xD8);
		__m256i s = _mm256_permute4x64_epi64(_mm256_unpackhi_epi64(v0, v1), 0xD8);
		__m256i lt1 = _mm256_cmpgt_epi64(pk, k);
		__m256i eq = _mm256_cmpeq_epi64(pk, k);
		__m256i lt2 = _mm256_and_si256(eq, _mm256_cmpgt_epi64(ps, s));
		unsigned m = (unsigned)_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_or_si256(lt1, lt2)));
		for (j = 0; j < 4; j++) {
			kv_t x = src[i + j];
			int go = (int)((m >> j) & 1u);
			lt[a] = x; ge[g] = x;
			a += go; g += !go;
		}
	}
	for (; i < n; i++) {
		kv_t x = src[i];
		if (kv_lt(x, p)) lt[a++] = x; else ge[g++] = x;
	}
	return a;
}

/* Count of keys < thr in 8 consecutive input keys, packed survivors
 * appended to buf as (key, seq) pairs.  Returns the number appended. */
__attribute__((target("avx512f,avx512vl"))) static int
filt_avx512(const int64_t *in, int base, int64_t thr, kv_t *buf)
{
	const __m512i t = _mm512_set1_epi64(thr);
	const __m512i lo = _mm512_set_epi64(11, 3, 10, 2, 9, 1, 8, 0);
	const __m512i hi = _mm512_set_epi64(15, 7, 14, 6, 13, 5, 12, 4);
	__m512i k = _mm512_loadu_si512((const void *)&in[base]);
	__mmask8 m = _mm512_cmplt_epi64_mask(k, t);
	__m512i s, ck, cs;
	if (m == 0) return 0;
	s = _mm512_add_epi64(_mm512_set1_epi64(base), _mm512_set_epi64(7, 6, 5, 4, 3, 2, 1, 0));
	ck = _mm512_maskz_compress_epi64(m, k);
	cs = _mm512_maskz_compress_epi64(m, s);
	_mm512_storeu_si512((void *)&buf[0], _mm512_permutex2var_epi64(ck, lo, cs));
	_mm512_storeu_si512((void *)&buf[4], _mm512_permutex2var_epi64(ck, hi, cs));
	return __builtin_popcount(m);
}
#endif

typedef int (*part_fn)(const kv_t *, int, kv_t, kv_t *, kv_t *);

/* QuickSelect by two-output partition: the < side is compacted in
 * place at b[lo..], the >= side goes to tmp and is copied back only when
 * the rank lies in it.  b[0..k) are the k smallest afterwards (slots at
 * or past the final hi may hold stale copies: they are beyond k). */
static void
sel_part(kv_t *b, int n, int k, kv_t *tmp, part_fn part, uint64_t *rng)
{
	int lo = 0, hi = n, a, ng, pi;          /* half-open [lo, hi) holds rank k-1 */
	kv_t p, t;
	while (hi - lo > 16) {
		pi = pick(b, lo, hi - 1, rng);
		p = b[pi];
		a = part(&b[lo], hi - lo, p, &b[lo], tmp);
		ng = hi - lo - a;
		if (k - 1 < lo + a) {           /* rank on the < side */
			hi = lo + a;
			continue;
		}
		memcpy(&b[lo + a], tmp, sizeof *b * (size_t)ng);
		if (a == 0) {                   /* p is the minimum: split it off */
			for (pi = lo; !(b[pi].k == p.k && b[pi].s == p.s); pi++)
				;
			t = b[lo]; b[lo] = b[pi]; b[pi] = t;
			a = 1;
		}
		lo += a;
	}
	if (hi - lo > 1) qsort(&b[lo], (size_t)(hi - lo), sizeof *b, kv_cmp);
}

enum { CUT_HOARE, CUT_SCALAR, CUT_AVX2, CUT_AVX512 };

static int
topk_qs(const int64_t *in, int n, int k, kv_t *out, int cut, int filt)
{
	int cap = k + (k > 32 ? k : 32), m = 0, i, w;
	kv_t *b = xmalloc(sizeof *b * (size_t)(cap + 16));
	kv_t *tmp = xmalloc(sizeof *tmp * (size_t)(cap + 16));
	uint64_t rng = 0x9E3779B97F4A7C15ULL;
	int64_t thr = INT64_MAX;
	int have_thr = 0;
	part_fn pf = part_scalar;
#ifdef TK_X86
	if (cut == CUT_AVX512) pf = part_avx512;
	if (cut == CUT_AVX2) pf = part_avx2;
#else
	(void)filt;
#endif
	for (i = 0; i < n; i++) {
#ifdef TK_X86
		if (filt && have_thr && i + 8 <= n && m + 8 <= cap) {
			m += filt_avx512(in, i, thr, &b[m]);
			i += 7;
			continue;
		}
#endif
		if (have_thr && in[i] >= thr) continue;
		if (m == cap) {
			if (cut == CUT_HOARE) sel_hoare(b, m, k, &rng);
			else sel_part(b, m, k, tmp, pf, &rng);
			m = k;
			for (thr = b[0].k, w = 1; w < k; w++)
				if (b[w].k > thr) thr = b[w].k;
			/* a later arrival must be strictly smaller than the K-th */
			have_thr = 1;
			if (in[i] >= thr) continue;
		}
		b[m].k = in[i];
		b[m].s = i;
		m++;
		if (!have_thr && m == k) {
			for (thr = b[0].k, w = 1; w < k; w++)
				if (b[w].k > thr) thr = b[w].k;
			have_thr = 1;
		}
	}
	if (m > k) {
		if (cut == CUT_HOARE) sel_hoare(b, m, k, &rng);
		else sel_part(b, m, k, tmp, pf, &rng);
		m = k;
	}
	qsort(b, (size_t)m, sizeof *b, kv_cmp);
	memcpy(out, b, sizeof *b * (size_t)m);
	free(b);
	free(tmp);
	return m;
}

/* ---- sqh: SimdQuickHeap (paper layout), keys only + seq payload ----- */
struct sqh_b { kv_t *v; int n, cap; };
struct sqh {
	struct sqh_b *b;     /* b[0] top (largest) .. b[nb-1] bottom */
	kv_t *piv;           /* piv[i] separates b[i] (>=) from b[i+1] (<) */
	int nb, capb;
	uint64_t rng;
};

static void
sqh_push_b(struct sqh_b *b, kv_t x)
{
	if (b->n + 8 >= b->cap) {
		b->cap = b->cap ? b->cap * 2 : 64;
		b->v = realloc(b->v, sizeof *b->v * (size_t)b->cap);
		if (b->v == NULL) exit(1);
	}
	b->v[b->n++] = x;
}

static void
sqh_push(struct sqh *q, kv_t x)
{
	int i = 0;
	while (i < q->nb - 1 && kv_lt(x, q->piv[i])) i++;   /* linear scan: few pivots */
	sqh_push_b(&q->b[i], x);
}

static kv_t
sqh_pop(struct sqh *q, kv_t *tmp, part_fn pf)
{
	struct sqh_b *bot;
	int i, mi, a;
	kv_t r, p;
	for (;;) {
		bot = &q->b[q->nb - 1];
		if (bot->n <= 16) break;
		p = bot->v[pick(bot->v, 0, bot->n - 1, &q->rng)];
		a = pf(bot->v, bot->n, p, tmp, bot->v);   /* < p to tmp, >= p stays */
		if (a == 0) break;
		if (q->nb == q->capb) {
			q->capb *= 2;
			q->b = realloc(q->b, sizeof *q->b * (size_t)q->capb);
			q->piv = realloc(q->piv, sizeof *q->piv * (size_t)q->capb);
			if (q->b == NULL || q->piv == NULL) exit(1);
			memset(&q->b[q->nb], 0, sizeof *q->b * (size_t)(q->capb - q->nb));
		}
		q->b[q->nb - 1].n -= a;
		q->piv[q->nb - 1] = p;
		bot = &q->b[q->nb++];
		bot->n = 0;
		for (i = 0; i < a; i++) sqh_push_b(bot, tmp[i]);
	}
	for (mi = 0, i = 1; i < bot->n; i++)
		if (kv_lt(bot->v[i], bot->v[mi])) mi = i;
	r = bot->v[mi];
	bot->v[mi] = bot->v[--bot->n];
	if (bot->n == 0 && q->nb > 1) q->nb--;
	return r;
}

static int
topk_sqh(const int64_t *in, int n, int k, kv_t *out)
{
	struct sqh q;
	kv_t *tmp = xmalloc(sizeof *tmp * (size_t)(n + 16));
	part_fn pf = part_scalar;
	int i;
#ifdef TK_X86
	if (g_avx512) pf = part_avx512;
#endif
	memset(&q, 0, sizeof q);
	q.capb = 64;
	q.b = calloc((size_t)q.capb, sizeof *q.b);
	q.piv = xmalloc(sizeof *q.piv * (size_t)q.capb);
	q.nb = 1;
	q.rng = 0x9E3779B97F4A7C15ULL;
	for (i = 0; i < n; i++) {
		kv_t x = { in[i], i };
		sqh_push(&q, x);
	}
	for (i = 0; i < k && i < n; i++) out[i] = sqh_pop(&q, tmp, pf);
	for (i = 0; i < q.capb; i++) free(q.b[i].v);
	free(q.b);
	free(q.piv);
	free(tmp);
	return k < n ? k : n;
}

static int
topk_sort(const int64_t *in, int n, int k, kv_t *out)
{
	kv_t *a = xmalloc(sizeof *a * (size_t)n);
	int i;
	for (i = 0; i < n; i++) { a[i].k = in[i]; a[i].s = i; }
	qsort(a, (size_t)n, sizeof *a, kv_cmp);
	memcpy(out, a, sizeof *a * (size_t)(k < n ? k : n));
	free(a);
	return k < n ? k : n;
}

static uint64_t
fnv(const kv_t *v, int m)
{
	uint64_t h = 1469598103934665603ULL;
	int i;
	for (i = 0; i < m; i++) {
		h = (h ^ (uint64_t)v[i].k) * 1099511628211ULL;
		h = (h ^ (uint64_t)v[i].s) * 1099511628211ULL;
	}
	return h;
}

static void
gen(int64_t *a, int n, const char *shape)
{
	uint64_t s = 88172645463325252ULL;
	int i;
	for (i = 0; i < n; i++) {
		if (!strcmp(shape, "asc")) a[i] = i;
		else if (!strcmp(shape, "desc")) a[i] = n - i;
		else if (!strcmp(shape, "eq")) a[i] = 7;
		else if (!strcmp(shape, "few")) a[i] = (int64_t)(xs(&s) % 4);
		else if (!strcmp(shape, "organ")) a[i] = i < n / 2 ? i : n - i;
		else a[i] = (int64_t)(xs(&s) >> 1);
	}
}

int
main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 1000000, reps = argc > 2 ? atoi(argv[2]) : 5;
	static const char *const shapes[] = { "desc", "asc", "rnd", "eq", "few", "organ" };
	static const int ks[] = { 10, 100, 1000, 10000, 100000 };
	static const char *const impl[] = { "repl", "bh", "qs", "qsv", "qs2", "qs5", "qsf", "sqh", "sort" };
	int64_t *in = xmalloc(sizeof *in * (size_t)n);
	kv_t *out = xmalloc(sizeof *out * (size_t)n);
	int si, ki, ii, r;
#ifdef TK_X86
	__builtin_cpu_init();
	g_avx2 = __builtin_cpu_supports("avx2") != 0;
	g_avx512 = __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512vl");
#endif
	printf("# n=%d avx2=%d avx512=%d (desc input = keys arrive largest first; asc = in order)\n",
	    n, g_avx2, g_avx512);
	printf("impl,input,n,k,rep,ns_per_elem,bad\n");
	for (r = 0; r < reps; r++)
		for (si = 0; si < 6; si++) {
			gen(in, n, shapes[si]);
			for (ki = 0; ki < 5; ki++) {
				int k = ks[ki];
				uint64_t ref;
				if (k >= n) continue;
				ref = fnv(out, topk_sort(in, n, k, out));
				for (ii = 0; ii < 9; ii++) {
					double t0, t1;
					int m = 0, bad;
					if ((ii == 4 && !g_avx2) || ((ii == 5 || ii == 6 || ii == 7) && !g_avx512)) continue;
					/* repl costs O(N K) when rows keep being admitted
					 * (desc, organ) and O(K^2 log(N/K)) on random input:
					 * skip the cells where one run would take minutes */
					if (ii == 0 && (k >= 100000 || (k >= 10000 &&
					    (!strcmp(shapes[si], "desc") || !strcmp(shapes[si], "organ")))))
						continue;
					/* sqh pushes all N: skip the cells where it is
					 * just the full sort again */
					if (ii == 7 && k > 1000) continue;
					t0 = now_ns();
					switch (ii) {
					case 0: m = topk_repl(in, n, k, out); break;
					case 1: m = topk_bh(in, n, k, out); break;
					case 2: m = topk_qs(in, n, k, out, CUT_HOARE, 0); break;
					case 3: m = topk_qs(in, n, k, out, CUT_SCALAR, 0); break;
					case 4: m = topk_qs(in, n, k, out, CUT_AVX2, 0); break;
					case 5: m = topk_qs(in, n, k, out, CUT_AVX512, 0); break;
					case 6: m = topk_qs(in, n, k, out, CUT_AVX512, 1); break;
					case 7: m = topk_sqh(in, n, k, out); break;
					default: m = topk_sort(in, n, k, out); break;
					}
					t1 = now_ns();
					bad = (m != k) || fnv(out, m) != ref;
					printf("%s,%s,%d,%d,%d,%.3f,%d\n", impl[ii], shapes[si], n, k, r,
					    (t1 - t0) / n, bad);
				}
			}
			fflush(stdout);
		}
	free(in);
	free(out);
	return 0;
}
