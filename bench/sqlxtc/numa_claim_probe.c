/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * examples/06_sqlxtc/numa_claim_probe.c
 *	RESEARCH SPIKE (not shipped): prove the sharing-degree premise from
 *	numa-buffer-pool-design.md BEFORE building any pool machinery.
 *
 *	The design's own instruction (section 17, final paragraph):
 *	  "Prototype step 5 and the sharing-degree metric first, standalone,
 *	   against a trace.  If index roots and catalog pages do not light up
 *	   as CLS_SHARED_HOT with a clean separation from everything else,
 *	   the premise is wrong and the rest of the architecture is not worth
 *	   building."
 *
 *	So this program implements ONLY:
 *	  - per-core sketches: doorkeeper Bloom + 4-bit count-min (section 6)
 *	    -- core-private writes only (Invariant 1), one sketch per carrier
 *	  - per-domain aggregation + count-min estimate (section 7.2/7.3)
 *	  - classify() into TRANSIENT / COLD / LOCAL_HOT / SHARED_HOT (7.4)
 *	It does NOT build the claim map, replication, thin slices, eviction
 *	changes, or epoch-replaces-pin.  Those (design sections 4/8-11, and
 *	the section 16 stages) are only worth building if THIS gate passes.
 *
 *	It hooks the single page-access choke point (bm_fix_pid, via the
 *	weak BM_ACCESS_PROBE hook) while running the SAME warm-tree +
 *	concurrent-reader workload as bench_btree_concurrent, then dumps the
 *	class histogram and the exact pids classified CLS_SHARED_HOT.
 *
 *	THE GATE:
 *	  - the B-tree root pid (and upper internal pids) must classify
 *	    CLS_SHARED_HOT (touched by >= SHARED_DEGREE_MIN domains), and
 *	  - leaf pids must NOT (they should be LOCAL_HOT / COLD / TRANSIENT),
 *	  - with a clean numeric separation between the two.
 *	If not: premise fails -> report honestly, do not build the pool.
 *
 *	DOMAINS ON THIS BOX: a single-NUMA-node dev box cannot discriminate
 *	real coherence domains, so we SYNTHESISE domains = cpu %
 *	N_SYNTH_DOMAINS.  That exercises the CLASSIFIER LOGIC faithfully
 *	(the sketch/aggregation/sharing-degree math is identical); it does
 *	NOT prove the hardware coherence win -- that needs a real multi-node
 *	box and is an explicit follow-up.  On a real NUMA box, set
 *	PROBE_REAL_NUMA=1 to use __os_numa_node_of_cpu instead.
 *
 *	Usage: numa_claim_probe [n_keys] [iters_per_reader]
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sched.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "bufmgr.h"
#include "btree.h"
#include "os_cpu.h"          /* __os_numa_node_of_cpu, __os_numa_nnodes */

/* ---- sizing (design section 6.1, scaled down for a probe) ---- */
#define PAGE_SZ            4096
#define N_FRAMES           4096
#define MAX_LOOPS          8
#define READERS_PER_LOOP   4

#define N_SYNTH_DOMAINS    4      /* synthetic coherence domains on 1 node */
#define SK_WAYS            4
#define SK_LOG_SLOTS       15
#define SK_SLOTS           (1u << SK_LOG_SLOTS)
#define SK_GENS            4      /* decay ring (design section 7.1) */
#define DK_LOG_BITS        17
#define DK_WORDS           (1u << (DK_LOG_BITS - 6))
#define MAX_CORES          512

/* ---- classification thresholds (design section 13) ---- */
#define SHARE_MIN_FREQ     4      /* per-domain estimate to "count" a domain */
#define SHARED_DEGREE_MIN  2      /* domains touching => SHARED_HOT (N=4 box) */
#define SHARED_FREQ_MIN    64
#define LOCAL_FREQ_MIN     8
#define TRANSIENT_MAX      1

typedef enum {
	CLS_TRANSIENT = 0, CLS_COLD, CLS_LOCAL_HOT, CLS_SHARED_HOT
} page_class_t;
static const char *CLS_NAME[4] = {
	"TRANSIENT", "COLD", "LOCAL_HOT", "SHARED_HOT"
};

