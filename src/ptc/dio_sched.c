/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/dio_sched.c
 *	Genetic-algorithm tuner.  See xtc_dio_sched.h.  Deterministic
 *	(seeded xorshift PRNG), no I/O, no global state.
 *
 *	Phenotypes (Moilanen): the genes are partitioned into groups,
 *	each tuned by its OWN fitness measure with its own selection,
 *	adaptive-mutation rate and converge-and-freeze state.  One shared
 *	population holds the full gene vectors; each phenotype evolves
 *	only its own gene columns against its own reported fitness, so a
 *	tunable that affects throughput is not muddied by a latency
 *	signal and vice-versa.  A single phenotype over all genes is the
 *	default (the simple single-fitness case).
 */

#include "xtc_int.h"
#include "xtc_dio_sched.h"

#include <math.h>
#include <string.h>

#define MUT_RATE_MAX   0.45
#define MUT_RATE_MIN   0.02
#define MUT_RATE_INIT  0.15
#define MUT_RATE_STEP  1.5

#define FREEZE_AFTER   10
#define IMPROVE_EPS    0.01
#define REGRESS_FRAC   0.15

struct pheno {
	int      own[XTC_DIO_SCHED_MAX_GENES];   /* owned global gene indices */
	int      own_n;
	double  *fitness;                        /* [pop] this generation */
	int      best[XTC_DIO_SCHED_MAX_GENES];  /* best values (owned slots) */
	double   best_fitness;
	int      have_best;
	double   prev_gen_best;
	int      have_prev;
	double   mut_rate;
	int      frozen;
	double   frozen_fitness;
	int      stagnant;
};

struct xtc_dio_sched {
	int       n_genes;
	int       gmin[XTC_DIO_SCHED_MAX_GENES];
	int       gmax[XTC_DIO_SCHED_MAX_GENES];
	int       gene_pheno[XTC_DIO_SCHED_MAX_GENES];
	int       pop;
	int       n_phenos;
	int     (*genes)[XTC_DIO_SCHED_MAX_GENES];   /* [pop][n_genes] */
	struct pheno ph[XTC_DIO_SCHED_MAX_GENES];

	int       cur;          /* candidate index under evaluation (shared) */
	uint64_t  generation;
	uint64_t  rng;
};

/* ---- xorshift64 PRNG ---- */
static uint64_t
rng_next(struct xtc_dio_sched *g)
{
	uint64_t x = g->rng;
	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	g->rng = x;
	return x;
}
static int
rng_range(struct xtc_dio_sched *g, int lo, int hi)   /* inclusive */
{
	if (hi <= lo) return lo;
	return lo + (int)(rng_next(g) % (uint64_t)(hi - lo + 1));
}
static double
rng_unit(struct xtc_dio_sched *g)
{
	return (double)(rng_next(g) >> 11) / (double)(1ULL << 53);
}
static int
clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

int
xtc_dio_sched_create(const xtc_dio_sched_spec_t *spec, xtc_dio_sched_t **out)
{
	xtc_dio_sched_t *g;
	int i, j, rc, np;

	if (spec == NULL || out == NULL) return XTC_E_INVAL;
	if (spec->n_genes < 1 || spec->n_genes > XTC_DIO_SCHED_MAX_GENES)
		return XTC_E_INVAL;
	if (spec->population < 2) return XTC_E_INVAL;
	for (i = 0; i < spec->n_genes; i++)
		if (spec->min[i] > spec->max[i]) return XTC_E_INVAL;
	np = spec->n_phenos > 1 ? spec->n_phenos : 1;
	if (np > spec->n_genes) return XTC_E_INVAL;
	if (spec->n_phenos > 1)
		for (i = 0; i < spec->n_genes; i++)
			if (spec->gene_pheno[i] < 0 || spec->gene_pheno[i] >= np)
				return XTC_E_INVAL;

	if ((rc = __os_calloc(1, sizeof *g, (void **)&g)) != XTC_OK) return rc;
	g->n_genes = spec->n_genes;
	g->pop = spec->population;
	g->n_phenos = np;
	g->rng = spec->seed ? spec->seed : 0x9e3779b97f4a7c15ULL;
	for (i = 0; i < g->n_genes; i++) {
		g->gmin[i] = spec->min[i];
		g->gmax[i] = spec->max[i];
		g->gene_pheno[i] = (spec->n_phenos > 1) ? spec->gene_pheno[i] : 0;
	}

	if ((rc = __os_calloc((size_t)g->pop, sizeof g->genes[0],
	    (void **)&g->genes)) != XTC_OK) { __os_free(g); return rc; }

	/* Build phenotype gene-ownership lists + per-phenotype state. */
	for (i = 0; i < np; i++) {
		struct pheno *p = &g->ph[i];
		p->own_n = 0;
		p->mut_rate = MUT_RATE_INIT;
		for (j = 0; j < g->n_genes; j++)
			if (g->gene_pheno[j] == i)
				p->own[p->own_n++] = j;
		if ((rc = __os_calloc((size_t)g->pop, sizeof p->fitness[0],
		    (void **)&p->fitness)) != XTC_OK) {
			while (--i >= 0) __os_free(g->ph[i].fitness);
			__os_free(g->genes); __os_free(g);
			return rc;
		}
	}

	/* Seed: candidate 0 = init, rest random within bounds. */
	for (j = 0; j < g->n_genes; j++)
		g->genes[0][j] = clampi(spec->init[j], g->gmin[j], g->gmax[j]);
	for (i = 1; i < g->pop; i++)
		for (j = 0; j < g->n_genes; j++)
			g->genes[i][j] = rng_range(g, g->gmin[j], g->gmax[j]);

	*out = g;
	return XTC_OK;
}

