/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/20_dst_app.c -- an application's own
 * deterministic simulation test, against the installed xtc_sim.h.
 *
 * Four worker procs on a two-loop executor each add a seeded number of
 * units to a total, and take a pessimal path (a 1 ms sleep) at a buggify
 * point.  Every seed runs twice: the second run must reproduce the first
 * exactly (the total and the scheduler state hash).  A planted bug,
 * guarded by XTC_DST_BUG(1), drops one unit; built with
 * -DXTC_DST_INJECT_BUG=1 the sweep must report it.
 *
 * Off a sim build, xtc_sim_exec_run returns XTC_E_NOSYS and the program
 * says so and exits 0, so it builds and runs against any libxtc.
 */

/* !region full */
#include <stdint.h>
#include <stdio.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

static int total, expect;

static void
worker(void *arg)
{
	int i, n = 1 + (int)xtc_sim_rand(8);
	(void)arg;
	expect += n;
	for (i = 0; i < n; i++) {
		if (XTC_SIM_BUGGIFY("worker.slow"))
			(void)xtc_proc_sleep(1000000);
		total++;
	}
#if XTC_DST_BUG(1)
	total--; /* planted: one unit lost */
#endif
}

/* One run of one seed: the final total, the expected one and the
 * scheduler's state hash. */
static int
run(uint64_t seed, int *out_total, int *out_expect, uint64_t *out_hash)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	total = expect = 0;
	if (xtc_exec_create(&(xtc_exec_opts_t){
	    .size = sizeof(xtc_exec_opts_t), .n_loops = 2 }, &e) != XTC_OK)
		return XTC_E_NOMEM;
	for (i = 0; i < 4; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, i % 2), worker, NULL,
		    NULL, NULL);
	xtc_sim_buggify_enable(500);
	rc = xtc_sim_exec_run(e, seed, 100000);
	xtc_sim_buggify_disable();
	*out_hash = xtc_sim_state_hash(e);
	xtc_exec_destroy(e);
	*out_total = total;
	*out_expect = expect;
	return rc;
}

int
main(void)
{
	uint64_t seed, h1, h2;
	int t1, t2, x1, x2, rc;

	for (seed = 1; seed <= 16; seed++) {
		rc = run(seed, &t1, &x1, &h1);
		if (rc == XTC_E_NOSYS) {
			printf("not a sim build: nothing to simulate\n");
			return 0;
		}
		if (rc != XTC_OK || t1 != x1) {
			printf("seed %llu: FAIL total %d of %d (rc %d)\n",
			    (unsigned long long)seed, t1, x1, rc);
			return 1;
		}
		if (run(seed, &t2, &x2, &h2) != XTC_OK || t2 != t1 ||
		    h2 != h1) {
			printf("seed %llu: FAIL the replay differs\n",
			    (unsigned long long)seed);
			return 1;
		}
	}
	printf("16 seeds, each replayed\n");
	return 0;
}
/* !endregion full */
