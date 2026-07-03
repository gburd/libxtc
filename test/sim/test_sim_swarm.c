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
#include "xtc_sim.h"

/*
 * DST SWARM / SOAK -- a large, shardable seed sweep over the RICH set of
 * fault scenarios the sim now models, extending test_sim_soak (which
 * sweeps a plain ping/pong + timer workload).  For each seed the swarm
 * runs a single mixed workload that combines, all under one seeded
 * schedule:
 *
 *   - cross-loop ping/pong pairs (mailbox park/wake across loops);
 *   - timer-driven sleepers (the virtual clock);
 *   - a SEEDED network condition: for some seeds a partition edge is cut
 *     and/or a seeded per-message delivery latency is set (the senders
 *     tolerate a dropped/delayed message with a bounded retry so a
 *     partition never hangs the run);
 *   - a SEEDED machine-death kill: for some seeds a reaper kills one
 *     ping/pong worker mid-run (xtc_exit_pid); the peer tolerates a lost
 *     reply with a bounded retry, so the killed proc never hangs the run;
 *   - Buggify (pessimal legal paths) enabled for some seeds.
 *
 * Every scenario knob is chosen from the seed itself (before the run) or
 * from the dedicated FAULT/APP streams (during the run), so a seed fully
 * determines the scenario AND the schedule, and each seed replays.
 *
 * Assertions per seed: the run reaches QUIESCENCE (no hang / livelock /
 * deadlock -- rc == XTC_OK), the per-step structural invariants hold
 * (xtc_sim_exec_run returns XTC_E_INTERNAL on violation), and the run
 * REPLAYS (two runs of the same seed produce the identical sim state
 * hash + application result).  Across the sweep the seeds must explore
 * many distinct schedules (the scheduler is seed-sensitive).
 *
 * Memory discipline: every run builds a fresh exec, spawns a BOUNDED set
 * of procs (N_LOOPS loops, a fixed pair/sleeper count), and frees all
 * per-run state (xtc_exec_fini, partition_clear, buggify_disable) before
 * the next seed -- so a large sweep stays memory-bounded (no growth
 * across seeds).
 *
 * Invocation:
 *   test_sim_swarm                 -- bounded default (a few hundred
 *                                     seeds) for make check / the CI job.
 *   test_sim_swarm <count>         -- sweep <count> seeds from base 0.
 *   test_sim_swarm <count> <base>  -- sweep <count> seeds from <base>
 *                                     (shard the seed space across
 *                                     parallel/nightly invocations).
 * A nightly 100k+ sweep: run several shards, e.g.
 *   test_sim_swarm 100000 0 & test_sim_swarm 100000 100000 & ...
 */

#define N_LOOPS    4
#define N_PAIRS    12       /* cross-loop ping/pong pairs (bounded) */
#define N_SLEEPERS 4        /* timer-driven procs (bounded) */
#define N_HOPS     3        /* round-trips per pair */

static atomic_int  g_replies;
static atomic_int  g_sleeps;
static atomic_int  g_killed;      /* 1 if the reaper fired this run */
static atomic_long g_app_hash;

/* Seeded per-run scenario, derived from the seed before the run. */
struct scenario {
	int partition;      /* cut loop 0 <-> loop 2 */
	int latency;        /* set a seeded net latency window */
	int buggify;        /* enable buggify */
	int machine_death;  /* a reaper kills a worker mid-run */
};

/* ---- ping/pong: a pong replies to N_HOPS pings; a ping does N_HOPS
 * round-trips.  Both tolerate a lost/dropped reply with a bounded,
 * clock-advancing retry so a partition / kill never hangs the run. ---- */
static void
pong(void *arg)
{
	(void)arg;
	int hops = N_HOPS;
	int idle = 0;
	while (hops > 0 && idle < 16) {
		void *m = NULL;
		size_t n = 0;
		xtc_pid_t from;
		if (xtc_recv(&m, &n, 3 * 1000 * 1000LL) != XTC_OK ||
		    m == NULL) {
			idle++;                /* timed out: bounded patience */
			continue;
		}
		memcpy(&from, m, sizeof from);
		free(m);
		int r = 1;
		(void)xtc_send(from, &r, sizeof r);  /* may AGAIN under cut */
		hops--;
	}
}

