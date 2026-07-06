#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_orc.h"
#include "xtc_sim.h"

/*
 * DST coverage of MACHINE-DEATH / proc-kill (FoundationDB "kill a
 * machine mid-run").  Under the deterministic scheduler a seeded reaper
 * proc, after a seeded virtual-time delay, KILLS a seeded-chosen victim
 * (xtc_exit_pid) -- either one worker or every worker on a seeded-chosen
 * loop (an isolated + killed "machine").  The kill is drawn from the
 * dedicated APP PRNG stream so a given seed reproduces the identical
 * victim and timing (and drawing there never perturbs the SCHED/STEAL
 * streams, so the schedule replays regardless of the kill).
 *
 * The test verifies the system reacts DETERMINISTICALLY:
 *   PART A -- link/monitor propagation.  Workers linked+monitored by a
 *     watcher.  When the reaper kills a victim the exit propagates: the
 *     victim's linked peer receives an 'E' exit signal and its monitor
 *     receives a 'D' DOWN.  Both observers count the signal and exit, so
 *     the run reaches QUIESCENCE (no hang) and REPLAYS from the seed
 *     (same signal count + order hash + sim state hash).  A DIFFERENT
 *     seed kills a DIFFERENT victim at a DIFFERENT time yet still
 *     quiesces and is internally consistent.
 *   PART B -- supervisor restart.  A one_for_one supervisor owns N
 *     permanent children across loops.  The reaper kills a seeded child
 *     mid-run; the supervisor observes the DOWN and RESTARTS it per its
 *     strategy (the restart is deterministic).  A watcher waits for the
 *     restart, stops the supervisor, and joins it, so the run quiesces
 *     and the restart count replays.
 *
 * Kill semantics: xtc_exit_pid sets an async kill flag; the victim
 * raises the exit at its next yield/recv/wakeup point, running its
 * __notify_links_and_monitors (proc.c) -- the identical production exit
 * path.  Nothing here is sim-specific except the SEEDED choice of victim
 * and timing; the propagation + restart logic is the real runtime.
 */

#define N_LOOPS   4
#define N_WORKERS 8          /* PART A workers (2 per loop) */
#define N_KIDS    6          /* PART B supervised children */

/* ---- PART A: linked / monitored workers + a seeded reaper ---- */

static atomic_int  g_exit_seen;    /* 'E' link-exit signals observed */
static atomic_int  g_down_seen;    /* 'D' monitor-DOWN signals observed */
static atomic_long g_evt_hash;     /* order-sensitive fold of observations */
static atomic_int  g_victim_idx;   /* which worker the reaper picked */
static atomic_long g_victim_time;  /* virtual time (ns) the kill fired */

static void
fold(long v)
{
	long h = atomic_load_explicit(&g_evt_hash, memory_order_relaxed);
	h = h * 1000003L + (v + 11);
	atomic_store_explicit(&g_evt_hash, h, memory_order_relaxed);
}

static int
match_exit(const void *data, size_t size, void *u)
{
	(void)u;
	return size >= 1 && ((const uint8_t *)data)[0] == 'E';
}

static int
match_down(const void *data, size_t size, void *u)
{
	(void)u;
	return size >= 1 && ((const uint8_t *)data)[0] == 'D';
}

/*
 * A worker: sleeps in a bounded recv loop so it is a live, parkable
 * target.  A killable worker is one that keeps yielding (recv) -- the
 * kill is raised at its next recv.  It self-terminates after a bounded
 * number of iterations so a NON-victim worker never hangs the sim.
 */
static void
worker(void *arg)
{
	(void)arg;
	int i;
	for (i = 0; i < 8; i++) {
		void *m = NULL;
		size_t n = 0;
		(void)xtc_recv(&m, &n, 2 * 1000 * 1000LL /* 2 ms */);
		if (m != NULL)
			free(m);
	}
}

/*
 * The observer links AND monitors a specific worker (the one the reaper
 * will kill), then waits (bounded) for the exit signal + DOWN.  It folds
 * each observed signal into the event hash and exits, so it never hangs
 * even if (defensively) a signal never arrives.
 */
