/*-
 * Copyright (c) 2026, The XTC Project
 * @stability experimental
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_sim.h
 *	Deterministic simulation testing (DST): run an application's
 *	procs under a seeded, single-threaded schedule against a virtual
 *	clock, inject faults, and replay any failing seed exactly.
 *
 *	A test builds against a libxtc configured --with-io-backend=sim
 *	(installed as libxtc-sim.a, pkg-config xtc-sim), creates an
 *	xtc_exec, spawns its procs, and calls xtc_sim_exec_run(exec, seed,
 *	budget).  The same seed and configuration replay byte-identically;
 *	a sweep over seeds explores interleavings.  Decision points in the
 *	application draw from xtc_sim_rand (seeded inside a run) and branch
 *	on XTC_SIM_BUGGIFY (a pessimal path taken on a seeded once-per-run
 *	coin).  xtc_sim_fd_ready_at makes an fd the test owns readable at a
 *	virtual time (a simulated device fence), and
 *	xtc_sim_external_begin / _complete schedule a completion that a
 *	plain OS thread produces at a seeded virtual time.
 *
 *	Every function here is linkable in every build.  Off a sim build
 *	they are dormant: xtc_sim_exec_run returns XTC_E_NOSYS, the fault
 *	and buggify knobs never fire, xtc_sim_rand falls back to
 *	xtc_rand_u64, and xtc_sim_external_complete calls its function at
 *	once.  Experimental: the surface may change in a minor release.
 */

#ifndef XTC_SIM_H
#define XTC_SIM_H

#include "xtc_export.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 1 while a deterministic simulation run is active (set by
 * xtc_sim_activate, and so by xtc_sim_exec_run; cleared by
 * xtc_sim_deactivate), else 0.  Always 0 off a sim build.
 *
 * PUBLIC: int      xtc_sim_active __P((void));
 * PUBLIC: uint64_t xtc_sim_rand __P((uint64_t));
 * PUBLIC: void     xtc_sim_strict __P((int));
 * PUBLIC: int      xtc_sim_nondeterminism_count __P((void));
 * PUBLIC: void     xtc_sim_activate __P((uint64_t));
 * PUBLIC: void     xtc_sim_deactivate __P((void));
 * PUBLIC: void     xtc_sim_clock_enable __P((int64_t));
 * PUBLIC: void     xtc_sim_clock_disable __P((void));
 * PUBLIC: void     xtc_sim_clock_advance __P((int64_t));
 * PUBLIC: void     xtc_sim_clock_set __P((int64_t));
 * PUBLIC: int      xtc_sim_clock_active __P((void));
 */
XTC_API int      xtc_sim_active(void);

/* An application draw: a uniform value in [0, bound) from the run's
 * application stream, which no runtime decision draws from, so adding a
 * draw never moves the schedule.  Seeded and replayed inside a run;
 * outside one (or off a sim build) it is xtc_rand_u64() % bound.
 * bound 0 returns 0. */
XTC_API uint64_t xtc_sim_rand(uint64_t bound);

/* Determinism guard.  Under a run, a sim-reachable primitive that would
 * break seed replay (a real clock, an environment read, a raw thread id)
 * records a violation and, in strict mode (the default), aborts naming
 * the source.  xtc_sim_strict toggles abort-versus-count;
 * xtc_sim_nondeterminism_count lets a harness assert zero to prove a
 * run was deterministic. */
XTC_API void     xtc_sim_strict(int on);
XTC_API int      xtc_sim_nondeterminism_count(void);

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
XTC_API void     xtc_sim_clock_skew(int64_t offset_ns, int jitter_ns);

/* Activate sim with a root seed (also resets every stream).  Idempotent
 * re-activation re-seeds.  xtc_sim_exec_run does this itself; call it
 * directly only to draw seeded values outside a run.  Test-only. */
XTC_API void     xtc_sim_activate(uint64_t seed);
XTC_API void     xtc_sim_deactivate(void);

/* Deterministic fault toggle: returns 1 with probability
 * pct_per_1000/1000, drawn from the dedicated FAULT stream (so enabling
 * faults never shifts the schedule); 0 when sim is inactive.  A test
 * calls it at a fault decision point; the same seed reproduces the
 * identical fault schedule.
 *
 * PUBLIC: int xtc_sim_fault __P((unsigned));
 */
XTC_API int      xtc_sim_fault(unsigned pct_per_1000);

