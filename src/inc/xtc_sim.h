/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_sim.h
 *	Deterministic Simulation Testing (DST) seams.  See docs/M_DST.md.
 *
 *	Phase 0 is the seeded PRNG tree: a single root seed splits into
 *	per-stream sub-streams so a draw at one decision site (steal
 *	victim, placement, lock victim, fault toggle) does not perturb
 *	another site's sequence -- the FoundationDB discipline that keeps
 *	a replay stable when a new draw site is added.
 *
 *	When sim is INACTIVE (the production default) __xtc_sim_active()
 *	is 0 and the runtime's existing ad-hoc randomness is used
 *	unchanged; the call sites consult __xtc_sim_active() and only draw
 *	from the seeded tree under sim.  This module compiles into every
 *	build but is dormant unless a sim run activates it.
 */

#ifndef XTC_SIM_H
#define XTC_SIM_H

#include <stdint.h>

/* Well-known PRNG streams.  Each scheduler-relevant random draw site
 * owns a stream so adding a draw in one site cannot shift another. */
enum xtc_sim_stream {
	XTC_SIM_RNG_SCHED   = 0,   /* which runnable loop advances next */
	XTC_SIM_RNG_STEAL   = 1,   /* work-stealing victim selection */
	XTC_SIM_RNG_PLACE   = 2,   /* round-robin proc/task placement */
	XTC_SIM_RNG_LOCKVIC = 3,   /* lock-manager deadlock victim */
	XTC_SIM_RNG_IO      = 4,   /* simulated I/O latency / completion order */
	XTC_SIM_RNG_FAULT   = 5,   /* fault-injection toggles */
	XTC_SIM_RNG_APP     = 6,   /* application/test draws */
	XTC_SIM_RNG_BUGGIFY = 7,   /* buggify per-call activation coins */
	XTC_SIM_RNG_NSTREAMS
};

/*
 * 1 while a deterministic simulation run is active (set by
 * xtc_sim_activate, cleared by xtc_sim_deactivate).  Hot-path call
 * sites branch on this to decide between seeded and ad-hoc randomness.
 *
 * PUBLIC: int      __xtc_sim_active __P((void));
 * PUBLIC: void     __xtc_sim_nondeterminism __P((const char *));
 * PUBLIC: void     xtc_sim_strict __P((int));
 * PUBLIC: int      xtc_sim_nondeterminism_count __P((void));
 * PUBLIC: void     xtc_sim_activate __P((uint64_t));
 * PUBLIC: void     xtc_sim_deactivate __P((void));
 * PUBLIC: uint64_t __xtc_sim_rng __P((int));
 * PUBLIC: uint64_t __xtc_sim_rng_range __P((int, uint64_t));
 * PUBLIC: void     xtc_sim_clock_enable __P((int64_t));
 * PUBLIC: void     xtc_sim_clock_disable __P((void));
 * PUBLIC: void     xtc_sim_clock_advance __P((int64_t));
 * PUBLIC: void     xtc_sim_clock_set __P((int64_t));
 * PUBLIC: int      __xtc_sim_vclock __P((int64_t *));
 */
int      __xtc_sim_active(void);

/* Determinism guard.  A sim-reachable primitive that would break seed
 * replay (real clock, unseeded RNG, env read, raw thread id) calls
 * __xtc_sim_nondeterminism(what); during a sim run it records the
 * violation and, in strict mode (default), aborts naming the source.
 * xtc_sim_strict toggles abort-vs-count; xtc_sim_nondeterminism_count
 * lets a harness assert zero to PROVE a run was fully deterministic. */
void     __xtc_sim_nondeterminism(const char *what);
void     xtc_sim_strict(int on);
int      xtc_sim_nondeterminism_count(void);