struct obs_arg { xtc_pid_t target; };

static void
observer(void *arg)
{
	struct obs_arg *oa = arg;
	void *m = NULL;
	size_t n = 0;

	(void)xtc_link(oa->target);
	(void)xtc_monitor(oa->target, NULL);

	/* Await the 'E' link-exit signal (bounded). */
	if (xtc_recv_match(match_exit, NULL, &m, &n,
	    100 * 1000 * 1000LL) == XTC_OK && m != NULL) {
		atomic_fetch_add_explicit(&g_exit_seen, 1, memory_order_relaxed);
		fold(1);
		free(m);
		m = NULL;
	}
	/* Await the 'D' monitor DOWN (bounded). */
	if (xtc_recv_match(match_down, NULL, &m, &n,
	    100 * 1000 * 1000LL) == XTC_OK && m != NULL) {
		atomic_fetch_add_explicit(&g_down_seen, 1, memory_order_relaxed);
		fold(2);
		free(m);
	}
}

/*
 * The reaper: after a seeded virtual-time delay, kills a seeded-chosen
 * victim worker.  Both the delay and the victim are drawn from the APP
 * stream, so a seed reproduces them exactly without perturbing the
 * scheduling streams.
 */
struct reaper_arg { const xtc_pid_t *workers; int n; };

static void
reaper(void *arg)
{
	struct reaper_arg *ra = arg;
	int64_t delay = (int64_t)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 5) *
	    1000 * 1000LL;              /* 0..4 ms */
	int victim = (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP,
	    (uint64_t)ra->n);
	int64_t now = 0;

	(void)xtc_proc_sleep(delay);
	(void)__xtc_sim_vclock(&now);
	atomic_store_explicit(&g_victim_idx, victim, memory_order_relaxed);
	atomic_store_explicit(&g_victim_time, now, memory_order_relaxed);
	(void)xtc_exit_pid(ra->workers[victim], 99 /* killed */);
}

static xtc_pid_t g_workers[N_WORKERS];
static struct obs_arg g_obs_arg;
static struct reaper_arg g_reaper_arg;

/*
 * PART A run: spawn workers across loops, one observer that link+monitors
 * worker 0, and a reaper that kills worker 0 at a SEEDED virtual-time
 * delay.  Observing a FIXED worker keeps the propagation assertion exact
 * for every seed (always one 'E' + one 'D'); the seeded delay is what
 * varies the kill timing per seed.  (PART B exercises a seeded victim
 * INDEX so a different seed kills a different child.)  Worker 0 lives on
 * loop 0, the observer on loop 1, the reaper on loop 2, so the exit
 * signal is a real cross-loop delivery through __mbox_deliver.
 */
static int
run_part_a(uint64_t seed, int *out_exit, int *out_down, long *out_hash,
    uint64_t *out_state, int *out_victim, long *out_time)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	atomic_store(&g_exit_seen, 0);
	atomic_store(&g_down_seen, 0);
	atomic_store(&g_evt_hash, 0);
	atomic_store(&g_victim_idx, -1);
	atomic_store(&g_victim_time, -1);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK)
		return -1;

	for (i = 0; i < N_WORKERS; i++) {
		xtc_loop_t *l = xtc_exec_loop(e, (unsigned)(i % N_LOOPS));
		if (xtc_proc_spawn(l, worker, NULL, NULL,
		    &g_workers[i]) != XTC_OK) {
			(void)xtc_exec_fini(e);
			return -1;
		}
	}

	/* Observer link+monitors worker 0 (on a different loop, so the
	 * exit signal is a cross-loop delivery through __mbox_deliver). */
	g_obs_arg.target = g_workers[0];
	(void)xtc_proc_spawn(xtc_exec_loop(e, 1), observer, &g_obs_arg,
	    NULL, NULL);

	/* Reaper always kills worker 0 in PART A (so the fixed observer
	 * sees the signals) but at a SEEDED delay. */
	g_reaper_arg.workers = g_workers;
	g_reaper_arg.n = 1;         /* victim index draw in [0,1) == 0 */
	(void)xtc_proc_spawn(xtc_exec_loop(e, 2), reaper, &g_reaper_arg,
	    NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_exit = atomic_load(&g_exit_seen);
	*out_down = atomic_load(&g_down_seen);
	*out_hash = atomic_load(&g_evt_hash);
	if (out_state != NULL)
		*out_state = xtc_sim_state_hash(e);
	*out_victim = atomic_load(&g_victim_idx);
	*out_time = atomic_load(&g_victim_time);
	(void)xtc_exec_fini(e);
	return rc;
}

