/*
 * test_sim_spawn_rel -- Deterministic Simulation Testing of the ATOMIC
 * spawn+link / spawn+monitor APIs (xtc_proc_spawn_link,
 * xtc_proc_spawn_monitor), the Erlang spawn_link/spawn_monitor
 * primitives.
 *
 * The point of the atomic APIs is that the parent<->child relationship
 * is established BEFORE the child can run, so even a child that runs and
 * exits immediately still delivers its EXIT (link) or DOWN (monitor) to
 * the parent -- never the XTC_DOWN_NOPROC "monitor raced a dead target"
 * outcome that a spawn-then-relate idiom can hit.
 *
 * Under DST this is stressed hardest with the PESSIMAL scheduler
 * enabled: it can monopolize the child's loop so the child runs to
 * completion the instant it is enqueued, before the parent yields again.
 * With the atomic API the relationship is already in place, so:
 *   - every spawn_monitor child delivers a DOWN with the child's real
 *     exit reason (here 0 for a clean exit), NEVER XTC_DOWN_NOPROC;
 *   - every spawn_link child delivers an EXIT ('E') with the real
 *     reason.
 * Assertions per seed: all children ran, every parent collected the
 * expected number of signals with the correct reason, zero NOPROC, and
 * the whole run replays byte-identically (state hash + counts).
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
#include "xtc_sim.h"

#define N_LOOPS   4
#define N_PER_SUP 6           /* children each supervisor spawns */

static atomic_int g_spawned;
static atomic_int g_mon_ok;       /* monitor DOWN, reason == expected */
static atomic_int g_mon_noproc;   /* monitor DOWN == XTC_DOWN_NOPROC (MUST be 0) */
static atomic_int g_mon_bad;      /* monitor DOWN, unexpected reason */
static atomic_int g_link_ok;      /* link EXIT, reason == expected */
static atomic_int g_link_bad;     /* link EXIT, unexpected reason */
static atomic_int g_sups_done;

/* Child: yield a seeded number of times, then exit with a known reason.
 * reason 0 for the monitored children (clean), a nonzero code for the
 * linked children so the EXIT reason is checkable. */
struct child_arg { int reason; };

static void
child_fn(void *arg)
{
	struct child_arg *c = arg;
	int k = (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 4);   /* 0..3 yields */
	int i;
	atomic_fetch_add(&g_spawned, 1);
	for (i = 0; i < k; i++)
		xtc_yield();
	xtc_exit_self(c->reason);
}

/* Link EXIT signal layout: { kind='E', reason, pid }. */
struct exit_sig { uint8_t kind; int reason; xtc_pid_t pid; }
    __attribute__((packed));

struct sup_arg {
	xtc_exec_t *e;
	int loop;
	struct child_arg mon_c;   /* reason 0 */
	struct child_arg lnk_c;    /* reason 7 */
};

/*
 * Supervisor: on its own loop, atomically spawn+monitor N children and
 * atomically spawn+link N children, then drain and classify every
 * signal.  It expects 2*N_PER_SUP signals (a DOWN per monitored child,
 * an EXIT per linked child).
 */
static void
supervisor(void *arg)
{
	struct sup_arg *sa = arg;
	xtc_loop_t *l = xtc_exec_loop(sa->e, sa->loop);
	int i, seen = 0, tries;
	int want = 2 * N_PER_SUP;

	for (i = 0; i < N_PER_SUP; i++) {
		xtc_pid_t pid;
		uint64_t ref = 0;
		(void)xtc_proc_spawn_monitor(l, child_fn, &sa->mon_c, NULL,
		    &pid, &ref);
	}
	for (i = 0; i < N_PER_SUP; i++) {
		xtc_pid_t pid;
		(void)xtc_proc_spawn_link(l, child_fn, &sa->lnk_c, NULL, &pid);
	}

	for (tries = 0; tries < 40000 && seen < want; tries++) {
		void *m = NULL;
		size_t n = 0;
		if (xtc_recv(&m, &n, 2 * 1000 * 1000LL) == XTC_OK && m != NULL) {
			const uint8_t *k = m;
			if (n >= 1 && *k == 'D') {
				int reason = 0;
				xtc_pid_t dpid;
				if (xtc_down_decode(m, n, &dpid, &reason)
				    == XTC_OK) {
					seen++;
					if (xtc_down_is_noproc(reason))
						atomic_fetch_add(&g_mon_noproc, 1);
					else if (reason == 0)
						atomic_fetch_add(&g_mon_ok, 1);
					else
						atomic_fetch_add(&g_mon_bad, 1);
				}
			} else if (n >= sizeof(struct exit_sig) && *k == 'E') {
				const struct exit_sig *ex = m;
				seen++;
				if (ex->reason == 7)
					atomic_fetch_add(&g_link_ok, 1);
				else
					atomic_fetch_add(&g_link_bad, 1);
			}
			xtc_free(m);
		}
	}

	if (atomic_fetch_add(&g_sups_done, 1) + 1 >= N_LOOPS)
		(void)xtc_exec_stop(sa->e);
}

