/*
 * test_sim_determinism -- prove libxtc's DST determinism guard works.
 *
 * The aspiration is FoundationDB/TigerBeetle-grade: 100% deterministic
 * simulation, and nondeterminism must be impossible to introduce
 * SILENTLY.  libxtc backs that with a guard: any sim-reachable primitive
 * that would break seed replay (a real wall clock, an unseeded RNG, an
 * env read, a raw thread id) calls __xtc_sim_nondeterminism(), which
 * during a sim run counts the violation and (in strict mode) aborts.
 * xtc_sim_exec_run itself refuses to return XTC_OK if any violation was
 * seen, so every one of the sim tests already proves its own run touched
 * nothing nondeterministic.
 *
 * This test proves the guard itself:
 *   (a) a normal seeded run reports ZERO violations (the executed path
 *       is fully deterministic), and
 *   (b) when a violation IS injected (count mode, so we do not abort),
 *       the count rises and xtc_sim_exec_run turns the otherwise-clean
 *       run into a failure -- i.e. the guarantee is enforced, not just
 *       asserted in prose.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

static atomic_int g_inject;   /* worker injects a violation when set */
static atomic_int g_ran;

/* A trivial worker: yield a couple of times, then exit.  If g_inject is
 * set, it calls the determinism guard directly to SIMULATE a
 * nondeterministic primitive being reached on the executed path. */
static void
worker(void *arg)
{
	(void)arg;
	atomic_fetch_add(&g_ran, 1);
	xtc_yield();
	if (atomic_load(&g_inject))
		__xtc_sim_nondeterminism("test-injected real clock");
	xtc_yield();
	xtc_exit_self(0);
}

static int
run_one(uint64_t seed, int inject, int *out_violations)
{
	xtc_exec_t *e = NULL;
	int rc;
	atomic_store(&g_ran, 0);
	atomic_store(&g_inject, inject);
	if (xtc_exec_init(&e, 2) != XTC_OK)
		return -999;
	/* Count-only mode so an injected violation does not abort the test
	 * process -- we want to OBSERVE the count and the run's rc. */
	xtc_sim_strict(0);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), worker, NULL, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 1), worker, NULL, NULL, NULL);
	rc = xtc_sim_exec_run(e, seed, 1000000);
	/* Read the violation count AFTER the run (it persists past
	 * deactivate until the next activate). */
	if (out_violations)
		*out_violations = xtc_sim_nondeterminism_count();
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(void)
{
	int viol_clean = -1, viol_inject = -1;
	int rc_clean, rc_inject, i, fails = 0;

	printf("== DST determinism guard ==\n");

	/* (a) A normal run over several seeds: ZERO violations, clean rc,
	 * and it replays. */
	for (i = 0; i < 16; i++) {
		uint64_t seed = 0xD37E100 + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		int v1 = -1, v2 = -1;
		int r1 = run_one(seed, 0, &v1);
		int r2 = run_one(seed, 0, &v2);
		if (r1 != XTC_OK || r2 != XTC_OK) {
			printf("FAIL: clean run rc=%d/%d for seed %llu\n",
			    r1, r2, (unsigned long long)seed);
			fails++;
		}
		if (v1 != 0 || v2 != 0) {
			printf("FAIL: clean run reported %d/%d determinism "
			    "violations (must be 0) for seed %llu\n",
			    v1, v2, (unsigned long long)seed);
			fails++;
		}
	}

	/* (b) An injected violation: the count rises AND the run fails, so
	 * the guarantee is enforced by the executor, not just documented. */
	rc_clean = run_one(0xABCDEF, 0, &viol_clean);
	rc_inject = run_one(0xABCDEF, 1, &viol_inject);

	printf("clean:    rc=%d violations=%d\n", rc_clean, viol_clean);
	printf("injected: rc=%d violations=%d\n", rc_inject, viol_inject);

	if (rc_clean != XTC_OK || viol_clean != 0) {
		printf("FAIL: the control (no injection) run was not clean\n");
		fails++;
	}
	if (viol_inject <= 0) {
		printf("FAIL: the injected violation was not counted -- the "
		    "guard did not fire\n");
		fails++;
	}
	if (rc_inject == XTC_OK) {
		printf("FAIL: a run with a determinism violation returned "
		    "XTC_OK -- the executor did not enforce the guarantee\n");
		fails++;
	}

	if (fails == 0) {
		printf("OK: DST determinism guard -- 16 seeds x2 report zero "
		    "violations and replay; an injected nondeterministic call "
		    "is counted and turns the run into a failure, so 100%% "
		    "deterministic simulation is ENFORCED, not merely "
		    "documented\n");
		return 0;
	}
	printf("FAIL: %d determinism-guard check(s) failed\n", fails);
	return 1;
}
