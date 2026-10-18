/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/coro_common.h
 *	Shared, substrate-agnostic memory-management helpers for the
 *	mmap-based coroutine substrates (coro_fctx.c, coro_uctx.c).
 *	Exactly one substrate is compiled per build (the others are empty
 *	TUs), so the `static` definitions here are instantiated once, into
 *	whichever substrate includes this header.  This consolidates code
 *	that was previously duplicated byte-for-byte between fctx and uctx:
 *	  - the per-thread fiber-stack pool (recycle freed stacks so a
 *	    spawn skips mmap+mprotect),
 *	  - the S1 stack-reclaim lever (madvise the unused stack tail on
 *	    park).
 *
 *	The winfiber substrate owns its stacks via CreateFiberEx (no mmap,
 *	no madvise), so it does NOT include this header.  The sanitizer
 *	fiber-switch annotations remain per-substrate (their switch call
 *	sites are substrate-specific), so they are deliberately NOT here.
 *
 *	Include AFTER coro_int.h (for struct xtc_coro) and the substrate's
 *	own headers (needs sysconf, madvise, atomics, XTC_THREAD_LOCAL).
 */

#ifndef XTC_CORO_COMMON_H
#define XTC_CORO_COMMON_H

/* Self-contained: __stack_pool_drain calls munmap and the reclaim lever
 * madvise.  The includer usually has these already, but the amalgamation
 * emits this header before coro_uctx.c's own <sys/mman.h>, and macOS
 * rejects the implicit declaration (and then the late asm-labelled one). */
#include <sys/mman.h>
#include <unistd.h>

#include "preempt_int.h"   /* __xtc_unsafe_enter/leave around the spare lock */

/* ---- Fiber-stack pool (per-thread) --------------------------------
 *
 * Every fresh fiber stack costs an mmap + an mprotect(PROT_NONE) for its
 * guard page, and mprotect write-locks the process-wide address-space
 * lock (mmap_lock) -- so many threads spawning fibers concurrently
 * serialize in the kernel, capping spawn throughput far below the core
 * count (192-core finding: spawn flat ~7 M/s while the scheduler
 * scales to 249 M/s).  A per-thread free list recycles freed stacks --
 * the guard page is already installed, so a reused stack skips BOTH the
 * mmap and the mprotect, and the list is thread-local so the hot path
 * takes no lock.  Bounded so idle threads do not hoard memory.
 *
 * Stacks are pooled only at the current __xtc_stack_size; a stack freed
 * at a different size (after xtc_set_stack_size) is munmap'd, not
 * pooled, so the pool never hands back a wrong-sized stack. */
#define XTC_STACK_POOL_MAX 64
struct stack_pool {
	void   *slots[XTC_STACK_POOL_MAX];
	size_t  size;                 /* stack_sz these were allocated at */
	int     n;
	int     atexit_armed;         /* thread-exit drain registered */
};
static XTC_THREAD_LOCAL struct stack_pool __stack_pool;

/* Thread exit: unmap the stacks still pooled.  Finished fibers now feed
 * the pool while a loop runs (not only at loop_fini), so an executor
 * worker would otherwise strand up to XTC_STACK_POOL_MAX stacks -- two
 * mappings each -- every time it exits. */
static void
__stack_pool_drain(void *arg)
{
	struct stack_pool *p = &__stack_pool;
	long pg = sysconf(_SC_PAGESIZE);
	size_t guard = pg > 0 ? (size_t)pg : 4096u;
	(void)arg;
	while (p->n > 0)
		(void)munmap(p->slots[--p->n], p->size + guard);
	p->atexit_armed = 0;
}

/* Pop a cached stack of `stack_sz` (guard already installed), or NULL. */
static void *
__stack_pool_get(size_t stack_sz)
{
	struct stack_pool *p = &__stack_pool;
	if (p->n > 0 && p->size == stack_sz)
		return p->slots[--p->n];
	return NULL;
}

/* Return a stack to the pool if it fits (same size, room left); else the
 * caller munmaps.  Returns 1 if pooled, 0 if the caller must free it. */