static int
run_one(uint64_t seed, int pessimal, int *out_mok, int *out_np,
    int *out_lok, int *out_bad, int *out_sp, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	struct sup_arg sa[N_LOOPS];
	int i, rc;

	atomic_store(&g_spawned, 0);
	atomic_store(&g_mon_ok, 0);
	atomic_store(&g_mon_noproc, 0);
	atomic_store(&g_mon_bad, 0);
	atomic_store(&g_link_ok, 0);
	atomic_store(&g_link_bad, 0);
	atomic_store(&g_sups_done, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK)
		return -1;
	xtc_exec_set_service_mode(e, 1);

	for (i = 0; i < N_LOOPS; i++) {
		sa[i].e = e;
		sa[i].loop = i;
		sa[i].mon_c.reason = 0;
		sa[i].lnk_c.reason = 7;
		(void)xtc_proc_spawn(xtc_exec_loop(e, i), supervisor, &sa[i],
		    NULL, NULL);
	}

	/* Stress the atomicity: force the child to often run to completion
	 * the instant it is enqueued (before the parent yields again). */
	if (pessimal)
		xtc_sim_sched_pessimal(700);

	rc = xtc_sim_exec_run(e, seed, 20000000);

	if (out_mok)   *out_mok = atomic_load(&g_mon_ok);
	if (out_np)    *out_np  = atomic_load(&g_mon_noproc);
	if (out_lok)   *out_lok = atomic_load(&g_link_ok);
	if (out_bad)   *out_bad = atomic_load(&g_mon_bad) +
	                          atomic_load(&g_link_bad);
	if (out_sp)    *out_sp  = atomic_load(&g_spawned);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0x73726c; /* "srl" */
	int n = 24, i, fails = 0;
	int want_sp = N_LOOPS * N_PER_SUP * 2;
	int want_mon = N_LOOPS * N_PER_SUP;
	int want_lnk = N_LOOPS * N_PER_SUP;

	if (argc > 1) base = strtoull(argv[1], NULL, 0);
	if (argc > 2) n = atoi(argv[2]);

	printf("== atomic spawn_link/spawn_monitor DST (pessimal "
	    "scheduler): %d seeds from base 0x%llx ==\n", n,
	    (unsigned long long)base);

	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		int mok = 0, np = 0, lok = 0, bad = 0, sp = 0;
		int mok2 = 0, np2 = 0, lok2 = 0, bad2 = 0, sp2 = 0;
		int rc, rc2, pass = 1;
		uint64_t st = 0, st2 = 0;

		rc = run_one(seed, 1, &mok, &np, &lok, &bad, &sp, &st);
		if (rc != XTC_OK) pass = 0;
		else if (sp != want_sp) pass = 0;      /* all children ran */
		else if (bad != 0) pass = 0;           /* every reason correct */
		/* The whole point: atomic relate => no NOPROC, ever. */
		else if (np != 0) pass = 0;
		else if (mok != want_mon) pass = 0;    /* every monitor DOWN */
		else if (lok != want_lnk) pass = 0;    /* every link EXIT */

		if (pass) {
			rc2 = run_one(seed, 1, &mok2, &np2, &lok2, &bad2, &sp2,
			    &st2);
			if (rc2 != rc || mok2 != mok || np2 != np ||
			    lok2 != lok || bad2 != bad || sp2 != sp ||
			    st2 != st)
				pass = 0;
		}

		if (!pass) {
			printf("  seed 0x%016llx: FAIL (spawned=%d mon_ok=%d "
			    "link_ok=%d noproc=%d bad=%d rc=%d)\n",
			    (unsigned long long)seed, sp, mok, lok, np, bad, rc);
			fails++;
		}
	}

	if (fails == 0) {
		printf("OK: atomic spawn_link/spawn_monitor DST -- %d seeds "
		    "under the pessimal scheduler, every spawn_monitor child "
		    "delivered a real-reason DOWN and every spawn_link child "
		    "an EXIT, zero NOPROC, all replay\n", n);
		return 0;
	}
	printf("FAIL: %d/%d spawn-rel seeds failed\n", fails, n);
	return 1;
}
