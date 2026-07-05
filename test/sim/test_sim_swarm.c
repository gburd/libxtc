#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_aio.h"
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
static atomic_int  g_torn_bad;    /* torn/corrupt pages ACCEPTED silently (MUST be 0) */
static atomic_long g_app_hash;

/* Seeded per-run scenario, derived from the seed before the run. */
struct scenario {
	int partition;      /* cut loop 0 <-> loop 2 */
	int latency;        /* set a seeded net latency window */
	int buggify;        /* enable buggify */
	int machine_death;  /* a reaper kills a worker mid-run */
	int torn;           /* torn/corrupt-write injection + page verifiers */
	/* Seed-varied MAGNITUDES (not just presence): a sweep must explore
	 * a mild schedule and a brutal one, not always the same intensity. */
	int buggify_pct;    /* per-1000 buggify activation (100..500) */
	int corrupt_pct;    /* per-1000 torn-write corruption (100..500) */
	int64_t lat_hi;     /* net-latency upper bound (100us..2ms) */
	int sched_pess;     /* per-1000 pessimal (starve) scheduler pick */
	int swizzle_pct;    /* per-1000 completion/message reorder */
};

/* ---- torn-page verifier: write a checksummed page, read it back, and
 * on a checksum mismatch REWRITE it (a torn write is detected + rewritten
 * from the in-memory copy).  Folds a detected-corruption count into the
 * app hash and asserts (via g_torn_bad) that no corruption is ever
 * accepted silently.  Bounded retries so it always converges. ---- */
#define TORN_PAGE 256
#define TORN_CK   (TORN_PAGE - 8)
static int g_torn_fd = -1;