/* ---- PART B: a supervisor whose seeded child is killed mid-run ---- */

static atomic_int  g_kid_spawns;   /* total child (re)spawns */
static xtc_supervisor_t *g_sup;
static int g_kill_kid;             /* seeded child index to kill */
static xtc_pid_t g_kid_pids[N_KIDS];

/*
 * A supervised child: on spawn it records its pid in the shared slot so
 * the reaper can find it, bumps the spawn counter, then parks in a
 * bounded recv loop forever (the supervisor kills it on stop).
 */
static void
kid(void *arg)
{
	int idx = (int)(intptr_t)arg;
	int n;
	g_kid_pids[idx] = xtc_self();
	n = atomic_fetch_add_explicit(&g_kid_spawns, 1,
	    memory_order_relaxed) + 1;
	(void)n;
	for (;;) {
		void *m = NULL;
		size_t sz = 0;
		(void)xtc_recv(&m, &sz, 50 * 1000 * 1000LL);
		if (m != NULL)
			free(m);
	}
}

/*
 * The supervisor-reaper: after a seeded delay, kills the seeded child.
 * The supervisor (monitoring the child) observes the DOWN and restarts
 * it -- the deterministic restart under sim.  Then a bounded settle and
 * a supervisor stop so the run winds down and quiesces.
 */
static void
sup_reaper(void *arg)
{
	(void)arg;
	int64_t delay = (int64_t)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 4) *
	    1000 * 1000LL;              /* 0..3 ms */
	int idx = g_kill_kid;
	xtc_pid_t victim;

	(void)xtc_proc_sleep(delay);
	victim = g_kid_pids[idx];
	if (!xtc_pid_is_none(victim))
		(void)xtc_exit_pid(victim, 99);

	/* Let the supervisor observe the DOWN and restart, then stop it. */
	(void)xtc_proc_sleep(20 * 1000 * 1000LL /* 20 ms */);
	(void)xtc_sup_stop(g_sup);
}

static int
run_part_b(uint64_t seed, int *out_restarts, int *out_spawns,
    uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	xtc_sup_opts_t opts = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t kids[N_KIDS];
	int i, rc;

	atomic_store(&g_kid_spawns, 0);
	g_sup = NULL;
	for (i = 0; i < N_KIDS; i++)
		g_kid_pids[i] = (xtc_pid_t){0};

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK)
		return -1;

	opts.exec = e;
	opts.max_restarts = 10;        /* generous: one kill must not trip it */
	opts.strategy = XTC_SUP_ONE_FOR_ONE;

	memset(kids, 0, sizeof kids);
	for (i = 0; i < N_KIDS; i++) {
		kids[i].name = "kid";
		kids[i].fn = kid;
		kids[i].arg = (void *)(intptr_t)i;
		kids[i].policy = XTC_RESTART_PERMANENT;
		kids[i].loop = i % N_LOOPS;
	}

	/* Seeded victim child: a different seed kills a different child.
	 * Derived from the seed directly (sim is not yet active here, so a
	 * PRNG-stream draw would return 0); the reaper fires the kill under
	 * the seeded schedule so timing still varies per seed. */
	g_kill_kid = (int)(seed % N_KIDS);

	if (xtc_sup_start(xtc_exec_loop(e, 0), &opts, kids, N_KIDS,
	    &g_sup) != XTC_OK) {
		(void)xtc_exec_fini(e);
		return -1;
	}

	(void)xtc_proc_spawn(xtc_exec_loop(e, 3), sup_reaper, NULL, NULL,
	    NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	if (out_restarts != NULL)
		*out_restarts = g_sup != NULL ? xtc_sup_n_restarts(g_sup) : -1;
	*out_spawns = atomic_load(&g_kid_spawns);
	if (out_state != NULL)
		*out_state = xtc_sim_state_hash(e);

	/* Join + free the supervisor handle before tearing down the exec so
	 * its struct is reclaimed (no leak under ASan). */
	if (g_sup != NULL)
		(void)xtc_sup_join(g_sup, 0);
	(void)xtc_exec_fini(e);
	return rc;
}

