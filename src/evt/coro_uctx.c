/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/evt/coro_uctx.c
 *	Stackful coroutine substrate via POSIX ucontext.h.
 *
 *	A coroutine is wrapped in an xtc_task_t.  The task's fn is the
 *	tiny step function below; on each invocation the loop swaps
 *	into the fiber's saved context.  The fiber returns to the loop
 *	either by completing (fn returned), yielding via xtc_yield(),
 *	or awaiting another coroutine.
 *
 *	M4 ships ucontext as the default substrate.  M4.5 will add
 *	per-architecture make_fcontext/jump_fcontext asm for ~30 ns
 *	switches; the surface and contract here are unchanged.
 */

/*
 * REG_RIP / REG_RSP / greg_t (used by __xtc_coro_preempt to rewrite the
 * signal mcontext for Phase 2b involuntary preemption) live in
 * <sys/ucontext.h> behind __USE_GNU on glibc; _DEFAULT_SOURCE alone
 * does not expose them.  Request _GNU_SOURCE for this TU before any
 * header pulls ucontext.h in.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "xtc_int.h"

/* Windows uses a separate fiber implementation in src/evt/coro_winfiber.c.
 * This file is the POSIX (ucontext-based) substrate.
 *
 * On musl libc, swapcontext / getcontext / makecontext are deliberately
 * omitted (the musl maintainers consider the ucontext API obsolete).
 * On such systems XTC_HAVE_UCONTEXT is left undefined by configure and
 * this translation unit becomes empty.  A future commit will provide a
 * coro_fctx.c substrate built on the make_fcontext / jump_fcontext
 * assembly that already ships with xtc; until that lands, builds
 * targeting musl must either accept that higher-layer tests don't link
 * (the OS layer alone still builds and tests cleanly) or use a glibc
 * toolchain. */
#if defined(_WIN32) || !defined(XTC_HAVE_UCONTEXT) || defined(XTC_CORO_FORCE_FCTX)
typedef int __xtc_coro_uctx_unused;
#else

#include "xtc_async.h"
#include "loop_int.h"
#include "coro_int.h"

#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Some platforms (FreeBSD, OpenBSD) only define MAP_ANON; Linux has
 * both spellings.  xtc_int.h already enables the BSD extension
 * namespace, so MAP_ANON should be visible by here. */
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
# define MAP_ANONYMOUS MAP_ANON
#endif

/*
 * Sanitizer fiber-switch annotations (ASan / TSan / LSan) -- see the
 * matching block in coro_fctx.c for the rationale.  Compiled to nothing
 * in an ordinary build.  The scheduler and each coro keep SEPARATE
 * fake-stack save slots (a coro overwrites its slot when it parks;
 * mixing the scheduler's token with it crashes finish()). */
#if !defined(__has_feature)
#  define __has_feature(x) 0     /* non-clang: the sanitizer probes are 0 */
#endif

/*
 * TSan and ASan use different, mutually exclusive fiber models.  Decide
 * TSan FIRST: under ThreadSanitizer emit ONLY the fiber-identity calls
 * (see coro_fctx.c for the rationale); the ASan stack-switch API is not
 * provided by the TSan runtime and would link-fail.
 */
#if defined(__has_feature) && __has_feature(thread_sanitizer)
#  include <sanitizer/tsan_interface.h>
#  define XTC_TSAN_FIBERS 1
#elif defined(__SANITIZE_ADDRESS__) || \
    __has_feature(address_sanitizer)
#  include <sanitizer/common_interface_defs.h>
#  define XTC_FIBER_SWITCH_ANNOTATE 1
#endif

#if defined(XTC_TSAN_FIBERS)
static XTC_THREAD_LOCAL void *__tsan_sched_fiber;
static inline void
__tsan_sched_capture(void)
{
	if (__tsan_sched_fiber == NULL)
		__tsan_sched_fiber = __tsan_get_current_fiber();
}
#  define __tsan_switch_into(c)  do { \
	__tsan_sched_capture(); \
	if ((c)->tsan_fiber != NULL) __tsan_switch_to_fiber((c)->tsan_fiber, 0); \
   } while (0)
#  define __tsan_switch_out()    do { \
	if (__tsan_sched_fiber != NULL) __tsan_switch_to_fiber(__tsan_sched_fiber, 0); \
   } while (0)