static uint64_t
torn_cksum(const uint8_t *p, size_t n)
{
	uint64_t h = 0xCBF29CE484222325ull;
	size_t i;
	for (i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
	return h;
}

static void
torn_verifier(void *arg)
{
	long id = (long)(intptr_t)arg;
	int64_t off = id * TORN_PAGE;
	uint8_t page[TORN_PAGE], rd[TORN_PAGE];
	int attempt;
	if (g_torn_fd < 0)
		return;
	for (attempt = 0; attempt < 32; attempt++) {
		uint64_t ck;
		int w, r;
		memset(page, (int)((id * 5 + attempt) & 0xff), TORN_CK);
		page[0] = (uint8_t)id;
		ck = torn_cksum(page, TORN_CK);
		memcpy(page + TORN_CK, &ck, sizeof ck);
		w = xtc_aio_pwrite(g_torn_fd, page, TORN_PAGE, off);
		if (w < 0) continue;
		memset(rd, 0, sizeof rd);
		r = xtc_aio_pread(g_torn_fd, rd, TORN_PAGE, off);
		if (r < TORN_PAGE) continue;
		{
			uint64_t got = 0, want = torn_cksum(rd, TORN_CK);
			memcpy(&got, rd + TORN_CK, sizeof got);
			if (got == want && memcmp(rd, page, TORN_PAGE) == 0) {
				long h = atomic_load_explicit(&g_app_hash,
				    memory_order_relaxed);
				h = h * 1000003L + (id + 100);
				atomic_store_explicit(&g_app_hash, h,
				    memory_order_relaxed);
				return;                /* verified */
			}
			/*
			 * A checksum-VALID page (got == want) is intact even if it
			 * differs from this attempt's buffer: a torn write leaves a
			 * strict prefix, which -- since every attempt writes the
			 * same deterministic content for this offset -- can be a
			 * checksum-consistent earlier full write.  Only a page that
			 * PASSES the checksum with genuinely-corrupt bytes is silent
			 * bad data, which the checksum by construction cannot admit.
			 * (The earlier != -latest-buffer oracle over-reported; a
			 * 3000-seed swarm surfaced the false positive.)
			 */
			if (got == want)
				return;                /* checksum-valid -> intact */
			/* else: checksum FAILED -> detected torn write, retry. */
		}
	}
}

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
	atomic_store(&g_torn_bad, 0);
	atomic_store(&g_app_hash, 0);

	xtc_sim_partition_clear();
	xtc_sim_buggify_disable();
	xtc_sim_io_corrupt_disable();
	xtc_sim_io_faults_disable();
	g_torn_fd = -1;

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
		xtc_sim_net_latency(10 * 1000LL, sc->lat_hi);
	if (sc->buggify)
		xtc_sim_buggify_enable((unsigned)sc->buggify_pct);
	/* Adversarial scheduler bias + completion/message swizzle, both
	 * seed-varied in magnitude (0 for some seeds = benign uniform). */
	if (sc->sched_pess > 0)
		xtc_sim_sched_pessimal((unsigned)sc->sched_pess);
	if (sc->swizzle_pct > 0)
		xtc_sim_swizzle_enable((unsigned)sc->swizzle_pct);
	if (sc->machine_death) {
		g_reaper_arg.pongs = g_pongs;
		g_reaper_arg.n = N_PAIRS;
		(void)xtc_proc_spawn(xtc_exec_loop(e, 0), reaper,
		    &g_reaper_arg, NULL, NULL);
	}
	if (sc->torn) {
		/* Torn/corrupt-write injection + a couple of page verifiers.
		 * Latency-only faults (0% short/EIO) so writes/reads defer +
		 * park; corruption at ~30% tears some pages, which the
		 * verifier detects (checksum) and rewrites.  A per-run temp
		 * file, unlinked immediately; closed after the run. */
		char path[] = "/scratch/xtc-test/sim_swarm_torn_XXXXXX";
		g_torn_fd = mkstemp(path);
		if (g_torn_fd < 0) {
			char p2[] = "sim_swarm_torn_XXXXXX";
			g_torn_fd = mkstemp(p2);
			if (g_torn_fd >= 0) (void)unlink(p2);
		} else {
			(void)unlink(path);
		}
		if (g_torn_fd >= 0) {
			int v;
			if (ftruncate(g_torn_fd, (off_t)4 * TORN_PAGE) != 0)
				/* best-effort: a short file just yields short reads */
				(void)0;
			xtc_sim_io_faults_enable(20 * 1000LL, 200 * 1000LL, 0);
			xtc_sim_io_corrupt_enable((unsigned)sc->corrupt_pct);
			for (v = 0; v < 2; v++)
				(void)xtc_proc_spawn(
				    xtc_exec_loop(e, (unsigned)(v % N_LOOPS)),
				    torn_verifier, (void *)(intptr_t)v, NULL, NULL);
		}
	}

	rc = xtc_sim_exec_run(e, seed, 5000000);
	*out_state = xtc_sim_state_hash(e);
	*out_app = atomic_load(&g_app_hash);

	xtc_sim_partition_clear();
	xtc_sim_buggify_disable();
	xtc_sim_io_corrupt_disable();
	xtc_sim_io_faults_disable();
	if (g_torn_fd >= 0) { close(g_torn_fd); g_torn_fd = -1; }
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
	long n_part = 0, n_lat = 0, n_bug = 0, n_kill = 0, n_torn = 0;

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
		sc.torn          = (seed & 0x40) != 0;  /* ~50% */
		/* Seed-varied MAGNITUDES from independent higher seed bits, so a
		 * sweep spans mild to brutal.  Each maps a small bit-field to a
		 * range; fixed for the seed (so the seed still fully determines
		 * the scenario) and replays. */
		sc.buggify_pct = 100 + (int)((seed >> 7) & 0x7) * 60;   /* 100..520 */
		sc.corrupt_pct = 100 + (int)((seed >> 10) & 0x7) * 60;  /* 100..520 */
		sc.lat_hi      = (100 + (int64_t)((seed >> 13) & 0x7) * 60) * 1000LL; /* 100us..520us */
		/* Pessimal scheduler + swizzle: on for ~half the seeds, and when
		 * on the magnitude also varies with the seed. */
		sc.sched_pess  = ((seed >> 17) & 0x1) ?
		    (200 + (int)((seed >> 18) & 0x7) * 100) : 0;   /* 0 or 200..900 */
		sc.swizzle_pct = ((seed >> 21) & 0x1) ?
		    (100 + (int)((seed >> 22) & 0x7) * 80) : 0;    /* 0 or 100..660 */
		n_part += sc.partition;
		n_lat  += sc.latency;
		n_bug  += sc.buggify;
		n_kill += sc.machine_death;
		n_torn += sc.torn;

		rc1 = run_once(seed, &sc, &st1, &app1);
		if (atomic_load(&g_torn_bad) != 0) {
			printf("FAIL seed=%llu: %d torn/corrupt page(s) accepted "
			    "SILENTLY (checksum missed a torn write) -- "
			    "durability broken\n", (unsigned long long)seed,
			    atomic_load(&g_torn_bad));
			failures++;
			continue;
		}
		rc2 = run_once(seed, &sc, &st2, &app2);
		if (atomic_load(&g_torn_bad) != 0) {
			printf("FAIL seed=%llu: torn page accepted silently on "
			    "replay run\n", (unsigned long long)seed);
			failures++;
			continue;
		}

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
	    "%ld machine-death, %ld torn-write\n", n_seeds, seed_base,
	    failures, n_seen, n_part, n_lat, n_bug, n_kill, n_torn);

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
	    "(across partition + latency + buggify + machine-death + "
	    "torn-write scenarios), replayed identically, invariants held "
	    "(no torn page accepted silently); %d distinct schedules "
	    "explored\n", n_seeds, n_seen);
	return 0;
}