static int
__stack_pool_put(void *base, size_t stack_sz)
{
	struct stack_pool *p = &__stack_pool;
	if (!p->atexit_armed) {
		if (__os_thread_atexit(__stack_pool_drain, NULL) != XTC_OK)
			return 0;     /* cannot drain at exit: do not pool */
		p->atexit_armed = 1;
	}
	if (p->n == 0) p->size = stack_sz;   /* first entry sets the pool size */
	if (p->size != stack_sz || p->n == XTC_STACK_POOL_MAX)
		return 0;
	p->slots[p->n++] = base;
	return 1;
}

/* ---- Fiber-stack spare (per loop) ---------------------------------
 *
 * The pool above recycles a stack only on the thread that freed it.  A
 * proc spawned onto ANOTHER loop takes its stack on the spawner's thread
 * and frees it on the target's (since 1.56 at DONE, not at loop_fini), so
 * in the round-robin acceptor shape the spawner's pool is always empty
 * (mmap + mprotect per spawn) and the target's always full (munmap per
 * exit, which with every loop thread running is a TLB-shootdown IPI to
 * each of them).  So a stack the finishing thread cannot pool goes to a
 * small locked spare on its task's HOME loop, and a spawn onto that loop
 * whose thread pool is empty takes from it.  Bounded per loop
 * (XTC_LOOP_STACK_SPARE), filled only on a loop that other threads spawn
 * onto, and unmapped at xtc_loop_fini, so retention stays bounded by the
 * live count plus the pools, never by history.
 * The spinlock is held for a few loads and stores; never across a yield,
 * an allocation, a syscall or another lock; preemption deferred. */
static void
__stack_spare_lock(xtc_loop_t *l)
{
	__xtc_unsafe_enter();
	while (atomic_exchange_explicit(&l->stack_spare_lock, 1,
	    memory_order_acquire))
		while (atomic_load_explicit(&l->stack_spare_lock,
		    memory_order_relaxed))
			__os_pause();
}

static void
__stack_spare_unlock(xtc_loop_t *l)
{
	atomic_store_explicit(&l->stack_spare_lock, 0, memory_order_release);
	__xtc_unsafe_leave();
}

/* Pop a spare stack of `stack_sz` from loop l, or NULL.  Any thread. */
static void *
__stack_spare_get(xtc_loop_t *l, size_t stack_sz)
{
	void *base = NULL;
	int n;
	if (__xtc_current_loop != l &&
	    atomic_load_explicit(&l->stack_spare_want, memory_order_relaxed) <
	    XTC_LOOP_STACK_SPARE)
		atomic_fetch_add_explicit(&l->stack_spare_want, 1,
		    memory_order_relaxed);
	if (atomic_load_explicit(&l->stack_spare_n, memory_order_relaxed) == 0)
		return NULL;
	__stack_spare_lock(l);
	n = atomic_load_explicit(&l->stack_spare_n, memory_order_relaxed);
	if (n > 0 && l->stack_spare_sz == stack_sz) {
		base = l->stack_spare[--n];
		atomic_store_explicit(&l->stack_spare_n, n,
		    memory_order_relaxed);
	}
	__stack_spare_unlock(l);
	return base;
}

/* Give a stack to loop l's spare.  1 if taken, 0 if the caller must
 * munmap it (spare full or a different size).  Any thread. */
static int
__stack_spare_put(xtc_loop_t *l, void *base, size_t stack_sz)
{
	int n, ok = 0;
	if (atomic_load_explicit(&l->stack_spare_want, memory_order_relaxed) <
	    XTC_LOOP_STACK_SPARE ||
	    atomic_load_explicit(&l->stack_spare_n, memory_order_relaxed) >=
	    XTC_LOOP_STACK_SPARE)
		return 0;
	__stack_spare_lock(l);
	n = atomic_load_explicit(&l->stack_spare_n, memory_order_relaxed);
	if (n == 0)
		l->stack_spare_sz = stack_sz;  /* first entry sets the size */
	if (l->stack_spare_sz == stack_sz && n < XTC_LOOP_STACK_SPARE) {
		l->stack_spare[n++] = base;
		atomic_store_explicit(&l->stack_spare_n, n,
		    memory_order_relaxed);
		ok = 1;
	}
	__stack_spare_unlock(l);
	return ok;
}

