/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/evt/coro_fctx.c
 *	Stackful coroutine substrate via hand-written make_fcontext /
 *	jump_fcontext assembly (src/os/asm/fctx_*.S), the portable
 *	alternative to ucontext.
 *
 *	This translation unit is the active substrate when the platform
 *	lacks ucontext -- notably musl libc, which omits swapcontext /
 *	getcontext / makecontext on purpose -- or when the build forces
 *	it with -DXTC_CORO_FORCE_FCTX (used to exercise this path on a
 *	glibc host that also has ucontext).  Exactly one of coro_uctx.c
 *	and coro_fctx.c provides the substrate symbols; the other
 *	compiles to an empty unit.
 *
 *	Surface and contract are identical to coro_uctx.c: xtc_async,
 *	__xtc_coro_step, xtc_await, xtc_yield, xtc_stack_size /
 *	xtc_set_stack_size, plus the busy-loop-avoidance verdict logic
 *	(return PENDING when parked on an awaitee, a timer, or an fd).
 *
 *	The fcontext model (see the asm headers):
 *	  void *make_fcontext(stack_top, size, fn) -> a saved sp; the
 *	    first jump into it calls fn(transfer) on the new stack.
 *	  void *jump_fcontext(&from, to, transfer) -> saves the current
 *	    sp into *from, switches to `to`, and returns the transfer
 *	    value supplied by whoever next jumps back.
 *
 *	A coroutine's resume point lives in c->fctx.  The scheduler's
 *	return point is a per-thread cursor (g_sched_fctx): coroutines
 *	always jump back to the scheduler, never directly to each other,
 *	so one cursor per worker thread suffices.
 */

/* REG_RIP / REG_RSP / greg_t for the x86-64 redirect (coro_preempt.h)
 * live behind __USE_GNU on glibc; request _GNU_SOURCE before any header
 * pulls <ucontext.h> in, as coro_uctx.c does. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "xtc_int.h"

/*
 * Substrate selection: src/inc/coro_select.h (one rule for every site,
 * so struct xtc_coro's layout never drifts between translation units).
 * This TU is live exactly when XTC_CORO_FCTX is defined.
 */
#include "coro_select.h"
#if defined(XTC_CORO_FCTX)

#include "xtc_async.h"
#include "loop_int.h"
#include "coro_int.h"
#include "xtc_inject.h"   /* coro.switch_window test hook */

#include <errno.h>
#include <pthread.h>     /* pthread_sigmask: __coro_open_sync_signals */
#include <signal.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
# define MAP_ANONYMOUS MAP_ANON
#endif

/*
 * Sanitizer fiber-switch annotations (ASan / TSan / LSan).  The
 * sanitizer runtimes track one "current stack" per OS thread and do not
 * know a user-space context switch moved us onto another stack; without
 * a notification at each switch they mis-attribute stack memory (false
 * stack-use-after-return, a false happens-before graph under TSan).
 * The compiler runtime ships an official API for cooperative schedulers
 * to fix this, used by Boost.Context et al.  Compiled to nothing in an
 * ordinary build, so release binaries are byte-for-byte unchanged.
 */
#if !defined(__has_feature)
#  define __has_feature(x) 0     /* non-clang: the sanitizer probes are 0 */
#endif

/*
 * TSan uses a DIFFERENT fiber model than ASan, and the two are mutually
 * exclusive per build.  Decide TSan FIRST: under ThreadSanitizer we use
 * ONLY the fiber-IDENTITY API (__tsan_create/switch/destroy_fiber, clang
 * compiler-rt only) and must NOT emit the ASan stack-switch calls
 * (__sanitizer_*_switch_fiber), which the TSan runtime does not provide
 * (they link-fail under -fsanitize=thread).  Under ASan/LSan we use the
 * stack-switch API.  Compiled to nothing in an ordinary build.
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
/* The scheduler thread's own fiber, captured once per thread on first
 * use so __tsan_switch_to_fiber has a valid "back to the scheduler"
 * target. */
static XTC_THREAD_LOCAL void *__tsan_sched_fiber;

static inline void
__tsan_sched_capture(void)
{
	if (__tsan_sched_fiber == NULL)
		__tsan_sched_fiber = __tsan_get_current_fiber();
}
#endif

/*
 * Per-thread saved "scheduler stack" bounds, captured the first time a
 * coro switch returns control to the scheduler stack.  finish() needs
 * the stack we ARRIVE on; on the way into a coro that is the coro's
 * stack (known), on the way back it is the scheduler's (captured here).
 */