/* ---- per-core sketch: core-private, no coherence traffic (section 6.1) ----
 * EXPERIMENT 1 (decay): a ring of SK_GENS generations of 8-BIT counters.
 * The current generation is written on the fast path; a background tick
 * seals it (rotates to the next, zeroing it + the doorkeeper).  Aggregation
 * reads only SEALED generations with decay weights 8/4/2/1, so a page hot
 * in EVERY generation outranks one hot in a single generation -- the ratio
 * survives instead of both counters pegging (the 4-bit flaw in v0). */
typedef struct {
	uint8_t  cm[SK_GENS][SK_SLOTS];   /* 8-bit count-min per generation */
	uint64_t dk[DK_WORDS];            /* doorkeeper Bloom (current gen) */
	uint32_t cur_gen;                 /* index of the generation being written */
	uint64_t last_tick;               /* last g_gen_tick this core saw */
} core_sketch_t;

/* One sketch per carrier core index; allocated lazily, touched only by
 * that carrier thread on the fast path. */
static core_sketch_t *g_core_sk[MAX_CORES];
static int            g_ncores;

/* Which synthetic domain a core belongs to (cpu -> domain). */
static uint8_t        g_core_domain[MAX_CORES];

static inline uint64_t bp_hash64(uint64_t x);   /* fwd decl */

/* Global generation tick: bumped by a background thread every GEN_MS.
 * Read-mostly (design 7.1): every access reads it, one writer bumps it. */
static _Atomic uint64_t g_gen_tick;
#define GEN_MS  50

/* ---- GROUND TRUTH (probe-only, NOT part of the design) ----
 * An exact per-(domain,pid) access counter so we can (a) enumerate ONLY
 * the pids actually touched -- avoiding the phantom-pid noise of
 * classifying a whole pid range against a lossy sketch -- and (b)
 * validate the sketch estimate against the real count.  The set of tree
 * pages is tiny (root + a few internal + leaves), so an exact table is
 * cheap here and keeps the premise test honest. */
#define GT_MAX_PIDS 262144
typedef struct {
	_Atomic uint64_t pid;                 /* 0 = empty */
	_Atomic uint64_t count[N_SYNTH_DOMAINS];
} gt_slot_t;
static gt_slot_t g_gt[GT_MAX_PIDS];       /* open-addressed, pid-keyed */

static void
gt_record(uint64_t pid, int domain)
{
	uint64_t i = bp_hash64(pid) & (GT_MAX_PIDS - 1);
	for (;;) {
		uint64_t k = atomic_load_explicit(&g_gt[i].pid, memory_order_acquire);
		if (k == pid)
			break;
		if (k == 0) {
			uint64_t exp = 0;
			if (atomic_compare_exchange_strong(&g_gt[i].pid, &exp, pid))
				break;
			continue;   /* lost race; re-read */
		}
		i = (i + 1) & (GT_MAX_PIDS - 1);
	}
	atomic_fetch_add_explicit(&g_gt[i].count[domain], 1,
	    memory_order_relaxed);
}

/* ---- splitmix64 (design section 4.2) ---- */
static inline uint64_t
bp_hash64(uint64_t x)
{
	x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
	x ^= x >> 27; x *= 0x94D049BB133111EBull;
	x ^= x >> 31;
	return x;
}

static inline int
this_domain(int cpu)
{
#if PROBE_REAL_NUMA
	int n = __os_numa_node_of_cpu(cpu);
	return n < 0 ? 0 : (n % N_SYNTH_DOMAINS);
#else
	return cpu % N_SYNTH_DOMAINS;      /* synthesise domains on 1 node */
#endif
}

static inline int
dk_test_and_set(core_sketch_t *s, uint64_t h)
{
	uint32_t b = (uint32_t)(h >> 20) & ((1u << DK_LOG_BITS) - 1);
	uint64_t *w = &s->dk[b >> 6];
	uint64_t  m = 1ull << (b & 63);
	int seen = (*w & m) != 0;
	*w |= m;                           /* plain store: core-private */
	return seen;
}

/* 8-bit saturating increment into the CURRENT generation. */
static inline void
sk_inc(core_sketch_t *s, uint32_t i)
{
	uint8_t *p = &s->cm[s->cur_gen][i];
	if (*p < 255) (*p)++;
}

