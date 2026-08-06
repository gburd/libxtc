/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * evict_policy_proto.c -- standalone proving ground for the NUMA
 * buffer-pool eviction policy (design doc section 8, section 16
 * stages 2-4).  This is a PURE, dependency-free policy module: it
 * operates on abstract frame ids and a synthetic access trace and is
 * NOT wired into the real buffer manager (bufmgr.c).  It exists to
 * prove the algorithms and measure their behaviour before any of it
 * touches the production path.
 *
 * Three mechanisms:
 *   1. Sampled power-of-D-choices victim selection (evict_score with
 *      the design's weights; partial-sort D_SAMPLE candidates, evict
 *      the EVICT_BATCH lowest scores).  Stage 2.
 *   2. Two-phase reversible cooling ring (cool -> reinstate | drain).
 *      Stage 3.
 *   3. W-TinyLFU admission + doorkeeper Bloom filter for scan
 *      resistance.  Stage 4.
 *
 * Driven by a synthetic trace (Zipfian hot-set + uniform warm set +
 * one-shot cold SCAN) and self-checks the key properties with plain
 * assert().  No munit, no libxtc, no bufmgr.
 *
 * Build (standalone, from examples/06_sqlxtc):
 *   gcc -O2 -g -std=c11 -Wall -Wextra -o evict_policy_proto \
 *       evict_policy_proto.c
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- *
 * Design constants (section 8.2, section 13 weight table)           *
 * ---------------------------------------------------------------- */

#define D_SAMPLE	32	/* frames sampled per eviction round     */
#define EVICT_BATCH	8	/* victims taken from the sample         */
#define MAG_CAP		64	/* per-core free magazine capacity       */

#define W_FREQ		1L		/* frequency contribution        */
#define W_SHARED	1000000L	/* SHARED_HOT: soft pin          */
#define W_LOCAL		1000L		/* LOCAL_HOT                     */
#define W_TRANSIENT	500L		/* subtracted: evict first       */
#define W_DIRTY		200L		/* added: prefer clean victims   */
#define W_HAS_PEER	100L		/* subtracted: peer holds a copy */

/* ---------------------------------------------------------------- *
 * Frame model                                                       *
 * ---------------------------------------------------------------- */

typedef enum {
	CLS_TRANSIENT = 0,	/* seen once, never reused: scan */
	CLS_COLD,
	CLS_LOCAL_HOT,		/* hot within one domain         */
	CLS_SHARED_HOT,		/* hot across domains: protect   */
} page_class_t;

typedef enum {
	ST_FREE = 0,
	ST_RESIDENT,
	ST_COOLING,		/* victim, reversible            */
	ST_EVICTING,		/* committed, unlinked           */
} page_state_t;

typedef struct {
	uint64_t	pgid;
	page_state_t	state;
	bool		dirty;
	bool		has_peer;	/* replica on another domain     */
	uint32_t	freq;		/* aggregate frequency estimate  */
	page_class_t	cls;
} frame_t;

/* ---------------------------------------------------------------- *
 * evict_score -- section 8.2.  Lower score = evict first.           *
 * ---------------------------------------------------------------- */

static int64_t
evict_score(const frame_t *f)
{
	int64_t s = (int64_t)f->freq * W_FREQ;

	switch (f->cls) {
	case CLS_SHARED_HOT:	s += W_SHARED;		break;	/* protect */
	case CLS_LOCAL_HOT:	s += W_LOCAL;		break;
	case CLS_TRANSIENT:	s -= W_TRANSIENT;	break;	/* first   */
	case CLS_COLD:					break;
	}

	if (f->dirty)
		s += W_DIRTY;		/* prefer clean victims          */
	if (f->has_peer)
		s -= W_HAS_PEER;	/* cheaper to drop               */

	return s;			/* lower = evict                 */
}

