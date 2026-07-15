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

#include "xtc_int.h"

/*
 * Apple Silicon default: prefer the hand-written fcontext substrate.
 * macOS ships ucontext (so XTC_HAVE_UCONTEXT is defined), but its
 * swapcontext does a sigprocmask syscall per switch -- microseconds vs
 * the low tens of ns of fcontext.  On arm64 we have the Mach-O
 * make/jump_fcontext (src/os/asm/fctx_aarch64_aapcs_macho.S), so make
 * fcontext the DEFAULT there (equivalent to XTC_CORO_FORCE_FCTX).
 * Apple x86-64 keeps ucontext (no Mach-O x86-64 fcontext .S).
 */
#if defined(__APPLE__) && defined(__aarch64__) && \
    !defined(XTC_CORO_FORCE_UCONTEXT) && !defined(XTC_CORO_FORCE_FCTX)
#  define XTC_CORO_FORCE_FCTX 1
#endif

#if !defined(_WIN32) && \
    (!defined(XTC_HAVE_UCONTEXT) || defined(XTC_CORO_FORCE_FCTX))

#include "xtc_async.h"
#include "loop_int.h"
#include "coro_int.h"

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
static XTC_THREAD_LOCAL const void *__san_sched_bottom;
static XTC_THREAD_LOCAL size_t      __san_sched_size;
static XTC_THREAD_LOCAL void       *__san_sched_fake;   /* scheduler's own
                                                         * fake-stack token */

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
static XTC_THREAD_LOCAL void *g_sched_fctx = NULL;

/* Fiber-stack pool + S1 stack-reclaim lever: shared with coro_uctx.c
 * (both mmap their stacks).  See src/inc/coro_common.h. */
#include "coro_common.h"

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
	c->result = c->fn(c->arg);
	c->done = 1;
	/* Final jump back to the scheduler.  We will not be resumed, so the
	 * outgoing fake stack is discarded (save slot NULL) and the
	 * destination is the scheduler stack. */
	__san_switch_to(NULL, __san_sched_bottom, __san_sched_size);
	__tsan_switch_out();
	(void)__xtc_jump_fcontext(&c->fctx, g_sched_fctx, NULL);
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

	saved = __xtc_current_coro;
	__xtc_current_coro = c;

	/* Save OUR (scheduler) fake stack into the per-thread slot -- NOT
	 * into c->san_fake_stack, which the coro overwrites when it parks;
	 * mixing the two crashes finish().  Jump into the coro. */
	__san_switch_to(&__san_sched_fake, __san_coro_bottom(c), c->stack_sz);
	__tsan_switch_into(c);
	(void)__xtc_jump_fcontext(&g_sched_fctx, c->fctx, c);
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
	/* Awaiting another coroutine: stay parked; the awaitee fires our
	 * waker when it completes. */
	if (c->_parked_on != NULL) {
		c->_parked_on = NULL;
		return XTC_TASK_PENDING;
	}
	/* Parked on a timer or fd: stay parked until a waker re-enqueues
	 * us, or xtc_recv-with-timeout would busy-spin (see coro_uctx.c). */
	if (c->self != NULL &&
	    (c->self->park_timer != NULL || c->self->park_fd >= 0)) {
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
		/* Recycle the stack (guard page intact) into the per-thread
		 * pool so the next spawn skips mmap+mprotect; munmap only if
		 * the pool is full or the size no longer matches. */
		if (!__stack_pool_put(c->stack, c->stack_sz))
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

	/* Fast path: reuse a pooled stack (guard page already installed),
	 * skipping the mmap + the mmap_lock-serializing mprotect. */
	base = __stack_pool_get(c->stack_sz);
	if (base == NULL) {
		base = mmap(NULL, total, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (base == MAP_FAILED) {
			__os_free(c);
			return XTC_E_NOMEM;
		}
		/* First page is the guard (installed once per mapping). */
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

	if ((rc = __xtc_task_spawn_ex(loop, __xtc_coro_step, c, 1, &t)) != XTC_OK) {
		__coro_destroy(c);
		return rc;
	}
	c->self = t;
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
	target->waiter = me->self;
	me->self->q_next = NULL;
	me->_parked_on = target;
	{
		void *pctx = __xtc_fiber_ctx_save ? __xtc_fiber_ctx_save() : NULL;
		__san_switch_to(&me->san_fake_stack, __san_sched_bottom,
		    __san_sched_size);
		__tsan_switch_out();
		(void)__xtc_jump_fcontext(&me->fctx, g_sched_fctx, NULL);
		__san_switch_done(me->san_fake_stack);   /* resumed on our stack */
		if (__xtc_fiber_ctx_restore) __xtc_fiber_ctx_restore(pctx);
	}
	/* Resumed: target->done is now true. */
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
	pctx = __xtc_fiber_ctx_save ? __xtc_fiber_ctx_save() : NULL;
	__san_switch_to(&c->san_fake_stack, __san_sched_bottom, __san_sched_size);
	__tsan_switch_out();
	(void)__xtc_jump_fcontext(&c->fctx, g_sched_fctx, NULL);
	__san_switch_done(c->san_fake_stack);   /* resumed on our stack */
	if (__xtc_fiber_ctx_restore) __xtc_fiber_ctx_restore(pctx);
	/* Universal resume point: honor a kill/cancel requested while we
	 * were away (e.g. an involuntary preemption of a pure CPU loop that
	 * never reaches a cooperative kill-check).  xtc_launch's cancel
	 * lands here for a runaway. */
	if (__xtc_fiber_kill_check) __xtc_fiber_kill_check();
}

/* PUBLIC: int __xtc_coro_preempt __P((void *)); */
/*
 * Signal-context involuntary yield (M_PREEMPTION Phase 2b).  The fctx
 * substrate CANNOT do a resumable in-handler stack swap portably (its
 * saved context is a raw stack pointer, not a kernel-restorable
 * ucontext, so redirecting sigreturn is arch-specific asm), so it
 * declines: the preemption timer falls back to Phase 1 cooperative-
 * assisted preemption on this substrate (notably musl).  The ucontext
 * substrate provides the real resumable version.
 */
int
__xtc_coro_preempt(void *uctx)
{
	(void)uctx;
	return 0;
}

/* PUBLIC: int __xtc_coro_preempt_effective __P((void)); */
/* The fctx substrate always declines the involuntary redirect (see
 * above), so involuntary preemption is never effective here. */
int
__xtc_coro_preempt_effective(void)
{
	return 0;
}

xtc_task_t *
__xtc_current_task(void)
{
	return __xtc_current_coro != NULL ? __xtc_current_coro->self : NULL;
}

#endif /* substrate active */
