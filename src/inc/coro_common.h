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

/* ---- Fiber-stack pool (per-thread) --------------------------------
 *
 * Every fresh fiber stack costs an mmap + an mprotect(PROT_NONE) for its
 * guard page, and mprotect write-locks the process-wide address-space
 * lock (mmap_lock) -- so many threads spawning fibers concurrently
 * serialize in the kernel, capping spawn throughput far below the core
 * count (EC2 192-core finding: spawn flat ~7 M/s while the scheduler
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
};
static XTC_THREAD_LOCAL struct stack_pool __stack_pool;

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
	if (p->n == 0) p->size = stack_sz;   /* first entry sets the pool size */
	if (p->size != stack_sz || p->n == XTC_STACK_POOL_MAX)
		return 0;
	p->slots[p->n++] = base;
	return 1;
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
