/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/evt/sim.c
 *	Deterministic Simulation Testing (DST) core.
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
#include "sim_int.h"
#include "xtc_io.h"         /* XTC_IO_*: xtc_sim_fd_ready_at */
#include "xtc_dst_inject.h" /* DST bug-injection harness (no-op in prod) */
#include "xtc_probes.h"
#include "coro_int.h" /* __xtc_current_task */

#include <pthread.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Activation state is process-global and read on hot paths, so it is a
 * relaxed atomic flag rather than a lock. */
static int g_sim_active;
static uint64_t         g_sim_stream[XTC_SIM_RNG_NSTREAMS];
/* Adversarial-mode knobs (see xtc_sim.h); defined early because
 * xtc_sim_deactivate resets them.  Both OFF (0) by default. */
static int g_sched_pessimal_pct; /* per-1000; 0 = uniform */
static int g_swizzle_pct;        /* per-1000; 0 = no reorder */
static int64_t g_clock_skew_ns;  /* observer clock offset */
static int g_clock_skew_jitter;  /* +/- ns seeded wobble */

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
static int g_vclock_on;
static int64_t g_vclock_ns;

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
	return xtc_atomic_load_i32(&g_sim_active, xtc_mo_relaxed);
}

/* PUBLIC: int xtc_sim_active __P((void)); */
/* The public spelling of __xtc_sim_active, for application tests. */
int
xtc_sim_active(void)
{
	return __xtc_sim_active();
}

/*
 * Determinism guard (FoundationDB/TigerBeetle discipline: nondeterminism
 * must be impossible to introduce SILENTLY, not merely avoided by
 * convention).  A primitive that would break seed replay -- a real wall
 * clock, an unseeded RNG, an environment read, a raw thread id -- calls
 * __xtc_sim_nondeterminism(what) from its sim-reachable path.  Outside a
 * sim run it is a no-op.  DURING a sim run it records the violation and,
 * in STRICT mode (the default under sim, and always in test builds via
 * xtc_sim_strict_enable), aborts loudly with the offending source named,
 * so a regression that adds nondeterminism to a sim-reachable path fails
 * a test rather than silently corrupting replay.  The count is queryable
 * so a harness can assert zero.
 */
static int g_nondet_count;
static int g_nondet_strict = 1; /* abort on violation by default */

/* PUBLIC: void __xtc_sim_nondeterminism __P((const char *)); */
void
__xtc_sim_nondeterminism(const char *what)
{
	if (!__xtc_sim_active())
		return;
	XTC_PROBE_SIM_NONDETERMINISM(what);
	xtc_atomic_fetch_add_i32(&g_nondet_count, 1, xtc_mo_relaxed);
	if (xtc_atomic_load_i32(&g_nondet_strict, xtc_mo_relaxed)) {
		fprintf(stderr, "xtc DST determinism violation: %s called on a "
		    "sim-reachable path (breaks seed replay)\n",
		    what ? what : "(unknown)");
		abort();
	}
}

/* PUBLIC: int __xtc_sim_offloop_wait __P((void)); */
/*
 * 1 when the caller is the simulator's own thread, outside any fiber,
 * during a run: an xtc_sim_external_complete delivery fn, or a test's
 * code between xtc_sim_activate and xtc_sim_deactivate.  A blocking wait
 * there can never be ended by the run, because the run is this thread
 * and it is blocked: no fiber runs and no virtual time passes.  Any
 * other thread (a device worker) gets 0, and so does every thread of a
 * native build.  g_sim_thread marks the thread that called
 * xtc_sim_activate, until xtc_sim_deactivate.
 */
static XTC_THREAD_LOCAL int g_sim_thread;

int
__xtc_sim_offloop_wait(void)
{
#if defined(XTC_IO_BACKEND_SIM) && !XTC_DST_BUG(XTC_DST_BUG_SIM_OFFLOOP_WAIT)
	return g_sim_thread && __xtc_sim_active() &&
	    __xtc_current_task() == NULL;
#else
	/* A native build's loops run on their own threads, which can end
	 * the wait (and bug 68 plants the 2.2.0 waits). */
	return 0;
#endif
}

/* PUBLIC: int __xtc_sim_cond_deadline __P((int64_t, struct timespec *)); */
/*
 * The absolute pthread_cond_timedwait deadline for a plain-thread wait
 * of timeout_ns (>= 0).  Returns 1 and an already-passed deadline when
 * the wait is made on the simulator's thread off a fiber during a run
 * (see __xtc_sim_offloop_wait): virtual time cannot pass while the only
 * thread is blocked, so the wait expires at once and never reads the
 * wall clock.  Otherwise returns 0 and now + timeout_ns on the wall
 * clock, the clock pthread_cond_timedwait measures.  Every timed
 * plain-thread wait in the library takes its deadline from here, so no
 * wall-clock read is left on a sim-reachable path.
 */
int
__xtc_sim_cond_deadline(int64_t timeout_ns, struct timespec *ts)
{
	int64_t now = 0;

	if (__xtc_sim_offloop_wait()) {
		ts->tv_sec = 0;
		ts->tv_nsec = 0;
		return 1;
	}
	(void)__os_clock_real(&now);
	now = timeout_ns > INT64_MAX - now ? INT64_MAX : now + timeout_ns;
	ts->tv_sec = (time_t)(now / 1000000000LL);
	ts->tv_nsec = (long)(now % 1000000000LL);
	return 0;
}

/* PUBLIC: int __xtc_sim_require_run __P((const char *)); */
/*
 * The entry guard of xtc_loop_run and xtc_exec_run on a sim build.  The
 * sim I/O backend has no kernel poller: its xtc_io_poll never blocks and
 * never looks at a real fd, so a loop driven outside a simulation run
 * spins a core forever on a socket or pipe it cannot see become ready.
 * Returns XTC_OK during a run (xtc_sim_exec_run, or an explicit
 * xtc_sim_activate) and XTC_E_STATE outside one, after saying why on
 * stderr, once per process.  Off a sim build it always returns XTC_OK.
 */