#else
#  define __tsan_switch_into(c)  ((void)0)
#  define __tsan_switch_out()    ((void)0)
#endif

#if defined(XTC_FIBER_SWITCH_ANNOTATE)
static XTC_THREAD_LOCAL const void *__san_sched_bottom;
static XTC_THREAD_LOCAL size_t      __san_sched_size;
static XTC_THREAD_LOCAL void       *__san_sched_fake;

static inline void
__san_switch_to(void **save_slot, const void *to_bottom, size_t to_size)
{
	__sanitizer_start_switch_fiber(save_slot, to_bottom, to_size);
}
static inline void
__san_switch_done(void *saved)
{
	const void *ob = NULL;
	size_t os = 0;
	__sanitizer_finish_switch_fiber(saved, &ob, &os);
	if (__san_sched_bottom == NULL && ob != NULL) {
		__san_sched_bottom = ob;
		__san_sched_size = os;
	}
}
static const void *
__san_coro_bottom(const struct xtc_coro *c)
{
	long pg = sysconf(_SC_PAGESIZE);
	size_t guard = pg > 0 ? (size_t)pg : 4096u;
	return (const void *)((uintptr_t)c->stack + guard);
}
#else
#  define __san_switch_to(save_slot, to_bottom, to_size) ((void)0)
#  define __san_switch_done(saved)                        ((void)0)
#endif

/* Per-thread cursor -- the coroutine currently executing on this thread. */
XTC_THREAD_LOCAL struct xtc_coro *__xtc_current_coro = NULL;

/* Default stack size; configurable via xtc_set_stack_size(). */
static size_t __xtc_stack_size = 64 * 1024;

size_t
xtc_stack_size(void)
{
	return __xtc_stack_size;
}

int
xtc_set_stack_size(size_t bytes)
{
	long pg;
	if (bytes < 16 * 1024)
		return XTC_E_INVAL;
	pg = sysconf(_SC_PAGESIZE);
	if (pg <= 0) pg = 4096;
	/* Round up to page multiple. */
	if (bytes % (size_t)pg != 0)
		bytes += (size_t)pg - (bytes % (size_t)pg);
	__xtc_stack_size = bytes;
	return XTC_OK;
}

/*
 * Per-thread guard for the involuntary-preemption redirect (Phase 2b).
 * It marks the brief windows during which the running context's
 * machine state is being saved/restored by a ucontext swapcontext (or
 * the trampoline is mid-save) -- exactly the windows in which an
 * involuntary redirect would corrupt a half-written context (verified:
 * a rare SIGSEGV in the scheduler's swapcontext resume under
 * aggressive slicing).
 *
 * It is a FLAG set immediately before each swapcontext and cleared
 * immediately after it returns -- on whichever side runs next, since a
 * swapcontext hands control to a context that itself returns just
 * after its own swapcontext.  So the flag is 1 only while a
 * swapcontext instruction is actually executing, NOT for the whole
 * time a fiber is parked (that earlier, depth-held-across-park design
 * wrongly blocked ALL preemption whenever any fiber sat parked).
 * __xtc_coro_preempt declines while the flag is set, deferring to the
 * cooperative pending flag.  volatile: the timer handler reads it on
 * the same thread that maintains it; a stale read only errs toward
 * "decline", which is safe.
 */
static XTC_THREAD_LOCAL volatile int g_in_preempt;

/* A swapcontext that must not be interrupted-and-redirected: mark the
 * window on both entry and (post-return, either side) exit. */
#define SAFE_SWAPCONTEXT(from, to) do {                                 \
	g_in_preempt = 1;                                              \
	(void)swapcontext((from), (to));                               \
	g_in_preempt = 0;                                              \
} while (0)

/* ---- Fiber-stack pool (per-thread) --------------------------------
 * Recycle freed fiber stacks so a spawn skips the mmap + the
 * mmap_lock-serializing mprotect(guard).  See the matching block in
 * coro_fctx.c for the full rationale (the EC2 192-core spawn ceiling).
 * Thread-local, bounded, single-size. */
#define XTC_STACK_POOL_MAX 64
struct stack_pool {
	void   *slots[XTC_STACK_POOL_MAX];
	size_t  size;
	int     n;
};
static XTC_THREAD_LOCAL struct stack_pool __stack_pool;

