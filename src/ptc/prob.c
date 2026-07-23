/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/prob.c
 *	Probabilistic data structures: Bloom filter (xtc_bloom) and
 *	HyperLogLog cardinality estimator (xtc_hll).  See xtc_prob.h for
 *	the API contract and the space/accuracy tradeoff each makes.
 *
 *	Both are SINGLE-THREADED (v1): no locking, no RCU.  Unlike
 *	xtc_chash / xtc_cskip these are plain owned buffers a single
 *	logical writer mutates; concurrent add on one instance is a data
 *	race the caller must prevent (or give each thread its own and
 *	xtc_hll_merge at the end -- the merge is lossless).
 *
 *	Hashing.  Keys are opaque byte spans hashed with 64-bit FNV-1a
 *	for h1 and a second independent avalanche mix (h2) of the same
 *	bytes, so a single pass yields two independent 64-bit hashes.
 *	The Bloom filter derives its k probe positions by the
 *	Kirsch-Mitzenmacher double-hashing scheme g_i = h1 + i*h2 (proven
 *	to preserve the asymptotic false-positive rate of k independent
 *	hashes), avoiding k separate hash passes.  HLL uses h1: the top p
 *	bits pick the register, the leading-zero run of the remaining
 *	bits (plus one) is the value stored.
 *
 *	No libm.  The Bloom sizing needs a natural log and the HLL
 *	estimator needs log/sqrt-free harmonic means; rather than pull in
 *	-lm across the whole build for a handful of once-per-init scalar
 *	calls, prob_ln() below computes ln to ~1e-12 by power-of-two range
 *	reduction plus the atanh series -- accurate far beyond what the
 *	statistical error of these structures could ever notice.
 */

#include "xtc_int.h"
#include "xtc_prob.h"

#include <stdint.h>
#include <string.h>

/* ---------- hashing: one pass -> two independent 64-bit hashes ---------- */

static void
__prob_hash2(const void *key, size_t len, uint64_t *h1, uint64_t *h2)
{
	const uint8_t *p = (const uint8_t *)key;
	uint64_t a = 0xcbf29ce484222325ULL;   /* FNV-1a 64 offset basis */
	uint64_t b = 0x9e3779b97f4a7c15ULL;   /* golden-ratio odd seed */
	size_t i;

	for (i = 0; i < len; i++) {
		a ^= (uint64_t)p[i];
		a *= 0x100000001b3ULL;            /* FNV-1a 64 prime */
		/* A different mixing recurrence for the second hash so h2
		 * is not a deterministic function of h1 (double hashing
		 * needs two INDEPENDENT hashes to hit the target FP rate). */
		b += (uint64_t)p[i];
		b += b << 10;
		b ^= b >> 6;
	}
	/* Final avalanche (splitmix64-style) on both so every input bit
	 * influences every output bit -- FNV-1a's low bits alone are a
	 * poor distribution for the modulo the Bloom filter does. */
	a ^= a >> 33; a *= 0xff51afd7ed558ccdULL;
	a ^= a >> 33; a *= 0xc4ceb9fe1a85ec53ULL; a ^= a >> 33;
	b += 0x9e3779b97f4a7c15ULL;
	b = (b ^ (b >> 30)) * 0xbf58476d1ce4e5b9ULL;
	b = (b ^ (b >> 27)) * 0x94d049bb133111ebULL; b ^= b >> 31;
	/* h2 must be odd so that g_i = h1 + i*h2 mod m cycles through all
	 * residues when m is a power of two -- guarantees the k probes are
	 * distinct positions rather than collapsing onto a few. */
	*h1 = a;
	*h2 = b | 1ULL;
}

/* ---------- natural log without libm (once-per-init scalars) ---------- */

/*
 * ln(x) for x > 0.  Reduce x = m * 2^e with m in [1,2), so
 * ln(x) = e*ln2 + ln(m).  For ln(m) use the fast-converging series
 * ln((1+s)/(1-s)) = 2*(s + s^3/3 + s^5/5 + ...) with s = (m-1)/(m+1),
 * |s| <= 1/3 so a dozen terms reach ~1e-15.  Exact enough that the
 * Bloom sizing is limited by ceil/round, not by this.
 */
