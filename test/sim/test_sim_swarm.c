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
/*
 * Bounded verifier retries.  This budget is a PROBABILITY argument, not a
 * round number, because corruption is injected on the READ path as well
 * as the write path (src/io/io_sim.c: the torn/ENOSPC write model AND
 * __xtc_sim_io_flip_byte on every pread).  So an attempt survives only if
 * the write is untorn AND the read-back is not flipped: at this sweep's
 * top corruption bucket (520 per 1000) that is
 *     P(attempt fails) = 1 - (1 - 0.52)^2 = 0.77
 * and a rewrite cannot "escape" a read-path flip, which is why the
 * verifier does not converge by retrying harder in kind.
 *
 * At 32 tries P(all fail) = 2.3e-4, and a 100k-seed sweep runs ~6,250
 * verifiers in that bucket, so ~2.9 spurious failures were EXPECTED per
 * 100k -- and a 4-shard 100k run measured exactly 3 (seeds
 * 571488401344257137, 12320922477591755847, and one more), every one
 * reproducible from its seed.  That was the harness, not a libxtc
 * durability defect: the oracle itself is correct and stays strict.
 * At 128 the same arithmetic gives ~3e-11 per 100k (about 1e-8 per
 * year of nightly sweeps), which is rare enough not to cry wolf while
 * still failing instantly on a verifier that genuinely cannot converge.
 */
#define TORN_TRIES 128

/*
 * WHERE THE PARTITION CUT GOES -- and why it must be an edge that
 * carries workload traffic.
 *
 * Pair i places its pong on loop INDEX (i % N_LOOPS) and its ping on
 * loop INDEX ((i + 1) % N_LOOPS), and both procs are PINNED (opts ==
 * NULL), so the only communicating loop-index pairs in this workload are
 * the NEIGHBOURS: k+1 -> k carries the ping, k -> k+1 carries the reply.
 * A cut between NON-adjacent indices (the old 0 <-> 2) severs an edge no
 * message ever crosses, so every "partition" seed silently ran an
 * UNPARTITIONED workload.  Cut a neighbour edge instead.
 *
 * partition_set is keyed by pid.loop_id == loop index + 1, hence the +1.
 * PAIR_IS_CUT(i) names the pairs the cut severs -- pong on CUT_LOOP_A,
 * ping on CUT_LOOP_B.  With N_LOOPS == 4 that is i in {0, 4, 8}: THREE
 * pairs, and the reaper can kill at most ONE pong, so at least two cut
 * pairs always attempt a send and always see it dropped.  Both
 * directions are blocked, so a cut pair completes ZERO round-trips.
 */
#define CUT_LOOP_A      0
#define CUT_LOOP_B      1
#define PAIR_IS_CUT(i)  (((i) % N_LOOPS) == CUT_LOOP_A && \
	((((i) + 1) % N_LOOPS) == CUT_LOOP_B))

static atomic_int  g_replies;
static atomic_int  g_pair_replies[N_PAIRS];   /* round-trips, PER PAIR */
static atomic_int  g_pair_drops[N_PAIRS];     /* sends DROPPED, per pair */
static atomic_int  g_sleeps;
static atomic_int  g_killed;      /* 1 if the reaper fired this run */
static atomic_int  g_kill_victim; /* pair index the reaper killed, else -1 */
static atomic_int  g_torn_bad;    /* torn/corrupt pages ACCEPTED silently (MUST be 0) */
static atomic_int  g_torn_detected;   /* tears the checksum CAUGHT */
static atomic_int  g_torn_verified;   /* verifiers that converged */
static atomic_int  g_torn_stuck;      /* verifiers that EXHAUSTED retries */
static atomic_int  g_torn_skipped;    /* torn workload NOT run (no temp file) */
static atomic_long g_app_hash;