/* Seal the current generation: rotate to the next, zero it + the
 * doorkeeper (design section 7.1).  Called by the core itself when the
 * global gen tick advances -- no cross-core writes. */
static inline void
sk_maybe_seal(core_sketch_t *s, uint64_t tick)
{
	if (tick == s->last_tick)
		return;
	s->last_tick = tick;
	s->cur_gen = (s->cur_gen + 1) & (SK_GENS - 1);
	memset(s->cm[s->cur_gen], 0, SK_SLOTS);
	memset(s->dk, 0, sizeof(s->dk));
}

/* ---- THE FAST-PATH HOOK: called from bm_fix_pid on every access ----
 * Records into the calling carrier's private sketch.  The doorkeeper
 * absorbs one-hit-wonders (scan resistance) exactly as the design says. */
void
bm_access_probe(bm_pid_t pid)
{
	int cpu = sched_getcpu();
	core_sketch_t *s;
	uint64_t h;
	int w, dom;

	if (cpu < 0) cpu = 0;
	if (cpu >= g_ncores) cpu %= (g_ncores > 0 ? g_ncores : 1);
	dom = g_core_domain[cpu];
	s = g_core_sk[cpu];
	if (s == NULL) return;             /* sketch not yet installed */

	gt_record((uint64_t)pid, dom);     /* ground truth (probe-only) */

	sk_maybe_seal(s, atomic_load_explicit(&g_gen_tick, memory_order_relaxed));

	h = bp_hash64((uint64_t)pid);
	if (!dk_test_and_set(s, h))
		return;                        /* first sight: doorkeeper only */
	for (w = 0; w < SK_WAYS; w++)
		sk_inc(s, (uint32_t)(h >> (w * 13)) & (SK_SLOTS - 1));
}

/* ---- per-domain aggregation (section 7.2) + estimate (7.3) ----
 * Sum the sketches of all cores in a domain; count-min estimate is the
 * MIN across the SK_WAYS ways of the per-domain merged counts. */
static uint32_t g_agg[N_SYNTH_DOMAINS][SK_SLOTS];

/* Decay weights by generation age (design 7.1): newest sealed = 8, ... */
static const uint8_t GEN_DECAY[SK_GENS] = { 8, 4, 2, 1 };

static void
aggregate_all(void)
{
	int d, c;
	uint32_t i, age;
	memset(g_agg, 0, sizeof(g_agg));
	for (c = 0; c < g_ncores; c++) {
		core_sketch_t *s = g_core_sk[c];
		if (s == NULL) continue;
		d = g_core_domain[c];
		/* At REPORT time the workload has stopped, so it is safe to
		 * include the current (unsealed) generation too -- otherwise a
		 * short run whose last window never sealed loses most traffic.
		 * age 0 = current gen (full weight), then decayed sealed gens. */
		for (age = 0; age < SK_GENS; age++) {
			uint32_t gi = (s->cur_gen - age) & (SK_GENS - 1);
			uint8_t  wgt = GEN_DECAY[age];
			const uint8_t *cm = s->cm[gi];
			for (i = 0; i < SK_SLOTS; i++)
				g_agg[d][i] += (uint32_t)cm[i] * wgt;
		}
	}
}

static uint16_t
agg_estimate(int d, uint64_t h)
{
	uint32_t m = 0xFFFFFFFFu;
	int w;
	for (w = 0; w < SK_WAYS; w++) {
		uint32_t v = g_agg[d][(uint32_t)(h >> (w * 13)) & (SK_SLOTS - 1)];
		if (v < m) m = v;
	}
	return m;
}

static uint16_t
domain_mask(uint64_t h)
{
	uint16_t mask = 0;
	int d;
	for (d = 0; d < N_SYNTH_DOMAINS; d++)
		if (agg_estimate(d, h) >= SHARE_MIN_FREQ)
			mask |= (uint16_t)(1u << d);
	return mask;
}

static uint32_t
global_freq(uint64_t h)
{
	uint32_t f = 0;
	int d;
	for (d = 0; d < N_SYNTH_DOMAINS; d++)
		f += agg_estimate(d, h);
	return f;
}

