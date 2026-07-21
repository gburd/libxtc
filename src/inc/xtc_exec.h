/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_exec.h
 *	The L2 multi-loop executor.  Owns N xtc_loop instances, each
 *	running on its own OS thread.  Tasks may be spawned on any loop
 *	from any thread; cross-thread wakers go through a per-loop MPSC
 *	inbox + xtc_io_wakeup pingback.
 *
 *	See M5_CLAIMS.md.
 */

#ifndef XTC_EXEC_H
#define XTC_EXEC_H

#include "xtc_export.h"

#include <stdint.h>

#include "xtc_loop.h"
#include "xtc_async.h"

typedef struct xtc_exec xtc_exec_t;

/*
 * PUBLIC: int  xtc_exec_init __P((xtc_exec_t **, int));
 * PUBLIC: int  xtc_exec_fini __P((xtc_exec_t *));
 * PUBLIC: int  xtc_exec_run __P((xtc_exec_t *));
 * PUBLIC: void xtc_exec_set_service_mode __P((xtc_exec_t *, int));
 * PUBLIC: void xtc_exec_set_eager_rebalance __P((xtc_exec_t *, int));
 * PUBLIC: int  xtc_exec_stop __P((xtc_exec_t *));
 * PUBLIC: int  xtc_exec_n_loops __P((xtc_exec_t *));
 * PUBLIC: int  xtc_exec_loop_id __P((void));
 * PUBLIC: int  xtc_shard_id __P((void));
 * PUBLIC: int  xtc_shard_count __P((void));
 * PUBLIC: xtc_loop_t *xtc_exec_loop __P((xtc_exec_t *, int));
 *
 * PUBLIC: int  xtc_exec_spawn __P((xtc_exec_t *, xtc_task_fn, void *, xtc_task_t **));
 * PUBLIC: int  xtc_exec_spawn_on __P((xtc_exec_t *, int, xtc_task_fn, void *, xtc_task_t **));
 * PUBLIC: int  xtc_exec_async __P((xtc_exec_t *, xtc_coro_fn, void *, xtc_task_t **));
 * PUBLIC: int  xtc_exec_async_on __P((xtc_exec_t *, int, xtc_coro_fn, void *, xtc_task_t **));
 */

/*
 * Lifecycle.  n_loops <= 0 selects __os_ncpus().
 *
 * `xtc_exec_run` blocks the calling thread until the executor stops:
 *   - all spawned tasks DONE and all timers fired/cancelled, or
 *   - xtc_exec_stop() called from any thread.
 *
 * On return all worker threads have been joined.
 */
XTC_API int  xtc_exec_init(xtc_exec_t **out, int n_loops);
XTC_API int  xtc_exec_fini(xtc_exec_t *exec);
XTC_API int  xtc_exec_run(xtc_exec_t *exec);

/* Service mode: when set, xtc_exec_run does not idle-auto-stop and runs
 * until xtc_exec_stop is called.  Used by a supervised xtc_app, which is
 * a long-running service rather than a finite work pool. */
XTC_API void xtc_exec_set_service_mode(xtc_exec_t *exec, int on);

/*
 * Eager work-stealing rebalance (OFF by default).
 *
 * By default a loop steals a migratable (xtc_proc_opts_t.migratable)
 * peer proc only when it is FULLY idle -- its own run queue empty AND
 * no parked fibers or timers -- and it discovers a sibling's stealable
 * work only on its next poll edge.  Under a load where every loop owns
 * parked fibers (e.g. many backends parked on client sockets while a
 * peer loop has a runnable query), no loop is ever "fully idle", so
 * migratable work sits on the stealable deque and is never taken.
 *
 * When eager rebalance is ON, two things change:
 *   - a loop whose RUN QUEUE is empty (even if it owns parked fibers or
 *     timers) attempts a steal before it blocks in the poller, so it
 *     can run a sibling's runnable proc instead of idling on its own
 *     fds; and
 *   - enqueuing a migratable task nudges one idle peer loop (via the
 *     poller wakeup) so it re-checks and steals promptly rather than
 *     waiting for a poll edge.
 *
 * This trades some cross-loop migration (and the cache/NUMA cost that
 * comes with it) for reclaiming idle capacity under partial load.  A
 * consumer that runs supervised, migratable procs and wants them
 * rebalanced under load (e.g. a threaded server whose backends park on
 * sockets) opts in; a latency-sensitive, cache-locality-bound workload
 * leaves it off.  Only migratable tasks are ever moved; pinned work
 * (the default) is unaffected either way.
 */
XTC_API void xtc_exec_set_eager_rebalance(xtc_exec_t *exec, int on);

int  xtc_exec_set_preempt(xtc_exec_t *exec, int64_t interval_ns);
XTC_API int  xtc_exec_stop(xtc_exec_t *exec);

XTC_API int  xtc_exec_n_loops(xtc_exec_t *exec);

/*
 * From inside a task running on a loop, returns that loop's
 * 0-based index.  From any other thread returns -1.  Tests use this
 * to verify cross-loop spawn placement and steals.
 */
XTC_API int  xtc_exec_loop_id(void);

/* Seastar-style per-shard API.  xtc_shard_id() is the 0-based index of
 * the loop the caller runs on (a standalone loop is shard 0 of 1; -1
 * off a loop); xtc_shard_count() is the number of shards (1 for a
 * standalone loop, 0 off a loop).  Index per-core state with these
 * for a shared-nothing design. */
XTC_API int  xtc_shard_id(void);
XTC_API int  xtc_shard_count(void);

/* Borrow a loop pointer (for tests; not generally needed). */
XTC_API xtc_loop_t *xtc_exec_loop(xtc_exec_t *exec, int idx);

/*
 * Per-loop work statistics, for observability and load-balance
 * diagnosis (e.g. confirming work stealing is distributing under a
 * tail-latency-sensitive workload).
 *
 *   tasks_run -- task steps executed on this loop
 *   steals    -- tasks this loop successfully stole from a peer
 *
 * Reads are lock-free snapshots of relaxed atomics; exactness across
 * a concurrently running executor is not guaranteed.
 */
typedef struct xtc_loop_stats {
	uint64_t tasks_run;
	uint64_t steals;
} xtc_loop_stats_t;

/*
 * PUBLIC: int  xtc_exec_loop_stats __P((xtc_exec_t *, int, xtc_loop_stats_t *));
 */
XTC_API int  xtc_exec_loop_stats(xtc_exec_t *exec, int idx, xtc_loop_stats_t *out);

/*
 * Spawns.  These are the multi-loop equivalents of xtc_task_spawn /
 * xtc_async; they may be called from any thread.
 */
XTC_API int  xtc_exec_spawn(xtc_exec_t *exec, xtc_task_fn fn, void *user,
                            xtc_task_t **out_task);
XTC_API int  xtc_exec_spawn_on(xtc_exec_t *exec, int loop_idx,
                               xtc_task_fn fn, void *user, xtc_task_t **out_task);
XTC_API int  xtc_exec_async(xtc_exec_t *exec, xtc_coro_fn fn, void *arg,
                            xtc_task_t **out_task);
XTC_API int  xtc_exec_async_on(xtc_exec_t *exec, int loop_idx,
                               xtc_coro_fn fn, void *arg, xtc_task_t **out_task);

#endif /* XTC_EXEC_H */