/* Clock skew (FoundationDB "the clock is not perfect").  The scheduler
 * still schedules timers against the true virtual clock, but a fiber
 * that reads the clock (xtc_proc time, timeout math) observes
 * true + offset (+/- a seeded jitter), so its notion of elapsed time can
 * disagree with when its timers actually fire -- stressing code that
 * assumes wall time equals scheduler time.  Seeded and bounded, so it
 * replays; applied only on the observation seam, so it can never desync
 * the scheduler.  offset_ns is a fixed per-run offset; jitter_ns (>=0)
 * is the +/- band of a seeded per-read wobble (0 = none).  Off by
 * default; reset on deactivate.
 *
 * PUBLIC: void xtc_sim_clock_skew __P((int64_t, int));
 */
void     xtc_sim_clock_skew(int64_t offset_ns, int jitter_ns);

/* Activate sim with a root seed (also resets every stream).  Idempotent
 * re-activation re-seeds.  Test-only; never called in production. */
void     xtc_sim_activate(uint64_t seed);
void     xtc_sim_deactivate(void);

/* Draw the next 64-bit value from stream `s` (0..XTC_SIM_RNG_NSTREAMS-1).
 * Deterministic given the activation seed.  Undefined if sim inactive
 * (callers must gate on __xtc_sim_active()). */
uint64_t __xtc_sim_rng(int s);

/* Convenience: a uniform value in [0, bound) from stream `s`.  bound==0
 * returns 0. */
uint64_t __xtc_sim_rng_range(int s, uint64_t bound);

/* Deterministic fault toggle: returns 1 with probability
 * pct_per_1000/1000, drawn from the dedicated FAULT stream (so enabling
 * faults never shifts the schedule); 0 when sim is inactive.  A test
 * calls it at a fault decision point; the same seed reproduces the
 * identical fault schedule.
 *
 * PUBLIC: int xtc_sim_fault __P((unsigned));
 */
int      xtc_sim_fault(unsigned pct_per_1000);

/*
 * Critical-section fault points.  A fault point marks an interleaving-
 * sensitive critical section; under sim, when points are enabled,
 * reaching one draws from the FAULT stream and, on a hit, records the
 * fire.  Enable with a per-1000 fire probability; query coverage with
 * the fires/seen accessors.  See docs/M_DST.md.  Production never
 * reaches a fire (sim inactive); the call is a single relaxed load.
 *
 * PUBLIC: void     xtc_sim_fault_points_enable __P((unsigned));
 * PUBLIC: void     xtc_sim_fault_points_disable __P((void));
 * PUBLIC: int      xtc_sim_fault_point __P((const char *));
 * PUBLIC: uint64_t xtc_sim_fault_point_fires __P((const char *));
 * PUBLIC: int      xtc_sim_fault_points_seen __P((void));
 */
void     xtc_sim_fault_points_enable(unsigned pct_per_1000);
void     xtc_sim_fault_points_disable(void);
int      xtc_sim_fault_point(const char *name);
uint64_t xtc_sim_fault_point_fires(const char *name);
int      xtc_sim_fault_points_seen(void);

/*
 * Simulated I/O faults (DST).  When enabled, the sim I/O backend defers
 * file-AIO completions by a seeded latency (so completion ORDER across
 * concurrent ops is part of the replayable schedule) and may inject a
 * seeded fault (short transfer or EIO).  Off by default -- a sim run
 * that does not enable them gets inline completion.  Seeded on the IO
 * stream, so enabling does not perturb the schedule.
 *
 * PUBLIC: void    xtc_sim_io_faults_enable __P((int64_t, int64_t, unsigned));
 * PUBLIC: void    xtc_sim_io_faults_disable __P((void));
 * PUBLIC: int     __xtc_sim_io_faults_active __P((void));
 * PUBLIC: int64_t __xtc_sim_io_latency __P((void));
 * PUBLIC: int     __xtc_sim_io_should_fault __P((void));
 */
void    xtc_sim_io_faults_enable(int64_t lat_min_ns, int64_t lat_max_ns,
            unsigned fault_pct_per_1000);