static double
__prob_ln(double x)
{
	static const double LN2 = 0.6931471805599453094172321214582;
	double s, s2, term, sum;
	int e = 0, i;

	if (x <= 0.0) return 0.0;         /* callers never pass x <= 0 */
	while (x >= 2.0) { x *= 0.5; e++; }
	while (x < 1.0)  { x *= 2.0; e--; }

	s = (x - 1.0) / (x + 1.0);
	s2 = s * s;
	term = s;
	sum = 0.0;
	for (i = 1; i < 40; i += 2) {
		sum += term / (double)i;
		term *= s2;
	}
	return (double)e * LN2 + 2.0 * sum;
}

/* ============================ Bloom filter ============================ */

struct xtc_bloom {
	uint64_t *bits;      /* m bits packed into words */
	size_t    nwords;    /* length of bits[] */
	size_t    m;         /* bit count (power of two) */
	uint64_t  mask;      /* m - 1, for the modulo */
	unsigned  k;         /* probe count */
};

static size_t
__prob_next_pow2(size_t v)
{
	size_t p = 1;
	if (v <= 1) return 1;
	/* Stop before the shift overflows to 0 (huge v is capped, not
	 * looped forever) -- same guard style as chash __chash_next_pow2. */
	while (p < v && (p << 1) != 0) p <<= 1;
	return p;
}

int
xtc_bloom_init(xtc_bloom_t **out, size_t n_expected, double fp_rate)
{
	xtc_bloom_t *b;
	double n, ln_p, m_opt, k_opt;
	size_t m;
	unsigned k;
	int rc;
	static const double LN2 = 0.6931471805599453094172321214582;

	if (out == NULL) return XTC_E_INVAL;
	if (!(fp_rate > 0.0) || !(fp_rate < 1.0)) return XTC_E_INVAL;
	if (n_expected == 0) n_expected = 1;

	/* m = ceil(-n*ln(p) / (ln2)^2); k = round((m/n)*ln2).  Round m up
	 * to a power of two so membership can mask instead of divide. */
	n = (double)n_expected;
	ln_p = __prob_ln(fp_rate);                /* negative */
	m_opt = -(n * ln_p) / (LN2 * LN2);
	if (m_opt < 1.0) m_opt = 1.0;
	/* ceil, then next power of two. */
	m = (size_t)m_opt;
	if ((double)m < m_opt) m++;
	m = __prob_next_pow2(m);

	k_opt = ((double)m / n) * LN2;
	k = (unsigned)(k_opt + 0.5);              /* round to nearest */
	if (k < 1) k = 1;
	if (k > 64) k = 64;                        /* diminishing returns cap */

	if ((rc = __os_calloc(1, sizeof *b, (void **)&b)) != XTC_OK)
		return rc;
	b->m = m;
	b->mask = (uint64_t)m - 1;
	b->k = k;
	b->nwords = (m + 63) / 64;
	if ((rc = __os_calloc(b->nwords, sizeof *b->bits,
	    (void **)&b->bits)) != XTC_OK) {
		__os_free(b);
		return rc;
	}
	*out = b;
	return XTC_OK;
}

/* Kirsch-Mitzenmacher: probe i sits at (h1 + i*h2) mod m. */
static inline uint64_t
__bloom_pos(const xtc_bloom_t *b, uint64_t h1, uint64_t h2, unsigned i)
{
	return (h1 + (uint64_t)i * h2) & b->mask;
}

void
xtc_bloom_add(xtc_bloom_t *b, const void *key, size_t len)
{
	uint64_t h1, h2;
	unsigned i;

	if (b == NULL) return;
	__prob_hash2(key, len, &h1, &h2);
	for (i = 0; i < b->k; i++) {
		uint64_t pos = __bloom_pos(b, h1, h2, i);
		b->bits[pos >> 6] |= (uint64_t)1 << (pos & 63);
	}
}

int
xtc_bloom_maybe_contains(const xtc_bloom_t *b, const void *key, size_t len)
{
	uint64_t h1, h2;
	unsigned i;

	if (b == NULL) return 0;
	__prob_hash2(key, len, &h1, &h2);
	for (i = 0; i < b->k; i++) {
		uint64_t pos = __bloom_pos(b, h1, h2, i);
		if ((b->bits[pos >> 6] & ((uint64_t)1 << (pos & 63))) == 0)
			return 0;         /* one clear bit -> definitely absent */
	}
	return 1;                          /* all set -> maybe present */
}

void
xtc_bloom_fini(xtc_bloom_t *b)
{
	if (b == NULL) return;
	__os_free(b->bits);
	__os_free(b);
}

/* ============================ HyperLogLog ============================ */

struct xtc_hll {
	uint8_t *reg;        /* 2^p registers, each a leading-zero run */
	size_t   m;          /* register count = 2^p */
	int      p;          /* precision */
};

