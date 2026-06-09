/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m9/test_dio_sched.c
 *	Genetic-scheduler core: convergence on a synthetic fitness,
 *	mutation-rate bounds, and input validation.
 */

#include "munit.h"
#include "xtc.h"
#include "xtc_dio_sched.h"

#include <stdlib.h>
#include <string.h>

/* Fitness peaks (==0) when genes equal the targets; otherwise negative.
 * The optimiser should drive the best gene-set toward the targets. */
static MunitResult
test_converge(const MunitParameter p[], void *d)
{
	static const int target[3] = { 25, 60, 90 };
	xtc_dio_sched_spec_t spec;
	xtc_dio_sched_t *g = NULL;
	int genes[3], best[3], i, eval;
	double bf = -1e18, mr;

	(void)p; (void)d;
	memset(&spec, 0, sizeof spec);
	spec.n_genes = 3;
	spec.min[0] = spec.min[1] = spec.min[2] = 0;
	spec.max[0] = spec.max[1] = spec.max[2] = 100;
	spec.init[0] = spec.init[1] = spec.init[2] = 0;
	spec.population = 16;
	spec.seed = 0xC0FFEEu;

	munit_assert_int(xtc_dio_sched_create(&spec, &g), ==, XTC_OK);

	/* ~150 generations of evaluation. */
	for (eval = 0; eval < spec.population * 150; eval++) {
		double f = 0.0;
		xtc_dio_sched_current(g, genes);
		for (i = 0; i < 3; i++) {
			double dlt = (double)(genes[i] - target[i]);
			f -= dlt * dlt;
		}
		xtc_dio_sched_report(g, f);

		mr = xtc_dio_sched_mutation_rate(g);
		munit_assert_double(mr, <=, 0.45 + 1e-9);  /* capped */
		munit_assert_double(mr, >=, 0.0);
	}

	xtc_dio_sched_best(g, best, &bf);
	/* Converged near the targets. */
	for (i = 0; i < 3; i++)
		munit_assert_int(abs(best[i] - target[i]), <=, 4);
	/* Best fitness near the optimum (0). */
	munit_assert_double(bf, >=, -48.0);
	munit_assert_uint64(xtc_dio_sched_generation(g), >=, 5);
	/* Converge-and-freeze: a steady fitness stops the mutation tax. */
	munit_assert_double(xtc_dio_sched_mutation_rate(g), ==, 0.0);

	xtc_dio_sched_destroy(g);
	return MUNIT_OK;
}

/* Best gene-set always stays within the declared bounds. */
static MunitResult
test_bounds(const MunitParameter p[], void *d)
{
	xtc_dio_sched_spec_t spec;
	xtc_dio_sched_t *g = NULL;
	int genes[2], i, eval;
	(void)p; (void)d;

	memset(&spec, 0, sizeof spec);
	spec.n_genes = 2;
	spec.min[0] = 10; spec.max[0] = 20; spec.init[0] = 15;
	spec.min[1] = -5; spec.max[1] = 5;  spec.init[1] = 0;
	spec.population = 8;
	spec.seed = 42;
	munit_assert_int(xtc_dio_sched_create(&spec, &g), ==, XTC_OK);

	for (eval = 0; eval < spec.population * 40; eval++) {
		xtc_dio_sched_current(g, genes);
		munit_assert_int(genes[0], >=, 10);
		munit_assert_int(genes[0], <=, 20);
		munit_assert_int(genes[1], >=, -5);
		munit_assert_int(genes[1], <=, 5);
		/* Arbitrary fitness preferring gene1 large. */
		xtc_dio_sched_report(g, (double)genes[1]);
	}
	xtc_dio_sched_best(g, genes, NULL);
	munit_assert_int(genes[1], ==, 5);   /* found the max */
	(void)i;
	xtc_dio_sched_destroy(g);
	return MUNIT_OK;
}

static MunitResult
test_validate(const MunitParameter p[], void *d)
{
	xtc_dio_sched_spec_t spec;
	xtc_dio_sched_t *g = NULL;
	(void)p; (void)d;
	memset(&spec, 0, sizeof spec);
	spec.n_genes = 0; spec.population = 8;
	munit_assert_int(xtc_dio_sched_create(&spec, &g), ==, XTC_E_INVAL);
	spec.n_genes = 2; spec.population = 1;          /* too small */
	munit_assert_int(xtc_dio_sched_create(&spec, &g), ==, XTC_E_INVAL);
	spec.population = 8; spec.min[0] = 9; spec.max[0] = 1;  /* min>max */
	munit_assert_int(xtc_dio_sched_create(&spec, &g), ==, XTC_E_INVAL);
	munit_assert_int(xtc_dio_sched_create(NULL, &g), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* Converge-and-freeze: a steady fitness freezes exploration (mutation
 * rate -> 0); a sharp regression (workload shift) thaws it. */
static MunitResult
test_freeze(const MunitParameter p[], void *d)
{
	xtc_dio_sched_spec_t spec;
	xtc_dio_sched_t *g = NULL;
	int genes[1], eval;
	(void)p; (void)d;
	memset(&spec, 0, sizeof spec);
	spec.n_genes = 1;
	spec.min[0] = 0; spec.max[0] = 100; spec.init[0] = 50;
	spec.population = 8; spec.seed = 99;
	munit_assert_int(xtc_dio_sched_create(&spec, &g), ==, XTC_OK);

	for (eval = 0; eval < spec.population * 300; eval++) {
		double f;
		xtc_dio_sched_current(g, genes);
		f = -(double)((genes[0] - 70) * (genes[0] - 70));  /* peak at 70 */
		xtc_dio_sched_report(g, f);
		if (xtc_dio_sched_mutation_rate(g) == 0.0)
			break;                              /* frozen */
	}
	munit_assert_double(xtc_dio_sched_mutation_rate(g), ==, 0.0);
	xtc_dio_sched_best(g, genes, NULL);
	munit_assert_int(abs(genes[0] - 70), <=, 4);

	/* Workload shift -> sharp regression -> re-arm. */
	xtc_dio_sched_report(g, -100000.0);
	munit_assert_double(xtc_dio_sched_mutation_rate(g), >, 0.0);

	xtc_dio_sched_destroy(g);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/converge", test_converge, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/bounds",   test_bounds,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/validate", test_validate, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/freeze",   test_freeze,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m9/dio_sched", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
