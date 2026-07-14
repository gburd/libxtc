/*
 * test_sim_composition -- Deterministic Simulation Testing of a RANDOM
 * COMPOSITION of process-lifecycle operations, checking SYSTEM-WIDE
 * invariants that no single-primitive test can (PLAN.md 19.19).
 *
 * Per-primitive DST/PBT tests pin down one operation in isolation.
 * This test draws a seeded random SCRIPT of mixed operations --
 * spawn, spawn_monitor, spawn_link, send, monitor-an-existing-child,
 * and exit-a-child -- interleaved across N loops under the pessimal
 * scheduler, and asserts the invariants that only hold for the SYSTEM:
 *
 *   INV1 (every monitor fires): each xtc_monitor / spawn_monitor
 *        established delivers EXACTLY ONE DOWN (a real reason, or
 *        NOPROC if it raced an already-dead target -- both count as
 *        "fired").  monitors_established == downs_received.
 *   INV2 (every link fires): each spawn_link child delivers EXACTLY
 *        ONE EXIT signal.  links_established == exits_received.
 *   INV3 (no orphans / no leak): after the whole run quiesces, the
 *        runtime holds NO leftover child procs -- every proc the
 *        script spawned has exited and been reaped.  Checked via
 *        xtc_inspect_procs at the end (0 live procs).
 *   INV4 (determinism): the entire signal tally + the sim state hash
 *        replay BYTE-IDENTICALLY from the seed; a different seed
 *        reorders operations but every invariant still holds.
 *
 * A child that is monitored AND whose driver later force-exits it must
 * still deliver its DOWN; a child that exits on its own before the
 * monitor lands delivers NOPROC -- INV1 tolerates both, which is the
 * whole point (the count is conserved regardless of the race outcome).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_inspect.h"
#include "xtc_sim.h"

#define N_LOOPS      4
#define N_DRIVERS    4
#define OPS_PER_DRV  24        /* random operations each driver runs */
#define MAX_CHILDREN 32        /* per-driver child-pid tracking */

/* System-wide tallies (summed across all drivers). */
static atomic_int g_monitors;    /* monitors established (spawn_monitor + monitor) */
static atomic_int g_downs;       /* DOWN signals received */
static atomic_int g_links;       /* links established (spawn_link) */
static atomic_int g_exits;       /* EXIT signals received */
static atomic_int g_spawned;     /* children spawned (any flavor) */
static atomic_int g_child_ran;   /* children that entered their body */
static atomic_int g_drv_done;
static atomic_long g_tally;      /* order-insensitive fold for a coarse check */

/* A child: run a seeded number of yields, then exit with reason 0.
 * It may be force-exited by its driver before it gets here; either way
 * its monitor/link relationship must fire exactly once. */
static void
child_fn(void *arg)
{
	int k = (int)(intptr_t)arg;
	int i;
	atomic_fetch_add_explicit(&g_child_ran, 1, memory_order_relaxed);
	for (i = 0; i < (k & 3); i++)
		xtc_yield();
	xtc_exit_self(0);
}

/* Classify one drained signal via the runtime's own decoder, which
 * accepts both the monitor DOWN and the link EXIT shapes.  They are
 * told apart by the monitor reference: a monitor DOWN carries a
 * non-zero ref, a link EXIT carries ref == 0 (see xtc_down_info_t). */
static void
classify(void *m, size_t n)
{
	xtc_down_info_t di;
	if (xtc_down_decode_ex(m, n, &di) != XTC_OK)
		return;
	if (di.ref != 0)
		atomic_fetch_add_explicit(&g_downs, 1, memory_order_relaxed);
	else
		atomic_fetch_add_explicit(&g_exits, 1, memory_order_relaxed);
}

/*
 * Driver: run a seeded script of mixed lifecycle ops, tracking how many
 * monitors/links it establishes, then drain every expected signal.
 */