static void *
__stack_pool_get(size_t stack_sz)
{
	struct stack_pool *p = &__stack_pool;
	if (p->n > 0 && p->size == stack_sz)
		return p->slots[--p->n];
	return NULL;
}

static int
__stack_pool_put(void *base, size_t stack_sz)
{
	struct stack_pool *p = &__stack_pool;
	if (p->n == 0) p->size = stack_sz;
	if (p->size != stack_sz || p->n == XTC_STACK_POOL_MAX)
		return 0;
	p->slots[p->n++] = base;
	return 1;
}

/* ---- Lever S1: stack-memory reclamation on park (see xtc_async.h) -- */
#if defined(MADV_DONTNEED)
#include <stdatomic.h>
#include <stdint.h>
static _Atomic int      g_reclaim_on = 0;
static _Atomic size_t   g_reclaim_keep = 0;
static _Atomic uint64_t g_reclaim_count = 0;

int
xtc_stack_reclaim_enable(size_t keep_bytes)
{
	long pg = sysconf(_SC_PAGESIZE);
	size_t page = (pg > 0) ? (size_t)pg : 4096u;
	if (keep_bytes == 0)
		keep_bytes = page;
	atomic_store(&g_reclaim_keep, keep_bytes);
	atomic_store(&g_reclaim_on, 1);
	return XTC_OK;
}

void     xtc_stack_reclaim_disable(void) { atomic_store(&g_reclaim_on, 0); }
int      xtc_stack_reclaim_enabled(void) { return atomic_load(&g_reclaim_on); }
uint64_t xtc_stack_reclaim_count(void)   { return atomic_load(&g_reclaim_count); }

/* Reclaim the unused tail below `sp` (stacks grow down); see the
 * coro_fctx.c twin for the geometry rationale. */
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
	guard = page;
	keep = atomic_load(&g_reclaim_keep);
	base = (uintptr_t)c->stack + guard;
	hi = (uintptr_t)sp;
	if (hi <= base + keep)
		return;
	hi = (hi - keep) & ~(uintptr_t)(page - 1);
	lo = (base + page - 1) & ~(uintptr_t)(page - 1);
	if (hi <= lo || hi - lo < page)
		return;
	if (madvise((void *)lo, (size_t)(hi - lo), MADV_DONTNEED) == 0)
		(void)atomic_fetch_add(&g_reclaim_count, 1);
}
#else
int      xtc_stack_reclaim_enable(size_t k) { (void)k; return XTC_E_NOSYS; }
void     xtc_stack_reclaim_disable(void) { }
int      xtc_stack_reclaim_enabled(void) { return 0; }
uint64_t xtc_stack_reclaim_count(void) { return 0; }
#define coro_stack_shrink(c, sp)  ((void)(c), (void)(sp))
#endif

/*
 * The trampoline that runs as the fiber's entry point.  We pull the
 * coro pointer back from the per-thread cursor (set just before
 * makecontext_swap).
 */
static void
__coro_entry(void)
{
	struct xtc_coro *c = __xtc_current_coro;
	/*
	 * Receiver-side clear: __xtc_coro_step set g_in_preempt just before
	 * the swapcontext that transferred control here (protecting its
	 * loop_ctx save).  Now that we hold control on the fiber, that
	 * save is complete -- clear the guard so this fiber body (which
	 * may never yield) can be involuntarily preempted.  Without this a
	 * fresh pure-tight-loop fiber would run with the guard stuck at 1
	 * and never be sliced.
	 */
	g_in_preempt = 0;
	__san_switch_done(c->san_fake_stack);   /* arrived on the coro stack */
	c->result = c->fn(c->arg);
	c->done = 1;
	/* Return to the loop's context.  We will not be resumed, so the
	 * outgoing fake stack is discarded (save slot NULL). */
	__san_switch_to(NULL, __san_sched_bottom, __san_sched_size);
	__tsan_switch_out();
	SAFE_SWAPCONTEXT(&c->ctx, &c->loop_ctx);
}