/* Seeded per-run scenario, derived from the seed before the run. */
struct scenario {
	int partition;      /* cut loop index CUT_LOOP_A <-> CUT_LOOP_B */
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

/*
 * The EXACT page image this verifier writes for (id, attempt).  Every
 * attempt's content is a pure function of (id, attempt), so the set of
 * images that can legitimately be on disk at this offset is the finite,
 * enumerable set { torn_page_image(id, a) : a in [0, TORN_TRIES) } plus
 * the initial all-zero page (which fails the checksum).  That is what
 * makes silent corruption DETECTABLE rather than assumed-impossible.
 */
static void
torn_page_image(uint8_t *p, long id, int attempt)
{
	uint64_t ck;
	memset(p, (int)((id * 5 + attempt) & 0xff), TORN_CK);
	p[0] = (uint8_t)id;
	ck = torn_cksum(p, TORN_CK);
	memcpy(p + TORN_CK, &ck, sizeof ck);
}

/* 1 if `rd` is one of the images this verifier could legitimately have
 * persisted at this offset; 0 means the bytes came from nowhere legal. */
static int
torn_page_legal(const uint8_t *rd, long id)
{
	uint8_t img[TORN_PAGE];
	int a;
	for (a = 0; a < TORN_TRIES; a++) {
		torn_page_image(img, id, a);
		if (memcmp(rd, img, TORN_PAGE) == 0)
			return 1;
	}
	return 0;
}

static void
torn_verifier(void *arg)
{
	long id = (long)(intptr_t)arg;
	int64_t off = id * TORN_PAGE;
	uint8_t page[TORN_PAGE], rd[TORN_PAGE];
	int attempt;
	if (g_torn_fd < 0) {
		/* Not reachable: run_once counts the skip before spawning. */
		atomic_fetch_add_explicit(&g_torn_skipped, 1,
		    memory_order_relaxed);
		return;
	}
	for (attempt = 0; attempt < TORN_TRIES; attempt++) {
		uint64_t ck = 0, got = 0, want;
		int w, r;
		torn_page_image(page, id, attempt);
		memcpy(&ck, page + TORN_CK, sizeof ck);
		w = xtc_aio_pwrite(g_torn_fd, page, TORN_PAGE, off);
		if (w < 0) continue;
		memset(rd, 0, sizeof rd);
		r = xtc_aio_pread(g_torn_fd, rd, TORN_PAGE, off);
		if (r < TORN_PAGE) continue;
		want = torn_cksum(rd, TORN_CK);
		memcpy(&got, rd + TORN_CK, sizeof got);
		if (got != want) {
			/* Checksum REJECTED the page: a torn write (or a
			 * corrupt read) was DETECTED.  Rewrite and retry. */
			atomic_fetch_add_explicit(&g_torn_detected, 1,
			    memory_order_relaxed);
			continue;
		}
		/*
		 * The checksum ACCEPTED this page.  That is only sound if the
		 * bytes are an image this verifier actually wrote: a
		 * checksum-valid page whose content is in the legal set is
		 * intact (possibly an EARLIER full write -- a 1-byte torn
		 * prefix reproduces the previous image byte-for-byte, since
		 * byte 0 is `id` in every attempt).  A checksum-valid page
		 * that is NOT in the legal set is CORRUPTION THE CHECKSUM
		 * ACCEPTED -- silent bad data, the durability violation this
		 * workload exists to find.  Recording it is what makes the
		 * oracle able to fail at all (g_torn_bad was previously never
		 * incremented, so the advertised check was vacuous).
		 */
		if (!torn_page_legal(rd, id)) {
			atomic_fetch_add_explicit(&g_torn_bad, 1,
			    memory_order_relaxed);
			return;
		}
		if (memcmp(rd, page, TORN_PAGE) != 0) {
			/* Legal, but an earlier image: this attempt's write was
			 * torn back to a prior consistent page -- detected as
			 * "not what I just wrote", and safe. */
			atomic_fetch_add_explicit(&g_torn_detected, 1,
			    memory_order_relaxed);
		}
		atomic_fetch_add_explicit(&g_torn_verified, 1,
		    memory_order_relaxed);
		{
			long h = atomic_load_explicit(&g_app_hash,
			    memory_order_relaxed);
			h = (long)((unsigned long)h * 1000003UL +
		    (unsigned long)(id + 100));
			atomic_store_explicit(&g_app_hash, h,
			    memory_order_relaxed);
		}
		return;                        /* verified */
	}
	/*
	 * Retries EXHAUSTED without ever reading back a checksum-valid page.
	 * The old loop simply fell out here and the run still "passed": a
	 * page that never converges is a page whose durability was never
	 * established, so record it and let the sweep FAIL on it.
	 */
	atomic_fetch_add_explicit(&g_torn_stuck, 1, memory_order_relaxed);
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
			/* dropped (partition) or soft-full: back off + retry.
			 * Counted PER PAIR: this is the observable that proves a
			 * partition actually cut this pair's traffic. */
			atomic_fetch_add_explicit(&g_pair_drops[pa->id], 1,
			    memory_order_relaxed);
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
		atomic_fetch_add_explicit(&g_pair_replies[pa->id], 1,
		    memory_order_relaxed);
		h = atomic_load_explicit(&g_app_hash, memory_order_relaxed);
		h = (long)((unsigned long)h * 1000003UL +
		    (unsigned long)(pa->id + 1));
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
		/* Which pair lost its pong: the partition oracle must not
		 * demand drops from a pair whose peer is simply gone. */
		atomic_store_explicit(&g_kill_victim, victim,
		    memory_order_relaxed);
	}
}