/* ---------------------------------------------------------------- *
 * Deterministic PRNG (splitmix64) -- no unseeded RNG, reproducible. *
 * ---------------------------------------------------------------- */

static uint64_t
xrand(uint64_t *st)
{
	uint64_t z = (*st += 0x9E3779B97F4A7C15ull);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

/* Uniform in [0, n). */
static uint64_t
xrand_n(uint64_t *st, uint64_t n)
{
	return xrand(st) % n;
}

/* ---------------------------------------------------------------- *
 * Sampled power-of-D-choices victim selection (section 8.2)         *
 * ---------------------------------------------------------------- */

typedef struct {
	int		idx;		/* frame index                   */
	int64_t		score;
} cand_t;

/* Selection-sort the SMALLEST k of n candidates to the front (ascending
 * by score).  n is D_SAMPLE (32), k is EVICT_BATCH (8): a full sort is
 * overkill, a partial selection is exactly what the design specifies. */
static void
partial_sort_ascending(cand_t *c, int n, int k)
{
	for (int i = 0; i < k && i < n; i++) {
		int min = i;
		for (int j = i + 1; j < n; j++)
			if (c[j].score < c[min].score)
				min = j;
		if (min != i) {
			cand_t t = c[i];
			c[i] = c[min];
			c[min] = t;
		}
	}
}

/* Sample D_SAMPLE resident frames at random, score them, return the
 * EVICT_BATCH lowest-score frame indices in out[].  Returns the count
 * actually chosen (< EVICT_BATCH if the pool is nearly empty). */
static int
sample_victims(frame_t *frames, int n_frames, uint64_t *rng,
    int *out /* [EVICT_BATCH] */)
{
	cand_t c[D_SAMPLE];
	int got = 0;
	int tries = 0;
	int cap = n_frames * 4;		/* bound the resident search     */

	while (got < D_SAMPLE && tries < cap) {
		int f = (int)xrand_n(rng, (uint64_t)n_frames);
		tries++;
		if (frames[f].state != ST_RESIDENT)
			continue;
		c[got].idx = f;
		c[got].score = evict_score(&frames[f]);
		got++;
	}
	if (got == 0)
		return 0;

	int k = EVICT_BATCH < got ? EVICT_BATCH : got;
	partial_sort_ascending(c, got, k);
	for (int i = 0; i < k; i++)
		out[i] = c[i].idx;
	return k;
}

/* ---------------------------------------------------------------- *
 * Two-phase reversible cooling ring (section 8.3)                   *
 * ---------------------------------------------------------------- */

typedef struct {
	int	*ring;		/* frame indices                          */
	int	head, tail, cap;
	int	target;		/* steady-state occupancy                 */
} cooling_t;

static void
cooling_init(cooling_t *cl, int cap, int target)
{
	cl->ring = calloc((size_t)cap, sizeof(int));
	assert(cl->ring != NULL);
	cl->head = cl->tail = 0;
	cl->cap = cap;
	cl->target = target;
}

static void
cooling_free(cooling_t *cl)
{
	free(cl->ring);
	cl->ring = NULL;
}

static int
cooling_len(const cooling_t *cl)
{
	return (cl->tail - cl->head + cl->cap) % cl->cap;
}

static void
ring_push(cooling_t *cl, int f)
{
	cl->ring[cl->tail] = f;
	cl->tail = (cl->tail + 1) % cl->cap;
	assert(cl->tail != cl->head);	/* ring never full in this rig   */
}

static int
ring_pop(cooling_t *cl)
{
	int f;

	if (cl->head == cl->tail)
		return -1;
	f = cl->ring[cl->head];
	cl->head = (cl->head + 1) % cl->cap;
	return f;
}

/* Choose f as a victim: RESIDENT -> COOLING, push to ring.  Reversible:
 * a re-access before drain can reinstate it. */
static void
cool_page(frame_t *frames, cooling_t *cl, int f)
{
	if (frames[f].state != ST_RESIDENT)
		return;			/* raced / already cooling: fine */
	frames[f].state = ST_COOLING;
	ring_push(cl, f);
}

/* Resurrect: COOLING -> RESIDENT if caught before commit.  Stale ring
 * entry is skipped by the drain.  Returns true if it caught it. */
static bool
cooling_reinstate(frame_t *frames, int f)
{
	if (frames[f].state != ST_COOLING)
		return false;
	frames[f].state = ST_RESIDENT;
	return true;
}

/* Commit victims past the ring target: COOLING -> EVICTING -> FREE.
 * Resurrected (now RESIDENT) entries are skipped.  Returns count freed;
 * frees are appended to freed[] (caller-sized). */
static int
cooling_drain(frame_t *frames, cooling_t *cl, int *freed, int freed_cap)
{
	int n = 0;

	while (cooling_len(cl) > cl->target) {
		int f = ring_pop(cl);
		if (f < 0)
			break;
		if (frames[f].state != ST_COOLING)
			continue;		/* resurrected or stale  */
		frames[f].state = ST_EVICTING;
		/* (writeback of dirty pages would happen here) */
		frames[f].state = ST_FREE;
		frames[f].dirty = false;
		if (n < freed_cap)
			freed[n] = f;
		n++;
	}
	return n;
}

/* ---------------------------------------------------------------- *
 * Doorkeeper Bloom filter + W-TinyLFU admission (section 8.4, 6.2)  *
 * ---------------------------------------------------------------- */

#define DK_BITS		(1u << 16)	/* 64 Kbit doorkeeper            */
#define DK_WORDS	(DK_BITS / 64)

typedef struct {
	uint64_t	bits[DK_WORDS];
} doorkeeper_t;

static uint64_t
mix64(uint64_t x)
{
	x ^= x >> 30;
	x *= 0xBF58476D1CE4E5B9ull;
	x ^= x >> 27;
	x *= 0x94D049BB133111EBull;
	x ^= x >> 31;
	return x;
}

/* Test-and-set two bits.  Returns true if the page was seen before
 * (both bits already set) -- i.e. NOT a first sight. */
static bool
dk_test_and_set(doorkeeper_t *dk, uint64_t pgid)
{
	uint64_t h = mix64(pgid);
	uint32_t b0 = (uint32_t)(h & (DK_BITS - 1));
	uint32_t b1 = (uint32_t)((h >> 20) & (DK_BITS - 1));
	uint64_t m0 = 1ull << (b0 & 63), m1 = 1ull << (b1 & 63);
	uint64_t *w0 = &dk->bits[b0 >> 6], *w1 = &dk->bits[b1 >> 6];
	bool seen = ((*w0 & m0) != 0) && ((*w1 & m1) != 0);

	*w0 |= m0;
	*w1 |= m1;
	return seen;
}

/*
 * W-TinyLFU admission rule (section 8.4, 6.2): a newcomer must beat the
 * victim it would displace.  admit() returns true if the incoming page
 * should be cached at the victim's expense.  With no pressure (free
 * frames available) the caller admits unconditionally; this rule only
 * fires when a victim must be displaced.
 *
 * The doorkeeper is the scan filter.  A page whose current access is
 * its FIRST sight (newcomer_first_sight) has, by definition, no reuse
 * history -- it is a streaming/scan access.  Admitting it can only be
 * justified if it STRICTLY beats the victim; a tie (both freq 0, the
 * classic scan-evicts-scan case) is rejected, so a run of never-before-
 * seen cold pages cannot displace the warm working set.  A page seen
 * before (real reuse) uses the ordinary >= rule.
 */
static bool
admit(uint32_t incoming_freq, uint32_t victim_freq, bool newcomer_first_sight)
{
	if (newcomer_first_sight)
		return incoming_freq > victim_freq;	/* strict */
	return incoming_freq >= victim_freq;
}

/* ---------------------------------------------------------------- *
 * Synthetic trace generator                                         *
 * ---------------------------------------------------------------- *
 *
 * Page-id layout (disjoint ranges so the checker can classify by id):
 *   [0, HOT_N)                         Zipfian hot-set ("the root")
 *   [HOT_N, HOT_N+WARM_N)              uniform warm working set
 *   [HOT_N+WARM_N, ...+SCAN_N)         one-shot cold SCAN pages
 */

#define HOT_N		8	/* a few very hot pages                  */
#define WARM_N		200	/* warm working set                      */
#define SCAN_N		2000	/* cold pages streamed once              */

#define PG_HOT_LO	0u
#define PG_WARM_LO	(HOT_N)
#define PG_SCAN_LO	(HOT_N + WARM_N)

static bool
is_hot_pg(uint64_t p)	{ return p < PG_WARM_LO; }
static bool
is_warm_pg(uint64_t p)	{ return p >= PG_WARM_LO && p < PG_SCAN_LO; }
static bool
is_scan_pg(uint64_t p)	{ return p >= PG_SCAN_LO; }

/* Zipf-ish pick over [0, n): bias hard toward id 0 (the root) by
 * squaring a uniform draw.  Deterministic, adequate for the property
 * we are proving (a handful of pages dominate). */
static uint64_t
zipf_pick(uint64_t *rng, uint64_t n)
{
	double u = (double)xrand(rng) / (double)UINT64_MAX;
	double z = u * u * u;		/* cube: heavy head              */
	uint64_t k = (uint64_t)(z * (double)n);
	return k >= n ? n - 1 : k;
}

/* ---------------------------------------------------------------- *
 * Frequency sketch (per-domain aggregate estimate, section 7.3)     *
 * A plain count-map is enough for a single-domain prototype; a real  *
 * deployment uses the 4-bit count-min sketch.                        *
 * ---------------------------------------------------------------- */

#define N_PAGES		(PG_SCAN_LO + SCAN_N)

typedef struct {
	uint32_t	freq[N_PAGES];	/* aggregate frequency by pgid   */
	doorkeeper_t	dk;		/* scan-resistance filter        */
} sketch_t;

/* Record an access.  The doorkeeper suppresses one-hit wonders from
 * the frequency sketch: a page seen for the first time bumps only the
 * doorkeeper, not the frequency count.  A page seen again is real
 * reuse and enters the sketch.  This is the scan filter (section 6.2).
 * Returns true if the access was a first sight (bypass candidate). */
static bool
sketch_touch(sketch_t *sk, uint64_t pgid)
{
	if (!dk_test_and_set(&sk->dk, pgid))
		return true;		/* first sight: keep out of sketch */
	if (sk->freq[pgid] < 0xFFFFFFFFu)
		sk->freq[pgid]++;
	return false;
}

static uint32_t
sketch_est(const sketch_t *sk, uint64_t pgid)
{
	return sk->freq[pgid];
}

/* ---------------------------------------------------------------- *
 * Classification (section 7.4) -- reduced to the single-domain rig. *
 * In the full design SHARED_HOT needs multi-domain sharing; here the *
 * hot-set pages are declared shared/replicated by the workload (they *
 * are the root, read by "every core"), and we classify by frequency  *
 * with the hot ids getting the shared bit.                           *
 * ---------------------------------------------------------------- */

#define LOCAL_FREQ_MIN	4u
#define TRANSIENT_MAX	1u

static page_class_t
classify(uint64_t pgid, uint32_t freq)
{
	if (is_hot_pg(pgid) && freq >= LOCAL_FREQ_MIN)
		return CLS_SHARED_HOT;		/* the root: shared      */
	if (freq == 0)
		return CLS_TRANSIENT;
	if (freq >= LOCAL_FREQ_MIN)
		return CLS_LOCAL_HOT;
	if (freq <= TRANSIENT_MAX)
		return CLS_TRANSIENT;
	return CLS_COLD;
}

/* ---------------------------------------------------------------- *
 * The rig                                                           *
 * ---------------------------------------------------------------- */

#define POOL_FRAMES	160	/* resident capacity (< hot+warm working set) */

typedef struct {
	long	hot_evicted;
	long	cold_evicted;	/* cold + transient (scan) evictions     */
	long	warm_evicted;
	long	shared_evicted;
	long	resurrections;
	long	admissions_rejected;
	long	scan_bypassed;
	long	admissions_ok;
} stats_t;

/* Find a resident frame holding pgid, or -1. */
static int
find_frame(const frame_t *frames, int n, uint64_t pgid)
{
	for (int i = 0; i < n; i++)
		if (frames[i].state != ST_FREE && frames[i].pgid == pgid)
			return i;
	return -1;
}

/* Grab a FREE frame index, or -1 if none. */
static int
find_free(const frame_t *frames, int n)
{
	for (int i = 0; i < n; i++)
		if (frames[i].state == ST_FREE)
			return i;
	return -1;
}

/* Refresh a resident frame's freq + class from the sketch. */
static void
refresh_frame(frame_t *fr, const sketch_t *sk)
{
	fr->freq = sketch_est(sk, fr->pgid);
	fr->cls = classify(fr->pgid, fr->freq);
	/* The hot-set root is replicated (has peers on other domains). */
	fr->has_peer = (fr->cls == CLS_SHARED_HOT);
}

/*
 * Handle one access.  Models the resident set + eviction + admission.
 * If the page is resident, record reuse and (if cooling) resurrect it.
 * If it misses, apply admission: on pressure the incoming page must
 * beat the coolest victim, else it is bypassed (read but not cached).
 */
static void
access_page(frame_t *frames, cooling_t *cl, sketch_t *sk, stats_t *st,
    uint64_t *rng, uint64_t pgid)
{
	bool first_sight = sketch_touch(sk, pgid);
	int f = find_frame(frames, POOL_FRAMES, pgid);

	if (f >= 0) {
		/* Hit.  Resurrect if we caught it mid-eviction. */
		if (frames[f].state == ST_COOLING) {
			if (cooling_reinstate(frames, f))
				st->resurrections++;
		}
		refresh_frame(&frames[f], sk);
		return;
	}

	/* Miss.  A first-sight page is a scan/one-hit-wonder (the doorkeeper
	 * has no reuse record for it): bypass it -- read, do not cache -- so
	 * a streaming scan never occupies frames the warm set could use.
	 * This is the doorkeeper acting as the scan filter (section 6.2),
	 * and it applies whether or not a free frame happens to exist. */
	if (first_sight) {
		st->scan_bypassed++;
		st->admissions_rejected++;
		return;
	}

	/* Reuse miss (seen before, evicted since).  Is there a free frame? */
	int slot = find_free(frames, POOL_FRAMES);

	if (slot < 0) {
		/*
		 * Pressure: run one sampled-eviction round to produce
		 * free frames (the proactive evictor's job), then apply
		 * admission against the coolest victim.
		 */
		int vic[EVICT_BATCH];
		int nv = sample_victims(frames, POOL_FRAMES, rng, vic);

		for (int i = 0; i < nv; i++) {
			frame_t *v = &frames[vic[i]];
			/* count what class we are about to cool */
			if (v->cls == CLS_SHARED_HOT)
				st->shared_evicted++;
			else if (v->cls == CLS_LOCAL_HOT)
				st->hot_evicted++;
			else if (is_warm_pg(v->pgid))
				st->warm_evicted++;
			else
				st->cold_evicted++;
			cool_page(frames, cl, vic[i]);
		}

		/* Admission: newcomer vs the coolest victim in the ring. */
		uint32_t f_in = sketch_est(sk, pgid);
		uint32_t f_v = 0;
		if (cooling_len(cl) > 0) {
			int vf = cl->ring[cl->head];
			f_v = sketch_est(sk, frames[vf].pgid);
		}
		if (!admit(f_in, f_v, first_sight)) {
			st->admissions_rejected++;
			if (first_sight || is_scan_pg(pgid))
				st->scan_bypassed++;
			/* bypassed: read, not cached.  Drain and return. */
			int freed[EVICT_BATCH];
			cooling_drain(frames, cl, freed, EVICT_BATCH);
			return;
		}
		st->admissions_ok++;

		/* Admitted: drain to reclaim a frame, then install. */
		int freed[EVICT_BATCH];
		int nf = cooling_drain(frames, cl, freed, EVICT_BATCH);
		if (nf > 0)
			slot = freed[0];
		else
			slot = find_free(frames, POOL_FRAMES);
		if (slot < 0)
			return;		/* nothing freed this round      */
	}

	/* Install the page in a free frame. */
	frames[slot].pgid = pgid;
	frames[slot].state = ST_RESIDENT;
	frames[slot].dirty = (xrand_n(rng, 4) == 0);	/* ~25% dirty    */
	refresh_frame(&frames[slot], sk);
}

/* Snapshot how much of the warm working set is resident. */
static int
warm_resident_count(const frame_t *frames, int n)
{
	int c = 0;

	for (int i = 0; i < n; i++)
		if (frames[i].state != ST_FREE && is_warm_pg(frames[i].pgid))
			c++;
	return c;
}

int
main(void)
{
	static frame_t frames[POOL_FRAMES];
	cooling_t cl;
	sketch_t *sk = calloc(1, sizeof(*sk));
	stats_t st = { 0 };
	uint64_t rng = 0xC0FFEEull;	/* fixed seed: reproducible      */

	assert(sk != NULL);
	assert(WARM_N > POOL_FRAMES / 2);	/* warm set > half pool  */

	/* -------- Phase 0: direct power-of-D victim-selection proof ---- *
	 * Build a synthetic mixed pool (equal parts SHARED_HOT, LOCAL_HOT,
	 * COLD, TRANSIENT) and run many sampled-eviction rounds, tallying
	 * which class each chosen victim belongs to.  This proves the
	 * scorer in isolation: sampled selection preferentially evicts
	 * TRANSIENT/COLD over the hot classes, and never SHARED_HOT.  A
	 * uniformly-random baseline over the same pool is ~25% per class. */
	{
		enum { NP = 400 };
		static frame_t pool[NP];
		uint64_t prng = 0xD15EA5Eull;
		long pick[4] = { 0 };	/* by page_class_t                */
		long rounds = 5000;

		for (int i = 0; i < NP; i++) {
			pool[i].state = ST_RESIDENT;
			pool[i].pgid = (uint64_t)i;
			page_class_t c = (page_class_t)(i % 4);
			pool[i].cls = c;
			/* Give hot classes higher freq, as reality would. */
			pool[i].freq = (c == CLS_SHARED_HOT) ? 50 :
			    (c == CLS_LOCAL_HOT) ? 20 :
			    (c == CLS_COLD) ? 2 : 0;
			pool[i].dirty = (i % 3) == 0;
			pool[i].has_peer = (c == CLS_SHARED_HOT);
		}
		for (long r = 0; r < rounds; r++) {
			int vic[EVICT_BATCH];
			int nv = sample_victims(pool, NP, &prng, vic);
			for (int i = 0; i < nv; i++)
				pick[pool[vic[i]].cls]++;
			/* restore: this is a selection test, not a real drain */
			for (int i = 0; i < nv; i++)
				pool[vic[i]].state = ST_RESIDENT;
		}
		printf("power-of-D victim selection (5000 rounds, equal-class "
		    "pool)\n");
		printf("  SHARED_HOT picked : %ld\n", pick[CLS_SHARED_HOT]);
		printf("  LOCAL_HOT  picked : %ld\n", pick[CLS_LOCAL_HOT]);
		printf("  COLD       picked : %ld\n", pick[CLS_COLD]);
		printf("  TRANSIENT  picked : %ld\n", pick[CLS_TRANSIENT]);
		printf("  --\n");
		/* P1: power-of-D beats random.  Random would pick each class
		 * ~25%; the scorer must pick TRANSIENT/COLD far more than the
		 * hot classes, and SHARED_HOT never. */
		assert(pick[CLS_SHARED_HOT] == 0);
		assert(pick[CLS_TRANSIENT] > 0);
		assert(pick[CLS_COLD] > 0);
		/* LOCAL_HOT is only ever picked when a 32-sample happens to
		 * contain fewer than EVICT_BATCH low-class frames -- rare, so
		 * it must be a negligible fraction (this is exactly where
		 * power-of-D differs from perfect LRU, and it is tiny). */
		assert(pick[CLS_LOCAL_HOT] * 1000L < pick[CLS_COLD] +
		    pick[CLS_TRANSIENT]);
		/* transient scores below cold (extra -W_TRANSIENT), so it is
		 * evicted first and dominates the picks. */
		assert(pick[CLS_TRANSIENT] >= pick[CLS_COLD]);
		/* cold+transient hugely outweigh any hot pick. */
		assert(pick[CLS_COLD] + pick[CLS_TRANSIENT] >
		    100L * (pick[CLS_LOCAL_HOT] + pick[CLS_SHARED_HOT] + 1));
	}

	cooling_init(&cl, POOL_FRAMES + EVICT_BATCH + 4, EVICT_BATCH / 2);

	for (int i = 0; i < POOL_FRAMES; i++)
		frames[i].state = ST_FREE;

	/* -------- Phase 1: warm the pool with hot + warm traffic ------ */
	for (int i = 0; i < 200000; i++) {
		uint64_t p;
		uint64_t r = xrand_n(&rng, 100);
		if (r < 40)			/* 40% hot (Zipf head)   */
			p = PG_HOT_LO + zipf_pick(&rng, HOT_N);
		else				/* 60% warm (uniform)    */
			p = PG_WARM_LO + xrand_n(&rng, WARM_N);
		access_page(frames, &cl, sk, &st, &rng, p);
	}

	int warm_before = warm_resident_count(frames, POOL_FRAMES);
	long scan_bypass_before = st.scan_bypassed;
	long rejected_before = st.admissions_rejected;

	/* Confirm the hot-set root(s) are resident and SHARED_HOT before
	 * the scan -- they must survive it. */
	int hot_resident_before = 0;
	for (int i = 0; i < POOL_FRAMES; i++)
		if (frames[i].state != ST_FREE && is_hot_pg(frames[i].pgid))
			hot_resident_before++;

	/* -------- Phase 2: the one-shot sequential COLD SCAN ---------- */
	for (uint64_t p = PG_SCAN_LO; p < PG_SCAN_LO + SCAN_N; p++)
		access_page(frames, &cl, sk, &st, &rng, p);

	int warm_after = warm_resident_count(frames, POOL_FRAMES);
	long scan_bypass_during = st.scan_bypassed - scan_bypass_before;
	long rejected_during = st.admissions_rejected - rejected_before;

	int hot_resident_after = 0;
	int scan_resident_after = 0;
	for (int i = 0; i < POOL_FRAMES; i++) {
		if (frames[i].state == ST_FREE)
			continue;
		if (is_hot_pg(frames[i].pgid))
			hot_resident_after++;
		if (is_scan_pg(frames[i].pgid))
			scan_resident_after++;
	}

	/* -------- Phase 3: explicit cooling resurrection check -------- */
	/* Cool a known-resident warm page, re-access it before drain,
	 * assert it is reinstated (not lost). */
	long res_before = st.resurrections;
	int probe = -1;
	for (int i = 0; i < POOL_FRAMES; i++)
		if (frames[i].state == ST_RESIDENT &&
		    is_warm_pg(frames[i].pgid)) {
			probe = i;
			break;
		}
	assert(probe >= 0);		/* warm survivor exists          */
	uint64_t probe_pg = frames[probe].pgid;
	cool_page(frames, &cl, probe);
	assert(frames[probe].state == ST_COOLING);
	access_page(frames, &cl, sk, &st, &rng, probe_pg);	/* re-touch */
	assert(frames[probe].state == ST_RESIDENT);	/* reinstated    */
	assert(find_frame(frames, POOL_FRAMES, probe_pg) == probe);
	assert(st.resurrections == res_before + 1);

	/* ---------------- Report ---------------- */
	printf("evict_policy_proto -- results\n");
	printf("  pool frames             : %d\n", POOL_FRAMES);
	printf("  working set (hot+warm)  : %d + %d\n", HOT_N, WARM_N);
	printf("  scan pages (one-shot)   : %d\n", SCAN_N);
	printf("  --\n");
	printf("  hot pages cooled        : %ld\n", st.hot_evicted);
	printf("  cold/scan pages cooled  : %ld\n", st.cold_evicted);
	printf("  warm pages cooled       : %ld\n", st.warm_evicted);
	printf("  SHARED_HOT cooled       : %ld\n", st.shared_evicted);
	printf("  resurrections           : %ld\n", st.resurrections);
	printf("  admissions ok           : %ld\n", st.admissions_ok);
	printf("  admissions rejected     : %ld\n", st.admissions_rejected);
	printf("  scan pages bypassed     : %ld\n", st.scan_bypassed);
	printf("  --\n");
	printf("  hot resident  before/after scan : %d / %d\n",
	    hot_resident_before, hot_resident_after);
	printf("  warm resident before/after scan : %d / %d\n",
	    warm_before, warm_after);
	printf("  scan pages left resident        : %d\n",
	    scan_resident_after);
	printf("  scan-round rejects / bypasses   : %ld / %ld\n",
	    rejected_during, scan_bypass_during);

	/* ---------------- Property assertions ---------------- */

	/* P1: power-of-D beats random -- proven directly in Phase 0
	 * (SHARED_HOT/LOCAL_HOT never picked; TRANSIENT/COLD dominate).
	 * In this integrated run scan pages are bypassed at admission
	 * before they ever become resident, so the pool holds only the
	 * hot+warm working set; SHARED_HOT is still never cooled (P2). */

	/* P2: SHARED_HOT pages are effectively never evicted. */
	assert(st.shared_evicted == 0);

	/* P3: cooling resurrection works (asserted inline in phase 3). */
	assert(st.resurrections >= 1);

	/* P4 (headline): admission scan-resistance -- the warm working
	 * set survives the scan.  The cold scan is bypassed (doorkeeper
	 * filters first-sight pages) rather than admitted at the warm
	 * set's expense. */
	assert(scan_bypass_during > 0);		/* scan was filtered     */
	assert(rejected_during > 0);
	assert(scan_resident_after * 4 < warm_after);	/* scan didn't take over */
	/* Warm residency preserved: only Bloom-collision noise may churn. */
	assert(warm_after * 100 >= warm_before * 95);	/* >= 95% retained */
	assert(warm_after > POOL_FRAMES / 2);		/* still dominant */

	/* P5: hot-set root survives the scan intact. */
	assert(hot_resident_after >= hot_resident_before);
	assert(hot_resident_after == HOT_N);		/* all hot present */

	/* P5: hot-set root survives the scan intact. */
	assert(hot_resident_after >= hot_resident_before);
	assert(hot_resident_after == HOT_N);		/* all hot present */

	printf("\nALL PROPERTIES PROVEN -- asserts passed.\n");

	cooling_free(&cl);
	free(sk);
	return 0;
}
