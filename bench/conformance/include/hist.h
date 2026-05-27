/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * bench/conformance/include/hist.h
 *   Log-linear (HDR-style) histogram for nanosecond latency capture.
 *   Single-header library: define HIST_IMPLEMENTATION in exactly one
 *   translation unit before including to emit function bodies.
 *
 * API
 *   hist_init(h, sub_bits)   allocate; sub_bits=7 gives ~2 sig-digit precision
 *   hist_fini(h)             release memory
 *   hist_record(h, ns)       record one observation (nanoseconds)
 *   hist_percentile(h, pct)  return pct-th percentile in nanoseconds
 *   hist_dump_csv(h, fp)     write non-empty bucket CSV to fp
 *
 * Precision
 *   Each power-of-2 band is divided into 2^sub_bits sub-buckets.
 *   sub_bits=7 -> 128 sub-buckets -> <=0.8% relative error (~=2 decimal sig-figs).
 *   sub_bits=4 -> 16  sub-buckets -> <=6%   (~=1 decimal sig-fig).
 *   sub_bits=10 -> 1024 sub-buckets -> <=0.1% (~=3 decimal sig-figs).
 *
 * Range
 *   1 ns to HIST_MAX_NS (60 x 10^9 ns = 60 s).  Values outside this range
 *   are clamped; zero is treated as 1.
 *
 * Bucket layout
 *   Linear region  (band 0): indices [0, S)        -- one count per nanosecond.
 *   Exponential band k>=1: indices [S*k, S*(k+1))  -- each bucket spans 2^(k-1) ns.
 *   where S = 2^sub_bits.
 *
 *   Total buckets = S x (K_max + 1), where K_max = msb(HIST_MAX_NS) - sub_bits + 1.
 *   For sub_bits=7: S=128, K_max=29, total=3840 buckets (30 KB at uint64_t).
 *
 * Headers required
 *   Always:  <stdint.h>, <stdlib.h>, <string.h>
 *   For hist_dump_csv: <stdio.h> (include before this header)
 *
 * Build
 *   Compiles clean with -std=c11 -Wall -Wextra -Wpedantic -fPIC.
 */

#ifndef HIST_H
#define HIST_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Maximum trackable value: 60 seconds expressed in nanoseconds.
 * Values larger than this are clamped to HIST_MAX_NS.
 */
#define HIST_MAX_NS     UINT64_C(60000000000)

/*
 * Default sub-bits: 2 significant decimal digit precision.
 */
#define HIST_SUB_BITS_DEFAULT  7u

typedef struct {
    uint64_t *buckets;    /* counts[0..n_buckets-1]         */
    uint64_t  total;      /* total observations recorded     */
    uint64_t  min_ns;     /* smallest value seen             */
    uint64_t  max_ns;     /* largest value seen              */
    uint32_t  n_buckets;  /* length of the buckets array     */
    uint32_t  sub_bits;   /* log2 of sub-buckets per band    */
} hist_t;

/*
 * hist_init -- allocate and initialise the histogram.
 *
 * sub_bits: number of sub-bucket bits; clamped to [1, 10].
 *   Use HIST_SUB_BITS_DEFAULT (7) for ~2 significant decimal digits.
 *
 * Returns 0 on success, -1 if malloc fails.
 */
int      hist_init(hist_t *h, uint32_t sub_bits);

/*
 * hist_fini -- free the bucket array and zero the struct.
 */
void     hist_fini(hist_t *h);

/*
 * hist_record -- add one observation of value_ns nanoseconds.
 * 0 is treated as 1; values above HIST_MAX_NS are clamped.
 */
void     hist_record(hist_t *h, uint64_t value_ns);

/*
 * hist_percentile -- return the value at the pct-th percentile.
 * pct is in [0.0, 100.0].  Returns 0 if no observations recorded.
 */
uint64_t hist_percentile(const hist_t *h, double pct);

/*
 * hist_dump_csv -- write non-empty buckets to fp as CSV.
 * Columns: lo_ns, hi_ns, count, cumulative_pct
 * Requires <stdio.h> to be included before this header.
 */
void     hist_dump_csv(const hist_t *h, FILE *fp);

/* -------------------------------------------------------------------------
 * Implementation -- compiled only when HIST_IMPLEMENTATION is defined.
 * ------------------------------------------------------------------------- */
#ifdef HIST_IMPLEMENTATION

/*
 * hist__msb -- position of the most-significant set bit (0-indexed from LSB).
 * Requires v >= 1.
 */
static uint32_t
hist__msb(uint64_t v)
{
    uint32_t k = 0;

    if (v >= UINT64_C(0x100000000)) { k += 32; v >>= 32; }
    if (v >= UINT64_C(0x00010000))  { k += 16; v >>= 16; }
    if (v >= UINT64_C(0x00000100))  { k +=  8; v >>=  8; }
    if (v >= UINT64_C(0x00000010))  { k +=  4; v >>=  4; }
    if (v >= UINT64_C(0x00000004))  { k +=  2; v >>=  2; }
    if (v >= UINT64_C(0x00000002))  { k +=  1;           }
    return k;
}