static page_class_t
classify(uint64_t h, uint16_t *out_mask, uint32_t *out_freq)
{
	uint16_t mask = domain_mask(h);
	uint32_t f    = global_freq(h);
	int deg = __builtin_popcount(mask);
	if (out_mask) *out_mask = mask;
	if (out_freq) *out_freq = f;
	if (f == 0)                                          return CLS_TRANSIENT;
	if (deg >= SHARED_DEGREE_MIN && f >= SHARED_FREQ_MIN) return CLS_SHARED_HOT;
	if (f >= LOCAL_FREQ_MIN)                              return CLS_LOCAL_HOT;
	if (f <= TRANSIENT_MAX)                               return CLS_TRANSIENT;
	return CLS_COLD;
}

/* EXPERIMENT 2: DEGREE-FIRST classifier.  Promotes to SHARED_HOT purely
 * on sharing degree (touched by >= K domains), using frequency only to
 * distinguish LOCAL_HOT from COLD.  Tests whether contention topology
 * -- the design's claimed novel signal -- is load-bearing on its own. */
static page_class_t
classify_degree_first(uint64_t h)
{
	uint16_t mask = domain_mask(h);
	uint32_t f    = global_freq(h);
	int deg = __builtin_popcount(mask);
	if (deg >= SHARED_DEGREE_MIN) return CLS_SHARED_HOT;   /* degree alone */
	if (deg == 1 && f >= LOCAL_FREQ_MIN) return CLS_LOCAL_HOT;
	if (f == 0)  return CLS_TRANSIENT;
	return CLS_COLD;
}

/* ---- the workload (mirrors bench_btree_concurrent) ---- */
static bm_t   *g_bm;
static bt_t   *g_bt;
static long    g_n_keys;
static long    g_iters_per_reader;
static atomic_int   g_go;
static atomic_int   g_readers_left;

static void
mkkey(long i, char *k)
{
	snprintf(k, 24, "k%010ld", i);
}

/* Install this carrier's sketch on first entry (records cpu->domain). */
static void
ensure_sketch_for_this_cpu(void)
{
	int cpu = sched_getcpu();
	if (cpu < 0) cpu = 0;
	if (cpu >= g_ncores) return;
	if (g_core_sk[cpu] == NULL) {
		core_sketch_t *s = calloc(1, sizeof(*s));
		/* Racy install across carriers is fine: worst case two
		 * carriers on the same cpu index briefly disagree; both are
		 * core-private and the probe is single-run.  In practice one
		 * carrier per cpu. */
		g_core_sk[cpu] = s;
		g_core_domain[cpu] = (uint8_t)this_domain(cpu);
	}
}

static int    g_partition;    /* 1 = each domain reads only its own key range */

static void
reader_proc(void *arg)
{
	uint64_t rng = (uint64_t)(uintptr_t)arg * 0x9E3779B97F4A7C15ull + 1;
	char k[24], buf[40];
	uint16_t vl;
	long i;
	long lo = 0, span = g_n_keys;

	ensure_sketch_for_this_cpu();

	if (g_partition) {
		/* Partition the key space by THIS reader's domain (the same
		 * domain its accesses will be attributed to), so leaves become
		 * domain-LOCAL while the root stays global -- the scenario
		 * where sharing-degree is expected to separate the root from
		 * the (now-local) leaves.  Domain is taken from the running
		 * cpu, matching the sketch's attribution. */
		int cpu = sched_getcpu();
		int dom = this_domain(cpu < 0 ? 0 : cpu);
		span = g_n_keys / N_SYNTH_DOMAINS;
		if (span < 1) span = 1;
		lo = (long)dom * span;
	}

	while (!atomic_load_explicit(&g_go, memory_order_acquire))
		xtc_yield();

	for (i = 0; i < g_iters_per_reader; i++) {
		long idx;
		rng = rng * 6364136223846793005ull + 1442695040888963407ull;
		idx = lo + (long)((rng >> 33) % (uint64_t)span);
		mkkey(idx, k);
		(void)bt_lookup(g_bt, k, (uint16_t)strlen(k), buf, sizeof buf, &vl);
		if ((i & 63) == 63)
			xtc_yield();
	}
	if (atomic_fetch_sub_explicit(&g_readers_left, 1,
	    memory_order_acq_rel) == 1)
		bm_provider_stop(g_bm);
}

static _Atomic int g_tick_stop;