int
__xtc_sim_require_run(const char *what)
{
#if defined(XTC_IO_BACKEND_SIM)
	static int warned;

	if (__xtc_sim_active())
		return XTC_OK;
	if (xtc_atomic_exchange_i32(&warned, 1, xtc_mo_relaxed) == 0)
		fprintf(stderr,
		    "xtc: %s outside a simulation run on a "
		    "--with-io-backend=sim library returns XTC_E_STATE: the "
		    "sim backend has no kernel poller, so the loop would spin "
		    "on fds it cannot see.  Drive the procs with "
		    "xtc_sim_exec_run, or link a native libxtc.\n",
		    what);
	return XTC_E_STATE;
#else
	(void)what;
	return XTC_OK;
#endif
}

/* PUBLIC: void xtc_sim_strict __P((int)); */
/* Toggle strict mode: nonzero aborts on a determinism violation (the
 * default), zero only counts them (for a diagnostic sweep that wants to
 * enumerate every violation rather than die on the first). */
void
xtc_sim_strict(int on)
{
	xtc_atomic_store_i32(&g_nondet_strict, on ? 1 : 0, xtc_mo_relaxed);
}

/* PUBLIC: int xtc_sim_nondeterminism_count __P((void)); */
/* Number of determinism violations seen since the last activate.  A
 * harness asserts this is 0 after a run to PROVE the run was fully
 * deterministic (nothing on the executed path could break replay). */
int
xtc_sim_nondeterminism_count(void)
{
	return xtc_atomic_load_i32(&g_nondet_count, xtc_mo_relaxed);
}

static void __ext_reset(void);

/* PUBLIC: void xtc_sim_activate __P((uint64_t)); */
void
xtc_sim_activate(uint64_t seed)
{
	int i;
	uint64_t s = seed ? seed : 0x9E3779B97F4A7C15ull;
	/* Derive each stream's initial state from the root seed so the
	 * streams are independent yet fully determined by the seed. */
	for (i = 0; i < XTC_SIM_RNG_NSTREAMS; i++) {
		uint64_t t = s + (uint64_t)(i + 1) * 0x9E3779B97F4A7C15ull;
		g_sim_stream[i] = splitmix64(&t);
	}
	xtc_atomic_store_i32(&g_nondet_count, 0, xtc_mo_relaxed);
	__ext_reset();
	g_sim_thread = 1;
	xtc_atomic_store_i32(&g_sim_active, 1, xtc_mo_release);
}

static void __fd_ready_reset(void);