#if defined(XTC_FIBER_SWITCH_ANNOTATE)
struct __san_sched_tls {
	const void *bottom;
	size_t      size;
	void       *fake;   /* scheduler's own fake-stack token */
};
static XTC_THREAD_LOCAL struct __san_sched_tls __san_sched_tls_v;
/* Reached through a non-inlinable accessor so every access names the
 * CURRENT thread's slot: a migratable fiber resumes from the jump on
 * another OS thread and clang keeps the TLS address live across it
 * (xtc_await/__coro_entry then touched the OLD thread's bounds).  Same
 * fix as coro_uctx.c. */
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

/* About to jump onto `to_bottom`/`to_size`; save the outgoing coro's
 * fake stack into *save_slot (NULL if it will never resume, so the fake
 * stack is discarded). */
static inline void
__san_switch_to(void **save_slot, const void *to_bottom, size_t to_size)
{
	__sanitizer_start_switch_fiber(save_slot, to_bottom, to_size);
}

/* Arrived on a new stack; restore this side's fake stack and (first
 * time) learn the scheduler stack bounds for the return trip. */
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
#else
#  define __san_switch_to(save_slot, to_bottom, to_size) ((void)0)
#  define __san_switch_done(saved)                        ((void)0)
#endif

/* TSan fiber-identity switch helpers.  Placed alongside every
 * __san_switch_to; __tsan_switch_into(c) targets the coro's fiber,
 * __tsan_switch_out() targets the (once-captured) scheduler fiber.
 * No-ops unless built with clang -fsanitize=thread. */
#if defined(XTC_TSAN_FIBERS)
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

/* A coro's usable stack low address, for the sanitizer bottom arg.  The
 * mapping is [stack, stack+guard+stack_sz); the usable region begins one
 * guard page in.  Kept simple: pass the coro's own stack base + a page. */
#if defined(XTC_FIBER_SWITCH_ANNOTATE)
static const void *
__san_coro_bottom(const struct xtc_coro *c)
{
	long pg = sysconf(_SC_PAGESIZE);
	size_t guard = pg > 0 ? (size_t)pg : 4096u;
	return (const void *)((uintptr_t)c->stack + guard);
}
#endif

/* The fcontext primitives (src/os/asm/fctx_x86_64_sysv.S). */
extern void *__xtc_make_fcontext(void *stack_top, size_t size,
                                 void (*fn)(void *transfer));
extern void *__xtc_jump_fcontext(void **from, void *to, void *transfer);

/* Per-thread cursor: the coroutine currently executing on this thread. */
XTC_THREAD_LOCAL struct xtc_coro *__xtc_current_coro = NULL;

/* Per-thread scheduler resume point.  Set by __xtc_coro_step's jump
 * into a coroutine; the coroutine jumps back here to yield/await/finish. */
static XTC_THREAD_LOCAL void *g_sched_fctx_tls = NULL;
/*
 * Read through a non-inlinable accessor so a fiber always jumps back to
 * the scheduler of the thread it is running on NOW.  A migratable fiber
 * can finish (or re-park, in xtc_await's loop) on another OS thread than
 * the one it started on, and on aarch64 the compiler keeps the thread
 * pointer (tpidr_el0) in a callee-saved register across calls:
 * __coro_entry read it at entry and, after the fiber body returned on
 * another thread, jumped to the first thread's scheduler context
 * (test_exec /Blk1../Blk8 and test_steal /M9 SIGSEGV on aarch64 fctx).
 * Same hazard and fix as g_in_preempt (coro_preempt.h) and
 * __current_proc (src/ptc/proc.c).
 */
__attribute__((noinline))
static void **
__sched_fctx_slot(void)
{
	void **volatile slot = &g_sched_fctx_tls;
	return slot;
}
#define g_sched_fctx (*__sched_fctx_slot())

/* Fiber-stack pool + S1 stack-reclaim lever: shared with coro_uctx.c
 * (both mmap their stacks).  See src/inc/coro_common.h. */
#include "coro_common.h"

/* The in-switch guard (g_in_preempt), the x86-64 vector-state probe and
 * the x86-64 signal-context redirect, shared with coro_uctx.c.  Every
 * __xtc_jump_fcontext below is guarded exactly as coro_uctx.c guards its
 * swapcontexts: the redirect declines while a switch is saving or
 * restoring a context, or the scheduler is between its cursor update
 * and the jump.  See coro_preempt.h. */
