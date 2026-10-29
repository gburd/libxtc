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
/*
 * Substrate selection: src/inc/coro_select.h (one rule for every site,
 * so struct xtc_coro's layout never drifts between translation units).
 * This TU is empty exactly when fcontext is live.
 */
#include "coro_select.h"
#if defined(_WIN32) || defined(XTC_CORO_FCTX)
typedef int __xtc_coro_uctx_unused;
#else

#include "xtc_async.h"
#include "loop_int.h"
#include "coro_int.h"

#include <errno.h>
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

/* Apple deprecated getcontext/makecontext/swapcontext (no replacement
 * offered) years ago; they still work and are the only portable stackful
 * substrate on Darwin until a Mach-O arm64 fcontext lands (see
 * KNOWN_ISSUES).  Silence the deprecation warning for just this TU so
 * the zero-warning policy stays honest on macOS without hiding a real
 * issue anywhere else. */
#if defined(__APPLE__)
# pragma clang diagnostic push
# pragma clang diagnostic ignored "-Wdeprecated-declarations"
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
#  include <sanitizer/asan_interface.h>   /* ASAN_UNPOISON_MEMORY_REGION */
#  define XTC_FIBER_SWITCH_ANNOTATE 1
#endif

#if defined(XTC_TSAN_FIBERS)
static XTC_THREAD_LOCAL void *__tsan_sched_fiber_tls;
/* Through a non-inlinable accessor, like g_in_preempt: a fiber that
 * finishes on another OS thread must switch to THAT thread's scheduler
 * fiber.  aarch64 clang kept tpidr_el0 in a callee-saved register
 * across c->fn() and handed TSan the first thread's (test_exec /Blk*,
 * test_steal /M9: CHECK thr->proc1 == nullptr, SEGV). */
__attribute__((noinline))
static void **
__tsan_sched_slot(void)
{
	void **volatile slot = &__tsan_sched_fiber_tls;
	return slot;
}
#define __tsan_sched_fiber (*__tsan_sched_slot())
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
struct __san_sched_tls {
	const void *bottom;
	size_t      size;
	void       *fake;
};
static XTC_THREAD_LOCAL struct __san_sched_tls __san_sched_tls_v;
/* Per-thread scheduler stack, reached through a non-inlinable accessor
 * for the same migration reason as g_in_preempt (see below). */
__attribute__((noinline))
static struct __san_sched_tls *
__san_sched_slot(void)
{
	struct __san_sched_tls *volatile slot = &__san_sched_tls_v;
	return slot;
}
#define __san_sched_bottom (__san_sched_slot()->bottom)
#define __san_sched_size   (__san_sched_slot()->size)
#define __san_sched_fake   (__san_sched_slot()->fake)

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


/* The in-switch guard (g_in_preempt), the x86-64 vector-state probe and
 * the x86-64 redirect, shared with coro_fctx.c.  See coro_preempt.h. */
#include "coro_preempt.h"

/* A swapcontext that must not be interrupted-and-redirected: mark the
 * window on both entry and (post-return, either side) exit. */
#define SAFE_SWAPCONTEXT(from, to) do {                                 \
	g_in_preempt = 1;                                              \
	(void)swapcontext((from), (to));                               \
	g_in_preempt = 0;                                              \
} while (0)

/* Fiber-stack pool + S1 stack-reclaim lever: shared with coro_fctx.c
 * (both mmap their stacks).  See src/inc/coro_common.h. */
#include "coro_common.h"

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
	__san_switch_done(c->san_fake_stack);   /* arrived on the coro stack */
	/* A new fiber is not a proc until __proc_entry says so: clear the
	 * proc TLS the previous fiber on this thread left behind, or a bare
	 * xtc_async coroutine runs (and yields) as that proc -- xtc_self()
	 * names it, xtc_recv steals its mail, and once it is freed every
	 * yield's kill check reads freed memory (review156 B1). */
	if (__xtc_fiber_ctx_restore) __xtc_fiber_ctx_restore(NULL);
	g_in_preempt = 0;
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
#if defined(XTC_CORO_REDIRECT_X86_64) || defined(XTC_CORO_REDIRECT_AARCH64)
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
 * The fctx substrate (coro_fctx.c) provides its own version with the
 * same redirects on glibc; it declines (returns 0) on musl, where the
 * timer falls back to Phase 1 cooperative-assisted preemption.
 */
