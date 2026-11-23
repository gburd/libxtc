/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_determinism_guard.c
 *	Meta-test: are the determinism guard's HOOKS actually in place?
 *
 *	test_sim_determinism (sibling) proves the guard MECHANISM works --
 *	it injects a violation by calling __xtc_sim_nondeterminism()
 *	directly and shows the count rises and xtc_sim_exec_run then
 *	refuses XTC_OK.  What it cannot show is whether the REAL
 *	nondeterministic primitives call the guard at all.
 *
 *	That distinction mattered: an audit (2026-09) found the guard
 *	wired into ONE of the four classes AGENTS.md claims, with
 *	__os_clock_real unguarded despite three live callers -- so the
 *	suite was green while wall-clock reads silently broke seed replay
 *	in the native-engine composition tests.  A synthetic injection
 *	test cannot catch that; this one does.
 *
 *	For EACH guarded class, call the actual primitive while a sim is
 *	active and assert the violation counter advanced.  The RNG is no
 *	longer a guarded class (2.0): under the simulator an un-seeded
 *	draw takes the run's own stream, so assert it does NOT trip and
 *	replays, and that a SEEDED draw does not trip either.  A draw with
 *	the virtual clock on but outside a run (a harness's setup) must not
 *	depend on the thread's TLS address either: two fresh threads, whose
 *	state cells sit at different addresses, must draw the same value.
 *
 *	Count mode (xtc_sim_strict(0)) is used deliberately: the guard's
 *	behavior under a real sim run is to abort, which a test cannot
 *	observe.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_sim.h"
#include "os_thread.h"   /* __os_thread_t, __os_thread_self */

/*
 * The primitives under test.  This test deliberately reaches past the
 * public API: the guard is an INTERNAL contract between the os layer and
 * the simulator, so the hooks can only be observed from inside.
 */
int      __os_clock_real(int64_t *out);
uint64_t __os_rand_u64(void);
void     __os_rand_seed(uint64_t seed);
int      __os_env_get(const char *name, char *buf, size_t bufsize);

static int g_failures;

/* Two threads alive at once, so their TLS state cells sit at different
 * addresses (a joined thread's stack and TLS are reused by the next). */
struct draw_pair {
	pthread_barrier_t bar;     /* both drawn before either exits */
	uint64_t r[2];
	const void *cell[2];       /* each thread's stack, for the check */
	atomic_int i;
};

/* One thread's first un-seeded draw. */
static void *
first_draw(void *arg)
{
	struct draw_pair *d = arg;
	int me = atomic_fetch_add(&d->i, 1);
	int tls_probe;

	d->r[me] = __os_rand_u64();
	d->cell[me] = &tls_probe;
	(void)pthread_barrier_wait(&d->bar);
	return NULL;
}

/* The first un-seeded draws of two fresh, concurrent threads, with the
 * virtual clock at `vns` and no run active.  -1 if the two did not run
 * on distinct stacks; a thread that cannot start ends the test. */
static int
vclock_draws(int64_t vns, uint64_t r[2])
{
	struct draw_pair d;
	pthread_t th[2];
	int k, rc = 0;

	memset(&d, 0, sizeof d);
	r[0] = r[1] = 0;
	xtc_sim_clock_set(vns);
	if (pthread_barrier_init(&d.bar, NULL, 2) != 0)
		return -1;
	for (k = 0; k < 2; k++)
		if (pthread_create(&th[k], NULL, first_draw, &d) != 0)
			abort();   /* the other thread would wait on the barrier */
	for (k = 0; k < 2; k++)
		(void)pthread_join(th[k], NULL);
	(void)pthread_barrier_destroy(&d.bar);
	if (d.cell[0] == d.cell[1])
		rc = -1;
	r[0] = d.r[0];
	r[1] = d.r[1];
	return rc;
}

static void
expect_trapped(const char *what, int before, int after)
{
	if (after > before) {
		printf("  [guard] OK: %s trapped (%d -> %d)\n",
		    what, before, after);
		return;
	}
	printf("  [guard] FAIL: %s NOT trapped (count stayed %d) -- the "
	    "determinism guard has a hole, so seed replay is NOT enforced "
	    "for this class\n", what, before);
	g_failures++;
}

int
main(void)
{
	int before, after;

	/*
	 * Arm the guard: every call site is gated on __xtc_sim_active().
	 * Count mode, so a trapped violation is observable instead of
	 * aborting the process.
	 */
	xtc_sim_activate(0xD371E511u);
	xtc_sim_strict(0);

	if (!__xtc_sim_active()) {
		printf("FAIL: sim did not activate; the guard is dormant and "
		    "this test would vacuously pass\n");
		return 1;
	}

	/* --- 1. real (WALL) clock -- never virtualized by the sim --- */
	{
		int64_t ns = 0;
		before = xtc_sim_nondeterminism_count();
		(void)__os_clock_real(&ns);
		after = xtc_sim_nondeterminism_count();
		expect_trapped("real clock (__os_clock_real)", before, after);
	}

	/*
	 * --- 2. un-seeded RNG -- replays from the run's stream ---
	 * Off the simulator an un-seeded thread seeds from the clock and an
	 * ASLR-dependent address, which no seed reproduces.  Under it the
	 * draw takes the run's XTC_SIM_RNG_RAND stream instead, so it must
	 * NOT trip the guard, and re-activating with the same seed must
	 * draw the same value (xtc_sim_activate also zeroes the count).
	 */
	{
		uint64_t r1, r2;
		xtc_sim_activate(0xD371E511u);
		r1 = __os_rand_u64();
		xtc_sim_activate(0xD371E511u);
		r2 = __os_rand_u64();
		after = xtc_sim_nondeterminism_count();
		if (after != 0 || r1 != r2) {
			printf("  [guard] FAIL: an un-seeded __os_rand_u64 draw "
			    "under sim %s\n", after != 0 ? "tripped the guard" :
			    "did not replay from the run's seed");
			g_failures++;
		} else {
			printf("  [guard] OK: un-seeded RNG draw replays from the "
			    "run's seed (not trapped)\n");
		}
	}

	/*
	 * Setup-time draws: the virtual clock on, no run active (the window
	 * between xtc_sim_clock_enable and xtc_sim_exec_run).  An un-seeded
	 * thread seeds itself there, and its seed must be a function of the
	 * virtual clock alone: mixing in the state cell's TLS address made
	 * a DST harness's CREATE INDEX key, and so its tree, differ from one
	 * process to the next under ASLR.  Two fresh threads have their
	 * cells at different addresses, so they must still agree.
	 */
	{
		uint64_t r[2] = { 0, 0 };
		int rc;
		xtc_sim_deactivate();
		xtc_sim_clock_enable(0);
		rc = vclock_draws(123456789, r);
		xtc_sim_clock_disable();
		xtc_sim_activate(0xD371E511u);
		xtc_sim_strict(0);
		if (rc != 0 || r[0] != r[1]) {
			printf("  [guard] FAIL: an un-seeded __os_rand_u64 draw "
			    "under the virtual clock outside a run depends on "
			    "the thread (%s%016llx vs %016llx)\n",
			    rc != 0 ? "threads not concurrent: " : "",
			    (unsigned long long)r[0], (unsigned long long)r[1]);
			g_failures++;
		} else {
			printf("  [guard] OK: an un-seeded RNG draw under the "
			    "virtual clock outside a run is the same on every "
			    "thread\n");
		}
	}

	/* A SEEDED stream is deterministic and must NOT trip the guard,
	 * or it would fire on every legitimate draw. */
	{
		__os_rand_seed(0x2545F4914F6CDD1DULL);
		before = xtc_sim_nondeterminism_count();
		(void)__os_rand_u64();
		(void)__os_rand_u64();
		after = xtc_sim_nondeterminism_count();
		if (after != before) {
			printf("  [guard] FAIL: a SEEDED __os_rand_u64 draw "
			    "tripped the guard (%d -> %d) -- false positive\n",
			    before, after);
			g_failures++;
		} else {
			printf("  [guard] OK: seeded RNG draw not trapped "
			    "(no false positive)\n");
		}
	}

	/* --- 3. environment read -- ambient host state --- */
	{
		char buf[64];
		before = xtc_sim_nondeterminism_count();
		(void)__os_env_get("PATH", buf, sizeof buf);
		after = xtc_sim_nondeterminism_count();
		expect_trapped("environment read (__os_env_get)",
		    before, after);
	}

	/* --- 4. raw OS thread id -- host-assigned, varies per run --- */
	{
		__os_thread_t self;
		memset(&self, 0, sizeof self);
		before = xtc_sim_nondeterminism_count();
		(void)__os_thread_self(&self);
		after = xtc_sim_nondeterminism_count();
		expect_trapped("raw OS thread id (__os_thread_self)",
		    before, after);
	}

	if (g_failures != 0) {
		printf("FAIL: %d determinism-guard hole(s) -- AGENTS.md "
		    "yardstick #1 claims these classes are trapped\n",
		    g_failures);
		return 1;
	}
	printf("OK: determinism guard traps the real clock, env reads and raw "
	    "thread ids; an un-seeded RNG draw replays from the run's seed, "
	    "does not depend on the thread under the virtual clock, and a "
	    "seeded one is not trapped\n");
	return 0;
}