static xtc_pid_t g_pongs[N_PAIRS];

/*
 * The buggify sites this workload can reach.  A site that stays 0 across
 * a whole sweep is unreachable from here (dead code or a workload gap).
 * File scope because the ACTIVATION COUNTS must be collected INSIDE
 * run_once -- see collect_buggify_cov.
 */
static const char *const known_sites[] = {
	"proc.mbox.spurious_full", "chan.mpsc.spurious_full",
	"chan.mpmc.spurious_full", "sync.sem.spurious_timeout",
	"sched.steal.skip_near", "timer.fire.late",
	"sched.inbox.drain_one_fewer", "sched.runq.defer_ready",
	"io.aio.slow_completion", "lock.grant.skip_head",
	"svr.recv.delay_dispatch", "reg.whereis.transient_miss",
	"wal.flush.tiny_batch", "btree.split.eager"
};
#define N_KNOWN ((int)(sizeof known_sites / sizeof known_sites[0]))

/*
 * Fold this run's ACTIVATED buggify sites into `cov` (per-site seed
 * counts); return this run's activation count.
 *
 * MUST be called BEFORE any xtc_sim_buggify_disable: disable RESETS the
 * decision table (src/evt/sim.c), so querying after per-run cleanup --
 * which is what main used to do -- always read an EMPTY table, making
 * the whole fault-activation report silently vacuous.
 */
static int
collect_buggify_cov(long *cov)
{
	int nr = xtc_sim_buggify_reached_count();
	int bi, n_act = 0;

	for (bi = 0; bi < nr; bi++) {
		char nm[48];
		int act = 0, ki;
		if (xtc_sim_buggify_site(bi, nm, sizeof nm, &act) != XTC_OK)
			continue;
		if (!act)
			continue;
		n_act++;
		for (ki = 0; ki < N_KNOWN; ki++)
			if (strcmp(nm, known_sites[ki]) == 0) {
				if (cov != NULL)
					cov[ki]++;
				break;
			}
	}
	return n_act;
}

/* Build + run the workload once with `seed` under scenario `sc`.  When
 * `cov` is non-NULL this run's activated buggify sites are folded into it
 * (collected BEFORE cleanup); *out_act gets this run's activation
 * count. */