#include "coro_preempt.h"

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
	if (bytes % (size_t)pg != 0)
		bytes += (size_t)pg - (bytes % (size_t)pg);
	__xtc_stack_size = bytes;
	return XTC_OK;
}

/*
 * Fiber entry trampoline.  Reached by the first jump_fcontext into a
 * freshly make_fcontext'd coroutine, with `transfer` carrying the
 * coro pointer the scheduler passed.  Runs the user fn to completion,
 * marks done, then jumps back to the scheduler and never returns.
 */
static void
__coro_entry(void *transfer)
{
	struct xtc_coro *c = (struct xtc_coro *)transfer;
	__san_switch_done(c->san_fake_stack);   /* arrived on the coro stack */
	__xtc_current_coro = c;
	/* A new fiber is not a proc until __proc_entry says so: clear the
	 * proc TLS the previous fiber on this thread left behind, or a bare
	 * xtc_async coroutine runs (and yields) as that proc -- xtc_self()
	 * names it, xtc_recv steals its mail, and once it is freed every
	 * yield's kill check reads freed memory (review156 B1). */
	if (__xtc_fiber_ctx_restore) __xtc_fiber_ctx_restore(NULL);
	/* Receiver-side clear: __xtc_coro_step raised the guard before the
	 * jump that landed here; the fiber body may now be preempted. */
	g_in_preempt = 0;
	c->result = c->fn(c->arg);
	c->done = 1;
	/* Final jump back to the scheduler.  We will not be resumed, so the
	 * outgoing fake stack is discarded (save slot NULL) and the
	 * destination is the scheduler stack. */
	g_in_preempt = 1;
	__san_switch_to(NULL, __san_sched_bottom, __san_sched_size);
	__tsan_switch_out();
	(void)__xtc_jump_fcontext(&c->fctx, g_sched_fctx, NULL);
}

/*
 * The signals a fiber must be able to take wherever it runs, unblocked
 * once per scheduler thread (first step).  ucontext carried them in
 * every fiber's uc_sigmask (coro_uctx.c, __xtc_async_ex), restored on
 * each swapcontext; fcontext touches no mask, so a fiber runs with its
 * thread's.  Runtime threads start with EVERY signal blocked
 * (__os_pthread_create_masked), and a fault raised while blocked is
 * force-delivered with the default action: a contained fault (the R1
 * guard, xtc_fault_guard_install) on an xtc_exec worker killed the
 * process instead of the proc.  The set is exactly the ucontext fiber
 * mask's exemptions: the synchronous faults, SIGABRT, and the
 * preemption signal.  Process-directed signals stay blocked.
 */
static XTC_THREAD_LOCAL int g_sync_sigs_open;

static void
__coro_open_sync_signals(void)
{
	sigset_t s;
	g_sync_sigs_open = 1;
	(void)sigemptyset(&s);
	(void)sigaddset(&s, SIGSEGV);
	(void)sigaddset(&s, SIGBUS);
	(void)sigaddset(&s, SIGFPE);
	(void)sigaddset(&s, SIGILL);
	(void)sigaddset(&s, SIGABRT);
	(void)sigaddset(&s, SIGVTALRM);
	(void)pthread_sigmask(SIG_UNBLOCK, &s, NULL);
}

/*
 * The task fn the L2 scheduler calls.  Jumps into the fiber; on return
 * the fiber has yielded, awaited, or finished, and c->fctx holds its
 * new resume point (saved by the fiber's own jump back).
 */
