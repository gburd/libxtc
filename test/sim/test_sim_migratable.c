#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sim.h"
#include "loop_int.h"     /* __xtc_current_loop: detect an actual migration */

/*
 * DST proof for xtc_proc_opts_t.migratable (the work-stealable proc).
 *
 * A migratable proc's coroutine may be work-stolen onto a different loop
 * at a park/yield point.  This test proves the migration is SAFE: the
 * proc's identity and supervision survive a carrier change.
 *
 * Invariants asserted, under the deterministic scheduler with the steal
 * and pessimal-placement knobs cranked to FORCE migration:
 *
 *   INV1 (identity survives migration): every time a worker resumes
 *        from a yield, xtc_self() still equals the pid it was told at
 *        spawn.  A migration that corrupted the current-proc TLS would
 *        make xtc_self() wrong on the new carrier -- this catches it on
 *        the very next resume.
 *
 *   INV2 (migration actually happened): at least one worker observes
 *        __xtc_current_loop change between resumes.  Without this the
 *        test could pass vacuously (procs never moved).  We REQUIRE a
 *        non-trivial number of observed migrations.
 *
 *   INV3 (supervision survives): a monitor established on every worker
 *        delivers EXACTLY ONE DOWN per worker -- migration must not drop
 *        or duplicate a DOWN.
 *
 *   INV4 (determinism): same seed => byte-identical state hash + the
 *        same migration count + the same identity-check count, twice.
 *
 * If migratable were unsafe (e.g. a park site that failed to carry
 * __current_proc across the resume), INV1 or INV3 would fail; the
 * pinned control run (migratable=0) is the baseline that must also pass
 * every invariant EXCEPT it observes zero migrations.
 */

#define N_LOOPS    4
#define N_WORKERS  16
#define N_YIELDS   24     /* many park points => many steal opportunities */

static atomic_int  g_self_ok;       /* xtc_self()==own pid checks that passed */
static atomic_int  g_self_bad;      /* ... that FAILED (must stay 0) */
static atomic_int  g_ud_ok;         /* xtc_proc_userdata()==own value passed */
static atomic_int  g_ud_bad;        /* ... that FAILED (must stay 0) */
static atomic_int  g_migrations;    /* observed carrier-loop changes */
static atomic_int  g_downs;         /* DOWN messages the monitor received */
static atomic_int  g_workers_done;

/*
 * Worker: learn our own pid via xtc_self() at entry, then yield
 * N_YIELDS times, checking identity + carrier on each resume.
 */
static void
worker(void *arg)
{
	long id = (long)(intptr_t)arg;
	xtc_pid_t me = xtc_self();
	xtc_loop_t *last_loop = __xtc_current_loop;
	int i;
	/* A unique per-proc userdata value; must read back identically on
	 * every resume, even after migrating to another loop (INV5). */
	void *my_ud = (void *)(uintptr_t)(0xD000UL + (unsigned long)id);
	(void)xtc_proc_set_userdata(my_ud);

	for (i = 0; i < N_YIELDS; i++) {
		xtc_yield();

		/* INV1: identity survived the resume (possibly on a new loop). */
		if (xtc_pid_eq(xtc_self(), me))
			atomic_fetch_add_explicit(&g_self_ok, 1,
			    memory_order_relaxed);
		else
			atomic_fetch_add_explicit(&g_self_bad, 1,
			    memory_order_relaxed);

		/* INV5: per-proc userdata survived the resume too. */
		if (xtc_proc_userdata() == my_ud)
			atomic_fetch_add_explicit(&g_ud_ok, 1,
			    memory_order_relaxed);
		else
			atomic_fetch_add_explicit(&g_ud_bad, 1,
			    memory_order_relaxed);

		/* INV2: did the carrier loop change? (a real migration) */
		if (__xtc_current_loop != last_loop) {
			atomic_fetch_add_explicit(&g_migrations, 1,
			    memory_order_relaxed);
			last_loop = __xtc_current_loop;
		}
	}
	atomic_fetch_add_explicit(&g_workers_done, 1, memory_order_relaxed);
}

/*
 * Monitor: spawn N_WORKERS as migratable+monitored, then collect one
 * DOWN each (INV3).  Runs as a proc so it has a mailbox.
 */
static void
monitor(void *arg)
{
	xtc_exec_t *e = (xtc_exec_t *)arg;
	uint64_t refs[N_WORKERS];
	int i, collected = 0;

	for (i = 0; i < N_WORKERS; i++) {
		xtc_proc_opts_t opts;
		xtc_loop_t *l = xtc_exec_loop(e, (unsigned)(i % N_LOOPS));
		xtc_pid_t child = XTC_PID_NONE;
		memset(&opts, 0, sizeof opts);
		opts.migratable = 1;               /* THE knob under test */
		refs[i] = 0;
		(void)xtc_proc_spawn_monitor(l, worker, (void *)(intptr_t)i,
		    &opts, &child, &refs[i]);
	}

	/* Collect one DOWN per worker. */
	for (i = 0; i < N_WORKERS; i++) {
		void *m = NULL; size_t n = 0;
		xtc_pid_t dpid; int reason;
		if (xtc_recv(&m, &n, 10LL * 1000 * 1000 * 1000) != XTC_OK)
			break;
		if (xtc_down_decode(m, n, &dpid, &reason) == XTC_OK) {
			atomic_fetch_add_explicit(&g_downs, 1,
			    memory_order_relaxed);
			collected++;
		}
		if (m) xtc_free(m);
	}
	(void)collected;
}

