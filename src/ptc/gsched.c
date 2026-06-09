/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/gsched.c
 *	Genetic-algorithm tuner.  See xtc_gsched.h.  Deterministic
 *	(seeded xorshift PRNG), no I/O, no global state.
 */

#include "xtc_int.h"
#include "xtc_gsched.h"

#include <string.h>

#define MUT_RATE_MAX   0.45     /* Moilanen's cap */
#define MUT_RATE_MIN   0.02
#define MUT_RATE_INIT  0.15
#define MUT_RATE_STEP  1.5      /* multiplicative up/down adjustment */

struct xtc_gsched {
	int       n_genes;
	int       min[XTC_GSCHED_MAX_GENES];
	int       max[XTC_GSCHED_MAX_GENES];
	int       pop;
	int     (*genes)[XTC_GSCHED_MAX_GENES];  /* [pop][n_genes] */
	double   *fitness;                       /* [pop], this generation */
	int       cur;                           /* candidate under evaluation */
	int       evaluated;                     /* candidates reported so far */

	int       best[XTC_GSCHED_MAX_GENES];
	double    best_fitness;
	int       have_best;

	double    prev_gen_best;                 /* best fitness last generation */
	int       have_prev;
	double    mut_rate;
	uint64_t  generation;

	uint64_t  rng;
};

/* ---- xorshift64 PRNG ---- */
static uint64_t
rng_next(struct xtc_gsched *g)
{
	uint64_t x = g->rng;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	g->rng = x;
	return x;
}

static int
rng_range(struct xtc_gsched *g, int lo, int hi)   /* inclusive */
{
	if (hi <= lo) return lo;
	return lo + (int)(rng_next(g) % (uint64_t)(hi - lo + 1));
}

static double
rng_unit(struct xtc_gsched *g)   /* [0,1) */
{
	return (double)(rng_next(g) >> 11) / (double)(1ULL << 53);
}

static int
clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

int
xtc_gsched_create(const xtc_gsched_spec_t *spec, xtc_gsched_t **out)
{
	xtc_gsched_t *g;
	int i, j, rc;

	if (spec == NULL || out == NULL) return XTC_E_INVAL;
	if (spec->n_genes < 1 || spec->n_genes > XTC_GSCHED_MAX_GENES)
		return XTC_E_INVAL;
	if (spec->population < 2) return XTC_E_INVAL;
	for (i = 0; i < spec->n_genes; i++)
		if (spec->min[i] > spec->max[i]) return XTC_E_INVAL;

	if ((rc = __os_calloc(1, sizeof *g, (void **)&g)) != XTC_OK) return rc;
	g->n_genes = spec->n_genes;
	g->pop = spec->population;
	for (i = 0; i < g->n_genes; i++) {
		g->min[i] = spec->min[i];
		g->max[i] = spec->max[i];
	}
	g->rng = spec->seed ? spec->seed : 0x9e3779b97f4a7c15ULL;
	g->mut_rate = MUT_RATE_INIT;
	g->best_fitness = 0.0;

	if ((rc = __os_calloc((size_t)g->pop, sizeof g->genes[0],
	    (void **)&g->genes)) != XTC_OK) { __os_free(g); return rc; }
	if ((rc = __os_calloc((size_t)g->pop, sizeof g->fitness[0],
	    (void **)&g->fitness)) != XTC_OK) {
		__os_free(g->genes); __os_free(g); return rc;
	}

	/* Candidate 0 = the seed; the rest are random within bounds. */
	for (j = 0; j < g->n_genes; j++)
		g->genes[0][j] = clampi(spec->init[j], g->min[j], g->max[j]);
	for (i = 1; i < g->pop; i++)
		for (j = 0; j < g->n_genes; j++)
			g->genes[i][j] = rng_range(g, g->min[j], g->max[j]);

	*out = g;
	return XTC_OK;
}

void
xtc_gsched_destroy(xtc_gsched_t *g)
{
	if (g == NULL) return;
	__os_free(g->genes);
	__os_free(g->fitness);
	__os_free(g);
}