int
__xtc_coro_step(xtc_task_t *self, void *user)
{
	struct xtc_coro *c = user;
	struct xtc_coro *saved;

	c->self = self;   /* bind on the running thread; see coro_uctx.c */
	if (XTC_UNLIKELY(!g_sync_sigs_open))
		__coro_open_sync_signals();

	/* Guard from here: once __xtc_current_coro == c a tick would take
	 * the scheduler's own execution for fiber c.  The fiber clears the
	 * guard when it lands (__coro_entry, or after its own jump). */
	g_in_preempt = 1;
	saved = __xtc_current_coro;
	__xtc_current_coro = c;
	XTC_INJECTION_POINT("coro.switch_window");   /* see xtc_yield */

	/* Save OUR (scheduler) fake stack into the per-thread slot -- NOT
	 * into c->san_fake_stack, which the coro overwrites when it parks;
	 * mixing the two crashes finish().  Jump into the coro. */
	__san_switch_to(&__san_sched_fake, __san_coro_bottom(c), c->stack_sz);
	__tsan_switch_into(c);
	(void)__xtc_jump_fcontext(&g_sched_fctx, c->fctx, c);
	__san_switch_done(__san_sched_fake);   /* back on the scheduler stack */

	/* Restore the cursor BEFORE dropping the guard (see coro_uctx.c). */
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
		 * was never handed out; an xtc_async task's was, so that one
		 * stays until xtc_loop_fini.  c->proc is set by __proc_entry
		 * before the body runs and never cleared. */
		if (c->proc != NULL)
			self->reclaim = 1;
		return XTC_TASK_DONE;
	}
	/* Awaiting another coroutine: stay parked; the awaitee fires our
	 * waker when it completes. */
	if (c->_parked_on != NULL) {
		c->_parked_on = NULL;
		return XTC_TASK_PENDING;
	}
	/* Parked on a timer or fd: stay parked until a waker re-enqueues
	 * us, or xtc_recv-with-timeout would busy-spin (see coro_uctx.c). */
	if (c->self != NULL &&
	    (atomic_load_explicit(&c->self->park_timer,
		memory_order_relaxed) != NULL ||
	     atomic_load_explicit(&c->self->park_fd, memory_order_relaxed) >= 0)) {
		return XTC_TASK_PENDING;
	}
	/* Voluntary park requested (xtc_amutex and friends). */
	if (c->self != NULL && c->self->park_requested) {
		c->self->park_requested = 0;
		return XTC_TASK_PENDING;
	}
	/* Plain yield: re-queue at the back of the run queue. */
	return XTC_TASK_RESCHED;
}

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
#if defined(XTC_FIBER_SWITCH_ANNOTATE)
		/* A finished fiber leaves its last frames' redzones poisoned
		 * (it never returned; a kill longjmp skipped more).  Clear them
		 * so the next fiber on this stack starts clean. */
		ASAN_UNPOISON_MEMORY_REGION((char *)c->stack +
		    (total - c->stack_sz), c->stack_sz);
#endif
		/* Recycle the stack (guard page intact) into the per-thread
		 * pool so the next spawn skips mmap+mprotect; munmap only if
		 * the pool is full or the size no longer matches. */
		if (!__stack_pool_put(c->stack, c->stack_sz) &&
		    (c->self == NULL || c->self->loop == NULL ||
		     !__stack_spare_put(c->self->loop, c->stack,
		     c->stack_sz)))
			(void)munmap(c->stack, total);
	}
	__os_free(c);
}

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

	/* Fast path: reuse a pooled stack (guard page already installed),
	 * skipping the mmap + the mmap_lock-serializing mprotect. */
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
		/* First page is the guard (installed once per mapping).  The
		 * mprotect SPLITS the mapping into two VMAs, so it is where
		 * the process map-count limit (Linux vm.max_map_count, 65530
		 * by default: ~32 K fibers) fails, with ENOMEM.  That is
		 * memory exhaustion, reported like the mmap failure above --
		 * not an internal error. */
		if (mprotect(base, guard, PROT_NONE) != 0) {
			int e = errno;
			(void)munmap(base, total);
			__os_free(c);
			return (e == ENOMEM || e == EAGAIN) ? XTC_E_NOMEM
			    : XTC_E_INTERNAL;
		}
	}
	c->stack = base;
	c->fn = fn;
	c->arg = arg;
	c->done = 0;
	c->waiter = NULL;
#if defined(XTC_TSAN_FIBERS)
	c->tsan_fiber = __tsan_create_fiber(0);