struct ping_arg { xtc_pid_t peer; long id; };
static struct ping_arg g_args[N_PAIRS];

static void
ping(void *arg)
{
	struct ping_arg *pa = arg;
	xtc_pid_t self = xtc_self();
	int hops = N_HOPS;
	int tries = 0;
	while (hops > 0 && tries < 24) {
		void *m = NULL;
		size_t n = 0;
		long h;
		int rc;
		tries++;
		rc = xtc_send(pa->peer, &self, sizeof self);
		if (rc == XTC_E_AGAIN) {
			/* dropped (partition) or soft-full: back off + retry */
			(void)xtc_proc_sleep(1 * 1000 * 1000LL);
			continue;
		}
		if (rc != XTC_OK)
			return;                /* peer gone (killed): give up */
		if (xtc_recv(&m, &n, 3 * 1000 * 1000LL) != XTC_OK ||
		    m == NULL)
			continue;              /* no reply (killed peer): retry */
		free(m);
		atomic_fetch_add_explicit(&g_replies, 1, memory_order_relaxed);
		h = atomic_load_explicit(&g_app_hash, memory_order_relaxed);
		h = h * 1000003L + (pa->id + 1);
		atomic_store_explicit(&g_app_hash, h, memory_order_relaxed);
		hops--;
	}
}

static void
sleeper(void *arg)
{
	long id = (long)(intptr_t)arg;
	int i;
	for (i = 0; i < 3; i++)
		(void)xtc_proc_sleep((int64_t)(id % 5 + 1) * 1000000LL);
	atomic_fetch_add_explicit(&g_sleeps, 1, memory_order_relaxed);
}

/* The reaper: kills a seeded pong proc mid-run.  The victim pid is
 * captured before the run; the delay is drawn from the APP stream. */
struct reaper_arg { const xtc_pid_t *pongs; int n; };
static struct reaper_arg g_reaper_arg;

static void
reaper(void *arg)
{
	struct reaper_arg *ra = arg;
	int64_t delay = (int64_t)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 4) *
	    1000 * 1000LL;
	int victim = (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, (uint64_t)ra->n);
	(void)xtc_proc_sleep(delay);
	if (!xtc_pid_is_none(ra->pongs[victim])) {
		(void)xtc_exit_pid(ra->pongs[victim], 99);
		atomic_store_explicit(&g_killed, 1, memory_order_relaxed);
	}
}

static xtc_pid_t g_pongs[N_PAIRS];