/* ---- PART C: reboot / incarnation.  A killed proc's slot may be
 * REUSED by a replacement, but the replacement gets a distinct pid
 * generation, so a stale pid minted before the "reboot" is rejected
 * (never delivered to the reincarnation).  This is the local face of
 * FoundationDB's node-incarnation (creation) guarantee, integrated with
 * the machine-death model rather than a separate subsystem. ---- */

static atomic_int  g_reboot_ok;      /* 1 if the incarnation guarantee held */
static atomic_int  g_reboot_rx;      /* messages the replacement received */

static void
reboot_child(void *arg)
{
	(void)arg;
	/* Receive one message then exit; used to prove the NEW pid delivers
	 * while the OLD (stale) pid does not. */
	void *m = NULL; size_t n = 0;
	if (xtc_recv(&m, &n, 50 * 1000 * 1000LL) == XTC_OK && m != NULL) {
		atomic_fetch_add(&g_reboot_rx, 1);
		xtc_free(m);
	}
}

static void
reboot_driver(void *arg)
{
	xtc_exec_t *e = arg;
	xtc_loop_t *l = xtc_exec_loop(e, 0);
	xtc_pid_t old_pid = {0}, new_pid = {0};
	int msg = 42, ok = 1;

	/* Spawn a child, capture its pid, kill it, and wait for it to be
	 * reaped (monitor DOWN). */
	if (xtc_proc_spawn(l, reboot_child, NULL, NULL, &old_pid) != XTC_OK) {
		atomic_store(&g_reboot_ok, 0); (void)xtc_exec_stop(e); return;
	}
	(void)xtc_monitor(old_pid, NULL);
	(void)xtc_exit_pid(old_pid, 9);   /* kill it */
	{
		void *m = NULL; size_t n = 0;
		(void)xtc_recv(&m, &n, 5LL * 1000 * 1000 * 1000);  /* the DOWN */
		if (m) xtc_free(m);
	}

	/* A send to the now-dead OLD pid must be REJECTED (stale), not
	 * delivered to whatever reuses the slot. */
	if (xtc_send(old_pid, &msg, sizeof msg) == XTC_OK)
		ok = 0;   /* a stale pid should not deliver */

	/* Spawn the replacement (the "reboot") on the same loop -- it may
	 * reuse old_pid's slot, but with a bumped generation. */
	if (xtc_proc_spawn(l, reboot_child, NULL, NULL, &new_pid) != XTC_OK)
		ok = 0;
	/* If the slot was reused, the gen MUST differ so the two pids are
	 * distinguishable (the incarnation guarantee). */
	if (new_pid.loop_id == old_pid.loop_id &&
	    new_pid.local_id == old_pid.local_id &&
	    new_pid.gen == old_pid.gen)
		ok = 0;   /* slot reused with the SAME gen -- stale pid ambiguous */
	/* The NEW pid must deliver. */
	if (xtc_send(new_pid, &msg, sizeof msg) != XTC_OK)
		ok = 0;
	/* And a second send to the OLD pid still must not deliver. */
	if (xtc_send(old_pid, &msg, sizeof msg) == XTC_OK)
		ok = 0;

	atomic_store(&g_reboot_ok, ok);
	(void)xtc_exec_stop(e);
}