/*
 * hist__bucket -- map a clamped, non-zero value to its bucket index.
 *
 * Linear region (band 0): index = value        for value in [0, S).
 * Exponential band k>=1:  index = S*k + sub     for value in [S*2^(k-1), S*2^k).
 *   where sub = (value >> (k-1)) - S.
 */
static uint32_t
hist__bucket(const hist_t *h, uint64_t v)
{
    uint32_t sub_count = 1u << h->sub_bits;
    uint32_t msb, k, sub;

    if (v < (uint64_t)sub_count)
        return (uint32_t)v;

    msb = hist__msb(v);
    k   = msb - h->sub_bits + 1u;
    sub = (uint32_t)(v >> (k - 1u)) - sub_count;
    return sub_count * k + sub;
}

/*
 * hist__bucket_lo -- reconstruct the lower bound of the value range for idx.
 */
static uint64_t
hist__bucket_lo(const hist_t *h, uint32_t idx)
{
    uint32_t sub_count = 1u << h->sub_bits;
    uint32_t k, sub;

    if (idx < sub_count)
        return (uint64_t)idx;

    k   = idx / sub_count;
    sub = idx % sub_count;
    /* lo = (sub + sub_count) * 2^(k-1) */
    return (uint64_t)(sub + sub_count) << (k - 1u);
}

/*
 * hist__bucket_step -- width (in ns) of bucket idx.
 */
static uint64_t
hist__bucket_step(const hist_t *h, uint32_t idx)
{
    uint32_t sub_count = 1u << h->sub_bits;
    uint32_t k;

    if (idx < sub_count)
        return UINT64_C(1);

    k = idx / sub_count;
    return UINT64_C(1) << (k - 1u);
}

/* ---- public functions ---- */

int
hist_init(hist_t *h, uint32_t sub_bits)
{
    uint32_t sub_count, msb_max, n_bands, n_buckets;

    if (sub_bits < 1u || sub_bits > 10u)
        sub_bits = HIST_SUB_BITS_DEFAULT;

    sub_count = 1u << sub_bits;
    /*
     * Band k covers [S*2^(k-1), S*2^k).  We need the largest band K
     * such that S*2^K > HIST_MAX_NS, i.e. K = msb(HIST_MAX_NS) - sub_bits + 1.
     */
    msb_max  = hist__msb(HIST_MAX_NS);      /* 35 for 60 000 000 000 */
    n_bands  = msb_max - sub_bits + 1u;     /* number of exponential bands */
    n_buckets = sub_count * (n_bands + 1u); /* +1 for the linear band 0   */

    memset(h, 0, sizeof(*h));
    h->sub_bits  = sub_bits;
    h->n_buckets = n_buckets;
    h->min_ns    = UINT64_MAX;
    h->max_ns    = 0;
    h->total     = 0;
    h->buckets   = (uint64_t *)calloc((size_t)n_buckets, sizeof(uint64_t));
    if (h->buckets == NULL)
        return -1;
    return 0;
}

void
hist_fini(hist_t *h)
{
    free(h->buckets);
    memset(h, 0, sizeof(*h));
}

void
hist_record(hist_t *h, uint64_t value_ns)
{
    uint32_t idx;

    if (value_ns == 0)
        value_ns = 1;
    if (value_ns > HIST_MAX_NS)
        value_ns = HIST_MAX_NS;

    idx = hist__bucket(h, value_ns);
    if (idx < h->n_buckets)
        h->buckets[idx]++;

    h->total++;
    if (value_ns < h->min_ns)
        h->min_ns = value_ns;
    if (value_ns > h->max_ns)
        h->max_ns = value_ns;
}

uint64_t
hist_percentile(const hist_t *h, double pct)
{
    uint64_t target, cumulative;
    uint32_t i;

    if (h->total == 0)
        return 0;
    if (pct <= 0.0)
        return h->min_ns;
    if (pct >= 100.0)
        return h->max_ns;

    target = (uint64_t)(pct / 100.0 * (double)h->total);
    if (target == 0)
        target = 1;

    cumulative = 0;
    for (i = 0; i < h->n_buckets; i++) {
        if (h->buckets[i] == 0)
            continue;
        cumulative += h->buckets[i];
        if (cumulative >= target) {
            uint64_t lo   = hist__bucket_lo(h, i);
            uint64_t step = hist__bucket_step(h, i);
            return lo + step / 2u;
        }
    }
    return h->max_ns;
}

void
hist_dump_csv(const hist_t *h, FILE *fp)
{
    uint64_t cumulative = 0;
    uint32_t i;

    fprintf(fp, "lo_ns,hi_ns,count,cumulative_pct\n");
    for (i = 0; i < h->n_buckets; i++) {
        uint64_t lo, step, hi;
        double   pct;

        if (h->buckets[i] == 0)
            continue;

        lo         = hist__bucket_lo(h, i);
        step       = hist__bucket_step(h, i);
        hi         = lo + step - 1u;
        cumulative += h->buckets[i];
        pct        = (double)cumulative / (double)h->total * 100.0;

        fprintf(fp, "%llu,%llu,%llu,%.6f\n",
                (unsigned long long)lo,
                (unsigned long long)hi,
                (unsigned long long)h->buckets[i],
                pct);
    }
}

#endif /* HIST_IMPLEMENTATION */
#endif /* HIST_H */