static void *
gen_tick_thread(void *arg)
{
	(void)arg;
	while (!atomic_load_explicit(&g_tick_stop, memory_order_relaxed)) {
		usleep(GEN_MS * 1000);
		atomic_fetch_add_explicit(&g_gen_tick, 1, memory_order_relaxed);
	}
	return NULL;
}

static void
run_workload(int nloops)
{
	xtc_exec_t *e = NULL;
	int readers = nloops * READERS_PER_LOOP;
	int i;
	pthread_t tick;

	atomic_store(&g_go, 0);
	atomic_store(&g_readers_left, readers);
	atomic_store(&g_tick_stop, 0);
	if (xtc_exec_init(&e, nloops) != XTC_OK) { fprintf(stderr, "exec_init\n"); exit(1); }
	if (bm_provider_spawn(g_bm, xtc_exec_loop(e, 0), 1LL * 1000 * 1000,
	    NULL) != XTC_OK) { fprintf(stderr, "provider\n"); exit(1); }
	for (i = 0; i < readers; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, i % nloops), reader_proc,
		    (void *)(intptr_t)(i + 1), NULL, NULL);
	pthread_create(&tick, NULL, gen_tick_thread, NULL);
	atomic_store_explicit(&g_go, 1, memory_order_release);
	(void)xtc_exec_run(e);
	atomic_store_explicit(&g_tick_stop, 1, memory_order_relaxed);
	pthread_join(tick, NULL);
	xtc_exec_fini(e);
}

/* ---- report ---- */
static void
report(void)
{
	int hist[4] = { 0, 0, 0, 0 };
	uint32_t sat = 0, sat_total = 0;
	int d;
	bt_stats_t ts;
	uint64_t i;
	long observed = 0;
	/* Collect observed pids into an array for sorting by true frequency. */
	struct row { uint64_t pid, total; int true_deg; int sk_deg; uint32_t est;
	             page_class_t cf, cd; };
	struct row *rows;
	long nr = 0, k;
	int sh_freq = 0, sh_deg = 0;   /* # pages each classifier calls SHARED_HOT */

	aggregate_all();

	for (d = 0; d < N_SYNTH_DOMAINS; d++) {
		uint32_t j;
		for (j = 0; j < SK_SLOTS; j++) {
			sat_total++;
			if (g_agg[d][j] >= 255u * 15u) sat++;   /* per-gen ceiling x max decay */
		}
	}
	for (i = 0; i < GT_MAX_PIDS; i++)
		if (atomic_load(&g_gt[i].pid) != 0) observed++;

	rows = calloc((size_t)(observed > 0 ? observed : 1), sizeof(*rows));
	if (rows == NULL) { fprintf(stderr, "oom\n"); return; }

	for (i = 0; i < GT_MAX_PIDS; i++) {
		uint64_t pid = atomic_load(&g_gt[i].pid);
		uint64_t tot = 0;
		int tdeg = 0;
		uint16_t mask; uint32_t freq;
		if (pid == 0) continue;
		for (d = 0; d < N_SYNTH_DOMAINS; d++) {
			uint64_t c = atomic_load(&g_gt[i].count[d]);
			tot += c;
			if (c > 0) tdeg++;
		}
		rows[nr].pid = pid;
		rows[nr].total = tot;
		rows[nr].true_deg = tdeg;
		rows[nr].cf = classify(bp_hash64(pid), &mask, &freq);
		rows[nr].cd = classify_degree_first(bp_hash64(pid));
		rows[nr].sk_deg = __builtin_popcount(mask);
		rows[nr].est = freq;
		hist[rows[nr].cf]++;
		if (rows[nr].cf == CLS_SHARED_HOT) sh_freq++;
		if (rows[nr].cd == CLS_SHARED_HOT) sh_deg++;
		nr++;
	}

	/* Sort rows by true total desc (simple insertion sort; nr is small). */
	for (k = 1; k < nr; k++) {
		struct row tmp = rows[k];
		long j = k - 1;
		while (j >= 0 && rows[j].total < tmp.total) { rows[j+1] = rows[j]; j--; }
		rows[j+1] = tmp;
	}

	bt_get_stats(g_bt, &ts);

	printf("# ---- observed pages, by TRUE access frequency (desc) ----\n");
	printf("# %-8s %-12s %-9s %-8s %-11s %-12s %s\n",
	    "pid", "true_total", "true_deg", "sk_deg", "freq-first", "degree-first", "sketch_est");
	for (k = 0; k < nr; k++) {
		if (rows[k].cf >= CLS_LOCAL_HOT || rows[k].cd >= CLS_LOCAL_HOT || k < 20) {
			printf("  %-8llu %-12llu %-9d %-8d %-11s %-12s %-10u%s\n",
			    (unsigned long long)rows[k].pid,
			    (unsigned long long)rows[k].total,
			    rows[k].true_deg, rows[k].sk_deg,
			    CLS_NAME[rows[k].cf], CLS_NAME[rows[k].cd], rows[k].est,
			    k == 0 ? "  <-- hottest (expect ROOT)" : "");
		}
	}

	printf("#\n# ---- EXPERIMENT 2: which signal flags ONLY the root? ----\n");
	printf("#   frequency-first classifier: %d pages SHARED_HOT\n", sh_freq);
	printf("#   degree-first    classifier: %d pages SHARED_HOT\n", sh_deg);
	printf("#   (ideal under a PARTITIONED workload = 1: only the root.\n"
	    "#    >1 means the classifier over-promotes leaves.)\n");

	printf("#\n# ---- class histogram (observed pids only: %ld) ----\n", nr);
	for (d = 0; d < 4; d++)
		printf("#   %-10s %d\n", CLS_NAME[d], hist[d]);
	printf("#\n# tree height=%llu\n", (unsigned long long)ts.height);
	printf("# sketch saturation: %.2f%% (%s)\n",
	    100.0 * (double)sat / (double)sat_total,
	    sat * 20 > sat_total ? "TOO HIGH -- raise SK_LOG_SLOTS; results suspect"
	                         : "ok");
	printf("#\n# GATE: does the globally-hottest page (root) classify\n"
	    "#       SHARED_HOT, cleanly separated from the leaf tail?\n");
	free(rows);
}

