/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/evt/sim.c
 *	Deterministic Simulation Testing (DST) core.  See docs/M_DST.md.
 *
 *	Phase 0: the seeded PRNG tree.  Later phases add the virtual clock
 *	and the single-thread deterministic scheduler that drives the N
 *	loops as N fibers under a seed-determined interleaving.
 *
 *	The PRNG is a per-stream splitmix64: the root seed mixes with the
 *	stream id to give each stream an independent, well-distributed
 *	sequence, so a draw added at one decision site never perturbs the
 *	sequence another site observes (stable replay under code change).
 */

#include "xtc_int.h"
#include "xtc_sim.h"

#include <stdatomic.h>
#include <pthread.h>
#include <string.h>

/* Activation state is process-global and read on hot paths, so it is a
 * relaxed atomic flag rather than a lock. */
static _Atomic int      g_sim_active;
static uint64_t         g_sim_seed;
static uint64_t         g_sim_stream[XTC_SIM_RNG_NSTREAMS];
/* Adversarial-mode knobs (see xtc_sim.h); defined early because
 * xtc_sim_deactivate resets them.  Both OFF (0) by default. */
static _Atomic int      g_sched_pessimal_pct;   /* per-1000; 0 = uniform */
static _Atomic int      g_swizzle_pct;          /* per-1000; 0 = no reorder */

/* Semantic consistency check (FoundationDB's end-of-test consistency
 * workload): an optional callback the sim runs at quiescence, AFTER all
 * the seeded chaos, to assert a GLOBAL application invariant (e.g. "the
 * B-tree is still a valid B-tree and holds exactly the acked-commit
 * set") that a per-step structural state hash cannot see.  NULL in
 * production and by default.  Set before a run; the sim invokes it once
 * when the run reaches quiescence and treats a nonzero return as a
 * failure.  Not reset by deactivate (the caller re-sets per run, like
 * the fault knobs). */
static xtc_sim_consistency_fn g_consistency_fn;
static void                 *g_consistency_arg;

/* Virtual (logical) clock.  When sim is active and the clock mode is
 * VIRTUAL, __os_clock_mono returns this instead of the host monotonic
 * clock, so time is a pure function of the schedule (hence the seed).
 * The scheduler advances it (later phase); phase 0/1 just establish the
 * seam and let a test drive it manually. */
static _Atomic int      g_vclock_on;
static _Atomic int64_t  g_vclock_ns;

/* splitmix64: the standard finalizer used to derive independent streams
 * from a seed (the 0x9E3779B97F4A7C15 increment is the golden-ratio
 * constant already used elsewhere in the tree). */