static int
run_once(uint64_t seed, const struct scenario *sc, uint64_t *out_state,
    long *out_app, long *cov, int *out_act)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	atomic_store(&g_replies, 0);
	atomic_store(&g_sleeps, 0);
	atomic_store(&g_killed, 0);
	atomic_store(&g_kill_victim, -1);
	atomic_store(&g_torn_bad, 0);
	atomic_store(&g_torn_detected, 0);
	atomic_store(&g_torn_verified, 0);
	atomic_store(&g_torn_stuck, 0);
	atomic_store(&g_torn_skipped, 0);
	atomic_store(&g_app_hash, 0);
	for (i = 0; i < N_PAIRS; i++) {
		atomic_store(&g_pair_replies[i], 0);
		atomic_store(&g_pair_drops[i], 0);
	}

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
		/* Cut a NEIGHBOUR edge -- the only kind this workload's pinned
		 * ping/pong pairs actually send across.  loop_id == index + 1. */
		xtc_sim_partition_set(CUT_LOOP_A + 1, CUT_LOOP_B + 1, 1);
		xtc_sim_partition_set(CUT_LOOP_B + 1, CUT_LOOP_A + 1, 1);
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
		 * park; seeded corruption tears some pages, which the verifier
		 * detects (checksum) and rewrites.  A per-run temp file,
		 * unlinked immediately; closed after the run. */
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
		} else {
			/*
			 * No temp storage: the torn workload does NOT run.  Record
			 * it so the sweep reports (and fails on) a scenario it
			 * ADVERTISED but did not execute.  Silently running a
			 * smaller workload while still printing "torn-write
			 * scenarios" is exactly how a check rots into a claim.
			 */
			atomic_store_explicit(&g_torn_skipped, 1,
			    memory_order_relaxed);
		}
	}

	rc = xtc_sim_exec_run(e, seed, 5000000);
	*out_state = xtc_sim_state_hash(e);
	*out_app = atomic_load(&g_app_hash);

	/*
	 * Collect the fault-activation report BEFORE any cleanup:
	 * xtc_sim_buggify_disable() below RESETS the decision table, so a
	 * post-cleanup query (what main used to do) always reads an empty
	 * table and reports zero activations for every seed.
	 */
	{
		int act = collect_buggify_cov(cov);
		if (out_act != NULL)
			*out_act = act;
	}

	xtc_sim_partition_clear();
	xtc_sim_buggify_disable();
	xtc_sim_io_corrupt_disable();
	xtc_sim_io_faults_disable();
	if (g_torn_fd >= 0) { close(g_torn_fd); g_torn_fd = -1; }
	(void)xtc_exec_fini(e);
	return rc;
}

/*
 * GATE THRESHOLDS.  Both gates need enough seeds to be statistically
 * meaningful, so they only apply to a real sweep (the corpus runner and
 * ad-hoc 1-seed invocations stay usable).
 */
#define MIN_GATE_SEEDS 20   /* per-scenario seeds before a gate applies */
#define MIN_ACT_SITES   3   /* distinct buggify sites a sweep must activate */