#endif

	/* The usable stack is [base+guard, base+guard+stack_sz); stacks
	 * grow down, so make_fcontext takes the HIGH end as stack_top. */
	c->fctx = __xtc_make_fcontext(base + guard + c->stack_sz,
	    c->stack_sz, __coro_entry);
	if (c->fctx == NULL) {
		(void)munmap(base, total);
		__os_free(c);
		return XTC_E_INTERNAL;
	}

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
 * The public entry: spawns a PINNED coroutine (its underlying task is
 * not work-stealable), preserving the long-standing default.  Internal
 * callers that want a migratable (stealable) coro -- notably the proc
 * layer when xtc_proc_opts_t.migratable is set -- use __xtc_async_ex
 * with pinned=0.
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

	if (target->done) {
		if (result) *result = target->result;
		return XTC_OK;
	}

	me = __xtc_current_coro;

	if (me == NULL) {
		/* Awaiting from outside any coroutine: drive the loop. */
		while (!target->done) {
			int rc = xtc_loop_run(t->loop);
			if (rc != XTC_OK) return rc;
		}
		if (result) *result = target->result;
		return XTC_OK;
	}

	/* Inside a coroutine: register as the target's waiter, set the
	 * "stay parked" hint, and jump back to the scheduler. */
	/* Re-park until the target is done: any other wake (a reclaimed
	 * task's late waker landing on this reused task, a kill wake) is
	 * spurious here.  See coro_uctx.c. */
	while (!target->done) {
		void *pctx;
		target->waiter = me->self;
		me->self->q_next = NULL;
		me->_parked_on = target;
		g_in_preempt = 1;
		pctx = __xtc_fiber_ctx_save ? __xtc_fiber_ctx_save() : NULL;
		__san_switch_to(&me->san_fake_stack, __san_sched_bottom,
		    __san_sched_size);
		__tsan_switch_out();
		(void)__xtc_jump_fcontext(&me->fctx, g_sched_fctx, NULL);
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
	volatile char probe;     /* address approximates the current SP */
	if (c == NULL) return;
	/* S1: return the unused stack tail below our SP to the OS before we
	 * park.  &probe is a live local, so the reclaim never crosses it. */
	coro_stack_shrink(c, (void *)&probe);
	/* Preserve the process layer's per-fiber TLS across the yield:
	 * the scheduler runs other fibers (which overwrite it) before we
	 * resume.  Without this a proc resumes running as whatever proc
	 * ran last, then registers I/O and parks the WRONG task. */
	g_in_preempt = 1;   /* the whole save/jump/restore; see coro_uctx.c */
	/* Test hook inside the guarded window (one relaxed load when nothing
	 * is attached): test_preempt_p2 /switch_window_declines raises the
	 * preemption signal here and on the scheduler side below. */
	XTC_INJECTION_POINT("coro.switch_window");
	pctx = __xtc_fiber_ctx_save ? __xtc_fiber_ctx_save() : NULL;
	__san_switch_to(&c->san_fake_stack, __san_sched_bottom, __san_sched_size);
	__tsan_switch_out();
	(void)__xtc_jump_fcontext(&c->fctx, g_sched_fctx, NULL);
	__san_switch_done(c->san_fake_stack);   /* resumed on our stack */
	if (__xtc_fiber_ctx_restore) __xtc_fiber_ctx_restore(pctx);
	g_in_preempt = 0;
	/* Universal resume point: honor a kill/cancel requested while we
	 * were away (e.g. an involuntary preemption of a pure CPU loop that
	 * never reaches a cooperative kill-check).  xtc_launch's cancel
	 * lands here for a runaway. */
	if (__xtc_fiber_kill_check) __xtc_fiber_kill_check();
}

/* PUBLIC: int __xtc_coro_preempt __P((void *)); */
/*
 * Signal-context involuntary yield (M_PREEMPTION Phase 2b), fcontext
 * substrate.  On x86-64 Linux (glibc) this is the SAME redirect the
 * ucontext substrate uses: the handler rewrites the interrupted RIP/RSP
 * so sigreturn resumes the fiber, on its own stack, in
 * __xtc_preempt_trampoline, which saves the interrupted register file
 * and vector state, calls xtc_yield() -- here a guarded jump_fcontext --
 * and returns to the interrupted instruction.  Nothing in that path
 * depends on how the scheduler switch saves its context.  It declines
 * (returns 0) while a switch is in flight (g_in_preempt), inside the
 * trampoline, and off a fiber.  On aarch64 Linux (glibc) it is likewise
 * the shared redirect (rt_sigreturn on the kernel's in-place signal
 * frame).  Elsewhere (musl, macOS) it declines, and the timer falls
 * back to Phase 1 cooperative-assisted preemption.
 */
int
__xtc_coro_preempt(void *uctx)
{
#if defined(XTC_CORO_REDIRECT_X86_64)
	return __coro_preempt_redirect_x86_64(uctx);
#elif defined(XTC_CORO_REDIRECT_AARCH64)
	return __coro_preempt_redirect_aarch64(uctx);
#else
	(void)uctx;
	return 0;
#endif
}

/* PUBLIC: int __xtc_coro_preempt_effective __P((void)); */
/* 1 iff __xtc_coro_preempt above can redirect (tracks its #if). */
int
__xtc_coro_preempt_effective(void)
{
#if defined(XTC_CORO_REDIRECT_X86_64) || defined(XTC_CORO_REDIRECT_AARCH64)
	return 1;
#else
	return 0;
#endif
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


#endif /* substrate active */