int
main(int argc, char **argv)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256] = "/tmp/sqlxtc-numa-probe-XXXXXX";
	int fd, i;
	long n = argc > 1 ? atol(argv[1]) : 50000;
	long iters = argc > 2 ? atol(argv[2]) : 2000000;

	g_n_keys = n < 1 ? 1 : n;
	g_iters_per_reader = iters < 1 ? 1 : iters;
	g_partition = argc > 3 ? atoi(argv[3]) : 0;   /* 1 = per-domain key range */
	g_ncores = __os_ncpus();
	if (g_ncores < 1) g_ncores = 1;
	if (g_ncores > MAX_CORES) g_ncores = MAX_CORES;

	printf("# numa_claim_probe: %d cores, %d synthetic domains, %s workload%s\n",
	    g_ncores, N_SYNTH_DOMAINS,
	    g_partition ? "PARTITIONED (per-domain key range)" : "uniform-random",
#if PROBE_REAL_NUMA
	    " (REAL numa nodes mapped)"
#else
	    " (synthesised: cpu %% N_SYNTH_DOMAINS -- single-node box; logic gate only)"
#endif
	    );

	fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); return 1; }
	close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = N_FRAMES;
	bo.cool_pct = 25;
	if (bm_create(&bo, &g_bm) != XTC_OK) { fprintf(stderr, "bm_create\n"); return 1; }
	if (bt_open(g_bm, &g_bt) != XTC_OK) { fprintf(stderr, "bt_open\n"); return 1; }

	{
		char k[24], v[32];
		for (i = 0; i < g_n_keys; i++) {
			mkkey(i, k);
			snprintf(v, sizeof v, "v%010d", i);
			if (bt_insert(g_bt, k, (uint16_t)strlen(k), v,
			    (uint16_t)strlen(v)) != XTC_OK) {
				fprintf(stderr, "warm insert failed at %d\n", i);
				return 1;
			}
		}
	}
	{
		bt_stats_t ts;
		bt_get_stats(g_bt, &ts);
		printf("# warm tree: %ld keys, height=%llu\n",
		    g_n_keys, (unsigned long long)ts.height);
	}

	run_workload(MAX_LOOPS);   /* full width: every domain gets readers */
	report();

	bt_close(g_bt);
	bm_destroy(g_bm);
	unlink(path);
	for (i = 0; i < g_ncores; i++)
		free(g_core_sk[i]);
	return 0;
}
