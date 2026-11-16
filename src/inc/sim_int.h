/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/sim_int.h
 *	The simulator's internal hooks: the seams the runtime itself calls
 *	(the seeded stream draws, the determinism guard, the I/O and network
 *	fault models, the virtual-clock read).  Split out of xtc_sim.h so
 *	the installed header carries only the API a test of an application
 *	uses, and no __-prefixed name.  Library code and libxtc's own DST
 *	tests include this; an application uses xtc_sim.h alone (its
 *	xtc_sim_active and xtc_sim_rand cover what a test needs from here).
 */

#ifndef XTC_SIM_INT_H
#define XTC_SIM_INT_H

#include "xtc_sim.h"

/* Well-known PRNG streams.  Each scheduler-relevant random draw site
 * owns a stream so adding a draw in one site cannot shift another. */
enum xtc_sim_stream {
	XTC_SIM_RNG_SCHED   = 0,   /* which runnable loop advances next */
	XTC_SIM_RNG_STEAL   = 1,   /* work-stealing victim selection */
	XTC_SIM_RNG_PLACE   = 2,   /* round-robin proc/task placement */
	XTC_SIM_RNG_LOCKVIC = 3,   /* lock-manager deadlock victim */
	XTC_SIM_RNG_IO      = 4,   /* simulated I/O latency / completion order */
	XTC_SIM_RNG_FAULT   = 5,   /* fault-injection toggles */
	XTC_SIM_RNG_APP     = 6,   /* application/test draws (xtc_sim_rand) */
	XTC_SIM_RNG_BUGGIFY = 7,   /* buggify per-call activation coins */
	XTC_SIM_RNG_RAND    = 8,   /* xtc_rand_u64 on an un-seeded thread */
	XTC_SIM_RNG_EXT     = 9,   /* xtc_sim_external_begin delays */
	XTC_SIM_RNG_NSTREAMS
};

/* 1 while a deterministic simulation run is active (the public
 * xtc_sim_active reads the same flag).  Hot-path call sites branch on
 * this to decide between seeded and ad-hoc randomness. */
XTC_API int      __xtc_sim_active(void);

/* Determinism guard.  A sim-reachable primitive that would break seed
 * replay (real clock, env read, raw thread id) calls
 * __xtc_sim_nondeterminism(what); during a sim run it records the
 * violation and, in strict mode (default), aborts naming the source. */
XTC_API void     __xtc_sim_nondeterminism(const char *what);

/* Draw the next 64-bit value from stream `s` (0..XTC_SIM_RNG_NSTREAMS-1).
 * Deterministic given the activation seed.  Undefined if sim inactive
 * (callers must gate on __xtc_sim_active()). */
XTC_API uint64_t __xtc_sim_rng(int s);

/* Convenience: a uniform value in [0, bound) from stream `s`.  bound==0
 * returns 0. */
XTC_API uint64_t __xtc_sim_rng_range(int s, uint64_t bound);

/* The I/O fault models' draws (xtc_sim_io_faults_enable and friends in
 * xtc_sim.h arm them; io_sim.c consumes them). */
XTC_API int      __xtc_sim_io_faults_active(void);
XTC_API int64_t  __xtc_sim_io_latency(void);
XTC_API int      __xtc_sim_io_should_fault(void);
XTC_API int      __xtc_sim_io_corrupt_active(void);
XTC_API int      __xtc_sim_io_torn_prefix(int full_len);
XTC_API int      __xtc_sim_io_flip_byte(int len);
XTC_API int      __xtc_sim_io_enospc(void);
XTC_API void     __xtc_sim_io_stale_record(int fd, uint64_t off,
                     const void *buf, int len);
XTC_API int      __xtc_sim_io_stale_read(int fd, uint64_t off, void *buf,
                     int len);
XTC_API void     __xtc_sim_io_wb_wrote(int fd, uint64_t end_off);
XTC_API void     __xtc_sim_io_wb_synced(int fd);

/* The network model's draws (proc.c's cross-loop delivery). */
XTC_API int      __xtc_sim_partition_blocked(int src_loop_id,
                     int dst_loop_id);
XTC_API int64_t  __xtc_sim_net_latency(void);

/* Query the virtual clock: returns 1 and writes *out_ns when active,
 * 0 otherwise.  The single seam __os_clock_mono consults. */
XTC_API int      __xtc_sim_vclock(int64_t *out_ns);

/* The adversarial-scheduler and swizzle settings (exec.c, io_sim.c). */
XTC_API int      __xtc_sim_sched_pessimal_pct(void);
XTC_API int      __xtc_sim_swizzle_pct(void);

/* Run the consistency check xtc_sim_set_consistency_check installed
 * (xtc_sim_exec_run, at quiescence). */
XTC_API int      __xtc_sim_run_consistency_check(void);

/* The readiness xtc_sim_fd_ready_at scheduled for fd: returns 1 and its
 * due time and revents, or 0 when none is scheduled (io_sim.c). */
XTC_API int      __xtc_sim_fd_ready(int fd, int64_t *out_due_ns,
                     uint32_t *out_revents);

/* XTC_OK during a run, XTC_E_STATE (said once on stderr) outside one on
 * a sim build: the entry guard of xtc_loop_run and xtc_exec_run, which
 * the sim backend cannot drive on real fds (sim.c). */
XTC_API int      __xtc_sim_require_run(const char *what);

#endif /* XTC_SIM_INT_H */