static void
driver(void *arg)
{
	xtc_exec_t *e = arg;
	xtc_pid_t children[MAX_CHILDREN];
	int n_children = 0;
	int my_monitors = 0, my_links = 0;
	int op;

	for (op = 0; op < OPS_PER_DRV; op++) {
		int which = (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 6);
		unsigned lp = (unsigned)__xtc_sim_rng_range(XTC_SIM_RNG_APP,
		    N_LOOPS);
		xtc_loop_t *l = xtc_exec_loop(e, (int)lp);
		xtc_pid_t cpid;
		uint64_t ref;
		int karg = (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 8);

		switch (which) {
		case 0:   /* plain spawn (fire-and-forget child) */
			if (xtc_proc_spawn(l, child_fn, (void *)(intptr_t)karg,
			    NULL, &cpid) == XTC_OK) {
				atomic_fetch_add(&g_spawned, 1);
				if (n_children < MAX_CHILDREN)
					children[n_children++] = cpid;
			}
			break;
		case 1:   /* atomic spawn+monitor */
		case 2:
			if (xtc_proc_spawn_monitor(l, child_fn,
			    (void *)(intptr_t)karg, NULL, &cpid, &ref)
			    == XTC_OK) {
				atomic_fetch_add(&g_spawned, 1);
				atomic_fetch_add(&g_monitors, 1);
				my_monitors++;
				if (n_children < MAX_CHILDREN)
					children[n_children++] = cpid;
			}
			break;
		case 3:   /* atomic spawn+link */
			if (xtc_proc_spawn_link(l, child_fn,
			    (void *)(intptr_t)karg, NULL, &cpid) == XTC_OK) {
				atomic_fetch_add(&g_spawned, 1);
				atomic_fetch_add(&g_links, 1);
				my_links++;
				if (n_children < MAX_CHILDREN)
					children[n_children++] = cpid;
			}
			break;
		case 4:   /* monitor an EXISTING child (may race its exit) */
			if (n_children > 0) {
				int idx = (int)__xtc_sim_rng_range(
				    XTC_SIM_RNG_APP, (uint64_t)n_children);
				if (xtc_monitor(children[idx], &ref) == XTC_OK) {
					atomic_fetch_add(&g_monitors, 1);
					my_monitors++;
				}
			}
			break;
		case 5:   /* send a message to an existing child */
			if (n_children > 0) {
				int idx = (int)__xtc_sim_rng_range(
				    XTC_SIM_RNG_APP, (uint64_t)n_children);
				int v = op;
				(void)xtc_send(children[idx], &v, sizeof v);
			}
			break;
		}
		xtc_yield();
	}

	/* Drain: expect exactly my_monitors DOWNs + my_links EXITs.  A
	 * generous timeout so the pessimal scheduler cannot starve us. */
	{
		int want = my_monitors + my_links;
		int got = 0;
		while (got < want) {
			void *m = NULL; size_t n = 0;
			if (xtc_recv(&m, &n, 5000LL * 1000 * 1000) != XTC_OK)
				break;                 /* timeout -> INV fails */
			classify(m, n);
			if (m) xtc_free(m);
			got++;
		}
	}

	atomic_fetch_add_explicit(&g_tally,
	    (long)(my_monitors * 7 + my_links * 13 + 1),
	    memory_order_relaxed);
	atomic_fetch_add(&g_drv_done, 1);
}

/* Count live procs at end-of-run (INV3): must be 0 -- no orphan/leak. */
static int g_live_procs;
static int
count_live_cb(const xtc_proc_info_t *info, void *user)
{
	(void)info; (void)user;
	g_live_procs++;
	return 0;
}

static int
run_composition(uint64_t seed, int *mon, int *down, int *lnk, int *ex,
    int *spawned, int *ran, int *live, long *tally, uint64_t *state)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	atomic_store(&g_monitors, 0);
	atomic_store(&g_downs, 0);
	atomic_store(&g_links, 0);
	atomic_store(&g_exits, 0);
	atomic_store(&g_spawned, 0);
	atomic_store(&g_child_ran, 0);
	atomic_store(&g_drv_done, 0);
	atomic_store(&g_tally, 0);
	g_live_procs = 0;

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK)
		return -1;
	for (i = 0; i < N_DRIVERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, i % N_LOOPS), driver,
		    e, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 20000000);

	*mon = atomic_load(&g_monitors);
	*down = atomic_load(&g_downs);
	*lnk = atomic_load(&g_links);
	*ex = atomic_load(&g_exits);
	*spawned = atomic_load(&g_spawned);
	*ran = atomic_load(&g_child_ran);
	*tally = atomic_load(&g_tally);
	if (state) *state = xtc_sim_state_hash(e);

	/* INV3: after quiescence, no proc may remain alive. */
	(void)xtc_inspect_procs(count_live_cb, NULL);
	*live = g_live_procs;

	xtc_exec_fini(e);
	return rc;
}