void    xtc_sim_io_faults_disable(void);
int     __xtc_sim_io_faults_active(void);
int64_t __xtc_sim_io_latency(void);
int     __xtc_sim_io_should_fault(void);

/*
 * Simulated TORN / CORRUPT writes and reads (DST) -- the torn-page fault
 * class FoundationDB models.  Distinct from the short-transfer / EIO
 * faults above (which report a truncated-but-clean result the caller
 * re-issues): a torn write actually PERSISTS fewer bytes than requested
 * while still REPORTING full success, and a corrupt read flips a byte in
 * the returned buffer.  Both leave latent bad data a checksum must catch
 * -- a storage engine's real durability hazard.  Off by default; seeded
 * on the IO stream so enabling does not perturb the schedule.  A write
 * is torn (its persisted prefix chosen) with probability corrupt_pct;
 * likewise a read is bit-flipped with the same probability.
 *
 * PUBLIC: void xtc_sim_io_corrupt_enable __P((unsigned));
 * PUBLIC: void xtc_sim_io_corrupt_disable __P((void));
 * PUBLIC: int  __xtc_sim_io_corrupt_active __P((void));
 * PUBLIC: int  __xtc_sim_io_torn_prefix __P((int));
 * PUBLIC: int  __xtc_sim_io_flip_byte __P((int));
 */
void xtc_sim_io_corrupt_enable(unsigned corrupt_pct_per_1000);
void xtc_sim_io_corrupt_disable(void);
int  __xtc_sim_io_corrupt_active(void);
int  __xtc_sim_io_torn_prefix(int full_len);
int  __xtc_sim_io_flip_byte(int len);

/*
 * Simulated network partition + message latency (DST).  A seeded,
 * deterministic model of a partitioned / lossy / delayed network at the
 * cross-LOOP message granularity xtc's sim models: xtc_send between
 * procs on different loops routes through __mbox_deliver, the single
 * cross-loop delivery seam.  These let a DST test cut message flow
 * between loops (a partition) and/or defer each cross-loop delivery by a
 * seeded latency so delivery ORDER is part of the replayable schedule.
 * All OFF by default (no behaviour change in production or normal sim).
 *
 * Loops are identified by pid.loop_id (== exec_id + 1; 0 == standalone).
 * A blocked edge makes __mbox_deliver DROP the message via the sender's
 * existing soft-full path (XTC_E_AGAIN), so a partitioned peer never
 * deadlocks the sim.  Scope: the in-process cross-loop path only -- the
 * real cross-machine raw-socket transport (io_net.c) cannot run under
 * the single-thread sim and is NOT modelled here.
 *
 * PUBLIC: void    xtc_sim_partition_set __P((int, int, int));
 * PUBLIC: void    xtc_sim_partition_isolate __P((int));
 * PUBLIC: void    xtc_sim_partition_clear __P((void));
 * PUBLIC: int     __xtc_sim_partition_blocked __P((int, int));
 * PUBLIC: void    xtc_sim_net_latency __P((int64_t, int64_t));
 * PUBLIC: int64_t __xtc_sim_net_latency __P((void));
 */
void    xtc_sim_partition_set(int src_loop_id, int dst_loop_id, int blocked);
void    xtc_sim_partition_isolate(int loop_id);
void    xtc_sim_partition_clear(void);
int     __xtc_sim_partition_blocked(int src_loop_id, int dst_loop_id);
void    xtc_sim_net_latency(int64_t min_ns, int64_t max_ns);
int64_t __xtc_sim_net_latency(void);

/* Plant a critical-section fault point in runtime code.  A single
 * relaxed load in production (sim inactive); under sim with points
 * enabled it perturbs/records per the FAULT stream.  Elided entirely
 * with XTC_INJECT_DISABLE, matching XTC_INJECTION_POINT. */