void
xtc_gsched_current(const xtc_gsched_t *g, int *out_genes)
{
	if (g == NULL || out_genes == NULL) return;
	memcpy(out_genes, g->genes[g->cur], (size_t)g->n_genes * sizeof(int));
}

/* Tournament select: pick the fitter of two random candidates. */
static int
tournament(struct xtc_gsched *g)
{
	int a = rng_range(g, 0, g->pop - 1);
	int b = rng_range(g, 0, g->pop - 1);
	return g->fitness[a] >= g->fitness[b] ? a : b;
}

static void
breed(struct xtc_gsched *g)
{
	int (*next)[XTC_GSCHED_MAX_GENES];
	int elite = 0, i, j;
	double gen_best;

	/* Find this generation's elite. */
	for (i = 1; i < g->pop; i++)
		if (g->fitness[i] > g->fitness[elite]) elite = i;
	gen_best = g->fitness[elite];

	/* Update the all-time best. */
	if (!g->have_best || gen_best > g->best_fitness) {
		memcpy(g->best, g->genes[elite],
		    (size_t)g->n_genes * sizeof(int));
		g->best_fitness = gen_best;
		g->have_best = 1;
	}

	/* Adaptive mutation: improving -> calmer, regressing -> explore. */
	if (g->have_prev) {
		if (gen_best > g->prev_gen_best)
			g->mut_rate /= MUT_RATE_STEP;
		else
			g->mut_rate *= MUT_RATE_STEP;
		if (g->mut_rate > MUT_RATE_MAX) g->mut_rate = MUT_RATE_MAX;
		if (g->mut_rate < MUT_RATE_MIN) g->mut_rate = MUT_RATE_MIN;
	}
	g->prev_gen_best = gen_best;
	g->have_prev = 1;

	if (__os_calloc((size_t)g->pop, sizeof next[0], (void **)&next)
	    != XTC_OK)
		return;   /* OOM: keep the current population for another round */

	/* Elitism: carry the elite unchanged. */
	memcpy(next[0], g->genes[elite],
	    (size_t)g->n_genes * sizeof(int));

	for (i = 1; i < g->pop; i++) {
		int pa = tournament(g), pb = tournament(g);
		for (j = 0; j < g->n_genes; j++) {
			int v = (rng_unit(g) < 0.5) ? g->genes[pa][j]
			                            : g->genes[pb][j];
			if (rng_unit(g) < g->mut_rate) {
				/* Mutate by a bounded random step (up to ~1/4 of
				 * the gene's range), then clamp. */
				int span = g->max[j] - g->min[j];
				int step = span > 0 ? (span / 4 + 1) : 1;
				v += rng_range(g, -step, step);
			}
			next[i][j] = clampi(v, g->min[j], g->max[j]);
		}
	}

	memcpy(g->genes, next, (size_t)g->pop * sizeof next[0]);
	__os_free(next);
	g->generation++;
	g->cur = 0;
	g->evaluated = 0;
}

void
xtc_gsched_report(xtc_gsched_t *g, double fitness)
{
	if (g == NULL) return;
	g->fitness[g->cur] = fitness;
	g->evaluated++;
	if (g->evaluated >= g->pop)
		breed(g);
	else
		g->cur++;
}

void
xtc_gsched_best(const xtc_gsched_t *g, int *out_genes, double *out_fitness)
{
	if (g == NULL) return;
	if (out_genes != NULL) {
		if (g->have_best)
			memcpy(out_genes, g->best,
			    (size_t)g->n_genes * sizeof(int));
		else
			memcpy(out_genes, g->genes[0],
			    (size_t)g->n_genes * sizeof(int));
	}
	if (out_fitness != NULL)
		*out_fitness = g->have_best ? g->best_fitness : 0.0;
}

double
xtc_gsched_mutation_rate(const xtc_gsched_t *g)
{
	return g != NULL ? g->mut_rate : 0.0;
}

uint64_t
xtc_gsched_generation(const xtc_gsched_t *g)
{
	return g != NULL ? g->generation : 0;
}
