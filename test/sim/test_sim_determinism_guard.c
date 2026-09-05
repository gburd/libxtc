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
 *	For EACH of the four classes, call the actual primitive while a
 *	sim is active and assert the violation counter advanced.  Also
 *	assert a SEEDED RNG draw does NOT trip it, so the guard is not
 *	simply crying wolf on every draw.
 *
 *	Count mode (xtc_sim_strict(0)) is used deliberately: the guard's
 *	behavior under a real sim run is to abort, which a test cannot
 *	observe.
 */

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
	 * --- 2. unseeded RNG -- the auto-seed path is unreplayable ---
	 * Only the FIRST draw on a thread auto-seeds (it mixes the clock
	 * with an ASLR-dependent address), so this must be this thread's
	 * first draw; nothing above draws.
	 */
	{
		before = xtc_sim_nondeterminism_count();
		(void)__os_rand_u64();
		after = xtc_sim_nondeterminism_count();
		expect_trapped("unseeded RNG (__os_rand_u64 auto-seed)",
		    before, after);
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
		    "yardstick #1 claims all four classes are trapped\n",
		    g_failures);
		return 1;
	}
	printf("OK: determinism guard traps all four nondeterminism classes "
	    "(real clock, unseeded RNG, env read, raw thread id) and does not "
	    "false-positive on a seeded RNG stream\n");
	return 0;
}
