/*-
 * Copyright (c) 2026, The XTC Project
 * @stability stable
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_runtime.h
 *	Process-level runtime introspection -- one call that reports
 *	what this process was given: how many event loops it runs, the
 *	CPU topology it sees, and any configured memory budget.  Where
 *	xtc_inspect.h (xtc_inspect_loops / xtc_inspect_procs) gives the
 *	live per-loop / per-proc detail, this is the single aggregate a
 *	libxtc application reads once at startup to size itself to the
 *	box: buffer-pool bytes, worker counts, morsel granularity.
 *
 *	The aim is that an application never has to reach into the
 *	internal __os_* topology surface (os_cpu.h) or stitch together
 *	xtc_exec_n_loops + xtc_app_exec by hand; xtc_runtime_info()
 *	collects it all into one struct.
 */

#ifndef XTC_RUNTIME_H
#define XTC_RUNTIME_H

#include "xtc_export.h"

#include <stdint.h>

#include "xtc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A one-shot snapshot of the process runtime environment.
 *
 * The CPU and NUMA fields are queried from the OS at call time
 * (they do not change over the life of the process on any supported
 * platform).  n_loops reflects the executor the CALLER is running on
 * (see below).  The memory fields report a configured cap and the
 * bytes currently charged against it, if such accounting is in force.
 */
typedef struct xtc_runtime_info {
	size_t   size;             /* sizeof(xtc_runtime_info_t): XTC_RUNTIME_INFO_INIT */
	int      n_loops;          /* event loops in the running app/exec, or 1 standalone */
	int      n_cpus_online;    /* __os_ncpus() */
	int      n_cpus_perf;      /* __os_ncpus_perf() -- performance cores */
	int      n_cpus_effic;     /* __os_ncpus_effic() -- efficiency cores */
	int      numa_nodes;       /* __os_numa_nnodes() */
	int64_t  mem_cap_bytes;    /* configured memory cap, or 0 if none/unknown */
	int64_t  mem_used_bytes;   /* currently accounted memory in use, or 0 */
} xtc_runtime_info_t;

#define XTC_RUNTIME_INFO_INIT { .size = sizeof(xtc_runtime_info_t) }

/*
 * PUBLIC: int xtc_runtime_info __P((xtc_runtime_info_t *));
 */

/*
 * Fill *out with a snapshot of the runtime environment.  Returns
 * XTC_OK on success, or XTC_E_INVAL if `out` is NULL.
 *
 * Field semantics:
 *
 *   n_loops
 *	The number of event loops in the executor the CALLER runs on.
 *	When called from a fiber/task on a loop owned by a multi-loop
 *	executor (xtc_exec / a multi-loop xtc_app), this is that
 *	executor's loop count.  On a standalone loop it is 1.  When
 *	called from a thread that is NOT on any loop (e.g. before the
 *	executor starts, or from a plain helper thread), there is no
 *	thread-local "current executor" to consult, so it defaults to
 *	1.  LIMITATION: libxtc keeps no process-global registry of the
 *	current app/exec, only a thread-local current-loop; to read the
 *	true loop count from off-loop, hold the exec handle and call
 *	xtc_exec_n_loops(xtc_app_exec(app)) directly.
 *
 *   n_cpus_online / n_cpus_perf / n_cpus_effic / numa_nodes
 *	The OS-reported CPU topology (online logical CPUs, performance
 *	cores, efficiency cores, NUMA nodes).  On platforms or
 *	hardware without a perf/effic split, all online CPUs count as
 *	performance cores and n_cpus_effic is 0.  numa_nodes is at
 *	least 1.
 *
 *   mem_cap_bytes / mem_used_bytes
 *	A configured memory budget and the bytes charged against it.
 *	LIMITATION: libxtc's memory accounting (xtc_res, XTC_RES_MEM_BYTES)
 *	is an opt-in, caller-owned facility -- there is no process-global
 *	or default resource accountant reachable from here, and these
 *	fields are NOT the OS-reported RSS.  Both are therefore reported
 *	as 0 ("no cap / unknown"); an application that wants its own
 *	quota reflected should read its xtc_res_t directly with
 *	xtc_res_used(r, XTC_RES_MEM_BYTES) against r->caps.mem_bytes.
 */