/*
 * PUBLIC: int __xtc_coro_preempt_effective __P((void));
 *
 * Returns 1 iff this build's coroutine substrate implements the
 * involuntary preemption redirect (Phase 2b-arch), 0 if it falls back
 * to Phase 1 cooperative-assisted preemption.  A runtime query so tests
 * (and callers) need not know the internal substrate/arch macros: it
 * tracks exactly the #if in __xtc_coro_preempt below.
 */
int
__xtc_coro_preempt_effective(void)
{
#if (defined(__x86_64__) || defined(__aarch64__)) && \
    defined(__linux__) && !defined(__APPLE__) && !defined(XTC_AMALGAMATION)
	return 1;
#else
	return 0;
#endif
}

/*
 * PUBLIC: int __xtc_coro_preempt __P((void *));
 *
 * Signal-context involuntary yield (M_PREEMPTION Phase 2b), ucontext
 * substrate.  Called ONLY from the preemption timer signal handler,
 * with `uctx` the interrupted ucontext_t* (the SA_SIGINFO third arg),
 * and ONLY when it is safe (crit_depth == 0 and unsafe_depth == 0 --
 * the handler checks).  It makes the running fiber yield to the
 * scheduler AS IF it had called xtc_yield() at the interrupted
 * instruction, resumably:
 *
 *   - copy the interrupted context into c->ctx, so the fiber resumes
 *     EXACTLY where the timer hit it when the scheduler next runs it;
 *   - overwrite *uctx with c->loop_ctx, so when the handler returns the
 *     kernel's sigreturn restores the scheduler's context instead of
 *     the interrupted one -- landing in __xtc_coro_step just after its
 *     swapcontext, which (the fiber not being done/parked) returns
 *     XTC_TASK_RESCHED and re-queues the fiber.
 *
 * This is Go's async-preemption mechanism: the kernel does the context
 * swap on sigreturn, so no arch-specific stack surgery is needed -- it
 * works on every ucontext platform.  Returns 1 if it armed the yield, 0
 * if there is no current fiber (nothing to preempt; the handler leaves
 * the tick pending for a cooperative yield instead).
 *
 * The fctx substrate (coro_fctx.c) provides its own version that
 * declines (returns 0), so on musl the timer falls back to Phase 1
 * cooperative-assisted preemption.
 */