int
main(int argc, char **argv)
{
	long n_seeds = (argc > 1) ? strtol(argv[1], NULL, 10) : 300;
	long seed_base = (argc > 2) ? strtol(argv[2], NULL, 10) : 0;
	long s;
	uint64_t seen[256];
	int n_seen = 0;
	long failures = 0;
	/* Failing base offsets, for the paste-ready corpus rows printed at
	 * the end (bounded; a sweep with more failures than this has bigger
	 * problems than an incomplete list). */
	long fail_off[64];
	long n_fail_off = 0;
	long fi;                        /* index over fail_off, not the sweep */
/*
 * Record this seed's base offset once, for the paste-ready corpus rows.
 * Guarded so several failing invariants on ONE seed pin one row, not six.
 */
#define NOTE_FAIL_OFFSET()                                            \
	do {                                                          \
		if (n_fail_off < (long)(sizeof fail_off /              \
		    sizeof fail_off[0]) &&                             \
		    (n_fail_off == 0 ||                                \
		     fail_off[n_fail_off - 1] != seed_base + s))       \
			fail_off[n_fail_off++] = seed_base + s;        \
	} while (0)
	long n_part = 0, n_lat = 0, n_bug = 0, n_kill = 0, n_torn = 0;
	/* Fault-space coverage (FoundationDB-style): how many seeds ACTIVATED
	 * each known buggify site, plus the sweep totals the MINIMUM
	 * ACTIVATION GATE below is measured against. */
	long cov_activated[N_KNOWN] = {0};
	long tot_activations = 0;       /* activated sites, summed over runs */
	long seeds_with_activation = 0;
	/* Partition oracle observables. */
	long part_cut_drops = 0;        /* drops on the CUT edge (part. seeds) */
	long part_seeds_with_drop = 0;
	long ctl_seeds_cut_traffic = 0; /* NON-partition seeds whose cut-edge
	                                 * pairs DID complete round-trips: the
	                                 * control proving the edge carries
	                                 * workload traffic at all */
	/* Torn-write oracle observables. */
	long torn_detected = 0, torn_verified = 0, torn_skipped = 0;
	const int n_cut = N_PAIRS / N_LOOPS;   /* cut pairs: i in {0,4,8} */

	if (n_seeds < 1)
		n_seeds = 1;

	for (s = 0; s < n_seeds; s++) {
		uint64_t seed = 0x9E3779B97F4A7C15ull *
		    (uint64_t)(seed_base + s + 1);
		struct scenario sc;
		uint64_t st1 = 0, st2 = 0;
		long app1 = 0, app2 = 0;
		int rc1, rc2, i, act1 = 0, act2 = 0;
		int cut_drop_pairs = 0, cut_reply_pairs = 0, cut_drops = 0;
		int victim, victim_is_cut;

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

		/* Fault-space coverage is collected INSIDE run_once, BEFORE its
		 * cleanup: xtc_sim_buggify_disable resets the decision table, so
		 * the old post-run query here always read an empty table and the
		 * whole activation report was vacuous. */
		rc1 = run_once(seed, &sc, &st1, &app1, cov_activated, &act1);
		tot_activations += act1;
		if (act1 > 0)
			seeds_with_activation++;
		torn_detected += atomic_load(&g_torn_detected);
		torn_verified += atomic_load(&g_torn_verified);
		torn_skipped  += atomic_load(&g_torn_skipped);

		/* ---- silent-corruption oracle.  Previously vacuous:
		 * g_torn_bad was never incremented anywhere, so this branch
		 * could not be taken by any input. ---- */
		if (atomic_load(&g_torn_bad) != 0) {
			printf("FAIL seed=%llu: %d torn/corrupt page(s) accepted "
			    "SILENTLY (a checksum-VALID page held bytes no "
			    "writer ever wrote) -- durability broken\n",
			    (unsigned long long)seed,
			    atomic_load(&g_torn_bad));
			failures++; NOTE_FAIL_OFFSET();
			continue;
		}
		/* A verifier that burned every retry never established its
		 * page's durability; the old loop just fell out and passed. */
		if (atomic_load(&g_torn_stuck) != 0) {
			printf("FAIL seed=%llu: %d torn-page verifier(s) "
			    "EXHAUSTED %d retries without a checksum-valid "
			    "read-back -- durability never established\n",
			    (unsigned long long)seed,
			    atomic_load(&g_torn_stuck), TORN_TRIES);
			failures++; NOTE_FAIL_OFFSET();
			continue;
		}
		/* A workload that did not run must be VISIBLE, not absent. */
		if (atomic_load(&g_torn_skipped) != 0) {
			printf("FAIL seed=%llu: the torn-write workload was "
			    "SKIPPED (no usable temp file) -- this seed "
			    "advertises a scenario it did not execute\n",
			    (unsigned long long)seed);
			failures++; NOTE_FAIL_OFFSET();
			continue;
		}

		/* ---- partition oracle: the cut must have severed REAL
		 * workload traffic.  A cut pair (pong on CUT_LOOP_A, ping on
		 * CUT_LOOP_B) has BOTH directions blocked, so it completes ZERO
		 * round-trips and its ping sees XTC_E_AGAIN on every send. ---- */
		victim = atomic_load(&g_kill_victim);
		victim_is_cut = (victim >= 0 && victim < N_PAIRS &&
		    PAIR_IS_CUT(victim));
		for (i = 0; i < N_PAIRS; i++) {
			if (!PAIR_IS_CUT(i))
				continue;
			if (atomic_load(&g_pair_drops[i]) > 0) {
				cut_drop_pairs++;
				cut_drops += atomic_load(&g_pair_drops[i]);
			}
			if (atomic_load(&g_pair_replies[i]) > 0)
				cut_reply_pairs++;
		}
		if (sc.partition) {
			part_cut_drops += cut_drops;
			if (cut_drop_pairs > 0)
				part_seeds_with_drop++;
			if (cut_reply_pairs != 0) {
				printf("FAIL seed=%llu: %d pair(s) on the CUT "
				    "edge (loop %d <-> %d) completed "
				    "round-trips -- the partition did not cut "
				    "the traffic it claims to\n",
				    (unsigned long long)seed, cut_reply_pairs,
				    CUT_LOOP_A, CUT_LOOP_B);
				failures++; NOTE_FAIL_OFFSET();
				continue;
			}
			/* Every cut pair must have OBSERVED the cut.  The reaper
			 * kills at most one pong and a send to a dead peer fails
			 * before reaching the partition seam, so allow exactly
			 * that one pair to report no drop. */
			if (cut_drop_pairs < n_cut - (victim_is_cut ? 1 : 0)) {
				printf("FAIL seed=%llu: only %d/%d cut-edge "
				    "pair(s) observed a DROPPED send (killed "
				    "pair %d) -- the partition cut no workload "
				    "traffic\n", (unsigned long long)seed,
				    cut_drop_pairs, n_cut, victim);
				failures++; NOTE_FAIL_OFFSET();
				continue;
			}
		} else if (cut_reply_pairs == n_cut) {
			/* CONTROL: uncut, every pair on that edge DOES complete
			 * round-trips -- so the edge genuinely carries workload
			 * traffic and the drops above are caused by the cut. */
			ctl_seeds_cut_traffic++;
		}

		rc2 = run_once(seed, &sc, &st2, &app2, NULL, &act2);
		if (atomic_load(&g_torn_bad) != 0 ||
		    atomic_load(&g_torn_stuck) != 0) {
			printf("FAIL seed=%llu: torn page accepted silently / "
			    "left unresolved on the replay run\n",
			    (unsigned long long)seed);
			failures++; NOTE_FAIL_OFFSET();
			continue;
		}
		if (act1 != act2) {
			printf("FAIL seed=%llu: fault activations differ across "
			    "replay (%d/%d) -- the fault schedule is not "
			    "seed-determined\n", (unsigned long long)seed,
			    act1, act2);
			failures++; NOTE_FAIL_OFFSET();
			continue;
		}

		if (rc1 != XTC_OK || rc2 != XTC_OK) {
			printf("FAIL seed=%llu: rc1=%d rc2=%d (part=%d lat=%d "
			    "bug=%d kill=%d) -- no quiescence / invariant "
			    "violation\n", (unsigned long long)seed, rc1, rc2,
			    sc.partition, sc.latency, sc.buggify,
			    sc.machine_death);
			failures++; NOTE_FAIL_OFFSET();
			continue;
		}
		if (st1 != st2 || app1 != app2) {
			printf("FAIL seed=%llu: replay mismatch (state "
			    "%016llx/%016llx app %ld/%ld) part=%d lat=%d "
			    "bug=%d kill=%d\n", (unsigned long long)seed,
			    (unsigned long long)st1, (unsigned long long)st2,
			    app1, app2, sc.partition, sc.latency, sc.buggify,
			    sc.machine_death);
			failures++; NOTE_FAIL_OFFSET();
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
	printf("partition effect: %ld drop(s) on the cut edge (loop %d <-> %d) "
	    "across %ld/%ld partition seeds; control: %ld non-partition "
	    "seed(s) completed round-trips on that SAME edge\n",
	    part_cut_drops, CUT_LOOP_A, CUT_LOOP_B, part_seeds_with_drop,
	    n_part, ctl_seeds_cut_traffic);
	printf("torn-write effect: %ld tear(s) DETECTED by checksum, %ld page "
	    "verification(s) converged, %ld workload(s) skipped\n",
	    torn_detected, torn_verified, torn_skipped);

	/* Fault-space coverage.  Note the swarm's own workload is ping/pong
	 * + timers + a torn file, so it only reaches the buggify sites on
	 * THOSE paths; the lock/WAL/btree/channel/server sites are reached
	 * by the targeted tests (test_sim_compose, _lockmgr, _crash_recover,
	 * _chan, _svr), not here.  This report tells you which sites THIS
	 * sweep exercised -- run it per targeted test to build the full
	 * fault-space picture. */
	{
		int ki, hit = 0;
		printf("buggify coverage (this workload): ");
		for (ki = 0; ki < N_KNOWN; ki++)
			if (cov_activated[ki] > 0) {
				printf("%s=%ld ", known_sites[ki],
				    cov_activated[ki]);
				hit++;
			}
		printf("(%d/%d known sites activated; %ld activation(s) over "
		    "%ld/%ld buggify seeds)\n", hit, N_KNOWN, tot_activations,
		    seeds_with_activation, n_bug);
		/*
		 * MINIMUM ACTIVATION GATE.  A sweep that activated (almost) no
		 * faults ran the benign schedule and learned nothing about the
		 * fault space -- so "swept N fault scenarios" would be a claim
		 * with no measurement behind it.  Demand breadth (distinct sites),
		 * coverage (most buggify seeds activated something) and volume.
		 */
		if (n_bug >= MIN_GATE_SEEDS) {
			if (hit < MIN_ACT_SITES) {
				printf("FAIL: only %d/%d buggify site(s) "
				    "ACTIVATED across the sweep (need >= %d) "
				    "-- the fault space was barely "
				    "explored\n", hit, N_KNOWN, MIN_ACT_SITES);
				return 1;
			}
			if (seeds_with_activation * 2 < n_bug) {
				printf("FAIL: only %ld/%ld buggify seed(s) "
				    "activated any fault (need more than "
				    "half) -- buggify is effectively off\n",
				    seeds_with_activation, n_bug);
				return 1;
			}
			if (tot_activations < n_bug) {
				printf("FAIL: %ld fault activation(s) over %ld "
				    "buggify seeds (need at least one per "
				    "seed)\n", tot_activations, n_bug);
				return 1;
			}
		}
	}

	if (failures > 0) {
		printf("FAIL: %ld seed(s) failed\n", failures);
		/*
		 * A failing seed is only worth something if it gets PINNED --
		 * otherwise the next sweep rolls different seeds and the
		 * evidence is gone.  Print the paste-ready corpus row(s) so
		 * the ledger cannot rot for want of knowing the format.  The
		 * pin is the base OFFSET, because the seed printed above is
		 * DERIVED from it and is not an argv the test accepts.
		 */
		printf("\nACTION REQUIRED -- pin the failing seed(s) in "
		    "test/sim/corpus/seeds.txt so a fix cannot silently "
		    "regress.  Re-run each offset alone to confirm it "
		    "reproduces, then add, with the fixing commit in the "
		    "description:\n");
		for (fi = 0; fi < n_fail_off; fi++)
			printf("test_sim_swarm          %-10ld 1   "
			    "<commit> <what failed> (seed %llu)\n",
			    fail_off[fi],
			    (unsigned long long)(0x9E3779B97F4A7C15ull *
			    (uint64_t)(fail_off[fi] + 1)));
		return 1;
	}
	if (n_seeds >= 20 && n_seen < 2) {
		printf("FAIL: the swarm explored only one schedule -- the "
		    "scheduler is not seed-sensitive\n");
		return 1;
	}
	/* The partition and torn-write scenarios must be MEASURABLY
	 * effective across the sweep, not merely configured. */
	if (n_part >= MIN_GATE_SEEDS) {
		if (part_cut_drops == 0) {
			printf("FAIL: %ld partition seeds produced ZERO "
			    "dropped sends -- the cut edge carries no "
			    "workload traffic\n", n_part);
			return 1;
		}
		if (ctl_seeds_cut_traffic == 0 && n_seeds > n_part) {
			printf("FAIL: no NON-partition seed completed "
			    "round-trips on the cut edge -- the control is "
			    "missing, so the drops above prove nothing\n");
			return 1;
		}
	}
	if (n_torn >= MIN_GATE_SEEDS) {
		if (torn_verified == 0) {
			printf("FAIL: %ld torn-write seeds verified ZERO pages "
			    "-- the durability oracle never ran\n", n_torn);
			return 1;
		}
		if (torn_detected == 0) {
			printf("FAIL: %ld torn-write seeds DETECTED no tear at "
			    "all -- the corruption injection is inert, so the "
			    "silent-corruption oracle was never "
			    "challenged\n", n_torn);
			return 1;
		}
	}
	printf("OK: %ld-seed swarm/soak -- every seed reached quiescence "
	    "(across partition + latency + buggify + machine-death + "
	    "torn-write scenarios), replayed identically, invariants held; "
	    "the cut edge demonstrably lost %ld send(s) while an UNCUT "
	    "control on that same edge completed, %ld injected tear(s) were "
	    "caught by checksum with NONE accepted silently and no verifier "
	    "left unresolved, %ld fault activation(s) recorded; %d distinct "
	    "schedules explored\n", n_seeds, part_cut_drops, torn_detected,
	    tot_activations, n_seen);
	return 0;
}