/*
 * Critical-section fault points.  A fault point marks an interleaving-
 * sensitive critical section; under sim, when points are enabled,
 * reaching one draws from the FAULT stream and, on a hit, records the
 * fire.  Enable with a per-1000 fire probability; query coverage with
 * the fires/seen accessors.  Production never
 * reaches a fire (sim inactive); the call is a single relaxed load.
 *
 * PUBLIC: void     xtc_sim_fault_points_enable __P((unsigned));
 * PUBLIC: void     xtc_sim_fault_points_disable __P((void));
 * PUBLIC: int      xtc_sim_fault_point __P((const char *));
 * PUBLIC: uint64_t xtc_sim_fault_point_fires __P((const char *));
 * PUBLIC: int      xtc_sim_fault_points_seen __P((void));
 */
XTC_API void     xtc_sim_fault_points_enable(unsigned pct_per_1000);
XTC_API void     xtc_sim_fault_points_disable(void);
XTC_API int      xtc_sim_fault_point(const char *name);
XTC_API uint64_t xtc_sim_fault_point_fires(const char *name);
XTC_API int      xtc_sim_fault_points_seen(void);

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
 */
XTC_API void    xtc_sim_io_faults_enable(int64_t lat_min_ns, int64_t lat_max_ns,
                    unsigned fault_pct_per_1000);
XTC_API void    xtc_sim_io_faults_disable(void);

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
 */
XTC_API void xtc_sim_io_corrupt_enable(unsigned corrupt_pct_per_1000);
XTC_API void xtc_sim_io_corrupt_disable(void);

/*
 * Disk-full (ENOSPC) injection: a seeded coin makes a WRITE fail with a
 * hard out-of-space error mid-workload -- the whole op fails and nothing
 * persists, distinct from a short transfer or a torn write -- so a
 * storage engine's graceful-degradation path is exercised.  Off by
 * default; reset via enable(0).  A no-op outside a sim run.
 *
 * PUBLIC: void xtc_sim_io_enospc_enable __P((unsigned));
 */
XTC_API void xtc_sim_io_enospc_enable(unsigned pct_per_1000);

/*
 * Stale-data reads (FoundationDB's "the disk returns an OLD durable
 * version").  On a seeded coin a read at an offset that was written,
 * then overwritten, returns the PRIOR (structurally valid but out of
 * date) contents -- catching recovery/cache code that skips a
 * version/LSN check.  General (fd+offset level): a bounded ring
 * snapshots superseded write payloads.  Off by default; enable(pct)
 * arms it and resets the ring for the run.  A no-op outside a sim run.
 *
 * PUBLIC: void xtc_sim_io_stale_enable __P((unsigned));
 */
XTC_API void xtc_sim_io_stale_enable(unsigned pct_per_1000);

/*
 * Write-back cache crash model (FoundationDB's simulated disk): pwrite
 * lands in a volatile per-fd cache (tracked as the written high-water),
 * fsync/fdatasync promotes the written extent to durable, and a crash
 * loses everything past the last fsync.  This lets a crash-recovery test
 * catch a writer that ACKs a commit WITHOUT fsyncing it -- the sim
 * writes to a real temp file, so without this model the bytes survive a
 * crash regardless of fsync, hiding an ack-before-durable bug.  A test
 * arms it, and after the crash asks xtc_sim_io_durable_end(fd) for the
 * true post-crash frontier (last fsync-confirmed byte) to truncate to,
 * instead of trusting the writer's self-reported durable point.  Off
 * unless armed; bounded fd table.
 *
 * PUBLIC: void     xtc_sim_io_wb_enable __P((int));
 * PUBLIC: uint64_t xtc_sim_io_durable_end __P((int));
 */
XTC_API void     xtc_sim_io_wb_enable(int on);
XTC_API uint64_t xtc_sim_io_durable_end(int fd);

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
 * Loops are identified by executor slot + 1 (xtc_exec_loop index + 1; 0 ==
 * standalone).  Not pid.loop_id, which since 2.2 is a process-wide id.
 * A blocked edge makes __mbox_deliver DROP the message via the sender's
 * existing soft-full path (XTC_E_AGAIN), so a partitioned peer never
 * deadlocks the sim.  A monitor's DOWN and a link's EXIT still cross the
 * cut: a real process never drops one, and nobody resends it.  Scope: the
 * in-process cross-loop path only -- the real cross-machine raw-socket
 * transport (io_net.c) cannot run under the single-thread sim and is NOT
 * modelled here.
 *
 * PUBLIC: void    xtc_sim_partition_set __P((int, int, int));
 * PUBLIC: void    xtc_sim_partition_isolate __P((int));
 * PUBLIC: void    xtc_sim_partition_clear __P((void));
 * PUBLIC: void    xtc_sim_net_latency __P((int64_t, int64_t));
 */
