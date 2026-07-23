/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_prob.h
 *	Probabilistic data structures: a Bloom filter (xtc_bloom) and a
 *	HyperLogLog cardinality estimator (xtc_hll).  Both trade a small,
 *	bounded, tunable error for memory that stays flat regardless of
 *	how many distinct keys pass through -- the classic space/accuracy
 *	bargain for membership tests and distinct-count estimates over
 *	streams far too large to keep exactly.
 *
 *	Keys are opaque byte spans (const void *, size_t len), hashed
 *	internally with FNV-1a plus a second independent mix, so any key
 *	type works with no caller-supplied hash or comparator.  Nothing
 *	is copied or retained -- only the derived hash bits update the
 *	structure, so a key buffer may be freed the moment the call
 *	returns.
 *
 *	SINGLE-THREADED, v1.  Neither structure is concurrency-safe:
 *	unlike xtc_chash / xtc_cskip there is no internal locking and no
 *	RCU.  Concurrent xtc_bloom_add / xtc_hll_add on one instance is a
 *	data race.  Serialize externally, or give each thread its own
 *	instance and (for HLL) xtc_hll_merge them at the end -- the merge
 *	is exact register-wise max, so N per-thread sketches combine into
 *	one whole-stream estimate with no loss.
 *
 *	Bloom filter (xtc_bloom): a bit array of m bits probed by k hash
 *	functions.  xtc_bloom_init derives the optimal m and k from the
 *	expected element count n and the target false-positive rate p:
 *	m = ceil(-n*ln(p) / (ln2)^2), k = round((m/n)*ln2).  A membership
 *	test has NO false negatives (a key that was added always tests
 *	"maybe present") and a false-positive probability that, once the
 *	filter holds about n elements, sits near the configured p.  Push
 *	well past n and the false-positive rate climbs -- the filter does
 *	not resize, so size it for the true expected load.
 *
 *	HyperLogLog (xtc_hll): 2^p registers (precision p in [4,18]) each
 *	holding the max leading-zero run seen for the hashes that fall in
 *	that register's bucket.  xtc_hll_count applies the standard
 *	bias-corrected harmonic-mean estimator with the small- and
 *	large-range corrections (linear counting for sparse fills, the
 *	2^32 wrap correction at the top), giving a distinct-count estimate
 *	with a relative standard error of about 1.04/sqrt(2^p) -- roughly
 *	1.6% at p=12, well under 1% at p=16 -- in a few KB regardless of
 *	the true cardinality.  xtc_hll_merge takes the register-wise max
 *	of two sketches, which is exactly the sketch of the union.
 */

#ifndef XTC_PROB_H
#define XTC_PROB_H

#include "xtc_export.h"

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"

typedef struct xtc_bloom xtc_bloom_t;
typedef struct xtc_hll   xtc_hll_t;

/*
 * PUBLIC: int      xtc_bloom_init __P((xtc_bloom_t **, size_t, double));
 * PUBLIC: void     xtc_bloom_add __P((xtc_bloom_t *, const void *, size_t));
 * PUBLIC: int      xtc_bloom_maybe_contains __P((const xtc_bloom_t *, const void *, size_t));
 * PUBLIC: void     xtc_bloom_fini __P((xtc_bloom_t *));
 * PUBLIC: int      xtc_hll_init __P((xtc_hll_t **, int));
 * PUBLIC: void     xtc_hll_add __P((xtc_hll_t *, const void *, size_t));
 * PUBLIC: uint64_t xtc_hll_count __P((const xtc_hll_t *));
 * PUBLIC: int      xtc_hll_merge __P((xtc_hll_t *, const xtc_hll_t *));
 * PUBLIC: void     xtc_hll_fini __P((xtc_hll_t *));
 */

/*
 * Create a Bloom filter sized for `n_expected` distinct elements at a
 * target false-positive rate `fp_rate` (0 < fp_rate < 1).  The optimal
 * bit count m and probe count k are computed from those two inputs.
 * n_expected of 0 is treated as 1 (a one-element filter).  Returns
 * XTC_OK and writes *out, or XTC_E_INVAL (NULL out, or fp_rate not in
 * the open interval (0,1)) / XTC_E_NOMEM.
 */
XTC_API int      xtc_bloom_init(xtc_bloom_t **out, size_t n_expected,
                                double fp_rate);

/*
 * Add `len` bytes at `key` to the filter.  Idempotent: adding the same
 * key twice is a no-op the second time.  A NULL filter is ignored; a
 * NULL key is allowed only when len is 0 (the empty key).
 */
XTC_API void     xtc_bloom_add(xtc_bloom_t *b, const void *key, size_t len);

/*
 * Test membership of the `len` bytes at `key`.  Returns 1 if the key
 * MAY be present (every added key returns 1 -- no false negatives) and
 * 0 if it is DEFINITELY absent.  A 1 result carries the filter's
 * configured false-positive probability.  A NULL filter returns 0.
 */
XTC_API int      xtc_bloom_maybe_contains(const xtc_bloom_t *b,
                                          const void *key, size_t len);

/* Free the filter.  A NULL argument is a no-op. */
XTC_API void     xtc_bloom_fini(xtc_bloom_t *b);

/*
 * Create a HyperLogLog sketch with 2^precision registers; `precision`
 * must be in [4,18] (16 to 262144 registers -- more precision, less
 * error, more memory).  Returns XTC_OK and writes *out, or
 * XTC_E_INVAL (NULL out or out-of-range precision) / XTC_E_NOMEM.
 */
XTC_API int      xtc_hll_init(xtc_hll_t **out, int precision);

/*
 * Add `len` bytes at `key` to the sketch.  A NULL sketch is ignored; a
 * NULL key is allowed only when len is 0.
 */
XTC_API void     xtc_hll_add(xtc_hll_t *h, const void *key, size_t len);

/*
 * Estimated number of DISTINCT keys added, via the bias-corrected
 * harmonic-mean estimator with the standard small-/large-range
 * corrections.  Relative standard error is about 1.04/sqrt(2^p).  A
 * NULL sketch returns 0.
 */
XTC_API uint64_t xtc_hll_count(const xtc_hll_t *h);

/*
 * Merge `src` into `dst` (register-wise max), so dst afterward
 * estimates the cardinality of the UNION of the two key sets -- exact,
 * no additional error introduced by the merge.  Both must have the
 * same precision.  Returns XTC_OK, or XTC_E_INVAL (NULL argument or
 * mismatched precision).
 */
XTC_API int      xtc_hll_merge(xtc_hll_t *dst, const xtc_hll_t *src);

/* Free the sketch.  A NULL argument is a no-op. */
XTC_API void     xtc_hll_fini(xtc_hll_t *h);

#endif /* XTC_PROB_H */
