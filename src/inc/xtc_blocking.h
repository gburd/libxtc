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
 *	other work meanwhile.
 *
 *	The wakeup reuses the runtime's existing machinery: the pool
 *	thread signals completion on a pipe the calling process waits on
 *	with xtc_proc_wait_fd, so no new scheduler integration is
 *	needed.
 */

#ifndef XTC_BLOCKING_H
#define XTC_BLOCKING_H

#include "xtc_export.h"

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
XTC_API int xtc_blocking_run(int (*fn)(void *), void *arg, int *out_result);

/*
 * Off-loop variant of xtc_blocking_run for a plain OS thread that is NOT
 * a libxtc fiber (xtc_self() == none): offload fn(arg) to the pool and
 * block the CALLING thread on the completion pipe with a real read(2),
 * instead of parking a fiber (which a bare thread cannot do).  fn runs
 * on a pool worker, not inline on the caller.
 *
 * It is still synchronous from the caller's view (it blocks the calling
 * thread until fn completes) and does NOT shorten any lock held across
 * the call -- it moves the syscall off the caller thread, it does not
 * let a non-fiber caller do other work meanwhile.  To keep serving
 * other multiplexed tasks during a blocking call, make those tasks
 * fibers on a loop and use xtc_blocking_run (which yields).  Returns
 * XTC_E_INVAL if fn is NULL or if called from a fiber/loop process
 * (use xtc_blocking_run there); on a pool/pipe setup failure it runs fn
 * inline and returns XTC_OK.
 *
 * PUBLIC: int  xtc_blocking_run_off_loop __P((int (*)(void *), void *, int *));
 */
XTC_API int xtc_blocking_run_off_loop(int (*fn)(void *), void *arg,
    int *out_result);

/*
 * Fire-and-forget variant: hand fn(arg) to the offload pool and return
 * immediately, without waiting for or collecting the result.  Never
 * parks, so it is callable from any context (e.g. prefetch/read-ahead).
 * The caller owns arg's lifetime until fn runs (or has fn free it);
 * there is no completion signal.
 *
 * PUBLIC: int  xtc_blocking_submit __P((int (*)(void *), void *));
 */
XTC_API int xtc_blocking_submit(int (*fn)(void *), void *arg);

/*
 * Pin the pool to a fixed size (worker threads), overriding the
 * automatic default.  Must be called before the first xtc_blocking_run
 * / xtc_blocking_submit; later calls return XTC_E_INVAL (too late).
 *
 * By DEFAULT the pool auto-sizes: it starts with a CPU-scaled number of
 * workers (max(4, online CPUs), capped at 64) and grows on demand up to
 * 64 when work queues up faster than idle workers can take it, so the
 * offload path is not an artificial bottleneck on a large host nor
 * over-provisioned on a small one.  Setting an explicit size disables
 * the growth and fixes the pool at exactly that many threads.
 *
 * PUBLIC: int  xtc_blocking_pool_size __P((int));
 */
XTC_API int xtc_blocking_pool_size(int nthreads);

/*
 * Stop the pool, joining its threads.  Idempotent; for orderly
 * shutdown and leak-checked test runs.  A new xtc_blocking_run after
 * shutdown restarts the pool.
 *
 * PUBLIC: void xtc_blocking_shutdown __P((void));
 */
XTC_API void xtc_blocking_shutdown(void);

#endif /* XTC_BLOCKING_H */