int
__xtc_coro_preempt(void *uctx)
{
#if defined(__x86_64__) && defined(__linux__) && !defined(__APPLE__) && !defined(XTC_AMALGAMATION)
	/*
	 * Phase 2b-arch, x86-64 System V: Go's async-preemption PC
	 * redirect.  We do NOT copy the (unsound-to-reuse) signal
	 * ucontext; we rewrite it so that on sigreturn the fiber resumes
	 * -- on its own stack, with its own real registers -- at
	 * __xtc_preempt_trampoline, which does a normal cooperative
	 * xtc_yield() and later returns to the interrupted PC.
	 *
	 * Only a real fiber can be preempted (the scheduler thread itself
	 * has no coro; nothing to yield).  The handler already checked
	 * crit_depth == 0 and unsafe_depth == 0.
	 *
	 * RE-ENTRANCY GUARD: a second timer tick that fires after this
	 * arms the redirect but before the trampoline has finished saving
	 * the interrupted register file would rewrite the same fiber's
	 * RIP/RSP a second time and corrupt c->ctx (verified: a rare
	 * SIGSEGV in the scheduler's swapcontext resume under aggressive
	 * slicing).  g_in_preempt closes that window: it is set here and
	 * cleared by the trampoline the instant its saves are complete
	 * (see __xtc_preempt_armed_clear), so a nested tick in the
	 * vulnerable window declines and falls back to the pending flag.
	 */
	extern void __xtc_preempt_trampoline(void);
	extern void __xtc_preempt_trampoline_end(void);
	ucontext_t *uc = (ucontext_t *)uctx;
	greg_t orig_pc, orig_sp, new_sp;

	if (__xtc_current_coro == NULL)
		return 0;   /* not in a fiber; leave the tick pending */
	if (g_in_preempt > 0)
		return 0;   /* inside a swapcontext or a redirect in flight */

	orig_pc = uc->uc_mcontext.gregs[REG_RIP];
	orig_sp = uc->uc_mcontext.gregs[REG_RSP];

	/*
	 * Decline if the fiber is executing inside the trampoline itself
	 * (prologue saving the register file, the xtc_yield call, or the
	 * epilogue restoring it): a nested redirect there would corrupt a
	 * half-saved/half-restored register file.  The epilogue runs with
	 * g_in_preempt already back to 0, so the flag alone cannot cover
	 * it -- this instruction-pointer range check does.
	 */
	if (orig_pc >= (greg_t)(uintptr_t)&__xtc_preempt_trampoline &&
	    orig_pc <  (greg_t)(uintptr_t)&__xtc_preempt_trampoline_end)
		return 0;

	/*
	 * Push the original PC as the trampoline's return address, so its
	 * terminal `ret` resumes at orig_pc with RSP == orig_sp exactly.
	 * Entry RSP for the trampoline is orig_sp - 8; the trampoline
	 * itself drops past the 128-byte red zone before saving anything,
	 * so this single 8-byte store is the only write above the
	 * interrupted RSP and it lands in the red zone (which the
	 * interrupted code cannot rely on across an asynchronous event).
	 */
	new_sp = orig_sp - 8;
	*(greg_t *)(uintptr_t)new_sp = orig_pc;

	g_in_preempt++;
	uc->uc_mcontext.gregs[REG_RSP] = new_sp;
	uc->uc_mcontext.gregs[REG_RIP] =
	    (greg_t)(uintptr_t)&__xtc_preempt_trampoline;
	return 1;   /* armed: sigreturn lands in the trampoline */
#elif defined(__aarch64__) && defined(__linux__) && !defined(__APPLE__) && !defined(XTC_AMALGAMATION)
	/*
	 * Phase 2b-arch, AArch64 (AAPCS64): the same Go-style PC redirect
	 * as x86-64.  aarch64 has no red zone, so we place orig_pc in a
	 * 16-aligned scratch slot just below the interrupted sp and set the
	 * trampoline's entry sp there; the trampoline recovers orig_pc and
	 * orig_sp from that slot on resume.  See
	 * preempt_trampoline_aarch64.S for the register-file save/restore
	 * and the x16/x17 caveat.
	 */
	extern void __xtc_preempt_trampoline(void);
	extern void __xtc_preempt_trampoline_end(void);
	ucontext_t *uc = (ucontext_t *)uctx;
	unsigned long orig_pc, orig_sp, scratch;

	if (__xtc_current_coro == NULL)
		return 0;
	if (g_in_preempt > 0)
		return 0;

	orig_pc = (unsigned long)uc->uc_mcontext.pc;
	orig_sp = (unsigned long)uc->uc_mcontext.sp;

	if (orig_pc >= (unsigned long)(uintptr_t)&__xtc_preempt_trampoline &&
	    orig_pc <  (unsigned long)(uintptr_t)&__xtc_preempt_trampoline_end)
		return 0;

	/* 16-aligned scratch below the interrupted sp; stash orig_pc there.
	 * orig_sp is already 16-aligned per AAPCS64, so orig_sp - 16 is
	 * 16-aligned.  The trampoline reads orig_pc from [scratch] and
	 * recovers orig_sp == scratch + 16. */
	scratch = orig_sp - 16;
	*(unsigned long *)(uintptr_t)scratch = orig_pc;

	g_in_preempt++;
	uc->uc_mcontext.sp = (unsigned long long)scratch;
	uc->uc_mcontext.pc =
	    (unsigned long long)(uintptr_t)&__xtc_preempt_trampoline;
	return 1;   /* armed: sigreturn lands in the trampoline */
#else
	/*
	 * Other architectures (aarch64, ...) and the single-file
	 * amalgamation (which cannot carry the trampoline's .S assembly)
	 * decline, so the timer falls back to Phase 1 cooperative-assisted
	 * preemption.  A resumable preemption needs the fiber's
	 * interrupted machine state as a resume point; the whole-ucontext
	 * copy is UNSOUND (a signal-delivered ucontext is not
	 * interchangeable with a swapcontext-captured one -> SIGSEGV in
	 * swapcontext).  The correct method is per-architecture: extract
	 * PC/SP from the signal mcontext and redirect the interrupted PC
	 * to an on-stack trampoline that does a normal cooperative switch
	 * (see the x86-64 path above).
	 */
	(void)uctx;
	return 0;
#endif
}