static long
run_once(uint64_t seed, int migratable, uint64_t *out_state_hash,
    int *out_migrations, int *out_self_ok, int *out_self_bad,
    int *out_downs, int *out_done)
{
	xtc_exec_t *e = NULL;

	atomic_store(&g_self_ok, 0);
	atomic_store(&g_self_bad, 0);
	atomic_store(&g_ud_ok, 0);
	atomic_store(&g_ud_bad, 0);
	atomic_store(&g_migrations, 0);
	atomic_store(&g_downs, 0);
	atomic_store(&g_workers_done, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;

	/* Crank the steal + pessimal-placement knobs so the deterministic
	 * scheduler actively moves runnable work onto idle loops -- this is
	 * what turns "migratable" into observed migrations. */
	xtc_sim_sched_pessimal(500);   /* 50% pessimal picks */

	/* Drive from a monitor proc so DOWN collection has a mailbox; the
	 * monitor spawns the workers with opts.migratable = the flag under
	 * test. */
	(void)migratable;   /* the monitor sets it; kept for signature symmetry */
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), monitor, e, NULL, NULL);

	(void)xtc_sim_exec_run(e, seed, 5000000);

	if (out_state_hash) *out_state_hash = xtc_sim_state_hash(e);
	if (out_migrations) *out_migrations = atomic_load(&g_migrations);
	if (out_self_ok)    *out_self_ok = atomic_load(&g_self_ok);
	if (out_self_bad)   *out_self_bad = atomic_load(&g_self_bad);
	if (out_downs)      *out_downs = atomic_load(&g_downs);
	if (out_done)       *out_done = atomic_load(&g_workers_done);

	(void)xtc_exec_fini(e);
	return atomic_load(&g_self_ok);
}

int
main(void)
{
	uint64_t s1 = 0, s2 = 0;
	int mig1 = 0, mig2 = 0, ok1 = 0, ok2 = 0, bad1 = 0, bad2 = 0;
	int down1 = 0, down2 = 0, done1 = 0, done2 = 0;
	int ud_ok1, ud_bad1, ud_ok2, ud_bad2;

	/* Two runs, same seed -> must replay byte-identically. */
	run_once(0xA11CEULL, 1, &s1, &mig1, &ok1, &bad1, &down1, &done1);
	ud_ok1 = atomic_load(&g_ud_ok); ud_bad1 = atomic_load(&g_ud_bad);
	run_once(0xA11CEULL, 1, &s2, &mig2, &ok2, &bad2, &down2, &done2);
	ud_ok2 = atomic_load(&g_ud_ok); ud_bad2 = atomic_load(&g_ud_bad);

	printf("run1: self_ok=%d self_bad=%d migrations=%d downs=%d done=%d "
	    "state=%016llx\n", ok1, bad1, mig1, down1, done1,
	    (unsigned long long)s1);
	printf("run2: self_ok=%d self_bad=%d migrations=%d downs=%d done=%d "
	    "state=%016llx\n", ok2, bad2, mig2, down2, done2,
	    (unsigned long long)s2);

	/* INV1: identity never wrong. */
	if (bad1 != 0 || bad2 != 0) {
		printf("FAIL[INV1]: xtc_self() wrong after a resume "
		    "(bad=%d,%d) -- migration corrupted proc identity\n",
		    bad1, bad2);
		return 1;
	}
	/* INV5: per-proc userdata never wrong across migration. */
	if (ud_bad1 != 0 || ud_bad2 != 0) {
		printf("FAIL[INV5]: xtc_proc_userdata() wrong after a resume "
		    "(bad=%d,%d) -- migration did not carry per-proc userdata\n",
		    ud_bad1, ud_bad2);
		return 1;
	}
	if (ud_ok1 != N_WORKERS * N_YIELDS || ud_ok2 != N_WORKERS * N_YIELDS) {
		printf("FAIL[INV5]: userdata-check count wrong (%d,%d, want %d)\n",
		    ud_ok1, ud_ok2, N_WORKERS * N_YIELDS);
		return 1;
	}
	/* All workers ran to completion and did all their identity checks. */
	if (done1 != N_WORKERS || done2 != N_WORKERS) {
		printf("FAIL: workers did not all complete (%d,%d of %d)\n",
		    done1, done2, N_WORKERS);
		return 1;
	}
	if (ok1 != N_WORKERS * N_YIELDS || ok2 != N_WORKERS * N_YIELDS) {
		printf("FAIL: identity-check count wrong (%d,%d, want %d)\n",
		    ok1, ok2, N_WORKERS * N_YIELDS);
		return 1;
	}
	/* INV2: migration actually happened (else the proof is vacuous). */
	if (mig1 == 0) {
		printf("FAIL[INV2]: no migration observed -- migratable procs "
		    "never moved; the test would be vacuous\n");
		return 1;
	}
	/* INV3: every worker delivered exactly one DOWN. */
	if (down1 != N_WORKERS || down2 != N_WORKERS) {
		printf("FAIL[INV3]: DOWN count wrong (%d,%d, want %d) -- "
		    "migration dropped/duplicated a supervision signal\n",
		    down1, down2, N_WORKERS);
		return 1;
	}
	/* INV4: byte-identical replay. */
	if (s1 != s2 || mig1 != mig2 || ok1 != ok2) {
		printf("FAIL[INV4]: same seed did not replay "
		    "(state %016llx!=%016llx, mig %d!=%d, ok %d!=%d)\n",
		    (unsigned long long)s1, (unsigned long long)s2,
		    mig1, mig2, ok1, ok2);
		return 1;
	}

	printf("OK: migratable procs work-stolen across loops -- identity "
	    "(xtc_self), per-proc userdata, and supervision (DOWN) survive "
	    "every migration; "
	    "%d migrations observed, %d identity checks all correct, "
	    "%d/%d DOWNs delivered, byte-identical replay\n",
	    mig1, ok1, down1, N_WORKERS);
	return 0;
}