int
__xtc_coro_preempt(void *uctx)
{
#if defined(XTC_CORO_REDIRECT_X86_64)
	return __coro_preempt_redirect_x86_64(uctx);
#elif defined(XTC_CORO_REDIRECT_AARCH64)
	/* Phase 2b-arch, AArch64 Linux: rt_sigreturn on the kernel's
	 * in-place signal frame (coro_preempt.h). */
	return __coro_preempt_redirect_aarch64(uctx);
#else
	/*
	 * Other architectures and the single-file
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

static void __coro_release_stack(struct xtc_coro *c); /* below */

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
	__san_switch_done(__san_sched_fake);   /* back on the scheduler stack */

	/* Restore the cursor BEFORE dropping the guard: with the guard down
	 * and the cursor still naming c, a tick would take the scheduler
	 * for fiber c and redirect it through the trampoline, whose yield
	 * then saves the scheduler's context into c (a SIGSEGV here, under
	 * test_wal_kill preempt-nokill). */
	__xtc_current_coro = saved;
	g_in_preempt = 0;
	/* The scheduler has control again: the per-thread current-proc TLS
	 * still names the fiber that just yielded (or stopped the executor
	 * and yielded).  Give it back to whoever runs here -- the outer
	 * coroutine's proc when this step is nested, else NO proc -- or
	 * xtc_self() / xtc_mask_enter / xtc_proc_critical_enter on this
	 * thread act on that fiber's proc, which a later xtc_exec_fini
	 * frees (test_sim_stop_tls; the heap corruption in
	 * test_sim_compose_crash). */
	if (__xtc_fiber_ctx_restore)
		__xtc_fiber_ctx_restore(saved != NULL ? saved->proc : NULL);

	if (c->done) {
		if (c->waiter != NULL) {
			xtc_waker_t w = { c->waiter->loop, c->waiter };
			(void)xtc_waker_wake(&w);
			c->waiter = NULL;
		}
		/* lifetime: a finished PROC fiber is reclaimed by the loop's
		 * DONE handler (task.c __xtc_task_reclaim).  Its task handle
		 * was never handed out; an xtc_async task's was, so that task
		 * and its coro stay until xtc_loop_fini (the stack does not).
		 * c->proc is set by __proc_entry before the body runs and never
		 * cleared. */
		if (c->proc != NULL)
			self->reclaim = 1;
		else
			/* A bare xtc_async coroutine keeps its task + coro for
			 * xtc_await until xtc_loop_fini, but not its stack:
			 * 20,000 async/await pairs held 20,000 stacks (two
			 * mappings each) until fini.  We are on the scheduler
			 * stack, so the finished fiber's is idle. */
			__coro_release_stack(c);
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
	    (atomic_load_explicit(&c->self->park_timer,
		memory_order_relaxed) != NULL ||
	     atomic_load_explicit(&c->self->park_fd, memory_order_relaxed) >= 0)) {
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
/* Give a coro's fiber stack back (the thread's pool, the loop's spare,
 * or munmap) and forget it.  Idempotent: a finished xtc_async coroutine
 * releases its stack at once (__xtc_coro_step), and the coro struct --
 * which xtc_await reads the result from -- is freed at xtc_loop_fini. */
static void
__coro_release_stack(struct xtc_coro *c)
{
#if defined(XTC_TSAN_FIBERS)
	/* TSan's per-fiber state is mapped too (161 -> 8,153 maps lines over
	 * 4,000 async/await under -fsanitize=thread); the fiber is never
	 * switched to again. */
	if (c->tsan_fiber != NULL) {
		__tsan_destroy_fiber(c->tsan_fiber);
		c->tsan_fiber = NULL;
	}
#endif
	if (c->stack != NULL) {
		long pg = sysconf(_SC_PAGESIZE);
		size_t total = c->stack_sz + (pg > 0 ? (size_t)pg : 4096);
		__xtc_vg_stack_deregister(c->vg_stack_id);
#if defined(XTC_FIBER_SWITCH_ANNOTATE)
		/* A finished fiber leaves its last frames' redzones poisoned
		 * (it never returned; a kill longjmp skipped more).  Clear them
		 * so the next fiber on this stack starts clean. */
		ASAN_UNPOISON_MEMORY_REGION((char *)c->stack +
		    (total - c->stack_sz), c->stack_sz);
#endif
		if (!__stack_pool_put(c->stack, c->stack_sz) &&
		    (c->self == NULL || c->self->loop == NULL ||
		     !__stack_spare_put(c->self->loop, c->stack,
		     c->stack_sz)))
			(void)munmap(c->stack, total);
	}
	c->stack = NULL;
}

static void
__coro_destroy(struct xtc_coro *c)
{
	if (c == NULL) return;
	__coro_release_stack(c);
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
__xtc_async_ex(xtc_loop_t *loop, xtc_coro_fn fn, void *arg, int pinned,
    xtc_task_t **out_task)
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
	if (base == NULL)
		base = __stack_spare_get(loop, c->stack_sz);
	if (base == NULL) {
		base = mmap(NULL, total, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (base == MAP_FAILED) {
			__os_free(c);
			return XTC_E_NOMEM;
		}
		/* The first page is the guard (installed once per mapping).
		 * The mprotect SPLITS the mapping into two VMAs, so it is
		 * where the process map-count limit (Linux
		 * vm.max_map_count, 65530 by default: ~32 K fibers) fails,
		 * with ENOMEM.  That is memory exhaustion, reported like the
		 * mmap failure above -- not an internal error. */
		if (mprotect(base, guard, PROT_NONE) != 0) {
			int e = errno;
			(void)munmap(base, total);
			__os_free(c);
			return (e == ENOMEM || e == EAGAIN) ? XTC_E_NOMEM
			    : XTC_E_INTERNAL;
		}
	}
	c->stack = base;
	c->vg_stack_id = __xtc_vg_stack_register(base + guard, c->stack_sz);
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

	/* cleanup releases the fiber stack + coro struct when the loop tears
	 * the task down at fini.  It is stored before the task is published
	 * (it may run at once on another loop); c->self needs no store here,
	 * __xtc_coro_step binds it on every step. */
	if ((rc = __xtc_task_spawn_cleanup(loop, __xtc_coro_step, c, pinned,
	    __coro_task_cleanup, c, &t)) != XTC_OK) {
		__coro_destroy(c);
		return rc;
	}

	if (out_task) *out_task = t;
	return XTC_OK;
}

/*
 * PUBLIC: int xtc_async __P((xtc_loop_t *, xtc_coro_fn, void *, xtc_task_t **));
 *
 * Public entry: spawns a PINNED coroutine (default, unchanged).
 * Internal callers wanting a migratable coro use __xtc_async_ex.
 */
int
xtc_async(xtc_loop_t *loop, xtc_coro_fn fn, void *arg, xtc_task_t **out_task)
{
	return __xtc_async_ex(loop, fn, arg, 1, out_task);
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
	/*
	 * Set the per-coro "i'm parked, don't reschedule" hint by
	 * stashing target into a side channel.  We use a simple flag
	 * on the coro: while parked == 1, __xtc_coro_step returns
	 * PENDING instead of RESCHED.
	 *
	 * Re-park until the target is done: a wake that is not the
	 * target's completion (a reclaimed task's late waker landing on
	 * this reused task struct) is spurious here.  A kill wake
	 * (xtc_exit_pid) is not: AWAIT_KILL_POLL delivers it now, once this
	 * fiber is no longer the target's waiter, instead of when the
	 * target finishes.  The target is left alone (another task may
	 * await it).  Masked, the kill is latched and the await goes on.
	 */
	while (!target->done) {
		void *pctx;
		AWAIT_KILL_POLL(me, target);
		target->waiter = me->self;
		me->self->q_next = NULL; /* belt-and-braces; see q-pop semantics */
		me->_parked_on = target;
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

__attribute__((noinline))
struct xtc_coro *
__xtc_current_coro_get(void)
{
	/* volatile round-trip: never folded into a caller's cached TLS
	 * address, even under LTO (see coro_int.h). */
	struct xtc_coro **volatile slot = &__xtc_current_coro;
	return *slot;
}

xtc_task_t *
__xtc_current_task(void)
{
	struct xtc_coro *c = __xtc_current_coro_get();
	return c != NULL ? c->self : NULL;
}


#if defined(__APPLE__)
# pragma clang diagnostic pop
#endif

#endif /* !_WIN32 */
