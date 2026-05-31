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

#include <stdint.h>

#include "xtc_loop.h"
#include "xtc_async.h"

typedef struct xtc_exec xtc_exec_t;

/*
 * PUBLIC: int  xtc_exec_init __P((xtc_exec_t **, int));
 * PUBLIC: int  xtc_exec_fini __P((xtc_exec_t *));
 * PUBLIC: int  xtc_exec_run __P((xtc_exec_t *));
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
int  xtc_exec_init(xtc_exec_t **out, int n_loops);
int  xtc_exec_fini(xtc_exec_t *exec);
int  xtc_exec_run(xtc_exec_t *exec);
int  xtc_exec_stop(xtc_exec_t *exec);

int  xtc_exec_n_loops(xtc_exec_t *exec);

/*
 * From inside a task running on a loop, returns that loop's
 * 0-based index.  From any other thread returns -1.  Tests use this
 * to verify cross-loop spawn placement and steals.
 */
int  xtc_exec_loop_id(void);

/* Seastar-style per-shard API.  xtc_shard_id() is the 0-based index of
 * the loop the caller runs on (a standalone loop is shard 0 of 1; -1
 * off a loop); xtc_shard_count() is the number of shards (1 for a
 * standalone loop, 0 off a loop).  Index per-core state with these
 * for a shared-nothing design. */
int  xtc_shard_id(void);
int  xtc_shard_count(void);

/* Borrow a loop pointer (for tests; not generally needed). */
xtc_loop_t *xtc_exec_loop(xtc_exec_t *exec, int idx);

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
int  xtc_exec_loop_stats(xtc_exec_t *exec, int idx, xtc_loop_stats_t *out);

/*
 * Spawns.  These are the multi-loop equivalents of xtc_task_spawn /
 * xtc_async; they may be called from any thread.
 */
int  xtc_exec_spawn(xtc_exec_t *exec, xtc_task_fn fn, void *user,
                    xtc_task_t **out_task);
int  xtc_exec_spawn_on(xtc_exec_t *exec, int loop_idx,
                       xtc_task_fn fn, void *user, xtc_task_t **out_task);
int  xtc_exec_async(xtc_exec_t *exec, xtc_coro_fn fn, void *arg,
                    xtc_task_t **out_task);
int  xtc_exec_async_on(xtc_exec_t *exec, int loop_idx,
                       xtc_coro_fn fn, void *arg, xtc_task_t **out_task);

#endif /* XTC_EXEC_H */