#if defined(XTC_INJECT_DISABLE)
# define XTC_SIM_FAULT_POINT(name)  ((void)0)
#else
# define XTC_SIM_FAULT_POINT(name)  ((void)xtc_sim_fault_point(name))
#endif

/*
 * Buggify (FoundationDB-style).  A named point in the REAL runtime code
 * that, under sim, lets the code take a legal-but-pessimal path.  Unlike
 * xtc_sim_fault (a fresh draw per call), a buggify point is a coin
 * flipped ONCE per run per site: decided on first reach, cached, and
 * every later reach of the same name returns the same decision -- so a
 * buggified site is consistent within a run and the run replays.  0 in
 * production / when disabled.  Enable with a per-1000 activation
 * probability; query coverage with the active-count.
 *
 * PUBLIC: void xtc_sim_buggify_enable __P((unsigned));
 * PUBLIC: void xtc_sim_buggify_disable __P((void));
 * PUBLIC: int  xtc_sim_buggify __P((const char *));
 * PUBLIC: int  xtc_sim_buggify_active_count __P((void));
 * PUBLIC: int  xtc_sim_buggify_reached_count __P((void));
 * PUBLIC: int  xtc_sim_buggify_site __P((int, char *, size_t, int *));
 * PUBLIC: int  xtc_sim_buggify_fault __P((unsigned));
 */
void xtc_sim_buggify_enable(unsigned pct_per_1000);
void xtc_sim_buggify_disable(void);
int  xtc_sim_buggify(const char *name);
int  xtc_sim_buggify_active_count(void);
int  xtc_sim_buggify_reached_count(void);
int  xtc_sim_buggify_site(int idx, char *buf, size_t buflen,
         int *out_activated);
int  xtc_sim_buggify_fault(unsigned pct_per_1000);

/* Branch on a buggify point in runtime code:
 *     if (XTC_SIM_BUGGIFY("wal.flush.tiny_batch")) { ... pessimal ... }
 * A single relaxed load in production; elided with XTC_INJECT_DISABLE. */
#if defined(XTC_INJECT_DISABLE)
# define XTC_SIM_BUGGIFY(name)  (0)
#else
# define XTC_SIM_BUGGIFY(name)  xtc_sim_buggify(name)
#endif

/* Virtual (logical) clock.  When enabled, __os_clock_mono returns the
 * virtual time instead of the host monotonic clock, so time is a pure
 * function of the schedule.  Test/scheduler-only. */
void     xtc_sim_clock_enable(int64_t start_ns);
void     xtc_sim_clock_disable(void);
void     xtc_sim_clock_advance(int64_t delta_ns);
void     xtc_sim_clock_set(int64_t ns);

/* Query the virtual clock: returns 1 and writes *out_ns when active,
 * 0 otherwise.  The single seam __os_clock_mono consults. */
int      __xtc_sim_vclock(int64_t *out_ns);

/*
 * Run an executor's loops deterministically (DST scheduler).  Activates
 * sim with `seed` + the virtual clock, then drives the N loops as N
 * cooperatively-scheduled entities on the calling thread under a
 * seed-determined interleaving, until quiescence, the step budget
 * (max_steps; <= 0 = unbounded), or xtc_exec_stop.  Requires a sim
 * build (--with-io-backend=sim).  Returns XTC_OK on quiescence,
 * XTC_E_AGAIN if the budget was hit with work remaining, or a negative
 * code on a loop-step error.  Declared opaquely (xtc_exec is defined in
 * xtc_exec.h).
 *
 * PUBLIC: int xtc_sim_exec_run __P((struct xtc_exec *, uint64_t, long));
 */
struct xtc_exec;
int      xtc_sim_exec_run(struct xtc_exec *e, uint64_t seed, long max_steps);