XTC_API int xtc_runtime_info(xtc_runtime_info_t *out);

/*
 * Process-global knobs (2.2).  Each set_ has a get_ that reads back the
 * value in force.  "Before first use" knobs are read when the thing they
 * size is created; set them before that.
 *
 * stack_size        default fiber stack bytes (16 KiB minimum, rounded up
 *                   to a page); any time, for fibers created afterward.
 *                   XTC_E_INVAL below 16 KiB.  Default 64 KiB.
 * stack_reclaim     keep_bytes > 0 turns on madvise-reclaim of a parked
 *                   fiber's unused stack tail, leaving keep_bytes mapped
 *                   below its SP; 0 turns it off.  Any time.
 *                   XTC_E_NOSYS where the platform cannot reclaim (it
 *                   stays off).  get_ returns 0 when off.  Default off.
 * blocking_threads  pin the blocking-offload pool (xtc_blocking.h) at n
 *                   threads, 1..64; before the pool's first use.
 *                   XTC_E_STATE once it has started, XTC_E_INVAL out of
 *                   range.  get_ returns 0 when the pool sizes itself
 *                   (the default).
 * preempt_involuntary  on != 0 enables signal-context preemption
 *                   (xtc_preempt_involuntary.h); any time.  XTC_E_NOSYS
 *                   where unsupported, and it stays off.  Default off.
 * iowq_workers      io_uring io-wq worker caps per ring (bounded,
 *                   unbounded; 0 = the kernel default); before the rings
 *                   are created.  Other backends accept and ignore it.
 *                   Default (4, 0).
 * alloc_audit       on != 0 wraps the allocator hook to track per-proc
 *                   allocations (xtc_alloc_audit.h); any time.
 *                   XTC_E_NOMEM if the table cannot be allocated.
 *
 * PUBLIC: int    xtc_runtime_set_stack_size __P((size_t));
 * PUBLIC: size_t xtc_runtime_get_stack_size __P((void));
 * PUBLIC: int    xtc_runtime_set_stack_reclaim __P((size_t));
 * PUBLIC: size_t xtc_runtime_get_stack_reclaim __P((void));
 * PUBLIC: int    xtc_runtime_set_blocking_threads __P((int));
 * PUBLIC: int    xtc_runtime_get_blocking_threads __P((void));
 * PUBLIC: int    xtc_runtime_set_preempt_involuntary __P((int));
 * PUBLIC: int    xtc_runtime_get_preempt_involuntary __P((void));
 * PUBLIC: void   xtc_runtime_set_iowq_workers __P((unsigned, unsigned));
 * PUBLIC: void   xtc_runtime_get_iowq_workers __P((unsigned *, unsigned *));
 * PUBLIC: int    xtc_runtime_set_alloc_audit __P((int));
 * PUBLIC: int    xtc_runtime_get_alloc_audit __P((void));
 */
XTC_API int    xtc_runtime_set_stack_size(size_t bytes);
XTC_API size_t xtc_runtime_get_stack_size(void);
XTC_API int    xtc_runtime_set_stack_reclaim(size_t keep_bytes);
XTC_API size_t xtc_runtime_get_stack_reclaim(void);
XTC_API int    xtc_runtime_set_blocking_threads(int nthreads);
XTC_API int    xtc_runtime_get_blocking_threads(void);
XTC_API int    xtc_runtime_set_preempt_involuntary(int on);
XTC_API int    xtc_runtime_get_preempt_involuntary(void);
XTC_API void   xtc_runtime_set_iowq_workers(unsigned bound, unsigned unbound);
XTC_API void   xtc_runtime_get_iowq_workers(unsigned *out_bound,
                                            unsigned *out_unbound);
XTC_API int    xtc_runtime_set_alloc_audit(int on);
XTC_API int    xtc_runtime_get_alloc_audit(void);

#ifdef __cplusplus
}
#endif

#endif /* XTC_RUNTIME_H */