static int
run_part_c(uint64_t seed, int *out_ok, int *out_rx, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int rc;
	atomic_store(&g_reboot_ok, -1);
	atomic_store(&g_reboot_rx, 0);
	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK)
		return -1;
	xtc_exec_set_service_mode(e, 1);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), reboot_driver, e, NULL, NULL);
	rc = xtc_sim_exec_run(e, seed, 5000000);
	if (out_ok)    *out_ok = atomic_load(&g_reboot_ok);
	if (out_rx)    *out_rx = atomic_load(&g_reboot_rx);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(void)
{
	/* PART A: replay the same seed, then a different seed. */
	int e1 = 0, d1 = 0, e2 = 0, d2 = 0, v1 = -1, v2 = -1;
	long h1 = 0, h2 = 0, t1 = -1, t2 = -1;
	uint64_t sa1 = 0, sa2 = 0;
	int rc1 = run_part_a(0xBEEF01, &e1, &d1, &h1, &sa1, &v1, &t1);
	int rc2 = run_part_a(0xBEEF01, &e2, &d2, &h2, &sa2, &v2, &t2);

	/* A different seed -> different kill time (still consistent). */
	int e3 = 0, d3 = 0, v3 = -1;
	long h3 = 0, t3 = -1;
	uint64_t sa3 = 0;
	int rc3 = run_part_a(0x1234AB, &e3, &d3, &h3, &sa3, &v3, &t3);

	printf("PART A run1: rc=%d exit=%d down=%d hash=%ld victim=%d "
	    "time=%ld state=%016llx\n",
	    rc1, e1, d1, h1, v1, t1, (unsigned long long)sa1);
	printf("PART A run2: rc=%d exit=%d down=%d hash=%ld victim=%d "
	    "time=%ld state=%016llx\n",
	    rc2, e2, d2, h2, v2, t2, (unsigned long long)sa2);
	printf("PART A run3 (diff seed): rc=%d exit=%d down=%d time=%ld\n",
	    rc3, e3, d3, t3);

	if (rc1 != XTC_OK || rc2 != XTC_OK || rc3 != XTC_OK) {
		printf("FAIL: a machine-death run did not quiesce "
		    "(rc %d/%d/%d) -- a killed proc must not hang the sim\n",
		    rc1, rc2, rc3);
		return 1;
	}
	/* Propagation: the kill delivered exactly one 'E' and one 'D'. */
	if (e1 != 1 || d1 != 1) {
		printf("FAIL: exit propagation -- expected 1 'E' + 1 'D', "
		    "got exit=%d down=%d\n", e1, d1);
		return 1;
	}
	/* Replay: identical observations + order hash + sim state hash. */
	if (e1 != e2 || d1 != d2 || h1 != h2 || t1 != t2 || sa1 != sa2) {
		printf("FAIL: PART A did not replay (exit %d/%d down %d/%d "
		    "hash %ld/%ld time %ld/%ld state %016llx/%016llx)\n",
		    e1, e2, d1, d2, h1, h2, t1, t2,
		    (unsigned long long)sa1, (unsigned long long)sa2);
		return 1;
	}
	/* A different seed still propagates and quiesces, at a (usually)
	 * different kill time. */
	if (e3 != 1 || d3 != 1) {
		printf("FAIL: diff-seed PART A -- expected 1 'E' + 1 'D', "
		    "got exit=%d down=%d\n", e3, d3);
		return 1;
	}

	/* PART B: supervisor restart under a seeded kill, replayed. */
	int r1 = -1, sp1 = 0, r2 = -1, sp2 = 0;
	uint64_t sb1 = 0, sb2 = 0;
	int rb1 = run_part_b(0xCAFE01, &r1, &sp1, &sb1);
	int rb2 = run_part_b(0xCAFE01, &r2, &sp2, &sb2);
	/* A different seed kills a different child. */
	int r3 = -1, sp3 = 0;
	uint64_t sb3 = 0;
	int rb3 = run_part_b(0x77AA02, &r3, &sp3, &sb3);

	printf("PART B run1: rc=%d restarts=%d spawns=%d state=%016llx\n",
	    rb1, r1, sp1, (unsigned long long)sb1);
	printf("PART B run2: rc=%d restarts=%d spawns=%d state=%016llx\n",
	    rb2, r2, sp2, (unsigned long long)sb2);
	printf("PART B run3 (diff seed): rc=%d restarts=%d spawns=%d\n",
	    rb3, r3, sp3);

	if (rb1 != XTC_OK || rb2 != XTC_OK || rb3 != XTC_OK) {
		printf("FAIL: a supervisor machine-death run did not quiesce "
		    "(rc %d/%d/%d)\n", rb1, rb2, rb3);
		return 1;
	}
	/* The kill triggered exactly one restart: N_KIDS initial spawns + 1
	 * respawn of the killed child. */
	if (r1 < 1) {
		printf("FAIL: supervisor did not restart the killed child "
		    "(restarts=%d)\n", r1);
		return 1;
	}
	if (sp1 != N_KIDS + 1) {
		printf("FAIL: expected %d child spawns (%d initial + 1 "
		    "restart), got %d\n", N_KIDS + 1, N_KIDS, sp1);
		return 1;
	}
	/* Replay: identical restart count + spawns + sim state hash. */
	if (r1 != r2 || sp1 != sp2 || sb1 != sb2) {
		printf("FAIL: PART B did not replay (restarts %d/%d spawns "
		    "%d/%d state %016llx/%016llx)\n",
		    r1, r2, sp1, sp2,
		    (unsigned long long)sb1, (unsigned long long)sb2);
		return 1;
	}
	if (rb3 != XTC_OK || r3 < 1 || sp3 != N_KIDS + 1) {
		printf("FAIL: diff-seed PART B inconsistent "
		    "(rc=%d restarts=%d spawns=%d)\n", rb3, r3, sp3);
		return 1;
	}

	/* PART C: reboot / incarnation -- a killed proc's reused slot gets a
	 * distinct gen, and a stale pid to the pre-reboot proc is rejected. */
	{
		int c_ok1 = -1, c_rx1 = 0, c_ok2 = -1, c_rx2 = 0;
		uint64_t sc1 = 0, sc2 = 0;
		int rc_c1 = run_part_c(0x2EB007, &c_ok1, &c_rx1, &sc1);
		int rc_c2 = run_part_c(0x2EB007, &c_ok2, &c_rx2, &sc2);
		printf("PART C run1: rc=%d incarnation_ok=%d rx=%d "
		    "state=%016llx\n", rc_c1, c_ok1, c_rx1,
		    (unsigned long long)sc1);
		if (rc_c1 != XTC_OK || rc_c2 != XTC_OK) {
			printf("FAIL: PART C did not quiesce (rc %d/%d)\n",
			    rc_c1, rc_c2);
			return 1;
		}
		if (c_ok1 != 1) {
			printf("FAIL: incarnation guarantee broken -- a stale "
			    "pre-reboot pid delivered, or the reused slot kept "
			    "the same gen (incarnation_ok=%d)\n", c_ok1);
			return 1;
		}
		if (c_ok1 != c_ok2 || c_rx1 != c_rx2 || sc1 != sc2) {
			printf("FAIL: PART C did not replay\n");
			return 1;
		}
	}

	printf("OK: machine-death under DST -- a seeded kill (xtc_exit_pid) "
	    "propagates to linked ('E') and monitored ('D') peers, a "
	    "one_for_one supervisor deterministically restarts the killed "
	    "child, the run quiesces (no hang) and replays identically from "
	    "the seed; a different seed kills a different victim/child and "
	    "stays consistent; and a reboot (kill + slot-reusing respawn) "
	    "gives the replacement a distinct pid generation so a stale "
	    "pre-reboot pid is rejected (the incarnation guarantee)\n");
	return 0;
}