int
xtc_hll_init(xtc_hll_t **out, int precision)
{
	xtc_hll_t *h;
	int rc;

	if (out == NULL) return XTC_E_INVAL;
	if (precision < 4 || precision > 18) return XTC_E_INVAL;

	if ((rc = __os_calloc(1, sizeof *h, (void **)&h)) != XTC_OK)
		return rc;
	h->p = precision;
	h->m = (size_t)1 << precision;
	if ((rc = __os_calloc(h->m, sizeof *h->reg,
	    (void **)&h->reg)) != XTC_OK) {
		__os_free(h);
		return rc;
	}
	*out = h;
	return XTC_OK;
}

void
xtc_hll_add(xtc_hll_t *h, const void *key, size_t len)
{
	uint64_t h1, h2, w;
	size_t idx;
	uint8_t rho;

	if (h == NULL) return;
	__prob_hash2(key, len, &h1, &h2);
	/* Top p bits select the register; the remaining (64-p) bits give
	 * the position of the leftmost 1 (rho), stored as its max. */
	idx = (size_t)(h1 >> (64 - h->p));
	w = h1 << h->p;                    /* the (64-p) low bits, left-aligned */
	if (w == 0) {
		rho = (uint8_t)(64 - h->p + 1);
	} else {
		/* rho = number of leading zeros in the (64-p)-bit window,
		 * plus one.  Count on the 64-bit left-aligned window: the p
		 * shifted-in zero bits are beyond the window, but since w!=0
		 * its leading zeros are all within the first (64-p) bits. */
		uint8_t lz = 0;
		while ((w & 0x8000000000000000ULL) == 0) { lz++; w <<= 1; }
		rho = (uint8_t)(lz + 1);
	}
	if (rho > h->reg[idx]) h->reg[idx] = rho;
}

/*
 * Standard HLL estimator: E = alpha_m * m^2 / sum(2^-reg[j]), with the
 * small-range (linear counting) and large-range (2^32 wrap) corrections
 * from Flajolet et al. 2007.
 */
uint64_t
xtc_hll_count(const xtc_hll_t *h)
{
	double m, alpha, sum, est;
	size_t j, zeros = 0;
	static const double TWO32 = 4294967296.0;

	if (h == NULL) return 0;
	m = (double)h->m;

	/* alpha_m: the bias-correction constant. */
	if (h->m == 16)      alpha = 0.673;
	else if (h->m == 32) alpha = 0.697;
	else if (h->m == 64) alpha = 0.709;
	else                 alpha = 0.7213 / (1.0 + 1.079 / m);

	sum = 0.0;
	for (j = 0; j < h->m; j++) {
		if (h->reg[j] == 0) zeros++;
		/* 2^-reg[j] without powf: reg[j] <= 65 here, so a shift on a
		 * double via ldexp-equivalent -- express as 1.0 / (1<<r) for
		 * r < 63, else scale down. */
		{
			uint8_t r = h->reg[j];
			double inv;
			if (r < 63) inv = 1.0 / (double)((uint64_t)1 << r);
			else inv = 1.0 / ((double)((uint64_t)1 << 62) * 2.0 *
			    (double)((uint64_t)1 << (r - 63)));
			sum += inv;
		}
	}

	est = alpha * m * m / sum;

	/* Small-range correction: linear counting when many registers are
	 * still empty (est <= 2.5m and there are zeros). */
	if (est <= 2.5 * m && zeros != 0)
		est = m * __prob_ln(m / (double)zeros);
	/* Large-range correction: undo the 32-bit hash-space wrap.  We
	 * hash to 64 bits, so this rarely triggers, but keep it for
	 * parity with the textbook estimator. */
	else if (est > TWO32 / 30.0)
		est = -TWO32 * __prob_ln(1.0 - est / TWO32);

	if (est < 0.0) est = 0.0;
	return (uint64_t)(est + 0.5);
}

int
xtc_hll_merge(xtc_hll_t *dst, const xtc_hll_t *src)
{
	size_t j;

	if (dst == NULL || src == NULL) return XTC_E_INVAL;
	if (dst->p != src->p) return XTC_E_INVAL;
	for (j = 0; j < dst->m; j++)
		if (src->reg[j] > dst->reg[j]) dst->reg[j] = src->reg[j];
	return XTC_OK;
}

void
xtc_hll_fini(xtc_hll_t *h)
{
	if (h == NULL) return;
	__os_free(h->reg);
	__os_free(h);
}