void
xtc_dio_sched_destroy(xtc_dio_sched_t *g)
{
	int i;
	if (g == NULL) return;
	for (i = 0; i < g->n_phenos; i++)
		__os_free(g->ph[i].fitness);
	__os_free(g->genes);
	__os_free(g);
}

void
xtc_dio_sched_current(const xtc_dio_sched_t *g, int *out_genes)
{
	int gi;
	if (g == NULL || out_genes == NULL) return;
	for (gi = 0; gi < g->n_genes; gi++) {
		const struct pheno *p = &g->ph[g->gene_pheno[gi]];
		out_genes[gi] = p->frozen ? p->best[gi] : g->genes[g->cur][gi];
	}
}

/* Tournament by a phenotype's fitness. */
static int
tournament(struct xtc_dio_sched *g, const struct pheno *p)
{
	int a = rng_range(g, 0, g->pop - 1);
	int b = rng_range(g, 0, g->pop - 1);
	return p->fitness[a] >= p->fitness[b] ? a : b;
}

/* Evolve one phenotype's owned columns into next[][]. */
static void
breed_pheno(struct xtc_dio_sched *g, int pi,
            int (*next)[XTC_DIO_SCHED_MAX_GENES])
{
	struct pheno *p = &g->ph[pi];
	int elite = 0, i, k;
	double gen_best;
	int improved;

	if (p->frozen) {
		/* Keep the frozen best in every slot's owned columns. */
		for (i = 0; i < g->pop; i++)
			for (k = 0; k < p->own_n; k++)
				next[i][p->own[k]] = p->best[p->own[k]];
		return;
	}

	for (i = 1; i < g->pop; i++)
		if (p->fitness[i] > p->fitness[elite]) elite = i;
	gen_best = p->fitness[elite];

	improved = !p->have_best ||
	    gen_best > p->best_fitness + fabs(p->best_fitness) * IMPROVE_EPS;
	if (!p->have_best || gen_best > p->best_fitness) {
		for (k = 0; k < p->own_n; k++)
			p->best[p->own[k]] = g->genes[elite][p->own[k]];
		p->best_fitness = gen_best;
		p->have_best = 1;
	}
	if (improved) p->stagnant = 0; else p->stagnant++;

	if (p->stagnant >= FREEZE_AFTER) {
		p->frozen = 1;
		p->frozen_fitness = p->best_fitness;
		p->mut_rate = 0.0;
		for (i = 0; i < g->pop; i++)
			for (k = 0; k < p->own_n; k++)
				next[i][p->own[k]] = p->best[p->own[k]];
		return;
	}

	if (p->have_prev) {
		if (gen_best > p->prev_gen_best) p->mut_rate /= MUT_RATE_STEP;
		else                             p->mut_rate *= MUT_RATE_STEP;
		if (p->mut_rate > MUT_RATE_MAX) p->mut_rate = MUT_RATE_MAX;
		if (p->mut_rate < MUT_RATE_MIN) p->mut_rate = MUT_RATE_MIN;
	}
	p->prev_gen_best = gen_best;
	p->have_prev = 1;

	/* Elitism + crossover + mutation on the owned columns only. */
	for (k = 0; k < p->own_n; k++)
		next[0][p->own[k]] = g->genes[elite][p->own[k]];
	for (i = 1; i < g->pop; i++) {
		int pa = tournament(g, p), pb = tournament(g, p);
		for (k = 0; k < p->own_n; k++) {
			int gi = p->own[k];
			int v = (rng_unit(g) < 0.5) ? g->genes[pa][gi]
			                            : g->genes[pb][gi];
			if (rng_unit(g) < p->mut_rate) {
				int span = g->gmax[gi] - g->gmin[gi];
				int step = span > 0 ? (span / 4 + 1) : 1;
				v += rng_range(g, -step, step);
			}
			next[i][gi] = clampi(v, g->gmin[gi], g->gmax[gi]);
		}
	}
}