XTC_API void    xtc_sim_partition_set(int src_loop_id, int dst_loop_id, int blocked);
XTC_API void    xtc_sim_partition_isolate(int loop_id);
XTC_API void    xtc_sim_partition_clear(void);
XTC_API void    xtc_sim_net_latency(int64_t min_ns, int64_t max_ns);

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
XTC_API void xtc_sim_buggify_enable(unsigned pct_per_1000);
XTC_API void xtc_sim_buggify_disable(void);
XTC_API int  xtc_sim_buggify(const char *name);
XTC_API int  xtc_sim_buggify_active_count(void);
XTC_API int  xtc_sim_buggify_reached_count(void);
XTC_API int  xtc_sim_buggify_site(int idx, char *buf, size_t buflen,
                 int *out_activated);
XTC_API int  xtc_sim_buggify_fault(unsigned pct_per_1000);

/* Branch on a buggify point in runtime code:
 *     if (XTC_SIM_BUGGIFY("wal.flush.tiny_batch")) { ... pessimal ... }
 * A single relaxed load in production; elided with XTC_INJECT_DISABLE. */
#if defined(XTC_INJECT_DISABLE)
# define XTC_SIM_BUGGIFY(name)  (0)
#else
# define XTC_SIM_BUGGIFY(name)  xtc_sim_buggify(name)
#endif

/* Virtual (logical) clock.  When enabled, xtc_clock_mono returns the
 * virtual time instead of the host monotonic clock, so time is a pure
 * function of the schedule.  xtc_sim_exec_run enables it at 0 for the
 * run.  Test/scheduler-only. */
XTC_API void     xtc_sim_clock_enable(int64_t start_ns);
XTC_API void     xtc_sim_clock_disable(void);
XTC_API void     xtc_sim_clock_advance(int64_t delta_ns);
XTC_API void     xtc_sim_clock_set(int64_t ns);

/* 1 while the virtual clock is active (inside a deterministic run), so
 * xtc_clock_mono returns virtual time; 0 otherwise.  For code that
 * derives a wall-clock-like value from xtc_clock_mono: under the virtual
 * clock it must not add an offset sampled from real time (that offset
 * would carry real time into the run and break replay). */
XTC_API int      xtc_sim_clock_active(void);

/*
 * Run an executor's loops deterministically (DST scheduler).  Activates
 * sim with `seed` + the virtual clock, then drives the N loops as N
 * cooperatively-scheduled entities on the calling thread under a
 * seed-determined interleaving, until quiescence, the step budget
 * (max_steps; <= 0 = unbounded), or xtc_exec_stop.  Requires a sim
 * build (--with-io-backend=sim): any other build returns XTC_E_NOSYS
 * and runs nothing.  Returns XTC_OK on quiescence (or on
 * xtc_exec_stop), XTC_E_AGAIN if the budget was hit with work
 * remaining, XTC_E_DEADLK if procs are alive but nothing can wake them,
 * XTC_E_INTERNAL on a determinism violation (count mode) or a broken
 * structural invariant, the consistency check's nonzero result, or a
 * negative code on a loop-step error.  Declared opaquely (xtc_exec is
 * defined in xtc_exec.h).
 *
 * PUBLIC: int xtc_sim_exec_run __P((struct xtc_exec *, uint64_t, long));
 */
struct xtc_exec;
XTC_API int      xtc_sim_exec_run(struct xtc_exec *e, uint64_t seed, long max_steps);

/* Structural invariant checker (run after each sim step): returns
 * XTC_OK if all per-loop invariants hold, XTC_E_INTERNAL on the first
 * violation.  A 64-bit digest of observable per-loop state for replay
 * equality (same seed+config -> same hash).  Both take an xtc_exec.
 *
 * PUBLIC: int      xtc_sim_check __P((struct xtc_exec *));
 * PUBLIC: uint64_t xtc_sim_state_hash __P((struct xtc_exec *));
 */
XTC_API int      xtc_sim_check(struct xtc_exec *e);
XTC_API uint64_t xtc_sim_state_hash(struct xtc_exec *e);

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
 */
XTC_API void xtc_sim_sched_pessimal(unsigned pct_per_1000);

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
 */
XTC_API void xtc_sim_swizzle_enable(unsigned pct_per_1000);
XTC_API void xtc_sim_swizzle_disable(void);

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
 */
typedef int (*xtc_sim_consistency_fn)(void *arg);
XTC_API void xtc_sim_set_consistency_check(xtc_sim_consistency_fn fn, void *arg);