static uint64_t
splitmix64(uint64_t *s)
{
	uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

/* PUBLIC: int __xtc_sim_active __P((void)); */
int
__xtc_sim_active(void)
{
	return atomic_load_explicit(&g_sim_active, memory_order_relaxed);
}

/* PUBLIC: void xtc_sim_activate __P((uint64_t)); */
void
xtc_sim_activate(uint64_t seed)
{
	int i;
	uint64_t s = seed ? seed : 0x9E3779B97F4A7C15ull;
	g_sim_seed = s;
	/* Derive each stream's initial state from the root seed so the
	 * streams are independent yet fully determined by the seed. */
	for (i = 0; i < XTC_SIM_RNG_NSTREAMS; i++) {
		uint64_t t = s + (uint64_t)(i + 1) * 0x9E3779B97F4A7C15ull;
		g_sim_stream[i] = splitmix64(&t);
	}
	atomic_store_explicit(&g_sim_active, 1, memory_order_release);
}

/* PUBLIC: void xtc_sim_deactivate __P((void)); */
void
xtc_sim_deactivate(void)
{
	atomic_store_explicit(&g_sim_active, 0, memory_order_release);
	/* Reset adversarial-mode knobs so one run's setting does not leak
	 * into the next in-process run (buggify/io-fault knobs are reset by
	 * their own enable/disable; these have no such call on every run). */
	atomic_store_explicit(&g_sched_pessimal_pct, 0, memory_order_release);
	atomic_store_explicit(&g_swizzle_pct, 0, memory_order_release);
	g_consistency_fn = NULL;
	g_consistency_arg = NULL;
}

/* PUBLIC: uint64_t __xtc_sim_rng __P((int)); */
uint64_t
__xtc_sim_rng(int s)
{
	if (s < 0 || s >= XTC_SIM_RNG_NSTREAMS)
		s = XTC_SIM_RNG_APP;
	return splitmix64(&g_sim_stream[s]);
}

/* PUBLIC: uint64_t __xtc_sim_rng_range __P((int, uint64_t)); */
uint64_t
__xtc_sim_rng_range(int s, uint64_t bound)
{
	if (bound == 0)
		return 0;
	/* Unbiased enough for scheduling decisions; the modulo bias over a
	 * 64-bit draw with a small bound is negligible and -- crucially --
	 * deterministic. */
	return __xtc_sim_rng(s) % bound;
}

/* ---- seeded fault injection ---- */

/* PUBLIC: int xtc_sim_fault __P((unsigned)); */
/*
 * Deterministic fault toggle: returns 1 with probability `pct_per_1000`
 * in 1000 (e.g. 50 == 5%), drawn from the dedicated FAULT stream so
 * enabling faults does not perturb the schedule stream (per-stream
 * isolation == stable replay regardless of fault config).  Returns 0
 * when sim is inactive (no faults in production).  A test calls this at
 * a fault decision point -- e.g. "if (xtc_sim_fault(50)) inject a short
 * read" -- and the SAME seed reproduces the identical fault schedule.
 */
int
xtc_sim_fault(unsigned pct_per_1000)
{
	if (!__xtc_sim_active())
		return 0;
	if (pct_per_1000 == 0)
		return 0;
	if (pct_per_1000 >= 1000)
		return 1;
	return __xtc_sim_rng_range(XTC_SIM_RNG_FAULT, 1000) < pct_per_1000;
}

/* ---- critical-section fault points ----
 *
 * A fault POINT marks an interleaving-sensitive critical section (a
 * lock CAS, an inbox push, a steal, a mailbox enqueue).  Under sim,
 * when fault points are enabled, reaching a point draws from the FAULT
 * stream and -- on a hit -- records the fire in a small per-name table.
 * A DST test enables the points, runs the workload under the seeded
 * scheduler, and verifies (a) the SAME seed fires the SAME points the
 * SAME number of times (deterministic critical-section coverage), and
 * (b) the run still reaches quiescence and replays.  Because the draw
 * is on the dedicated FAULT stream, turning points on/off does not
 * perturb the schedule stream.
 *
 * The fire count is the coverage signal: a point that never fires
 * across a seed sweep is a critical section the sim never exercised
 * under fault timing.  Production never reaches this (sim inactive).
 */
#define XTC_SIM_FP_MAX 64
static _Atomic int      g_fp_on;          /* fault points enabled */
static unsigned         g_fp_pct;         /* per-1000 fire probability */
static char             g_fp_name[XTC_SIM_FP_MAX][48];
static _Atomic uint64_t g_fp_fires[XTC_SIM_FP_MAX];
static _Atomic int      g_fp_n;           /* number of distinct names seen */
static pthread_mutex_t  g_fp_lock = PTHREAD_MUTEX_INITIALIZER;

/* Find (or register) the slot for `name`.  Returns the index, or -1 if
 * the table is full.  Caller holds g_fp_lock. */
static int
__fp_slot_locked(const char *name)
{
	int i, n = atomic_load_explicit(&g_fp_n, memory_order_relaxed);
	for (i = 0; i < n; i++)
		if (strncmp(g_fp_name[i], name, sizeof g_fp_name[0]) == 0)
			return i;
	if (n >= XTC_SIM_FP_MAX)
		return -1;
	(void)strncpy(g_fp_name[n], name, sizeof g_fp_name[0] - 1);
	g_fp_name[n][sizeof g_fp_name[0] - 1] = '\0';
	atomic_store_explicit(&g_fp_fires[n], 0, memory_order_relaxed);
	atomic_store_explicit(&g_fp_n, n + 1, memory_order_relaxed);
	return n;
}

/* PUBLIC: void xtc_sim_fault_points_enable __P((unsigned)); */
void
xtc_sim_fault_points_enable(unsigned pct_per_1000)
{
	(void)pthread_mutex_lock(&g_fp_lock);
	atomic_store_explicit(&g_fp_n, 0, memory_order_relaxed);
	g_fp_pct = pct_per_1000;
	atomic_store_explicit(&g_fp_on, 1, memory_order_release);
	(void)pthread_mutex_unlock(&g_fp_lock);
}

/* PUBLIC: void xtc_sim_fault_points_disable __P((void)); */
void
xtc_sim_fault_points_disable(void)
{
	atomic_store_explicit(&g_fp_on, 0, memory_order_release);
}

/* PUBLIC: int xtc_sim_fault_point __P((const char *)); */
/*
 * Reach a critical-section fault point.  Returns 1 (and records a fire)
 * when sim + fault points are active AND the FAULT-stream draw hits;
 * else 0.  The XTC_SIM_FAULT_POINT(name) macro (xtc_sim.h) wraps this
 * and compiles to nothing in a build with sim disabled at the call
 * site only by the __xtc_sim_active() fast-out -- the call itself is a
 * single relaxed load in production.
 */
int
xtc_sim_fault_point(const char *name)
{
	int slot, hit;
	if (name == NULL)
		return 0;
	if (!atomic_load_explicit(&g_fp_on, memory_order_acquire))
		return 0;
	if (!__xtc_sim_active())
		return 0;
	hit = (__xtc_sim_rng_range(XTC_SIM_RNG_FAULT, 1000) < g_fp_pct);
	(void)pthread_mutex_lock(&g_fp_lock);
	slot = __fp_slot_locked(name);
	if (slot >= 0 && hit)
		atomic_fetch_add_explicit(&g_fp_fires[slot], 1,
		    memory_order_relaxed);
	else if (slot < 0)
		hit = 0;   /* table full: cannot record, treat as no fire */
	(void)pthread_mutex_unlock(&g_fp_lock);
	return hit;
}

/* PUBLIC: uint64_t xtc_sim_fault_point_fires __P((const char *)); */
/* Total fires recorded for `name` since the last enable.  0 if unknown. */
uint64_t
xtc_sim_fault_point_fires(const char *name)
{
	int i, n;
	uint64_t v = 0;
	if (name == NULL)
		return 0;
	(void)pthread_mutex_lock(&g_fp_lock);
	n = atomic_load_explicit(&g_fp_n, memory_order_relaxed);
	for (i = 0; i < n; i++)
		if (strncmp(g_fp_name[i], name, sizeof g_fp_name[0]) == 0) {
			v = atomic_load_explicit(&g_fp_fires[i],
			    memory_order_relaxed);
			break;
		}
	(void)pthread_mutex_unlock(&g_fp_lock);
	return v;
}

/* PUBLIC: int xtc_sim_fault_points_seen __P((void)); */
/* Number of distinct fault-point names reached since the last enable
 * (the coverage breadth: how many distinct critical sections the run
 * actually executed). */
int
xtc_sim_fault_points_seen(void)
{
	return atomic_load_explicit(&g_fp_n, memory_order_relaxed);
}

/* ---- Buggify (DST) ----
 *
 * FoundationDB's "buggify": a named point in the REAL runtime code that,
 * under sim, lets the code take a legal-but-pessimal path (a delay, the
 * smallest legal buffer, an early spurious return, the slow branch).
 * The key semantics differ from xtc_sim_fault (a fresh draw per call):
 * a buggify point is a coin flipped ONCE per run per site -- the first
 * time a named point is reached the seeded coin decides, and every
 * later hit of that name returns the SAME decision that run.  So a
 * buggified site behaves consistently within a run and the whole run
 * replays from the seed.  Off (returns 0) in production and when
 * buggify is not enabled.  Drawn from the dedicated BUGGIFY stream, so
 * enabling buggify does not perturb the scheduling or FAULT streams
 * (the critical-section / fault-point tests replay against FAULT
 * unaffected).
 */
#define XTC_SIM_BUG_MAX 128
static _Atomic int      g_bug_on;         /* buggify enabled */
static unsigned         g_bug_pct;        /* per-1000 activation probability */
static char             g_bug_name[XTC_SIM_BUG_MAX][48];
static signed char      g_bug_decided[XTC_SIM_BUG_MAX]; /* -1 undecided, 0/1 */
static _Atomic int      g_bug_n;
static pthread_mutex_t  g_bug_lock = PTHREAD_MUTEX_INITIALIZER;

/* Adversarial scheduler bias + completion/message swizzle (see
 * xtc_sim.h).  State defined near the top (xtc_sim_deactivate resets
 * it); the accessors follow. */

/* PUBLIC: void xtc_sim_sched_pessimal __P((unsigned)); */
void
xtc_sim_sched_pessimal(unsigned pct_per_1000)
{
	if (pct_per_1000 > 1000) pct_per_1000 = 1000;
	atomic_store_explicit(&g_sched_pessimal_pct, (int)pct_per_1000,
	    memory_order_release);
}

/* PUBLIC: int __xtc_sim_sched_pessimal_pct __P((void)); */
int
__xtc_sim_sched_pessimal_pct(void)
{
	return atomic_load_explicit(&g_sched_pessimal_pct,
	    memory_order_acquire);
}

/* PUBLIC: void xtc_sim_swizzle_enable __P((unsigned)); */
void
xtc_sim_swizzle_enable(unsigned pct_per_1000)
{
	if (pct_per_1000 > 1000) pct_per_1000 = 1000;
	atomic_store_explicit(&g_swizzle_pct, (int)pct_per_1000,
	    memory_order_release);
}

/* PUBLIC: void xtc_sim_swizzle_disable __P((void)); */
void
xtc_sim_swizzle_disable(void)
{
	atomic_store_explicit(&g_swizzle_pct, 0, memory_order_release);
}

/* PUBLIC: int __xtc_sim_swizzle_pct __P((void)); */
int
__xtc_sim_swizzle_pct(void)
{
	return atomic_load_explicit(&g_swizzle_pct, memory_order_acquire);
}

/* PUBLIC: void xtc_sim_set_consistency_check __P((xtc_sim_consistency_fn, void *)); */
void
xtc_sim_set_consistency_check(xtc_sim_consistency_fn fn, void *arg)
{
	g_consistency_fn = fn;
	g_consistency_arg = arg;
}

/* PUBLIC: int __xtc_sim_run_consistency_check __P((void)); */
/* Invoke the installed consistency check (if any).  Returns XTC_OK when
 * none is installed or it passes; the callback's nonzero code otherwise.
 * Called by the sim executor at quiescence. */
int
__xtc_sim_run_consistency_check(void)
{
	xtc_sim_consistency_fn fn = g_consistency_fn;
	if (fn == NULL)
		return XTC_OK;
	return fn(g_consistency_arg);
}

/* PUBLIC: void xtc_sim_buggify_enable __P((unsigned)); */
void
xtc_sim_buggify_enable(unsigned pct_per_1000)
{
	(void)pthread_mutex_lock(&g_bug_lock);
	atomic_store_explicit(&g_bug_n, 0, memory_order_relaxed);
	g_bug_pct = pct_per_1000;
	atomic_store_explicit(&g_bug_on, 1, memory_order_release);
	(void)pthread_mutex_unlock(&g_bug_lock);
}

/* PUBLIC: void xtc_sim_buggify_disable __P((void)); */
void
xtc_sim_buggify_disable(void)
{
	(void)pthread_mutex_lock(&g_bug_lock);
	atomic_store_explicit(&g_bug_on, 0, memory_order_release);
	/* Reset the decision table so a subsequent run (or an
	 * active-count query while disabled) reflects no activations. */
	atomic_store_explicit(&g_bug_n, 0, memory_order_relaxed);
	(void)pthread_mutex_unlock(&g_bug_lock);
}

/* PUBLIC: int xtc_sim_buggify __P((const char *)); */
/*
 * Return the once-per-run decision for buggify point `name`: 1 to take
 * the pessimal path, 0 otherwise.  0 in production / when disabled.  The
 * decision is decided on first reach (seeded coin) and cached, so all
 * reaches of the same name in one run agree.
 */
int
xtc_sim_buggify(const char *name)
{
	int i, n, decision;
	if (name == NULL)
		return 0;
	if (!atomic_load_explicit(&g_bug_on, memory_order_acquire))
		return 0;
	if (!__xtc_sim_active())
		return 0;
	(void)pthread_mutex_lock(&g_bug_lock);
	n = atomic_load_explicit(&g_bug_n, memory_order_relaxed);
	for (i = 0; i < n; i++)
		if (strncmp(g_bug_name[i], name, sizeof g_bug_name[0]) == 0) {
			decision = g_bug_decided[i];
			(void)pthread_mutex_unlock(&g_bug_lock);
			return decision;
		}
	/* First reach: flip the seeded coin and cache it. */
	if (n >= XTC_SIM_BUG_MAX) {
		(void)pthread_mutex_unlock(&g_bug_lock);
		return 0;     /* table full: no buggify */
	}
	if (g_bug_pct == 0)
		decision = 0;
	else if (g_bug_pct >= 1000)
		decision = 1;
	else
		decision = __xtc_sim_rng_range(XTC_SIM_RNG_BUGGIFY, 1000) <
		    g_bug_pct;
	(void)strncpy(g_bug_name[n], name, sizeof g_bug_name[0] - 1);
	g_bug_name[n][sizeof g_bug_name[0] - 1] = '\0';
	g_bug_decided[n] = (signed char)decision;
	atomic_store_explicit(&g_bug_n, n + 1, memory_order_relaxed);
	(void)pthread_mutex_unlock(&g_bug_lock);
	return decision;
}

/* PUBLIC: int xtc_sim_buggify_active_count __P((void)); */
/* Number of buggify points that were reached AND activated (decided 1)
 * this run -- the coverage signal: how many pessimal paths the run
 * actually took. */
int
xtc_sim_buggify_active_count(void)
{
	int i, n, c = 0;
	(void)pthread_mutex_lock(&g_bug_lock);
	n = atomic_load_explicit(&g_bug_n, memory_order_relaxed);
	for (i = 0; i < n; i++)
		if (g_bug_decided[i] == 1)
			c++;
	(void)pthread_mutex_unlock(&g_bug_lock);
	return c;
}

/* PUBLIC: int xtc_sim_buggify_reached_count __P((void)); */
/* Number of buggify points REACHED this run (activated or not) -- the
 * denominator for a coverage ratio.  A known site that is never in this
 * count across a whole seed sweep is unreachable (dead code, or the
 * workload never exercises its path). */
int
xtc_sim_buggify_reached_count(void)
{
	int n;
	(void)pthread_mutex_lock(&g_bug_lock);
	n = atomic_load_explicit(&g_bug_n, memory_order_relaxed);
	(void)pthread_mutex_unlock(&g_bug_lock);
	return n;
}

/* PUBLIC: int xtc_sim_buggify_site __P((int, char *, size_t, int *)); */
/* Read the i-th buggify site reached this run: copies its name into buf
 * and (if out_activated != NULL) reports whether its seeded coin came up
 * active.  Returns XTC_OK for a valid index, XTC_E_INVAL past the end.
 * A sweep driver iterates 0..reached_count-1 to build a per-seed and
 * aggregate coverage map (which sites the sweep hit, and how often each
 * activated) -- FoundationDB-style fault-space coverage tracking. */
int
xtc_sim_buggify_site(int idx, char *buf, size_t buflen, int *out_activated)
{
	int n, rc = XTC_E_INVAL;
	(void)pthread_mutex_lock(&g_bug_lock);
	n = atomic_load_explicit(&g_bug_n, memory_order_relaxed);
	if (idx >= 0 && idx < n) {
		if (buf != NULL && buflen > 0) {
			(void)strncpy(buf, g_bug_name[idx], buflen - 1);
			buf[buflen - 1] = '\0';
		}
		if (out_activated != NULL)
			*out_activated = (g_bug_decided[idx] == 1);
		rc = XTC_OK;
	}
	(void)pthread_mutex_unlock(&g_bug_lock);
	return rc;
}

/* PUBLIC: int xtc_sim_buggify_fault __P((unsigned)); */
/*
 * Per-CALL buggify coin drawn from the dedicated BUGGIFY stream (NOT
 * the FAULT stream), so a buggify site's per-call "fire this time?"
 * draw never perturbs the FAULT-stream sequence the critical-section /
 * fault-point tests replay against.  Returns 1 with probability
 * pct_per_1000/1000, but ONLY when buggify is enabled and sim is active
 * -- so a site written as
 *     if (XTC_SIM_BUGGIFY(name) && xtc_sim_buggify_fault(250)) { ... }
 * draws from the BUGGIFY stream exclusively when its once-per-run coin
 * is armed AND landed 1, and draws nothing at all when buggify is off
 * (the && short-circuits on XTC_SIM_BUGGIFY == 0).  Newer sites use
 * this in place of xtc_sim_fault so they cannot desync unrelated tests.
 */
int
xtc_sim_buggify_fault(unsigned pct_per_1000)
{
	if (!atomic_load_explicit(&g_bug_on, memory_order_acquire))
		return 0;
	if (!__xtc_sim_active())
		return 0;
	if (pct_per_1000 == 0)
		return 0;
	if (pct_per_1000 >= 1000)
		return 1;
	return __xtc_sim_rng_range(XTC_SIM_RNG_BUGGIFY, 1000) < pct_per_1000;
}

/* ---- simulated I/O faults (DST) ----
 *
 * Control knobs the sim I/O backend (io_sim.c) consults so file-AIO
 * completions are DEFERRED by a seeded latency (making completion ORDER
 * across concurrent ops part of the replayable schedule) and may carry
 * a seeded fault (short read/write or EIO).  Off by default: a sim run
 * that does not enable them gets the simple inline completion, so
 * existing single-fiber sim AIO is unchanged.  All seeded draws are on
 * the dedicated IO stream, so enabling I/O faults does not perturb the
 * scheduling streams (stable replay regardless of I/O config).
 */
static _Atomic int     g_io_faults_on;
static _Atomic int64_t g_io_lat_min_ns;
static _Atomic int64_t g_io_lat_max_ns;
static unsigned        g_io_fault_pct;     /* per-1000 op fault probability */

/* PUBLIC: void xtc_sim_io_faults_enable __P((int64_t, int64_t, unsigned)); */
void
xtc_sim_io_faults_enable(int64_t lat_min_ns, int64_t lat_max_ns,
    unsigned fault_pct_per_1000)
{
	if (lat_min_ns < 0) lat_min_ns = 0;
	if (lat_max_ns < lat_min_ns) lat_max_ns = lat_min_ns;
	atomic_store_explicit(&g_io_lat_min_ns, lat_min_ns, memory_order_relaxed);
	atomic_store_explicit(&g_io_lat_max_ns, lat_max_ns, memory_order_relaxed);
	g_io_fault_pct = fault_pct_per_1000;
	atomic_store_explicit(&g_io_faults_on, 1, memory_order_release);
}

/* PUBLIC: void xtc_sim_io_faults_disable __P((void)); */
void
xtc_sim_io_faults_disable(void)
{
	atomic_store_explicit(&g_io_faults_on, 0, memory_order_release);
}

/* PUBLIC: int __xtc_sim_io_faults_active __P((void)); */
int
__xtc_sim_io_faults_active(void)
{
	return atomic_load_explicit(&g_io_faults_on, memory_order_acquire);
}

/* PUBLIC: int64_t __xtc_sim_io_latency __P((void)); */
/* A seeded I/O latency in [lat_min, lat_max] ns from the IO stream.
 * 0 when I/O faults are off. */
int64_t
__xtc_sim_io_latency(void)
{
	int64_t lo, hi;
	if (!__xtc_sim_io_faults_active() || !__xtc_sim_active())
		return 0;
	lo = atomic_load_explicit(&g_io_lat_min_ns, memory_order_relaxed);
	hi = atomic_load_explicit(&g_io_lat_max_ns, memory_order_relaxed);
	if (hi <= lo)
		return lo;
	return lo + (int64_t)__xtc_sim_rng_range(XTC_SIM_RNG_IO,
	    (uint64_t)(hi - lo + 1));
}

/* PUBLIC: int __xtc_sim_io_should_fault __P((void)); */
/* 1 if this op should carry a seeded I/O fault (drawn from the IO
 * stream), 0 otherwise / when off. */
int
__xtc_sim_io_should_fault(void)
{
	if (!__xtc_sim_io_faults_active() || !__xtc_sim_active())
		return 0;
	if (g_io_fault_pct == 0)
		return 0;
	if (g_io_fault_pct >= 1000)
		return 1;
	return __xtc_sim_rng_range(XTC_SIM_RNG_IO, 1000) < g_io_fault_pct;
}

/* ---- simulated TORN / CORRUPT writes and reads (DST) ----
 *
 * The torn-page fault class FoundationDB models, distinct from the
 * short-transfer / EIO faults above.  A short transfer reports fewer
 * bytes and moves fewer bytes (the caller re-issues the remainder --
 * clean).  A TORN WRITE instead PERSISTS only a prefix of the buffer
 * while REPORTING full success, and a CORRUPT READ flips a byte in the
 * returned data -- both leave latent bad bytes a checksum must catch.
 * Off by default; seeded on the IO stream so enabling does not perturb
 * the schedule.
 */
static _Atomic int g_io_corrupt_on;
static unsigned    g_io_corrupt_pct;    /* per-1000 op corruption probability */

/* PUBLIC: void xtc_sim_io_corrupt_enable __P((unsigned)); */
void
xtc_sim_io_corrupt_enable(unsigned corrupt_pct_per_1000)
{
	g_io_corrupt_pct = corrupt_pct_per_1000;
	atomic_store_explicit(&g_io_corrupt_on, 1, memory_order_release);
}

/* PUBLIC: void xtc_sim_io_corrupt_disable __P((void)); */
void
xtc_sim_io_corrupt_disable(void)
{
	atomic_store_explicit(&g_io_corrupt_on, 0, memory_order_release);
}

/* PUBLIC: int __xtc_sim_io_corrupt_active __P((void)); */
int
__xtc_sim_io_corrupt_active(void)
{
	return atomic_load_explicit(&g_io_corrupt_on, memory_order_acquire);
}

/* Draw whether this op is corrupted (per-1000, IO stream). */
static int
__io_should_corrupt(void)
{
	if (!__xtc_sim_io_corrupt_active() || !__xtc_sim_active())
		return 0;
	if (g_io_corrupt_pct == 0)
		return 0;
	if (g_io_corrupt_pct >= 1000)
		return 1;
	return __xtc_sim_rng_range(XTC_SIM_RNG_IO, 1000) < g_io_corrupt_pct;
}

/* PUBLIC: int __xtc_sim_io_torn_prefix __P((int)); */
/*
 * For a write of full_len bytes, return the number of bytes that
 * actually PERSIST when this write is torn: a seeded value in
 * [1, full_len-1] (a strict prefix -- a torn write always loses at
 * least the last byte).  Returns full_len (untorn) when corruption is
 * off, not selected this op, or full_len < 2 (nothing to tear).  The
 * caller writes only the returned prefix but still reports full success
 * -- exactly a torn page.  Seeded on the IO stream.
 */
int
__xtc_sim_io_torn_prefix(int full_len)
{
	if (full_len < 2 || !__io_should_corrupt())
		return full_len;
	return 1 + (int)__xtc_sim_rng_range(XTC_SIM_RNG_IO,
	    (uint64_t)(full_len - 1));
}

/* PUBLIC: int __xtc_sim_io_flip_byte __P((int)); */
/*
 * For a read that returned `len` bytes, return the index of a byte to
 * bit-flip when this read is corrupted, or -1 for no corruption (off,
 * not selected, or len <= 0).  The caller XORs one bit into that byte
 * of the returned buffer -- a silent corrupt read a checksum must
 * detect.  Seeded on the IO stream.
 */
int
__xtc_sim_io_flip_byte(int len)
{
	if (len <= 0 || !__io_should_corrupt())
		return -1;
	return (int)__xtc_sim_rng_range(XTC_SIM_RNG_IO, (uint64_t)len);
}

/* ---- simulated network partition + message latency (DST) ----
 *
 * A seeded, deterministic model of a partitioned / lossy network at the
 * cross-LOOP message granularity xtc's sim actually models: xtc_send
 * between procs on different loops (one exec) routes through
 * __mbox_deliver (proc.c), the single cross-loop delivery seam.  These
 * knobs let a DST test cut message flow between groups of loops (a
 * partition) and/or defer each cross-loop delivery by a seeded latency
 * (so delivery ORDER is part of the replayable schedule) -- FoundationDB
 * network-partition testing at loop granularity.
 *
 * Scope (honest limitation): this covers the in-process cross-loop
 * message path ONLY.  xtc's real cross-MACHINE transport is raw sockets
 * (io_net.c), which cannot run under the single-thread sim; simulating
 * sockets/TLS is a separate, larger effort and is NOT modelled here.
 *
 * Partition matrix: a directed adjacency of "loop i cannot reach loop
 * j".  Loops are identified by pid.loop_id (== exec_id + 1, or 0 for a
 * standalone loop), so the matrix is indexed by loop_id in
 * [0, XTC_SIM_PART_MAX).  A blocked edge causes __mbox_deliver to DROP
 * the message the same way a soft-full mailbox does (return XTC_E_AGAIN;
 * the sender already handles that path), so a partitioned peer never
 * deadlocks the sim.  Off by default (no edge blocked).
 *
 * Latency: when a [min,max] window is set, each cross-loop delivery is
 * deferred to now + a seeded draw from the IO stream (reusing the IO
 * stream -- not a new PRNG stream -- so enabling latency does not
 * perturb the schedule).  Off by default (min==max==0: inline delivery).
 */
#define XTC_SIM_PART_MAX 64        /* == LOOP_TABLE_MAX in proc.c */
static _Atomic int      g_part_on;
static unsigned char    g_part_block[XTC_SIM_PART_MAX][XTC_SIM_PART_MAX];
static _Atomic int64_t  g_net_lat_min_ns;
static _Atomic int64_t  g_net_lat_max_ns;
static pthread_mutex_t  g_part_lock = PTHREAD_MUTEX_INITIALIZER;

/* PUBLIC: void xtc_sim_partition_set __P((int, int, int)); */
/*
 * Set (or clear) the directed edge src_loop_id -> dst_loop_id in the
 * partition matrix: when `blocked` is nonzero a cross-loop message from
 * a proc on src_loop_id to a proc on dst_loop_id is dropped under sim.
 * loop_id is pid.loop_id (exec_id + 1; 0 == standalone).  For a
 * symmetric cut call it both ways.  Enables the partition subsystem on
 * first blocking edge.  Out-of-range ids are ignored.
 */
void
xtc_sim_partition_set(int src_loop_id, int dst_loop_id, int blocked)
{
	if (src_loop_id < 0 || src_loop_id >= XTC_SIM_PART_MAX ||
	    dst_loop_id < 0 || dst_loop_id >= XTC_SIM_PART_MAX)
		return;
	(void)pthread_mutex_lock(&g_part_lock);
	g_part_block[src_loop_id][dst_loop_id] = blocked ? 1 : 0;
	(void)pthread_mutex_unlock(&g_part_lock);
	if (blocked)
		atomic_store_explicit(&g_part_on, 1, memory_order_release);
}

/* PUBLIC: void xtc_sim_partition_isolate __P((int)); */
/*
 * Cut loop `loop_id` off from every other loop in both directions (a
 * fully-partitioned / "dead machine" peer).  Self-delivery (same loop)
 * is left reachable so a proc can still message peers on its own loop.
 */
void
xtc_sim_partition_isolate(int loop_id)
{
	int j;
	if (loop_id < 0 || loop_id >= XTC_SIM_PART_MAX)
		return;
	(void)pthread_mutex_lock(&g_part_lock);
	for (j = 0; j < XTC_SIM_PART_MAX; j++) {
		if (j == loop_id)
			continue;
		g_part_block[loop_id][j] = 1;
		g_part_block[j][loop_id] = 1;
	}
	(void)pthread_mutex_unlock(&g_part_lock);
	atomic_store_explicit(&g_part_on, 1, memory_order_release);
}

/* PUBLIC: void xtc_sim_partition_clear __P((void)); */
/* Heal the network: clear every edge and disable the partition matrix
 * (all cross-loop messages deliver again).  Also clears the latency
 * window. */
void
xtc_sim_partition_clear(void)
{
	(void)pthread_mutex_lock(&g_part_lock);
	memset(g_part_block, 0, sizeof g_part_block);
	(void)pthread_mutex_unlock(&g_part_lock);
	atomic_store_explicit(&g_part_on, 0, memory_order_release);
	atomic_store_explicit(&g_net_lat_min_ns, 0, memory_order_relaxed);
	atomic_store_explicit(&g_net_lat_max_ns, 0, memory_order_relaxed);
}

/* PUBLIC: int __xtc_sim_partition_blocked __P((int, int)); */
/*
 * 1 if a cross-loop message from src_loop_id to dst_loop_id must be
 * dropped under the active partition; 0 otherwise / when sim or the
 * partition matrix is inactive.  The single seam __mbox_deliver
 * consults.  A single relaxed load in the common (no-partition) case.
 */
int
__xtc_sim_partition_blocked(int src_loop_id, int dst_loop_id)
{
	int b;
	if (!atomic_load_explicit(&g_part_on, memory_order_acquire))
		return 0;
	if (!__xtc_sim_active())
		return 0;
	if (src_loop_id < 0 || src_loop_id >= XTC_SIM_PART_MAX ||
	    dst_loop_id < 0 || dst_loop_id >= XTC_SIM_PART_MAX)
		return 0;
	(void)pthread_mutex_lock(&g_part_lock);
	b = g_part_block[src_loop_id][dst_loop_id];
	(void)pthread_mutex_unlock(&g_part_lock);
	return b;
}

/* PUBLIC: void xtc_sim_net_latency __P((int64_t, int64_t)); */
/*
 * Set the per-message cross-loop latency window [min,max] ns.  When
 * nonzero, __mbox_deliver defers each surviving (not-partitioned) cross-
 * loop delivery to now + a seeded draw in [min,max], so delivery ORDER
 * across concurrent sends becomes part of the replayable schedule (the
 * fiber's peer genuinely observes the message later in virtual time).
 * min==max==0 (the default) delivers inline.  Seeded on the IO stream.
 */
void
xtc_sim_net_latency(int64_t min_ns, int64_t max_ns)
{
	if (min_ns < 0) min_ns = 0;
	if (max_ns < min_ns) max_ns = min_ns;
	atomic_store_explicit(&g_net_lat_min_ns, min_ns, memory_order_relaxed);
	atomic_store_explicit(&g_net_lat_max_ns, max_ns, memory_order_relaxed);
}

/* PUBLIC: int64_t __xtc_sim_net_latency __P((void)); */
/* A seeded per-message latency in [min,max] ns drawn from the IO stream,
 * or 0 when sim inactive or no latency window is set (inline delivery).
 * __mbox_deliver consults this to decide whether to defer a delivery. */
int64_t
__xtc_sim_net_latency(void)
{
	int64_t lo, hi;
	if (!__xtc_sim_active())
		return 0;
	lo = atomic_load_explicit(&g_net_lat_min_ns, memory_order_relaxed);
	hi = atomic_load_explicit(&g_net_lat_max_ns, memory_order_relaxed);
	if (hi <= 0)
		return 0;
	if (hi <= lo)
		return lo;
	return lo + (int64_t)__xtc_sim_rng_range(XTC_SIM_RNG_IO,
	    (uint64_t)(hi - lo + 1));
}

/* ---- virtual clock ---- */

/* PUBLIC: void xtc_sim_clock_enable __P((int64_t)); */
void
xtc_sim_clock_enable(int64_t start_ns)
{
	atomic_store_explicit(&g_vclock_ns, start_ns, memory_order_relaxed);
	atomic_store_explicit(&g_vclock_on, 1, memory_order_release);
}

/* PUBLIC: void xtc_sim_clock_disable __P((void)); */
void
xtc_sim_clock_disable(void)
{
	atomic_store_explicit(&g_vclock_on, 0, memory_order_release);
}

/* PUBLIC: void xtc_sim_clock_advance __P((int64_t)); */
void
xtc_sim_clock_advance(int64_t delta_ns)
{
	if (delta_ns > 0)
		(void)atomic_fetch_add_explicit(&g_vclock_ns, delta_ns,
		    memory_order_relaxed);
}

/* PUBLIC: void xtc_sim_clock_set __P((int64_t)); */
void
xtc_sim_clock_set(int64_t ns)
{
	atomic_store_explicit(&g_vclock_ns, ns, memory_order_relaxed);
}

/*
 * Query the virtual clock.  Returns 1 and writes *out_ns when the
 * virtual clock is active; returns 0 otherwise (caller uses the host
 * clock).  __os_clock_mono consults this -- the single seam point.
 *
 * PUBLIC: int __xtc_sim_vclock __P((int64_t *));
 */
int
__xtc_sim_vclock(int64_t *out_ns)
{
	if (!atomic_load_explicit(&g_vclock_on, memory_order_acquire))
		return 0;
	if (out_ns != NULL)
		*out_ns = atomic_load_explicit(&g_vclock_ns,
		    memory_order_relaxed);
	return 1;
}
