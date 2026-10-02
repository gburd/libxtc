/*
 * docs/_includes/snippets/stats_dist.c
 *
 * xtc_dist: a mergeable online mean/variance aggregator.  Record a
 * stream of samples and read back the running mean and (population)
 * variance -- the per-CPU shards are merged with Chan's parallel
 * formula on read, so this is cheap on the hot path and exact on the
 * slow path.
 *
 * Compiled AND run by test/docs/test_doc_snippets.sh (a release gate):
 * it must exit 0.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "xtc.h"
#include "xtc_stats.h"

int
main(void)
{
	/* The textbook Welford sample set: mean 5, population variance 4. */
	static const double sample[] = { 2, 4, 4, 4, 5, 5, 7, 9 };
	xtc_dist_t *d = NULL;
	size_t i;

	if (xtc_dist_create("snippet.batch_size", &d) != XTC_OK)
		return 1;

	/* Empty dist: well-defined, no divide-by-zero. */
	assert(xtc_dist_count(d) == 0);
	assert(xtc_dist_mean(d) == 0.0);
	assert(xtc_dist_variance(d) == 0.0);

	for (i = 0; i < sizeof sample / sizeof sample[0]; i++)
		xtc_dist_record(d, sample[i]);

	assert(xtc_dist_count(d) == 8);
	assert(fabs(xtc_dist_mean(d) - 5.0) < 1e-9);
	assert(fabs(xtc_dist_variance(d) - 4.0) < 1e-9);  /* population */
	assert(fabs(xtc_dist_stddev(d) - 2.0) < 1e-9);

	xtc_dist_destroy(d);
	printf("ok\n");
	return 0;
}