/*
 * Scheduled fd readiness: make fd ready for `revents` (XTC_IO_READABLE,
 * _WRITABLE, _HUP, _ERR) from virtual time due_ns on.  A simulated
 * device hands its client a real fd it owns (a pipe or an eventfd) as a
 * completion fence and schedules it: a proc parked on the fd
 * (xtc_proc_wait_fd, xtc_accel_wait_fence) wakes at due_ns, replayed
 * from the seed.  The readiness is level: every registration of the fd
 * whose interest overlaps revents fires from due_ns on, including one
 * made after due_ns.  A later call for the fd replaces the schedule and
 * revents 0 withdraws it; every run ends with none.  The fd is never
 * read or written.  XTC_E_INVAL (negative fd or time, another bit in
 * revents), XTC_E_RESOURCE (64 fds already scheduled), XTC_E_NOSYS off
 * a sim build.
 *
 * PUBLIC: int xtc_sim_fd_ready_at __P((int, int64_t, uint32_t));
 */
XTC_API int xtc_sim_fd_ready_at(int fd, int64_t due_ns, uint32_t revents);

/*
 * External completions: give a completion that a plain OS thread
 * produces (a device worker, a vendor runtime's callback thread) a
 * seeded virtual time, so the application's real thread code runs under
 * a simulation and its delivery still replays.
 *
 *     xtc_sim_ext_t *tok;
 *     xtc_sim_external_begin(0, 2 * MS, &tok);  // on the engine fiber
 *     hand_to_device_thread(job, tok);
 *     ...                                       // on the device thread
 *     xtc_sim_external_complete(tok, deliver, job);  // deliver(job)
 *                                                    // calls xtc_send
 *
 * begin, on a fiber of the run, fixes the delivery's due time: now plus
 * a seeded delay in [0, max_delay_ns].  lane 0..15 keeps one lane's
 * deliveries in begin order (a FIFO device queue); -1 draws each delay
 * freely.  complete, from any thread, records fn(arg); at the due time
 * the simulator waits (in real time) for complete if the thread has not
 * called it yet, then runs fn(arg) on its own thread, outside any proc
 * (xtc_send from there is a send from a foreign thread).  Call complete
 * exactly once per token, also when the work failed.
 *
 * fn runs on the only thread of the run, so nothing else runs while it
 * waits.  It uses xtc_send: an XTC_E_AGAIN (a full mailbox) leaves the
 * message fn's, to keep and retry, for example by handing it to a proc
 * that can wait.  A wait for something only the run can do returns at
 * once: xtc_send_wait on a full mailbox, xtc_reg_whereis_wait /
 * xtc_reg_send_wait on a missing name and an off-proc xtc_svr_call
 * return XTC_E_STATE.  Any timed wait made on that thread off a fiber
 * (xtc_notify_wait, xtc_future_wait, a lock with a timeout) never
 * sleeps: virtual time cannot pass while the only thread is blocked, so
 * it expires at once (XTC_E_AGAIN) without reading the wall clock.
 *
 * Outside a run, begin stores NULL and complete(NULL, fn, arg) calls
 * fn(arg) at once on the calling thread: production code calls both
 * unconditionally.  begin returns XTC_E_INVAL (NULL out, a negative
 * delay, a lane past 15), XTC_E_STATE (under a run but not on one of its
 * fibers), or XTC_E_NOMEM.
 *
 * PUBLIC: int xtc_sim_external_begin __P((int, int64_t, xtc_sim_ext_t **));
 * PUBLIC: int xtc_sim_external_complete __P((xtc_sim_ext_t *, void (*)(void *), void *));
 */
typedef struct xtc_sim_ext xtc_sim_ext_t;
XTC_API int xtc_sim_external_begin(int lane, int64_t max_delay_ns,
                xtc_sim_ext_t **out);
XTC_API int xtc_sim_external_complete(xtc_sim_ext_t *tok,
                void (*fn)(void *), void *arg);

/*
 * Planted bugs (the "bug-detection latency" yardstick).  Guard a
 * deliberate defect with XTC_DST_BUG(n) and build the test once with
 * -DXTC_DST_INJECT_BUG=n: the DST run must then fail within a few seeds,
 * proving the simulation catches that class of bug.  XTC_DST_BUG(n) is
 * 1 only in the build that defines XTC_DST_INJECT_BUG to n, and 0 in
 * every other build, so the defect never reaches a normal build.  The
 * ids are the application's own; libxtc's planted bugs use the same
 * mechanism with ids it reserves for itself.
 *
 *     #if !XTC_DST_BUG(3)
 *         fsync_before_ack(wal);
 *     #endif
 */
#if defined(XTC_DST_INJECT_BUG)
# define XTC_DST_BUG(n)  ((XTC_DST_INJECT_BUG) == (n))
#else
# define XTC_DST_BUG(n)  (0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* XTC_SIM_H */