/* Structural invariant checker (run after each sim step): returns
 * XTC_OK if all per-loop invariants hold, XTC_E_INTERNAL on the first
 * violation.  A 64-bit digest of observable per-loop state for replay
 * equality (same seed+config -> same hash).  Both take an xtc_exec.
 *
 * PUBLIC: int      xtc_sim_check __P((struct xtc_exec *));
 * PUBLIC: uint64_t xtc_sim_state_hash __P((struct xtc_exec *));
 */
int      xtc_sim_check(struct xtc_exec *e);
uint64_t xtc_sim_state_hash(struct xtc_exec *e);

/*
 * Adversarial scheduler bias (FoundationDB "hunt the worst interleaving"
 * rather than a benign uniform-random order).  When enabled, the sim
 * scheduler, on a seeded XTC_SIM_RNG_SCHED coin, takes the PESSIMAL
 * pick: it keeps running one pinned loop as long as that loop stays
 * runnable -- monopolizing the executor and starving every peer (the
 * classic worst order: a fiber holding a resource a peer is blocked on
 * never yields).  When the pin parks, a new victim is pinned (the
 * least-recently-run runnable loop, so starvation rotates over a long
 * run).  pct_per_1000 is how often the pessimal pick is taken (0
 * disables; e.g. 500 = half the picks monopolize, half uniform, so
 * both orders are still explored across a seed sweep).  OFF by default
 * -- the default uniform pick keeps existing tests' schedules stable;
 * opt in from an adversarial test / swarm.  A no-op outside a sim run.
 *
 * PUBLIC: void xtc_sim_sched_pessimal __P((unsigned));
 * PUBLIC: int  __xtc_sim_sched_pessimal_pct __P((void));
 */
void xtc_sim_sched_pessimal(unsigned pct_per_1000);
int  __xtc_sim_sched_pessimal_pct(void);

/*
 * Completion / message SWIZZLE (reorder), independent of latency.  The
 * sim I/O event queue is normally due-time ordered; a deferred AIO
 * completion or a cross-loop message is delivered in due order, so
 * reordering only happens as a side effect of latency jitter.  When
 * swizzle is enabled, on a seeded XTC_SIM_RNG_IO coin an event is
 * inserted one slot LATER than its due order would place it -- a
 * legal reordering (the waiter simply wakes after a sibling completion
 * it would otherwise have preceded), which explicitly explores
 * completion/message-order interleavings the way FoundationDB swizzles
 * connection and disk completions.  pct_per_1000 is the per-insert
 * reorder probability (0 disables).  OFF by default.  A no-op outside
 * a sim run.
 *
 * PUBLIC: void xtc_sim_swizzle_enable __P((unsigned));
 * PUBLIC: void xtc_sim_swizzle_disable __P((void));
 * PUBLIC: int  __xtc_sim_swizzle_pct __P((void));
 */
void xtc_sim_swizzle_enable(unsigned pct_per_1000);
void xtc_sim_swizzle_disable(void);
int  __xtc_sim_swizzle_pct(void);

/*
 * Semantic consistency check (FoundationDB's end-of-test consistency
 * workload).  Install a callback that the sim runs ONCE at quiescence,
 * after all the seeded faults/chaos, to assert a GLOBAL application
 * invariant a per-step structural state hash cannot see -- e.g. "the
 * B-tree is still well formed and holds exactly the acked-commit set".
 * The callback returns XTC_OK if the invariant holds, nonzero otherwise;
 * the sim propagates a nonzero result as the run's failure.  NULL (the
 * default) means no check.  Set it before each run.  A no-op in
 * production.
 *
 * PUBLIC: void xtc_sim_set_consistency_check __P((xtc_sim_consistency_fn, void *));
 * PUBLIC: int  __xtc_sim_run_consistency_check __P((void));
 */
typedef int (*xtc_sim_consistency_fn)(void *arg);
void xtc_sim_set_consistency_check(xtc_sim_consistency_fn fn, void *arg);
int  __xtc_sim_run_consistency_check(void);

#endif /* XTC_SIM_H */