/* xtc_loop_fini, after every task's cleanup has run (single-threaded). */
void
__xtc_coro_stack_spare_fini(xtc_loop_t *l)
{
	long pg = sysconf(_SC_PAGESIZE);
	size_t guard = pg > 0 ? (size_t)pg : 4096u;
	int n = atomic_load_explicit(&l->stack_spare_n, memory_order_relaxed);
	while (n > 0)
		(void)munmap(l->stack_spare[--n], l->stack_spare_sz + guard);
	atomic_store_explicit(&l->stack_spare_n, 0, memory_order_relaxed);
}

/* ---- Lever S1: stack-memory reclamation on park -------------------
 *
 * OFF by default.  When enabled, a parking fiber returns the unused
 * tail of its stack (everything below its current SP, beyond a live
 * margin, page-aligned, above the guard) to the OS with
 * madvise(MADV_DONTNEED); it faults back zero-filled on resume.  See
 * xtc_async.h. */
#if defined(MADV_DONTNEED)
static _Atomic int      g_reclaim_on = 0;
static _Atomic size_t   g_reclaim_keep = 0;      /* live margin below SP */
static _Atomic uint64_t g_reclaim_count = 0;     /* madvise calls made */

int
xtc_stack_reclaim_enable(size_t keep_bytes)
{
	long pg = sysconf(_SC_PAGESIZE);
	size_t page = (pg > 0) ? (size_t)pg : 4096u;
	if (keep_bytes == 0)
		keep_bytes = page;      /* default live margin: one page */
	atomic_store(&g_reclaim_keep, keep_bytes);
	atomic_store(&g_reclaim_on, 1);
	return XTC_OK;
}

void
xtc_stack_reclaim_disable(void)
{
	atomic_store(&g_reclaim_on, 0);
}

int
xtc_stack_reclaim_enabled(void)
{
	return atomic_load(&g_reclaim_on);
}

uint64_t
xtc_stack_reclaim_count(void)
{
	return atomic_load(&g_reclaim_count);
}

/*
 * Reclaim the unused tail of the current fiber's stack.  Called from a
 * PARK path with `sp` = the fiber's current stack pointer (a stack
 * address captured by the caller just before it jumps to the
 * scheduler).  Stacks grow DOWN, so the unused region is
 * [stack_base + guard, sp - keep), page-aligned inward.  No-op unless
 * the reclaimable span exceeds one page (avoids fault churn on shallow
 * fibers).
 */
static void
coro_stack_shrink(struct xtc_coro *c, void *sp)
{
	long pgl;
	size_t page, keep;
	uintptr_t lo, hi, guard, base;

	if (c == NULL || c->stack == NULL || !atomic_load(&g_reclaim_on))
		return;
	pgl = sysconf(_SC_PAGESIZE);
	page = (pgl > 0) ? (size_t)pgl : 4096u;
	guard = page;                    /* one guard page at the base */
	keep = atomic_load(&g_reclaim_keep);

	base = (uintptr_t)c->stack + guard;      /* first usable byte */
	hi = (uintptr_t)sp;
	if (hi <= base + keep)
		return;                  /* SP too close to the base */
	/* Reclaimable tail: [base, hi - keep), rounded to whole pages so we
	 * never touch a partially-live page. */
	hi = (hi - keep) & ~(uintptr_t)(page - 1);   /* round DOWN */
	lo = (base + page - 1) & ~(uintptr_t)(page - 1); /* round UP */
	if (hi <= lo || hi - lo < page)
		return;                  /* nothing worth a syscall */
	if (madvise((void *)lo, (size_t)(hi - lo), MADV_DONTNEED) == 0)
		(void)atomic_fetch_add(&g_reclaim_count, 1);
}
#else  /* no MADV_DONTNEED: reclaim is a no-op */
int      xtc_stack_reclaim_enable(size_t k) { (void)k; return XTC_E_NOSYS; }
void     xtc_stack_reclaim_disable(void) { }
int      xtc_stack_reclaim_enabled(void) { return 0; }
uint64_t xtc_stack_reclaim_count(void) { return 0; }
#define coro_stack_shrink(c, sp)  ((void)(c), (void)(sp))
#endif

#endif  /* XTC_CORO_COMMON_H */