/*
 * The task fn that the L2 scheduler calls.  Swaps into the fiber.
 * On return, decides what to tell the loop based on the fiber's
 * state.
 */
int
__xtc_coro_step(xtc_task_t *self, void *user)
{
	struct xtc_coro *c = user;
	struct xtc_coro *saved;

	/* Bind the coro's task back-pointer here, on the running thread,
	 * every step.  It is also set at creation, but under the multi-loop
	 * executor a freshly published coro can first run on another thread
	 * before that store is visible, leaving c->self (hence
	 * __xtc_current_task, hence a proc's self->task) transiently NULL.
	 * Setting it from the task the loop hands us closes that race. */
	c->self = self;

	/* Set the per-thread cursor so xtc_yield() can find us.  Guard from
	 * here: once __xtc_current_coro == c a timer tick would treat the
	 * scheduler's own execution as "in fiber c" and could redirect it
	 * before the swap; hold the guard across the cursor update and the
	 * swap (the fiber clears it on resume). */
	g_in_preempt = 1;
	saved = __xtc_current_coro;
	__xtc_current_coro = c;

	__san_switch_to(&__san_sched_fake, __san_coro_bottom(c), c->stack_sz);
	__tsan_switch_into(c);
	(void)swapcontext(&c->loop_ctx, &c->ctx);
	g_in_preempt = 0;
	__san_switch_done(__san_sched_fake);   /* back on the scheduler stack */

	__xtc_current_coro = saved;

	if (c->done) {
		if (c->waiter != NULL) {
			xtc_waker_t w = { c->waiter->loop, c->waiter };
			(void)xtc_waker_wake(&w);
			c->waiter = NULL;
		}
		return XTC_TASK_DONE;
	}
	/* Awaiting another coroutine?  Stay parked; the awaitee will
	 * fire our waker when it completes. */
	if (c->_parked_on != NULL) {
		c->_parked_on = NULL;
		return XTC_TASK_PENDING;
	}
	/* Parked on a timer or fd via xtc_task_park_on_*?  Stay parked
	 * until the timer fires or the fd is ready -- a waker will
	 * re-enqueue us.  Without this check, xtc_recv with a timeout
	 * would busy-spin: every yield re-queues the task, and the loop
	 * never sleeps in xtc_io_poll because the runqueue is
	 * non-empty. */
	if (c->self != NULL &&
	    (c->self->park_timer != NULL || c->self->park_fd >= 0)) {
		return XTC_TASK_PENDING;
	}
	/* Voluntary park requested (xtc_amutex and friends): sleep until
	 * a waker re-enqueues us instead of busy-rescheduling. */
	if (c->self != NULL && c->self->park_requested) {
		c->self->park_requested = 0;
		return XTC_TASK_PENDING;
	}
	/* Plain yield; re-queue at the back of the run queue. */
	return XTC_TASK_RESCHED;
}

/*
 * Free the fiber's stack on task completion.  Hooked from the
 * loop's task-cleanup pass at fini time.  For now we register a
 * destructor on the task user pointer; M4 just frees in xtc_loop_fini
 * via the all_tasks walk.
 */
static void
__coro_destroy(struct xtc_coro *c)
{
	if (c == NULL) return;
#if defined(XTC_TSAN_FIBERS)
	if (c->tsan_fiber != NULL) __tsan_destroy_fiber(c->tsan_fiber);
#endif
	if (c->stack != NULL) {
		long pg = sysconf(_SC_PAGESIZE);
		size_t total = c->stack_sz + (pg > 0 ? (size_t)pg : 4096);
		if (!__stack_pool_put(c->stack, c->stack_sz))
			(void)munmap(c->stack, total);
	}
	__os_free(c);
}

/* Task cleanup trampoline: xtc_loop_fini calls this for each coro
 * task so the fiber stack + coro struct are released with the loop. */
static void
__coro_task_cleanup(void *coro)
{
	__coro_destroy((struct xtc_coro *)coro);
}

/*
 * PUBLIC: int xtc_async __P((xtc_loop_t *, xtc_coro_fn, void *, xtc_task_t **));
 */