/* PUBLIC: void xtc_sim_deactivate __P((void)); */
void
xtc_sim_deactivate(void)
{
	xtc_atomic_store_i32(&g_sim_active, 0, xtc_mo_release);
	g_sim_thread = 0;
	__fd_ready_reset();
	/* Reset adversarial-mode knobs so one run's setting does not leak
	 * into the next in-process run (buggify/io-fault knobs are reset by
	 * their own enable/disable; these have no such call on every run). */
	xtc_atomic_store_i32(&g_sched_pessimal_pct, 0, xtc_mo_release);
	xtc_atomic_store_i32(&g_swizzle_pct, 0, xtc_mo_release);
	xtc_atomic_store_i64(&g_clock_skew_ns, 0, xtc_mo_release);
	xtc_atomic_store_i32(&g_clock_skew_jitter, 0, xtc_mo_release);
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

/* PUBLIC: uint64_t xtc_sim_rand __P((uint64_t)); */
/*
 * An application draw in [0, bound): the APP stream inside a run, which
 * no runtime decision site draws from, so an application's draws never
 * move the schedule; xtc_rand_u64 outside one.
 */
uint64_t
xtc_sim_rand(uint64_t bound)
{
	if (bound == 0)
		return 0;
	if (__xtc_sim_active())
		return __xtc_sim_rng_range(XTC_SIM_RNG_APP, bound);
	return xtc_rand_u64() % bound;
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
static int g_fp_on;                       /* fault points enabled */
static unsigned         g_fp_pct;         /* per-1000 fire probability */
static char             g_fp_name[XTC_SIM_FP_MAX][48];
static uint64_t g_fp_fires[XTC_SIM_FP_MAX];
static int g_fp_n; /* number of distinct names seen */
static pthread_mutex_t  g_fp_lock = PTHREAD_MUTEX_INITIALIZER;

/* Find (or register) the slot for `name`.  Returns the index, or -1 if
 * the table is full.  Caller holds g_fp_lock. */
static int
__fp_slot_locked(const char *name)
{
	int i, n = xtc_atomic_load_i32(&g_fp_n, xtc_mo_relaxed);
	for (i = 0; i < n; i++)
		if (strncmp(g_fp_name[i], name, sizeof g_fp_name[0]) == 0)
			return i;
	if (n >= XTC_SIM_FP_MAX)
		return -1;
	(void)strncpy(g_fp_name[n], name, sizeof g_fp_name[0] - 1);
	g_fp_name[n][sizeof g_fp_name[0] - 1] = '\0';
	xtc_atomic_store_u64(&g_fp_fires[n], 0, xtc_mo_relaxed);
	xtc_atomic_store_i32(&g_fp_n, n + 1, xtc_mo_relaxed);
	return n;
}

/* PUBLIC: void xtc_sim_fault_points_enable __P((unsigned)); */
void
xtc_sim_fault_points_enable(unsigned pct_per_1000)
{
	(void)pthread_mutex_lock(&g_fp_lock);
	xtc_atomic_store_i32(&g_fp_n, 0, xtc_mo_relaxed);
	g_fp_pct = pct_per_1000;
	xtc_atomic_store_i32(&g_fp_on, 1, xtc_mo_release);
	(void)pthread_mutex_unlock(&g_fp_lock);
}

/* PUBLIC: void xtc_sim_fault_points_disable __P((void)); */
void
xtc_sim_fault_points_disable(void)
{
	xtc_atomic_store_i32(&g_fp_on, 0, xtc_mo_release);
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
	if (!xtc_atomic_load_i32(&g_fp_on, xtc_mo_acquire))
		return 0;
	if (!__xtc_sim_active())
		return 0;
	hit = (__xtc_sim_rng_range(XTC_SIM_RNG_FAULT, 1000) < g_fp_pct);
	(void)pthread_mutex_lock(&g_fp_lock);
	slot = __fp_slot_locked(name);
	if (slot >= 0 && hit)
		xtc_atomic_fetch_add_u64(&g_fp_fires[slot], 1, xtc_mo_relaxed);
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
	n = xtc_atomic_load_i32(&g_fp_n, xtc_mo_relaxed);
	for (i = 0; i < n; i++)
		if (strncmp(g_fp_name[i], name, sizeof g_fp_name[0]) == 0) {
			v = xtc_atomic_load_u64(&g_fp_fires[i], xtc_mo_relaxed);
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
	return xtc_atomic_load_i32(&g_fp_n, xtc_mo_relaxed);
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
static int g_bug_on;                      /* buggify enabled */
static unsigned         g_bug_pct;        /* per-1000 activation probability */
static char             g_bug_name[XTC_SIM_BUG_MAX][48];
static signed char      g_bug_decided[XTC_SIM_BUG_MAX]; /* -1 undecided, 0/1 */
static int g_bug_n;
static pthread_mutex_t  g_bug_lock = PTHREAD_MUTEX_INITIALIZER;

/* Adversarial scheduler bias + completion/message swizzle (see
 * xtc_sim.h).  State defined near the top (xtc_sim_deactivate resets
 * it); the accessors follow. */

/* PUBLIC: void xtc_sim_sched_pessimal __P((unsigned)); */
void
xtc_sim_sched_pessimal(unsigned pct_per_1000)
{
	if (pct_per_1000 > 1000) pct_per_1000 = 1000;
	xtc_atomic_store_i32(&g_sched_pessimal_pct, (int)pct_per_1000,
	    xtc_mo_release);
}

/* PUBLIC: int __xtc_sim_sched_pessimal_pct __P((void)); */
int
__xtc_sim_sched_pessimal_pct(void)
{
	return xtc_atomic_load_i32(&g_sched_pessimal_pct, xtc_mo_acquire);
}

/* PUBLIC: void xtc_sim_swizzle_enable __P((unsigned)); */
void
xtc_sim_swizzle_enable(unsigned pct_per_1000)
{
	if (pct_per_1000 > 1000) pct_per_1000 = 1000;
	xtc_atomic_store_i32(&g_swizzle_pct, (int)pct_per_1000, xtc_mo_release);
}

/* PUBLIC: void xtc_sim_swizzle_disable __P((void)); */
void
xtc_sim_swizzle_disable(void)
{
	xtc_atomic_store_i32(&g_swizzle_pct, 0, xtc_mo_release);
}

/* PUBLIC: int __xtc_sim_swizzle_pct __P((void)); */
int
__xtc_sim_swizzle_pct(void)
{
	return xtc_atomic_load_i32(&g_swizzle_pct, xtc_mo_acquire);
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
	xtc_atomic_store_i32(&g_bug_n, 0, xtc_mo_relaxed);
	g_bug_pct = pct_per_1000;
	xtc_atomic_store_i32(&g_bug_on, 1, xtc_mo_release);
	(void)pthread_mutex_unlock(&g_bug_lock);
}

/* PUBLIC: void xtc_sim_buggify_disable __P((void)); */
void
xtc_sim_buggify_disable(void)
{
	(void)pthread_mutex_lock(&g_bug_lock);
	xtc_atomic_store_i32(&g_bug_on, 0, xtc_mo_release);
	/* Reset the decision table so a subsequent run (or an
	 * active-count query while disabled) reflects no activations. */
	xtc_atomic_store_i32(&g_bug_n, 0, xtc_mo_relaxed);
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
	if (!xtc_atomic_load_i32(&g_bug_on, xtc_mo_acquire))
		return 0;
	if (!__xtc_sim_active())
		return 0;
	(void)pthread_mutex_lock(&g_bug_lock);
	n = xtc_atomic_load_i32(&g_bug_n, xtc_mo_relaxed);
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
	xtc_atomic_store_i32(&g_bug_n, n + 1, xtc_mo_relaxed);
	(void)pthread_mutex_unlock(&g_bug_lock);
	XTC_PROBE_SIM_BUGGIFY(name, decision);
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
	n = xtc_atomic_load_i32(&g_bug_n, xtc_mo_relaxed);
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
	n = xtc_atomic_load_i32(&g_bug_n, xtc_mo_relaxed);
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
	n = xtc_atomic_load_i32(&g_bug_n, xtc_mo_relaxed);
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
	if (!xtc_atomic_load_i32(&g_bug_on, xtc_mo_acquire))
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
static int g_io_faults_on;
static int64_t g_io_lat_min_ns;
static int64_t g_io_lat_max_ns;
static unsigned        g_io_fault_pct;     /* per-1000 op fault probability */

/* PUBLIC: void xtc_sim_io_faults_enable __P((int64_t, int64_t, unsigned)); */
void
xtc_sim_io_faults_enable(int64_t lat_min_ns, int64_t lat_max_ns,
    unsigned fault_pct_per_1000)
{
	if (lat_min_ns < 0) lat_min_ns = 0;
	if (lat_max_ns < lat_min_ns) lat_max_ns = lat_min_ns;
	xtc_atomic_store_i64(&g_io_lat_min_ns, lat_min_ns, xtc_mo_relaxed);
	xtc_atomic_store_i64(&g_io_lat_max_ns, lat_max_ns, xtc_mo_relaxed);
	g_io_fault_pct = fault_pct_per_1000;
	xtc_atomic_store_i32(&g_io_faults_on, 1, xtc_mo_release);
}

/* PUBLIC: void xtc_sim_io_faults_disable __P((void)); */
void
xtc_sim_io_faults_disable(void)
{
	xtc_atomic_store_i32(&g_io_faults_on, 0, xtc_mo_release);
}

/* PUBLIC: int __xtc_sim_io_faults_active __P((void)); */
int
__xtc_sim_io_faults_active(void)
{
	return xtc_atomic_load_i32(&g_io_faults_on, xtc_mo_acquire);
}

/* Disk-full (ENOSPC) injection: a seeded coin makes a WRITE fail with a
 * hard out-of-space error mid-workload (distinct from a short transfer
 * or a torn write -- the whole op fails, nothing persists), exercising a
 * storage engine's graceful-degradation path.  FoundationDB's disk-full
 * fault class.  Off by default. */
static int g_io_enospc_pct; /* per-1000 write ENOSPC probability */

/* PUBLIC: void xtc_sim_io_enospc_enable __P((unsigned)); */
void
xtc_sim_io_enospc_enable(unsigned pct_per_1000)
{
	if (pct_per_1000 > 1000) pct_per_1000 = 1000;
	xtc_atomic_store_i32(&g_io_enospc_pct, (int)pct_per_1000,
	    xtc_mo_release);
}

/* PUBLIC: int __xtc_sim_io_enospc __P((void)); */
/* 1 on a seeded coin (from the IO stream) when a write should fail with
 * ENOSPC; 0 otherwise / when disabled or sim inactive. */
int
__xtc_sim_io_enospc(void)
{
	int pct = xtc_atomic_load_i32(&g_io_enospc_pct, xtc_mo_acquire);
	if (pct <= 0 || !__xtc_sim_active())
		return 0;
	return (int)__xtc_sim_rng_range(XTC_SIM_RNG_IO, 1000) < pct;
}

/*
 * Stale-data reads (FoundationDB's "the disk returns an OLD durable
 * version").  Distinct from a torn write (silent short tail) or a
 * corrupt read (bit flip): the bytes are STRUCTURALLY VALID but OUT OF
 * DATE -- a page durably written, then overwritten, and on a seeded
 * read the disk hands back the PREVIOUS contents.  Catches recovery /
 * cache code that fails to validate a version or LSN and silently
 * accepts stale-but-well-formed data.  General (fd+offset level, not
 * tied to sqlxtc): a small bounded ring records recent write payloads;
 * a stale read at a matching offset returns a prior one.  The ring is
 * process-global, reset per run in xtc_sim_activate, and bounded in
 * count AND per-entry size so memory is fixed regardless of workload.
 */
#define XTC_SIM_STALE_RING   32
#define XTC_SIM_STALE_MAXLEN 4096
struct sim_stale_ent {
	int      fd;
	uint64_t off;
	int      len;
	unsigned char buf[XTC_SIM_STALE_MAXLEN];
};
static struct sim_stale_ent g_stale[XTC_SIM_STALE_RING];
static int g_stale_head;
static int g_stale_pct;
static pthread_mutex_t g_stale_lock = PTHREAD_MUTEX_INITIALIZER;

/* PUBLIC: void xtc_sim_io_stale_enable __P((unsigned)); */
void
xtc_sim_io_stale_enable(unsigned pct_per_1000)
{
	if (pct_per_1000 > 1000) pct_per_1000 = 1000;
	/* Reset the ring so each run's stale history is fully determined by
	 * its seed (this is the per-run arming point, like the other io
	 * fault knobs). */
	(void)pthread_mutex_lock(&g_stale_lock);
	xtc_atomic_store_i32(&g_stale_head, 0, xtc_mo_relaxed);
	{
		int k;
		for (k = 0; k < XTC_SIM_STALE_RING; k++)
			g_stale[k].len = 0;
	}
	(void)pthread_mutex_unlock(&g_stale_lock);
	xtc_atomic_store_i32(&g_stale_pct, (int)pct_per_1000, xtc_mo_release);
}

/* PUBLIC: void __xtc_sim_io_stale_record __P((int, uint64_t, const void *, int)); */
void
__xtc_sim_io_stale_record(int fd, uint64_t off, const void *buf, int len)
{
	int h, cp;
	if (xtc_atomic_load_i32(&g_stale_pct, xtc_mo_acquire) <= 0 ||
	    !__xtc_sim_active() || buf == NULL || len <= 0)
		return;
	cp = len < XTC_SIM_STALE_MAXLEN ? len : XTC_SIM_STALE_MAXLEN;
	(void)pthread_mutex_lock(&g_stale_lock);
	h = xtc_atomic_load_i32(&g_stale_head, xtc_mo_relaxed);
	g_stale[h].fd = fd;
	g_stale[h].off = off;
	g_stale[h].len = cp;
	memcpy(g_stale[h].buf, buf, (size_t)cp);
	xtc_atomic_store_i32(&g_stale_head, (h + 1) % XTC_SIM_STALE_RING,
	    xtc_mo_relaxed);
	(void)pthread_mutex_unlock(&g_stale_lock);
}

/* PUBLIC: int __xtc_sim_io_stale_read __P((int, uint64_t, void *, int)); */
int
__xtc_sim_io_stale_read(int fd, uint64_t off, void *buf, int len)
{
	int pct, i, h, hit = 0;
	if ((pct = xtc_atomic_load_i32(&g_stale_pct, xtc_mo_acquire)) <= 0 ||
	    !__xtc_sim_active() || buf == NULL || len <= 0)
		return 0;
	if ((int)__xtc_sim_rng_range(XTC_SIM_RNG_IO, 1000) >= pct)
		return 0;
	(void)pthread_mutex_lock(&g_stale_lock);
	h = xtc_atomic_load_i32(&g_stale_head, xtc_mo_relaxed);
	for (i = 0; i < XTC_SIM_STALE_RING; i++) {
		int idx = (h - 1 - i + 2 * XTC_SIM_STALE_RING) %
		    XTC_SIM_STALE_RING;
		if (g_stale[idx].fd == fd && g_stale[idx].off == off &&
		    g_stale[idx].len > 0) {
			int cp = g_stale[idx].len < len ? g_stale[idx].len : len;
			memcpy(buf, g_stale[idx].buf, (size_t)cp);
			hit = 1;
			break;
		}
	}
	(void)pthread_mutex_unlock(&g_stale_lock);
	return hit;
}

/*
 * Write-back cache crash model (FoundationDB's simulated disk).  The
 * problem it closes: the sim writes to a REAL temp file, so bytes reach
 * the file on pwrite regardless of whether fsync was called -- meaning a
 * crash-recovery test that trusts the writer's self-reported durable
 * point cannot catch a writer that ACKs a commit WITHOUT fsyncing it.
 * This models the disk honestly: pwrite lands in a volatile write-back
 * cache (tracked as the per-fd high-water WRITTEN offset); only fsync
 * promotes the written extent to the DURABLE offset; a crash loses
 * everything past the last fsync.  A recovery test asks
 * __xtc_sim_io_durable_end(fd) for the true post-crash durable frontier
 * (the last fsync-confirmed byte) instead of trusting the writer.
 *
 * Bounded to a small fixed table of fds (a test opens one or two WAL
 * files); reset per run in xtc_sim_activate.  Off unless armed, so it is
 * zero-overhead when a test does not use it.
 */
#define XTC_SIM_WB_FDS 8
struct sim_wb_ent { int fd; uint64_t written_end; uint64_t durable_end; };
static struct sim_wb_ent g_wb[XTC_SIM_WB_FDS];
static int g_wb_on;
static pthread_mutex_t g_wb_lock = PTHREAD_MUTEX_INITIALIZER;

/* PUBLIC: void xtc_sim_io_wb_enable __P((int)); */
/* Arm (nonzero) or disarm the write-back crash model, resetting the fd
 * table.  A test arms it before the run and reads durable_end after. */
void
xtc_sim_io_wb_enable(int on)
{
	(void)pthread_mutex_lock(&g_wb_lock);
	if (on) {
		int k;
		for (k = 0; k < XTC_SIM_WB_FDS; k++) {
			g_wb[k].fd = -1;
			g_wb[k].written_end = 0;
			g_wb[k].durable_end = 0;
		}
	}
	xtc_atomic_store_i32(&g_wb_on, on ? 1 : 0, xtc_mo_release);
	(void)pthread_mutex_unlock(&g_wb_lock);
}

static struct sim_wb_ent *
wb_slot_locked(int fd)
{
	int k, free_k = -1;
	for (k = 0; k < XTC_SIM_WB_FDS; k++) {
		if (g_wb[k].fd == fd) return &g_wb[k];
		if (free_k < 0 && g_wb[k].fd < 0) free_k = k;
	}
	if (free_k >= 0) { g_wb[free_k].fd = fd; return &g_wb[free_k]; }
	return NULL;   /* table full: this fd is not tracked (bounded) */
}

/* PUBLIC: void __xtc_sim_io_wb_wrote __P((int, uint64_t)); */
/* Record that a pwrite reached [.., end_off) on fd -- it is in the
 * volatile cache now, durable only after a subsequent fsync. */
void
__xtc_sim_io_wb_wrote(int fd, uint64_t end_off)
{
	struct sim_wb_ent *e;
	if (!xtc_atomic_load_i32(&g_wb_on, xtc_mo_acquire))
		return;
	(void)pthread_mutex_lock(&g_wb_lock);
	e = wb_slot_locked(fd);
	if (e != NULL && end_off > e->written_end)
		e->written_end = end_off;
	(void)pthread_mutex_unlock(&g_wb_lock);
}

/* PUBLIC: void __xtc_sim_io_wb_synced __P((int)); */
/* An fsync/fdatasync on fd promoted everything WRITTEN so far to
 * durable. */
void
__xtc_sim_io_wb_synced(int fd)
{
	struct sim_wb_ent *e;
	if (!xtc_atomic_load_i32(&g_wb_on, xtc_mo_acquire))
		return;
	(void)pthread_mutex_lock(&g_wb_lock);
	e = wb_slot_locked(fd);
	if (e != NULL)
		e->durable_end = e->written_end;
	(void)pthread_mutex_unlock(&g_wb_lock);
}

/* PUBLIC: uint64_t xtc_sim_io_durable_end __P((int)); */
/* The last fsync-confirmed byte offset on fd -- what survives a crash.
 * A recovery test truncates the file here.  Returns 0 for an untracked
 * fd or when the model is disarmed. */
uint64_t
xtc_sim_io_durable_end(int fd)
{
	struct sim_wb_ent *e;
	uint64_t d = 0;
	if (!xtc_atomic_load_i32(&g_wb_on, xtc_mo_acquire))
		return 0;
	(void)pthread_mutex_lock(&g_wb_lock);
	e = wb_slot_locked(fd);
	if (e != NULL) d = e->durable_end;
	(void)pthread_mutex_unlock(&g_wb_lock);
	return d;
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
	lo = xtc_atomic_load_i64(&g_io_lat_min_ns, xtc_mo_relaxed);
	hi = xtc_atomic_load_i64(&g_io_lat_max_ns, xtc_mo_relaxed);
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
static int g_io_corrupt_on;
static unsigned    g_io_corrupt_pct;    /* per-1000 op corruption probability */

/* PUBLIC: void xtc_sim_io_corrupt_enable __P((unsigned)); */
void
xtc_sim_io_corrupt_enable(unsigned corrupt_pct_per_1000)
{
	g_io_corrupt_pct = corrupt_pct_per_1000;
	xtc_atomic_store_i32(&g_io_corrupt_on, 1, xtc_mo_release);
}

/* PUBLIC: void xtc_sim_io_corrupt_disable __P((void)); */
void
xtc_sim_io_corrupt_disable(void)
{
	xtc_atomic_store_i32(&g_io_corrupt_on, 0, xtc_mo_release);
}

/* PUBLIC: int __xtc_sim_io_corrupt_active __P((void)); */
int
__xtc_sim_io_corrupt_active(void)
{
	return xtc_atomic_load_i32(&g_io_corrupt_on, xtc_mo_acquire);
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
 * j".  Loops are identified by their executor slot + 1 (0 for a
 * standalone loop), so the matrix is indexed by that in
 * [0, XTC_SIM_PART_MAX).  (Through 2.1 this was also pid.loop_id; since
 * 2.2 pid.loop_id is a process-wide id, and the matrix kept the slot.)
 * A blocked edge causes __mbox_deliver to DROP the message the same way
 * a soft-full mailbox does (return XTC_E_AGAIN; the sender already
 * handles that path), so a partitioned peer never deadlocks the sim.
 * System messages (a DOWN, a link EXIT) are not cut (see __mbox_deliver).
 * Off by default (no edge blocked).
 *
 * Latency: when a [min,max] window is set, each cross-loop delivery is
 * deferred to now + a seeded draw from the IO stream (reusing the IO
 * stream -- not a new PRNG stream -- so enabling latency does not
 * perturb the schedule).  Off by default (min==max==0: inline delivery).
 */
#define XTC_SIM_PART_MAX 64        /* == LOOP_TABLE_MAX in proc.c */
static int g_part_on;
static unsigned char    g_part_block[XTC_SIM_PART_MAX][XTC_SIM_PART_MAX];
static int64_t g_net_lat_min_ns;
static int64_t g_net_lat_max_ns;
static pthread_mutex_t  g_part_lock = PTHREAD_MUTEX_INITIALIZER;

/* PUBLIC: void xtc_sim_partition_set __P((int, int, int)); */
/*
 * Set (or clear) the directed edge src_loop_id -> dst_loop_id in the
 * partition matrix: when `blocked` is nonzero a cross-loop message from
 * a proc on src_loop_id to a proc on dst_loop_id is dropped under sim.
 * loop_id is the executor slot + 1 (0 == standalone).  The edge is
 * DIRECTED: block only src->dst for a one-way (asymmetric) partition --
 * where src cannot reach dst but dst's replies to src still flow, the
 * harder failure mode that exposes asymmetric-timeout bugs.  For a
 * symmetric cut call it both ways (or use xtc_sim_partition_isolate).
 * Enables the partition subsystem on first blocking edge.  Out-of-range
 * ids are ignored.
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
		xtc_atomic_store_i32(&g_part_on, 1, xtc_mo_release);
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
	xtc_atomic_store_i32(&g_part_on, 1, xtc_mo_release);
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
	xtc_atomic_store_i32(&g_part_on, 0, xtc_mo_release);
	xtc_atomic_store_i64(&g_net_lat_min_ns, 0, xtc_mo_relaxed);
	xtc_atomic_store_i64(&g_net_lat_max_ns, 0, xtc_mo_relaxed);
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
	if (!xtc_atomic_load_i32(&g_part_on, xtc_mo_acquire))
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
	xtc_atomic_store_i64(&g_net_lat_min_ns, min_ns, xtc_mo_relaxed);
	xtc_atomic_store_i64(&g_net_lat_max_ns, max_ns, xtc_mo_relaxed);
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
	lo = xtc_atomic_load_i64(&g_net_lat_min_ns, xtc_mo_relaxed);
	hi = xtc_atomic_load_i64(&g_net_lat_max_ns, xtc_mo_relaxed);
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
	xtc_atomic_store_i64(&g_vclock_ns, start_ns, xtc_mo_relaxed);
	xtc_atomic_store_i32(&g_vclock_on, 1, xtc_mo_release);
}

/* PUBLIC: void xtc_sim_clock_disable __P((void)); */
void
xtc_sim_clock_disable(void)
{
	xtc_atomic_store_i32(&g_vclock_on, 0, xtc_mo_release);
}

/* PUBLIC: void xtc_sim_clock_advance __P((int64_t)); */
void
xtc_sim_clock_advance(int64_t delta_ns)
{
	if (delta_ns > 0)
		(void)xtc_atomic_fetch_add_i64(&g_vclock_ns, delta_ns,
		    xtc_mo_relaxed);
}

/* PUBLIC: void xtc_sim_clock_set __P((int64_t)); */
void
xtc_sim_clock_set(int64_t ns)
{
	xtc_atomic_store_i64(&g_vclock_ns, ns, xtc_mo_relaxed);
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
	if (!xtc_atomic_load_i32(&g_vclock_on, xtc_mo_acquire))
		return 0;
	if (out_ns != NULL)
		*out_ns = xtc_atomic_load_i64(&g_vclock_ns, xtc_mo_relaxed);
	return 1;
}

/* PUBLIC: int xtc_sim_clock_active __P((void)); */
int
xtc_sim_clock_active(void)
{
	return __xtc_sim_vclock(NULL);
}

/*
 * Clock skew (FoundationDB "the clock is not perfect").  The SCHEDULER
 * schedules timers against the true virtual clock (via __xtc_sim_vclock
 * above), but a FIBER that reads the clock for its own timeout math
 * observes a SKEWED value -- so "has my deadline passed?" as the fiber
 * computes it can disagree with when its timer actually fires.  This is
 * where clock-assumption bugs hide (code that assumes elapsed wall time
 * equals scheduler time, or that the clock is monotonic to the ns).
 *
 * The skew is seeded and bounded, so it replays: a per-run fixed offset
 * plus, when jitter is enabled, a seeded per-read wobble within the
 * band.  Applied ONLY on the observation seam (__os_clock_mono ->
 * __xtc_sim_vclock_observed), never to the scheduler's timer math, so a
 * skewed clock can never desync the scheduler or break quiescence -- it
 * only stresses the OBSERVER.  Off by default.
 */

/* PUBLIC: void xtc_sim_clock_skew __P((int64_t, int)); */
void
xtc_sim_clock_skew(int64_t offset_ns, int jitter_ns)
{
	xtc_atomic_store_i64(&g_clock_skew_ns, offset_ns, xtc_mo_release);
	xtc_atomic_store_i32(&g_clock_skew_jitter,
	    jitter_ns < 0 ? 0 : jitter_ns, xtc_mo_release);
}

/* PUBLIC: int __xtc_sim_vclock_observed __P((int64_t *)); */
/* The observation seam __os_clock_mono uses under sim: the true virtual
 * clock plus the seeded skew.  Monotonic within a run per observer
 * (the fixed offset is constant; the jitter is a bounded seeded draw,
 * added to the base, so successive reads can wobble but the sequence is
 * fully determined by the seed).  Falls back to the unskewed value when
 * skew is off. */
int
__xtc_sim_vclock_observed(int64_t *out_ns)
{
	int64_t base, off;
	int jit;
	if (!xtc_atomic_load_i32(&g_vclock_on, xtc_mo_acquire))
		return 0;
	base = xtc_atomic_load_i64(&g_vclock_ns, xtc_mo_relaxed);
	off = xtc_atomic_load_i64(&g_clock_skew_ns, xtc_mo_relaxed);
	jit = xtc_atomic_load_i32(&g_clock_skew_jitter, xtc_mo_relaxed);
	if (jit > 0 && __xtc_sim_active()) {
		/* Seeded wobble in [-jit, +jit] from the IO stream (an
		 * observation draw, distinct from scheduler decisions). */
		int64_t w = (int64_t)(__xtc_sim_rng_range(XTC_SIM_RNG_IO,
		    (uint64_t)(2 * jit + 1))) - jit;
		off += w;
	}
	if (out_ns != NULL) {
		int64_t v = base + off;
		*out_ns = v < 0 ? 0 : v;   /* never report a negative clock */
	}
	return 1;
}

/* ---- scheduled fd readiness (DST) ----
 *
 * The sim backend has no kernel poller: until xtc_sim_fd_ready_at, no
 * fd ever became ready under a run, so a proc parked on an fd woke only
 * by its timeout, mail or a kill.  This table makes an fd the test owns
 * (a pipe or eventfd standing for a device fence) ready at a virtual
 * time.  The readiness is a LEVEL: from due on, every registration of
 * the fd whose interest overlaps revents fires (io_sim.c), so a waiter
 * that registers after the due time still sees it, as poll(2) would.
 * One entry per fd; a later call replaces it; revents 0 withdraws it.
 * Cleared by xtc_sim_deactivate, so no run inherits another's.
 */
#define XTC_SIM_FD_READY_MAX 64
struct sim_fd_ready {
	int fd; /* -1: free slot */
	int64_t due_ns;
	uint32_t revents;
};
static struct sim_fd_ready g_fdr[XTC_SIM_FD_READY_MAX];
static int g_fdr_n; /* slots in use at the front of g_fdr (xtc_atomic) */
static pthread_mutex_t g_fdr_lock = PTHREAD_MUTEX_INITIALIZER;

static void
__fd_ready_reset(void)
{
	(void)pthread_mutex_lock(&g_fdr_lock);
	xtc_atomic_store_i32(&g_fdr_n, 0, xtc_mo_relaxed);
	(void)pthread_mutex_unlock(&g_fdr_lock);
}

/* PUBLIC: int xtc_sim_fd_ready_at __P((int, int64_t, uint32_t)); */
/*
 * Make fd ready for revents (XTC_IO_* bits) from virtual time due_ns on,
 * for every proc that waits on it under the run.  Replaces an earlier
 * schedule for the same fd; revents 0 removes it.  XTC_E_INVAL for a
 * negative fd or due time, or a revents bit outside READABLE, WRITABLE,
 * HUP and ERR; XTC_E_RESOURCE when 64 fds are already scheduled;
 * XTC_E_NOSYS off a sim build.  Call it from inside a run (a proc of the
 * simulated device) or before xtc_sim_exec_run; the schedule is cleared
 * at the end of every run.  The fd itself is never read or written: a
 * waiter that then reads it reads what the test wrote there.
 */
int
xtc_sim_fd_ready_at(int fd, int64_t due_ns, uint32_t revents)
{
	const uint32_t ok =
	    XTC_IO_READABLE | XTC_IO_WRITABLE | XTC_IO_HUP | XTC_IO_ERR;
#if defined(XTC_IO_BACKEND_SIM)
	int i, n, rc = XTC_OK;
#endif

	if (fd < 0 || due_ns < 0 || (revents & ~ok) != 0)
		return XTC_E_INVAL;
#if !defined(XTC_IO_BACKEND_SIM)
	return XTC_E_NOSYS;
#else
	(void)pthread_mutex_lock(&g_fdr_lock);
	n = xtc_atomic_load_i32(&g_fdr_n, xtc_mo_relaxed);
	for (i = 0; i < n && g_fdr[i].fd != fd; i++)
		;
	if (revents == 0) {
		if (i < n) {
			g_fdr[i] = g_fdr[n - 1];
			xtc_atomic_store_i32(&g_fdr_n, n - 1, xtc_mo_relaxed);
		}
	} else if (i == n && n == XTC_SIM_FD_READY_MAX) {
		rc = XTC_E_RESOURCE;
	} else {
		if (i == n)
			xtc_atomic_store_i32(&g_fdr_n, n + 1, xtc_mo_relaxed);
		g_fdr[i].fd = fd;
		g_fdr[i].due_ns = due_ns;
		g_fdr[i].revents = revents;
	}
	(void)pthread_mutex_unlock(&g_fdr_lock);
	return rc;
#endif
}

/* PUBLIC: int __xtc_sim_fd_ready __P((int, int64_t *, uint32_t *)); */
int
__xtc_sim_fd_ready(int fd, int64_t *out_due_ns, uint32_t *out_revents)
{
	int i, n, found = 0;
	/* The sim scheduler asks for every registration on every step:
	 * nothing scheduled (every run that does not use the seam) costs
	 * one load. */
	if (xtc_atomic_load_i32(&g_fdr_n, xtc_mo_relaxed) == 0)
		return 0;
	(void)pthread_mutex_lock(&g_fdr_lock);
	n = xtc_atomic_load_i32(&g_fdr_n, xtc_mo_relaxed);
	for (i = 0; i < n; i++) {
		if (g_fdr[i].fd != fd)
			continue;
		*out_due_ns = g_fdr[i].due_ns;
		*out_revents = g_fdr[i].revents;
		found = 1;
		break;
	}
	(void)pthread_mutex_unlock(&g_fdr_lock);
	return found;
}

/* ---- external completions (DST) ----
 *
 * A completion produced by a plain OS thread (a device worker running
 * model compute, a vendor callback thread) cannot be scheduled by the
 * sim: the thread runs on the kernel's schedule.  These two calls give
 * it a seeded virtual time instead.  begin() runs on a fiber of the run
 * before the work is handed to the thread and fixes the completion's due
 * time; complete() runs on the thread when the work is done and records
 * the delivery function.  At the due time the sim thread waits (in real
 * time) for complete() if the thread has not called it yet, then runs
 * fn(arg) on the sim thread.  So when the delivery happens is a function
 * of the seed alone, whatever the thread's real speed, and the
 * application's real thread code runs unchanged.
 *
 * Outside a run (production, or a native build) begin() hands out a
 * token that complete() recognizes as inert: fn(arg) runs at once on the
 * calling thread, which is the production behaviour.
 */
struct xtc_sim_ext {
	pthread_mutex_t lock;
	int done; /* complete() has run */
	void (*fn)(void *);
	void *arg;
};

/* Lanes: a lane's completions are delivered in begin order (a device
 * queue is FIFO).  The due time of the newest begin per lane. */
#define XTC_SIM_EXT_LANES 16
static int64_t g_ext_lane_due[XTC_SIM_EXT_LANES];

/* How long, in real time, the run waits for a producer at a due time
 * before it gives up and names the cause. */
#define XTC_SIM_EXT_WAIT_MS 60000

int __xtc_sim_ext_schedule(int64_t due_ns, void (*run)(void *),
    void (*drop)(void *), void *arg);

/*
 * Wait for complete() on x, in real time; returns its fn and arg.  The
 * run does not advance meanwhile, so a producer that waits for the run
 * to do something first can never complete: after a minute that is
 * reported and the process aborts, the way a determinism violation is.
 */
static void
__ext_wait(struct xtc_sim_ext *x, void (**fn)(void *), void **arg)
{
	int waited_ms = 0;
	for (;;) {
		(void)pthread_mutex_lock(&x->lock);
		if (x->done) {
			*fn = x->fn;
			*arg = x->arg;
			(void)pthread_mutex_unlock(&x->lock);
			return;
		}
		(void)pthread_mutex_unlock(&x->lock);
		if (waited_ms++ == XTC_SIM_EXT_WAIT_MS) {
			fprintf(stderr,
			    "xtc DST: an external completion "
			    "(xtc_sim_external_begin) was not completed after "
			    "%d s of real time; its producer must not wait on "
			    "the run to call xtc_sim_external_complete\n",
			    XTC_SIM_EXT_WAIT_MS / 1000);
			abort();
		}
		(void)__os_sleep_ns(1000 * 1000);
	}
}

/* The due event: wait for the producer, then deliver. */
static void
__ext_due(void *a)
{
	struct xtc_sim_ext *x = a;
	void (*fn)(void *) = NULL;
	void *arg = NULL;
#if XTC_DST_BUG(XTC_DST_BUG_EXT_NOWAIT)
	/* planted bug: deliver whatever is there now; a token the producer
	 * has not completed is dropped (and leaked, so its late complete()
	 * stays a valid write) */
	(void)pthread_mutex_lock(&x->lock);
	fn = x->done ? x->fn : NULL;
	arg = x->arg;
	(void)pthread_mutex_unlock(&x->lock);
	if (fn == NULL)
		return;
#else
	__ext_wait(x, &fn, &arg);
#endif
	(void)pthread_mutex_destroy(&x->lock);
	__os_free(x);
	if (fn != NULL)
		fn(arg);
}

/* The loop was finalized with the event pending: the delivery is lost
 * with the loop, as a message to a dead proc is. */
static void
__ext_drop(void *a)
{
	struct xtc_sim_ext *x = a;
	void (*fn)(void *) = NULL;
	void *arg = NULL;
	__ext_wait(x, &fn, &arg);
	(void)pthread_mutex_destroy(&x->lock);
	__os_free(x);
}

/* PUBLIC: int xtc_sim_external_begin __P((int, int64_t, xtc_sim_ext_t **)); */
/*
 * Announce an external completion.  Under a run (a sim build, called on
 * a fiber of the run), its delivery is due at now + a seeded delay in
 * [0, max_delay_ns]; lane >= 0 (0..15) keeps the lane's deliveries in
 * begin order (a later begin is never due before an earlier one), -1
 * draws freely.  *out is the token for xtc_sim_external_complete.
 * Outside a run *out is NULL, which complete() treats as "deliver now".
 * XTC_E_INVAL for a NULL out, a negative delay or a lane past 15;
 * XTC_E_STATE under a run but off a fiber of it (no loop to schedule
 * the delivery on); XTC_E_NOMEM.
 */
int
xtc_sim_external_begin(int lane, int64_t max_delay_ns, xtc_sim_ext_t **out)
{
	struct xtc_sim_ext *x;
	int64_t now = 0, due;
	int rc;

	if (out == NULL)
		return XTC_E_INVAL;
	*out = NULL;
	if (max_delay_ns < 0 || lane < -1 || lane >= XTC_SIM_EXT_LANES)
		return XTC_E_INVAL;
#if defined(XTC_IO_BACKEND_SIM)
	if (!__xtc_sim_active())
		return XTC_OK;
#else
	return XTC_OK; /* no run outside a sim build: deliver at complete */
#endif
	(void)__xtc_sim_vclock(&now);
	due = now +
	    (int64_t)__xtc_sim_rng_range(XTC_SIM_RNG_EXT,
	        (uint64_t)max_delay_ns + 1);
#if !XTC_DST_BUG(XTC_DST_BUG_EXT_LANE)
	if (lane >= 0 && due < g_ext_lane_due[lane])
		due = g_ext_lane_due[lane];
#endif
	if ((rc = __os_calloc(1, sizeof *x, (void **)&x)) != XTC_OK)
		return rc;
	(void)pthread_mutex_init(&x->lock, NULL);
	rc = __xtc_sim_ext_schedule(due, __ext_due, __ext_drop, x);
	if (rc != XTC_OK) {
		(void)pthread_mutex_destroy(&x->lock);
		__os_free(x);
		return rc;
	}
	if (lane >= 0)
		g_ext_lane_due[lane] = due;
	*out = x;
	return XTC_OK;
}

/* PUBLIC: int xtc_sim_external_complete __P((xtc_sim_ext_t *, void (*)(void *), void *)); */
/*
 * Deliver an external completion, from any thread.  A NULL token (begun
 * outside a run) runs fn(arg) now, on the calling thread.  A token begun
 * under a run records fn(arg) and returns; the sim runs fn(arg) on its
 * own thread at the due time (fn may be NULL: the event is only a point
 * in virtual time).  Call it exactly once per begun token, including
 * when the work failed (deliver the failure through fn): until it is
 * called the run waits at the due time.  The token is invalid after.
 */
int
xtc_sim_external_complete(xtc_sim_ext_t *t, void (*fn)(void *), void *arg)
{
	if (t == NULL) {
		if (fn != NULL)
			fn(arg);
		return XTC_OK;
	}
	(void)pthread_mutex_lock(&t->lock);
	t->fn = fn;
	t->arg = arg;
	t->done = 1;
	(void)pthread_mutex_unlock(&t->lock);
	return XTC_OK;
}

/* Per-run reset of the lane order (xtc_sim_activate). */
static void
__ext_reset(void)
{
	int i;
	for (i = 0; i < XTC_SIM_EXT_LANES; i++)
		g_ext_lane_due[i] = 0;
}
