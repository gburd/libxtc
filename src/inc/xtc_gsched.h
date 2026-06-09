/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_gsched.h
 *	A small genetic-algorithm tuner for runtime self-tuning of integer
 *	"genes" (tunables) against an observed fitness, after Jake
 *	Moilanen's genetic scheduler (OLS 2005): a population of candidate
 *	gene-sets is evaluated, the best are bred (elitism + per-gene
 *	crossover + mutation), and the mutation rate ADAPTS -- it falls
 *	while fitness improves (hone in) and rises when fitness drops (a
 *	workload shift), so the tuner re-converges quickly.  Capped at 45%
 *	to keep it from thrashing.
 *
 *	It is a pure, deterministic (seeded) optimiser with no I/O of its
 *	own: the caller reads the current candidate's genes, uses them for
 *	one evaluation window, and reports the fitness it observed.  The
 *	xtc_iosched write scheduler drives it with throughput/latency
 *	fitness; the unit tests drive it with a synthetic function.
 */

#ifndef XTC_GSCHED_H
#define XTC_GSCHED_H

#include <stdint.h>

#include "xtc.h"

#define XTC_GSCHED_MAX_GENES 8

typedef struct xtc_gsched xtc_gsched_t;

typedef struct xtc_gsched_spec {
	int      n_genes;                       /* 1 .. XTC_GSCHED_MAX_GENES */
	int      min[XTC_GSCHED_MAX_GENES];     /* per-gene inclusive bounds */
	int      max[XTC_GSCHED_MAX_GENES];
	int      init[XTC_GSCHED_MAX_GENES];    /* seed candidate */
	int      population;                    /* candidates/generation (>=2) */
	uint64_t seed;                          /* PRNG seed (0 = default) */
} xtc_gsched_spec_t;

/*
 * PUBLIC: int    xtc_gsched_create __P((const xtc_gsched_spec_t *, xtc_gsched_t **));
 * PUBLIC: void   xtc_gsched_destroy __P((xtc_gsched_t *));
 * PUBLIC: void   xtc_gsched_current __P((const xtc_gsched_t *, int *));
 * PUBLIC: void   xtc_gsched_report __P((xtc_gsched_t *, double));
 * PUBLIC: void   xtc_gsched_best __P((const xtc_gsched_t *, int *, double *));
 * PUBLIC: double xtc_gsched_mutation_rate __P((const xtc_gsched_t *));
 * PUBLIC: uint64_t xtc_gsched_generation __P((const xtc_gsched_t *));
 */
int  xtc_gsched_create(const xtc_gsched_spec_t *spec, xtc_gsched_t **out);
void xtc_gsched_destroy(xtc_gsched_t *g);

/* Copy the genes of the candidate currently under evaluation into
 * out_genes (n_genes ints).  Use these until the next report. */
void xtc_gsched_current(const xtc_gsched_t *g, int *out_genes);

/* Report the fitness observed while using the current candidate (higher
 * is better).  Advances to the next candidate; when the whole
 * population has been evaluated, breeds the next generation and adapts
 * the mutation rate. */
void xtc_gsched_report(xtc_gsched_t *g, double fitness);

/* Best gene-set found so far and its fitness.  Either pointer may be
 * NULL.  This is the set to use once tuning has converged / frozen. */
void xtc_gsched_best(const xtc_gsched_t *g, int *out_genes, double *out_fitness);

double   xtc_gsched_mutation_rate(const xtc_gsched_t *g);
uint64_t xtc_gsched_generation(const xtc_gsched_t *g);

#endif /* XTC_GSCHED_H */