static void
breed_generation(struct xtc_dio_sched *g)
{
	int (*next)[XTC_DIO_SCHED_MAX_GENES];
	int i;
	if (__os_calloc((size_t)g->pop, sizeof next[0], (void **)&next) != XTC_OK)
		return;   /* OOM: keep the current population */
	for (i = 0; i < g->n_phenos; i++)
		breed_pheno(g, i, next);
	memcpy(g->genes, next, (size_t)g->pop * sizeof next[0]);
	__os_free(next);
	g->generation++;
	g->cur = 0;
}

void
xtc_dio_sched_report_multi(xtc_dio_sched_t *g, const double *fitness, int n)
{
	int i, thawed = 0;
	if (g == NULL || fitness == NULL || n < g->n_phenos) return;

	for (i = 0; i < g->n_phenos; i++) {
		struct pheno *p = &g->ph[i];
		if (p->frozen) {
			double thr = p->frozen_fitness -
			    fabs(p->frozen_fitness) * REGRESS_FRAC;
			if (fitness[i] < thr) {
				int s, k;
				p->frozen = 0;
				p->mut_rate = MUT_RATE_INIT;
				p->stagnant = 0;
				p->have_prev = 0;
				p->best_fitness = fitness[i];
				/* re-seed owned columns: slot0 = best, rest random */
				for (k = 0; k < p->own_n; k++)
					g->genes[0][p->own[k]] = p->best[p->own[k]];
				for (s = 1; s < g->pop; s++)
					for (k = 0; k < p->own_n; k++) {
						int gi = p->own[k];
						g->genes[s][gi] = rng_range(g,
						    g->gmin[gi], g->gmax[gi]);
					}
				thawed = 1;
			}
		} else {
			p->fitness[g->cur] = fitness[i];
		}
	}

	if (thawed) { g->cur = 0; return; }   /* restart the generation */

	g->cur++;
	if (g->cur >= g->pop)
		breed_generation(g);
}

void
xtc_dio_sched_report(xtc_dio_sched_t *g, double fitness)
{
	xtc_dio_sched_report_multi(g, &fitness, 1);
}

void
xtc_dio_sched_best(const xtc_dio_sched_t *g, int *out_genes, double *out_fitness)
{
	int gi;
	double fsum = 0.0;
	if (g == NULL) return;
	if (out_genes != NULL) {
		for (gi = 0; gi < g->n_genes; gi++) {
			const struct pheno *p = &g->ph[g->gene_pheno[gi]];
			out_genes[gi] = p->have_best ? p->best[gi] : g->genes[0][gi];
		}
	}
	if (out_fitness != NULL) {
		int i;
		for (i = 0; i < g->n_phenos; i++)
			fsum += g->ph[i].have_best ? g->ph[i].best_fitness : 0.0;
		*out_fitness = (g->n_phenos == 1) ? g->ph[0].best_fitness : fsum;
	}
}

double
xtc_dio_sched_mutation_rate(const xtc_dio_sched_t *g)
{
	double m = 0.0;
	int i;
	if (g == NULL) return 0.0;
	for (i = 0; i < g->n_phenos; i++)
		if (!g->ph[i].frozen && g->ph[i].mut_rate > m)
			m = g->ph[i].mut_rate;
	return m;   /* 0 only when every phenotype is frozen */
}

uint64_t
xtc_dio_sched_generation(const xtc_dio_sched_t *g)
{
	return g != NULL ? g->generation : 0;
}