int
xtc_async(xtc_loop_t *loop, xtc_coro_fn fn, void *arg, xtc_task_t **out_task)
{
	struct xtc_coro *c;
	xtc_task_t *t;
	int rc;
	long pg;
	size_t guard, total;
	char *base;

	if (loop == NULL || fn == NULL)
		return XTC_E_INVAL;

	if ((rc = __os_calloc(1, sizeof *c, (void **)&c)) != XTC_OK)
		return rc;

	pg = sysconf(_SC_PAGESIZE);
	if (pg <= 0) pg = 4096;
	guard = (size_t)pg;
	c->stack_sz = __xtc_stack_size;
	total = c->stack_sz + guard;

	base = __stack_pool_get(c->stack_sz);
	if (base == NULL) {
		base = mmap(NULL, total, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (base == MAP_FAILED) {
			__os_free(c);
			return XTC_E_NOMEM;
		}
		/* The first page is the guard (installed once per mapping). */
		if (mprotect(base, guard, PROT_NONE) != 0) {
			(void)munmap(base, total);
			__os_free(c);
			return XTC_E_INTERNAL;
		}
	}
	c->stack = base;
	c->fn = fn;
	c->arg = arg;
	c->done = 0;
	c->waiter = NULL;

	if (getcontext(&c->ctx) != 0) {
		__coro_destroy(c);
		return XTC_E_INTERNAL;
	}
	c->ctx.uc_stack.ss_sp = base + guard;
	c->ctx.uc_stack.ss_size = c->stack_sz;
	c->ctx.uc_link = NULL;       /* end-of-coroutine returns via swap */

	/*
	 * Block process-directed signals in the fiber's context so they do
	 * not land on a runtime scheduler thread, but EXEMPT the signals
	 * that must stay deliverable:
	 *   - SIGVTALRM: the involuntary-preemption timer, delivered to a
	 *     running fiber that armed preemption.
	 *   - SIGSEGV/SIGBUS/SIGFPE/SIGILL: synchronous, thread-directed
	 *     hardware faults.  libxtc's fault guard catches these for R1
	 *     containment; blocking them would prevent containment and is
	 *     undefined behavior per POSIX for a hardware-generated fault.
	 *   - SIGABRT: the assert/panic path must be able to fire.
	 *
	 * Rationale: swapcontext restores uc_sigmask on every switch, and
	 * getcontext captured the CREATING thread's mask -- which may have a
	 * process-directed signal unblocked (a proc spawned from the
	 * embedder's main thread).  Restoring that on a loop/worker thread
	 * would let a process-directed signal (e.g. SIGCHLD) land on a
	 * scheduler thread instead of the embedder's designated handler.
	 * A proc that fork()s resets its own mask in the child (xtc_osproc).
	 */
	sigfillset(&c->ctx.uc_sigmask);
	sigdelset(&c->ctx.uc_sigmask, SIGVTALRM);
	sigdelset(&c->ctx.uc_sigmask, SIGSEGV);
	sigdelset(&c->ctx.uc_sigmask, SIGBUS);
	sigdelset(&c->ctx.uc_sigmask, SIGFPE);
	sigdelset(&c->ctx.uc_sigmask, SIGILL);
	sigdelset(&c->ctx.uc_sigmask, SIGABRT);

	/* The trampoline reads __xtc_current_coro on entry; that cursor is
	 * set by __xtc_coro_step when the loop first runs us, so we don't
	 * need to touch it here.  Critically, we must NOT clobber the
	 * caller's cursor when xtc_async is invoked from inside another
	 * coroutine. */
	makecontext(&c->ctx, __coro_entry, 0);
#if defined(XTC_TSAN_FIBERS)
	c->tsan_fiber = __tsan_create_fiber(0);
#endif

	if ((rc = __xtc_task_spawn_ex(loop, __xtc_coro_step, c, 1, &t)) != XTC_OK) {
		__coro_destroy(c);
		return rc;
	}
	c->self = t;
	/* Release the fiber stack + coro struct when the loop tears the
	 * task down at fini. */
	t->cleanup = __coro_task_cleanup;
	t->cleanup_arg = c;

	if (out_task) *out_task = t;
	return XTC_OK;
}

/*
 * PUBLIC: int xtc_await __P((xtc_task_t *, intptr_t *));
 */
int
xtc_await(xtc_task_t *t, intptr_t *result)
{
	struct xtc_coro *me;
	struct xtc_coro *target;

	if (t == NULL) return XTC_E_INVAL;

	target = (struct xtc_coro *)t->user;

	/* Already done?  Take the fast path. */
	if (target->done) {
		if (result) *result = target->result;
		return XTC_OK;
	}

	me = __xtc_current_coro;

	if (me == NULL) {
		/* Awaiting from outside any coroutine.  Drive the loop
		 * until the target completes.  Useful for tests + main(). */
		while (!target->done) {
			int rc = xtc_loop_run(t->loop);
			if (rc != XTC_OK) return rc;
		}
		if (result) *result = target->result;
		return XTC_OK;
	}

	/* Inside a coroutine.  Register me as the target's waiter, yield,
	 * and resume after the target wakes me.  We park by setting our
	 * task's state to PARKED before yielding; the loop's task verdict
	 * for the next step is already RESCHED by virtue of __xtc_coro_step
	 * returning, so we have to coordinate manually:
	 *
	 *   - target->waiter = me->self
	 *   - swap back to loop with yield-shaped semantics
	 *   - the LAST thing __xtc_coro_step does is RESCHED, which would
	 *     re-run us prematurely.  Solution: leave a flag on the coro
	 *     that the step function checks; when set, return PENDING and
	 *     reset the flag.
	 */
	target->waiter = me->self;
	me->done = 0;            /* unchanged; for clarity */
	me->self->q_next = NULL; /* belt-and-braces; see q-pop semantics */

	/*
	 * Set the per-coro "i'm parked, don't reschedule" hint by
	 * stashing target into a side channel.  We use a simple flag
	 * on the coro: while parked == 1, __xtc_coro_step returns
	 * PENDING instead of RESCHED.
	 */
	me->_parked_on = target;
	{
		void *pctx;
		g_in_preempt = 1;
		pctx = __xtc_fiber_ctx_save ? __xtc_fiber_ctx_save() : NULL;
		__san_switch_to(&me->san_fake_stack, __san_sched_bottom,
		    __san_sched_size);
		__tsan_switch_out();
		(void)swapcontext(&me->ctx, &me->loop_ctx);
		__san_switch_done(me->san_fake_stack);   /* resumed on our stack */
		if (__xtc_fiber_ctx_restore) __xtc_fiber_ctx_restore(pctx);
		g_in_preempt = 0;
	}
	/* When we return here, target->done must be true. */
	if (result) *result = target->result;
	return XTC_OK;
}

/*
 * PUBLIC: void xtc_yield __P((void));
 */
void
xtc_yield(void)
{
	struct xtc_coro *c = __xtc_current_coro;
	void *pctx;
	volatile char probe;
	if (c == NULL) return;
	/* S1: return the unused stack tail below our SP before parking. */
	coro_stack_shrink(c, (void *)&probe);
	/* Preserve the process layer's per-fiber TLS across the yield --
	 * see the coro_fctx.c xtc_yield for the rationale.  Guard the whole
	 * save/swap/restore against involuntary preemption: a redirect
	 * caught while the machine context OR the per-fiber TLS is
	 * half-swapped corrupts the fiber.  The receiver-side clear (in
	 * __xtc_coro_step / __coro_entry) re-opens preemption once control
	 * has fully landed. */
	g_in_preempt = 1;
	pctx = __xtc_fiber_ctx_save ? __xtc_fiber_ctx_save() : NULL;
	__san_switch_to(&c->san_fake_stack, __san_sched_bottom, __san_sched_size);
	__tsan_switch_out();
	(void)swapcontext(&c->ctx, &c->loop_ctx);
	__san_switch_done(c->san_fake_stack);   /* resumed on our stack */
	if (__xtc_fiber_ctx_restore) __xtc_fiber_ctx_restore(pctx);
	g_in_preempt = 0;
	/* Universal resume point: honor a kill/cancel requested while away
	 * (e.g. an involuntary preemption of a pure CPU loop) -- xtc_launch
	 * cancel of a runaway lands here. */
	if (__xtc_fiber_kill_check) __xtc_fiber_kill_check();
}

xtc_task_t *
__xtc_current_task(void)
{
	return __xtc_current_coro != NULL ? __xtc_current_coro->self : NULL;
}

#endif /* !_WIN32 */
