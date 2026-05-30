/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_blocking.h
 *	Offload blocking work to a thread pool, parking the calling
 *	process instead of blocking the loop.
 *
 *	A loop thread must never block in a syscall: doing so stalls
 *	every other process sharing that loop.  But some work is
 *	unavoidably blocking -- file reads and fsync (regular files are
 *	not pollable), getaddrinfo, third-party libraries.  xtc_blocking
 *	runs such a call on a dedicated pool thread and parks the
 *	calling process until it finishes, so the loop keeps running
 *	other work meanwhile.  This is the same division of labour the
 *	BEAM (dirty schedulers) and Tokio / libuv (blocking pool) use.
 *
 *	The wakeup reuses the runtime's existing machinery: the pool
 *	thread signals completion on a pipe the calling process waits on
 *	with xtc_proc_wait_fd, so no new scheduler integration is
 *	needed.
 */

#ifndef XTC_BLOCKING_H
#define XTC_BLOCKING_H

#include "xtc.h"

/*
 * Run fn(arg) on a blocking-pool thread and park the calling process
 * until it completes; fn's return value is stored in *out_result.
 *
 * Must be called from within a process / coroutine running on a loop.
 * Called outside that context (or where the offload cannot be set up)
 * it runs fn synchronously on the current thread -- always correct,
 * just not yielding.  Returns XTC_OK once fn has run.
 *
 * PUBLIC: int  xtc_blocking_run __P((int (*)(void *), void *, int *));
 */
int xtc_blocking_run(int (*fn)(void *), void *arg, int *out_result);

/*
 * Set the pool size (worker threads).  Must be called before the
 * first xtc_blocking_run; later calls are ignored.  Default 4.
 *
 * PUBLIC: int  xtc_blocking_pool_size __P((int));
 */
int xtc_blocking_pool_size(int nthreads);

/*
 * Stop the pool, joining its threads.  Idempotent; for orderly
 * shutdown and leak-checked test runs.  A new xtc_blocking_run after
 * shutdown restarts the pool.
 *
 * PUBLIC: void xtc_blocking_shutdown __P((void));
 */
void xtc_blocking_shutdown(void);

#endif /* XTC_BLOCKING_H */