/* Build + run the workload once with `seed` under scenario `sc`. */
static int
run_once(uint64_t seed, const struct scenario *sc, uint64_t *out_state,
    long *out_app)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	atomic_store(&g_replies, 0);
	atomic_store(&g_sleeps, 0);
	atomic_store(&g_killed, 0);
	atomic_store(&g_app_hash, 0);

	xtc_sim_partition_clear();
	xtc_sim_buggify_disable();

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK)
		return -1;

	for (i = 0; i < N_PAIRS; i++) {
		xtc_loop_t *lp = xtc_exec_loop(e, (unsigned)(i % N_LOOPS));
		xtc_loop_t *li = xtc_exec_loop(e, (unsigned)((i + 1) % N_LOOPS));
		(void)xtc_proc_spawn(lp, pong, NULL, NULL, &g_pongs[i]);
		g_args[i].peer = g_pongs[i];
		g_args[i].id = i;
		(void)xtc_proc_spawn(li, ping, &g_args[i], NULL, NULL);
	}
	for (i = 0; i < N_SLEEPERS; i++) {
		xtc_loop_t *l = xtc_exec_loop(e, (unsigned)(i % N_LOOPS));
		(void)xtc_proc_spawn(l, sleeper, (void *)(intptr_t)i, NULL, NULL);
	}

	/* Seeded scenario knobs, installed before the run advances. */
	if (sc->partition) {
		/* Cut loop 0 <-> loop 2 (loop_id = exec_id + 1). */
		xtc_sim_partition_set(1, 3, 1);
		xtc_sim_partition_set(3, 1, 1);
	}
	if (sc->latency)
		xtc_sim_net_latency(10 * 1000LL, 300 * 1000LL);
	if (sc->buggify)
		xtc_sim_buggify_enable(300);   /* 30% per site */
	if (sc->machine_death) {
		g_reaper_arg.pongs = g_pongs;
		g_reaper_arg.n = N_PAIRS;
		(void)xtc_proc_spawn(xtc_exec_loop(e, 0), reaper,
		    &g_reaper_arg, NULL, NULL);
	}

	rc = xtc_sim_exec_run(e, seed, 5000000);
	*out_state = xtc_sim_state_hash(e);
	*out_app = atomic_load(&g_app_hash);

	xtc_sim_partition_clear();
	xtc_sim_buggify_disable();
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(int argc, char **argv)
{
	long n_seeds = (argc > 1) ? strtol(argv[1], NULL, 10) : 300;
	long seed_base = (argc > 2) ? strtol(argv[2], NULL, 10) : 0;
	long s;
	uint64_t seen[256];
	int n_seen = 0;
	long failures = 0;
	long n_part = 0, n_lat = 0, n_bug = 0, n_kill = 0;

	if (n_seeds < 1)
		n_seeds = 1;

	for (s = 0; s < n_seeds; s++) {
		uint64_t seed = 0x9E3779B97F4A7C15ull *
		    (uint64_t)(seed_base + s + 1);
		struct scenario sc;
		uint64_t st1 = 0, st2 = 0;
		long app1 = 0, app2 = 0;
		int rc1, rc2, i;

		/* Derive the scenario from the seed (independent of the PRNG
		 * streams so it is fixed for the seed regardless of the
		 * schedule). */
		sc.partition     = (seed & 0x3) == 0;   /* ~25% */
		sc.latency       = (seed & 0x4) != 0;   /* ~50% */
		sc.buggify       = (seed & 0x8) != 0;   /* ~50% */
		sc.machine_death = (seed & 0x30) == 0;  /* ~25% */
		n_part += sc.partition;
		n_lat  += sc.latency;
		n_bug  += sc.buggify;
		n_kill += sc.machine_death;

		rc1 = run_once(seed, &sc, &st1, &app1);
		rc2 = run_once(seed, &sc, &st2, &app2);

		if (rc1 != XTC_OK || rc2 != XTC_OK) {
			printf("FAIL seed=%llu: rc1=%d rc2=%d (part=%d lat=%d "
			    "bug=%d kill=%d) -- no quiescence / invariant "
			    "violation\n", (unsigned long long)seed, rc1, rc2,
			    sc.partition, sc.latency, sc.buggify,
			    sc.machine_death);
			failures++;
			continue;
		}
		if (st1 != st2 || app1 != app2) {
			printf("FAIL seed=%llu: replay mismatch (state "
			    "%016llx/%016llx app %ld/%ld) part=%d lat=%d "
			    "bug=%d kill=%d\n", (unsigned long long)seed,
			    (unsigned long long)st1, (unsigned long long)st2,
			    app1, app2, sc.partition, sc.latency, sc.buggify,
			    sc.machine_death);
			failures++;
			continue;
		}
		for (i = 0; i < n_seen; i++)
			if (seen[i] == st1)
				break;
		if (i == n_seen && n_seen < (int)(sizeof seen / sizeof seen[0]))
			seen[n_seen++] = st1;
	}

	printf("swarm swept %ld seeds (base %ld): %ld failures, %d distinct "
	    "schedules; scenarios: %ld partition, %ld latency, %ld buggify, "
	    "%ld machine-death\n", n_seeds, seed_base, failures, n_seen,
	    n_part, n_lat, n_bug, n_kill);

	if (failures > 0) {
		printf("FAIL: %ld seed(s) failed\n", failures);
		return 1;
	}
	if (n_seeds >= 20 && n_seen < 2) {
		printf("FAIL: the swarm explored only one schedule -- the "
		    "scheduler is not seed-sensitive\n");
		return 1;
	}
	printf("OK: %ld-seed swarm/soak -- every seed reached quiescence "
	    "(across partition + latency + buggify + machine-death "
	    "scenarios), replayed identically, invariants held; %d distinct "
	    "schedules explored\n", n_seeds, n_seen);
	return 0;
}