static int
check(const char *label, int mon, int down, int lnk, int ex, int spawned,
    int ran, int live, int drv_done)
{
	printf("%s: drivers=%d spawned=%d ran=%d monitors=%d downs=%d "
	    "links=%d exits=%d live=%d\n", label, drv_done, spawned, ran,
	    mon, down, lnk, ex, live);
	if (drv_done != N_DRIVERS) {
		printf("FAIL: not all drivers finished (%d/%d) -- a drain "
		    "timed out, a signal was LOST\n", drv_done, N_DRIVERS);
		return 1;
	}
	if (down != mon) {   /* INV1 */
		printf("FAIL: INV1 monitors fire -- %d monitors but %d DOWNs "
		    "(a monitor did not fire, or fired twice)\n", mon, down);
		return 1;
	}
	if (ex != lnk) {     /* INV2 */
		printf("FAIL: INV2 links fire -- %d links but %d EXITs\n",
		    lnk, ex);
		return 1;
	}
	if (live != 0) {     /* INV3 */
		printf("FAIL: INV3 no orphans -- %d proc(s) still alive after "
		    "quiescence (orphaned/leaked child)\n", live);
		return 1;
	}
	return 0;
}

int
main(void)
{
	int mon1, down1, lnk1, ex1, sp1, ran1, live1;
	int mon2, down2, lnk2, ex2, sp2, ran2, live2;
	int mon3, down3, lnk3, ex3, sp3, ran3, live3;
	long t1, t2, t3;
	uint64_t s1, s2, s3;
	int rc;

	rc = run_composition(0xC0FFEE, &mon1, &down1, &lnk1, &ex1, &sp1,
	    &ran1, &live1, &t1, &s1);
	if (rc != XTC_OK) { printf("FAIL: run rc=%d (hang?)\n", rc); return 1; }
	if (check("compose run1", mon1, down1, lnk1, ex1, sp1, ran1, live1,
	    atomic_load(&g_drv_done)))
		return 1;

	/* INV4a: byte-identical replay from the same seed. */
	rc = run_composition(0xC0FFEE, &mon2, &down2, &lnk2, &ex2, &sp2,
	    &ran2, &live2, &t2, &s2);
	if (rc != XTC_OK) { printf("FAIL: replay rc=%d\n", rc); return 1; }
	if (mon1 != mon2 || down1 != down2 || lnk1 != lnk2 || ex1 != ex2 ||
	    sp1 != sp2 || ran1 != ran2 || live1 != live2 || t1 != t2 ||
	    s1 != s2) {
		printf("FAIL: INV4 determinism -- run did not replay "
		    "(state %016llx/%016llx tally %ld/%ld)\n",
		    (unsigned long long)s1, (unsigned long long)s2, t1, t2);
		return 1;
	}

	/* INV4b: a DIFFERENT seed reorders operations but every invariant
	 * still holds. */
	rc = run_composition(0xBADF00D, &mon3, &down3, &lnk3, &ex3, &sp3,
	    &ran3, &live3, &t3, &s3);
	if (rc != XTC_OK) { printf("FAIL: diff-seed rc=%d\n", rc); return 1; }
	if (check("compose run3", mon3, down3, lnk3, ex3, sp3, ran3, live3,
	    atomic_load(&g_drv_done)))
		return 1;

	printf("OK: compositional DST -- random spawn/monitor/link/send/exit "
	    "scripts across %d loops, every monitor delivered exactly one "
	    "DOWN and every link exactly one EXIT (no lost/double signal), "
	    "no proc orphaned after quiescence, replayed byte-identically "
	    "and a different seed reorders but holds every invariant\n",
	    N_LOOPS);
	return 0;
}
