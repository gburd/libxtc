/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/evt/task.c
 *	Task spawn / waker / park-on-{timer,fd}.
 *	See M3_CLAIMS.md, Ts1-Ts6, Wk1-Wk4, Io1-Io3.
 */

#include "xtc_int.h"
#include "xtc_tail.h"     /* SCHED: dispatch (WAKE) hook */
#include "tail_int.h"     /* __xtc_tail_emit / __xtc_tail_on */
#include "xtc_probes.h"
#include "loop_int.h"
#include "preempt_int.h"  /* __xtc_unsafe_enter/leave around the spare lock */

#include <stdint.h>
#include <string.h>
#include <unistd.h>

/*
 * Per-loop task-struct free-list -- a plain single-threaded LIFO stack
 * of recycled task structs, threaded through the task's own q_next
 * field while it sits on the free list.  A completed PLAIN task on its
 * home loop is pushed here instead of being freed; the next spawn on
 * that loop pops it instead of calling malloc.  malloc is only hit to
 * grow the pool.  This is a deliberate rejection of xtc_slab for this
 * path: xtc_slab is a shm-capable, chunked, magazine + mutex allocator
 * (measured ~25x slower than malloc for the task pattern); a bare
 * per-loop free-list, touched only by the owning loop thread, is
 * lock-free and beats malloc on the recycle-heavy hot path.
 *
 * Ownership: only the HOME loop's thread pushes/pops its free list
 * (the DONE-handler recycle is gated on t->loop == running loop), so
 * no synchronization is needed.  A task that finishes on a thief is
 * handed back to its home loop (__xtc_task_reclaim).  Beyond a small
 * local cap the home loop pushes onto a locked spare list that a spawn
 * from another thread can pop (below).  Both are drained (structs
 * __os_free'd) at loop_fini.
 */

/* The free list a struct of the given pinned-ness lives on.  Pinned and
 * migratable structs are kept apart so a stale waker left by a migratable
 * incarnation (which may name a thief loop) can never re-queue a pinned
 * task on the wrong loop; see __xtc_task_reclaim. */
static xtc_task_t **
__task_freelist(xtc_loop_t *loop, int pinned)
{
	return pinned ? &loop->task_free : &loop->task_free_mig;
}

/*
 * Cross-thread spares.  A spawn onto a loop from ANOTHER thread (the
 * round-robin acceptor: accept on loop A, spawn the connection proc on
 * loop B) cannot pop B's owner-only free list, and used to malloc a fresh
 * struct every time while B recycled every finished one onto that list:
 * retention grew by one struct per spawn ever made.  So the owner keeps
 * at most XTC_TASK_FREELIST_LOCAL structs on its own lists and pushes the
 * rest onto task_spare, which any thread pops under a spinlock (held for
 * a few pointer moves; never across a yield, an allocation or another
 * lock; preemption deferred).  Recycled structs still never go back to
 * the allocator before xtc_loop_fini (type-stable, see below), and the
 * structs of each pinned-ness belonging to a loop stay within its peak
 * live count + XTC_TASK_FREELIST_LOCAL + 1: a home spawn mallocs only
 * when its free list and spare are both empty, a foreign one only when
 * the spare is empty (checked under the lock) and the free list holds at
 * most the local cap.
 */
#ifndef XTC_TASK_FREELIST_LOCAL
#define XTC_TASK_FREELIST_LOCAL 64
#endif

static void
__task_spare_lock(xtc_loop_t *loop)
{
	__xtc_unsafe_enter();
	while (atomic_exchange_explicit(&loop->task_spare_lock, 1,
	    memory_order_acquire))
		while (atomic_load_explicit(&loop->task_spare_lock,
		    memory_order_relaxed))
			__os_pause();
}

static void
__task_spare_unlock(xtc_loop_t *loop)
{
	atomic_store_explicit(&loop->task_spare_lock, 0, memory_order_release);
	__xtc_unsafe_leave();
}

/* Push a struct onto loop's spare for its pinned-ness.  Any thread. */
static void
__task_spare_push(xtc_loop_t *loop, xtc_task_t *t)
{
	int k = t->pinned ? 1 : 0;
	__task_spare_lock(loop);
	t->q_next = loop->task_spare[k];
	loop->task_spare[k] = t;
	atomic_store_explicit(&loop->task_spare_n,
	    atomic_load_explicit(&loop->task_spare_n, memory_order_relaxed) + 1,
	    memory_order_relaxed);
	__task_spare_unlock(loop);
}

/* Pop a struct of the given pinned-ness from loop's spare, or NULL. */
static xtc_task_t *
__task_spare_pop(xtc_loop_t *loop, int pinned)
{
	int k = pinned ? 1 : 0;
	xtc_task_t *t;
	__task_spare_lock(loop);
	if ((t = loop->task_spare[k]) != NULL) {
		loop->task_spare[k] = t->q_next;
		atomic_store_explicit(&loop->task_spare_n,
		    atomic_load_explicit(&loop->task_spare_n,
		    memory_order_relaxed) - 1, memory_order_relaxed);
	}
	__task_spare_unlock(loop);
	return t;
}

/* Recycled structs held by loop (free lists + spare); for the plain cap. */
static int
__task_retained(xtc_loop_t *loop)
{
	return loop->task_free_n +
	    atomic_load_explicit(&loop->task_spare_n, memory_order_relaxed);
}

/* Push a completed task struct onto its loop's free list, or its spare
 * once the free list is at the local cap.  Home loop thread only. */
static void
__task_freelist_push(xtc_loop_t *loop, xtc_task_t *t)
{
	xtc_task_t **head;
	if (loop->task_free_n >= XTC_TASK_FREELIST_LOCAL) {
		__task_spare_push(loop, t);
		return;
	}
	head = __task_freelist(loop, t->pinned);
	t->q_next = *head;   /* reuse q_next as free-list link */
	*head = t;
	loop->task_free_n++;
}

/* Pop a recycled task struct for a spawn onto loop, or NULL.  The home
 * thread takes its own free list first; any thread may take a spare. */
static xtc_task_t *
__task_freelist_pop(xtc_loop_t *loop, int pinned)
{
	xtc_task_t **head;
	xtc_task_t *t = NULL;
	/* A spawning fiber preempted between the load and the head store
	 * would let a same-loop spawn pop the same struct. */
	__xtc_unsafe_enter();
	if (__xtc_current_loop == loop) {    /* home thread owns the list */
		head = __task_freelist(loop, pinned);
		if ((t = *head) != NULL) {
			*head = t->q_next;
			loop->task_free_n--;
		}
	}
	__xtc_unsafe_leave();
	return t != NULL ? t : __task_spare_pop(loop, pinned);
}

/* xtc_loop_fini: move the spares onto the free lists, which fini frees.
 * Declared in loop_int.h. */
void
__xtc_task_spare_fini(xtc_loop_t *loop)
{
	xtc_task_t *t;
	int k;
	for (k = 0; k < 2; k++)
		while ((t = loop->task_spare[k]) != NULL) {
			loop->task_spare[k] = t->q_next;
			t->q_next = *__task_freelist(loop, k);
			*__task_freelist(loop, k) = t;
		}
	atomic_store_explicit(&loop->task_spare_n, 0, memory_order_relaxed);
}

/* exec.c: count a live unpinned task (its loop's 0 -> 1 wakes sleepers). */
extern void __xtc_exec_mig_alive(xtc_loop_t *l, int delta);

/* Allocate a zeroed task struct: reuse a recycled one from the loop's
 * free list or spare if available, else malloc.  recyclable marks a free-
 * list-eligible struct (recycle on completion). */
static int
__task_alloc(xtc_loop_t *loop, int pinned, xtc_task_t **out)
{
	xtc_task_t *t;
	int rc;
	t = __task_freelist_pop(loop, pinned);
	if (t != NULL) {
		int typestable = t->typestable;
		memset(t, 0, sizeof *t);
		t->recyclable = 1;
		t->typestable = typestable;
		*out = t;
		return XTC_OK;
	}
	if ((rc = __os_calloc(1, sizeof(*t), (void **)&t)) != XTC_OK)
		return rc;
	t->recyclable = 1;   /* freelist-eligible on completion */
	*out = t;
	return XTC_OK;
}

/* A cap so a burst of simultaneously-live tasks that all complete does
 * not pin unbounded memory on the free list; beyond it, recycled
 * structs are returned to the OS.  Overridable at build time (set to 0
 * to disable recycling entirely, e.g. for an A/B measurement). */
#ifndef XTC_TASK_FREELIST_MAX
#define XTC_TASK_FREELIST_MAX 4096
#endif

/*
 * PUBLIC: void __xtc_task_free __P((xtc_task_t *));
 *
 * Reclaim a completed task: unlink it O(1) from its home loop's
 * all_tasks list and push it onto the loop's task free list for reuse
 * (or __os_free if the list is full or the struct was a bare alloc).
 * Called from the loop's DONE handler for plain tasks (cleanup ==
 * NULL) completing ON THEIR HOME LOOP -- so the free-list push and the
 * all_tasks unlink are single-threaded (the home loop's thread).
 */
void
__xtc_task_free(xtc_task_t *t)
{
	xtc_loop_t *loop;
	if (t == NULL)
		return;
	loop = t->loop;
	/* Unlink from all_tasks (doubly linked -> O(1)). */
	if (t->all_prev != NULL)
		t->all_prev->all_next = t->all_next;
	else if (loop != NULL && loop->all_tasks == t)
		loop->all_tasks = t->all_next;
	if (t->all_next != NULL)
		t->all_next->all_prev = t->all_prev;
	if (loop != NULL && (t->typestable || (t->recyclable &&
	    __task_retained(loop) < XTC_TASK_FREELIST_MAX)))
		__task_freelist_push(loop, t);
	else
		__os_free(t);
}

/* --- lifetime (fix155): reclaim a finished proc fiber at DONE ---------
 *
 * A proc's coro task used to stay on its loop's all_tasks, with its fiber
 * stack (two mappings: stack + guard) and coro struct, until
 * xtc_loop_fini.  A server spawning one proc per connection therefore
 * grew by ~6 KB RSS and two VMAs per connection EVER served, and on a
 * stock kernel (vm.max_map_count 65530) every spawn failed after ~32K.
 *
 * The coro step marks a finished proc fiber (t->reclaim; a proc's task
 * handle is never handed out, unlike xtc_async's out_task) and the loop's
 * DONE handler calls __xtc_task_reclaim on the scheduler stack, after the
 * switch back, so the dead fiber stack is no longer executing:
 *
 *   - its fiber stack, coro and any leftover park timer / fd registration
 *     are released at once, on whichever loop it finished (all
 *     thread-agnostic; the stack goes to this thread's bounded stack pool
 *     or is unmapped);
 *   - the task STRUCT goes back to its home loop: all_tasks and the free
 *     lists are owned by the home loop's thread, so a task that finished
 *     on a thief is handed back through the home loop's lock-free dead
 *     stack and recycled when the home loop next steps (or at fini).
 *
 * Wakers hold a raw task pointer (xtc_waker_t is a frozen public
 * {loop, task} pair, so there is no room for a generation), and a few
 * can fire after the task finished (a wake racing a migrated fiber's
 * exit).  So a reclaimed struct is never returned to the allocator while
 * its loop lives: it stays on the loop's free list (bounded by the
 * high-water count of live tasks) and is freed at xtc_loop_fini.  A late
 * waker then lands on a valid task struct -- a no-op on a DONE task, at
 * worst one spurious wake of the next incarnation, which every park site
 * already tolerates (xtc_await re-parks until its target is done).  The
 * free lists are split by pinned-ness so that spurious wake can never
 * move a pinned task to a thief loop.
 */

/* Release what a finished task holds besides its struct.  Any thread. */
static void
__task_release_parts(xtc_task_t *t)
{
	extern int __xtc_io_defer_del_fd(xtc_io_t *, int);
	xtc_io_t *io;
	int fd;

	/* A proc parks only through sites that disarm on resume, so these
	 * are normally clear; release them anyway so a recycled struct never
	 * inherits a timer or an fd tag naming it. */
	__xtc_task_cancel_park_timer(t);
	fd = atomic_exchange_explicit(&t->park_fd, -1, memory_order_acq_rel);
	io = t->park_io;
	t->park_io = NULL;
	if (fd >= 0)
		(void)__xtc_io_defer_del_fd(io != NULL ? io : t->loop->io, fd);
	if (t->cleanup != NULL) {
		t->cleanup(t->cleanup_arg);   /* fiber stack + coro */
		t->cleanup = NULL;
		t->cleanup_arg = NULL;
	}
}

/* Unlink t from its home loop's all_tasks and keep the struct on the
 * loop's free list.  Home loop thread only. */
static void
__task_recycle_home(xtc_loop_t *loop, xtc_task_t *t)
{
	if (t->all_prev != NULL)
		t->all_prev->all_next = t->all_next;
	else if (loop->all_tasks == t)
		loop->all_tasks = t->all_next;
	if (t->all_next != NULL)
		t->all_next->all_prev = t->all_prev;
	t->all_next = t->all_prev = NULL;
	t->typestable = 1;
	__task_freelist_push(loop, t);
}

/*
 * Reclaim a finished task marked t->reclaim.  Called by `loop`'s DONE
 * handler on the scheduler stack; `loop` is the loop it finished on.
 */
void
__xtc_task_reclaim(xtc_loop_t *loop, xtc_task_t *t)
{
	xtc_loop_t *home = t->loop;
	xtc_task_t *old;

	__task_release_parts(t);
	if (home == loop) {
		__task_recycle_home(loop, t);
		return;
	}
	/* Finished on a thief: hand the struct back to its home loop. */
	old = atomic_load_explicit(&home->dead_head, memory_order_relaxed);
	do {
		t->dead_next = old;
	} while (!atomic_compare_exchange_weak_explicit(&home->dead_head,
	    &old, t, memory_order_release, memory_order_relaxed));
}

/* Recycle the tasks other loops handed back.  Owner thread only; called
 * at the top of every loop step and by xtc_loop_fini. */
void
__xtc_task_reclaim_drain(xtc_loop_t *loop)
{
	xtc_task_t *t, *n;

	if (atomic_load_explicit(&loop->dead_head,
	    memory_order_relaxed) == NULL)
		return;
	t = atomic_exchange_explicit(&loop->dead_head, NULL,
	    memory_order_acquire);
	for (; t != NULL; t = n) {
		n = t->dead_next;
		t->dead_next = NULL;
		__task_recycle_home(loop, t);
	}
}

/*
 * PUBLIC: int xtc_task_spawn __P((xtc_loop_t *, xtc_task_fn, void *, xtc_task_t **));
 */
int
__xtc_task_spawn_ex(xtc_loop_t *loop, xtc_task_fn fn, void *user,
                    int pinned, xtc_task_t **out_task)
{
	return __xtc_task_spawn_cleanup(loop, fn, user, pinned, NULL, NULL,
	    out_task);
}

/*
 * __xtc_task_spawn_ex plus a fini-time cleanup hook.  The hook is stored
 * BEFORE the task is enqueued or published to another loop's inbox, so
 * the loop that runs it (possibly on another thread, possibly to DONE)
 * never sees a half-initialized task.  xtc_async passes its coro teardown
 * here; storing it after the spawn returned was a TSan-reported race.
 */
int
__xtc_task_spawn_cleanup(xtc_loop_t *loop, xtc_task_fn fn, void *user,
    int pinned, void (*cleanup)(void *), void *cleanup_arg,
    xtc_task_t **out_task)
{
	xtc_task_t *t;
	int rc;

	if (loop == NULL || fn == NULL)
		return XTC_E_INVAL;

	/* Charge the task slot before allocating.  If the cap is hit we
	 * return XTC_E_RESOURCE without ever touching malloc. */
	if (loop->res != NULL) {
		if ((rc = xtc_res_acquire(loop->res, XTC_RES_TASKS, 1)) != XTC_OK)
			return rc;
	}

	if ((rc = __task_alloc(loop, pinned, &t)) != XTC_OK) {
		if (loop->res != NULL)
			xtc_res_release(loop->res, XTC_RES_TASKS, 1);
		return rc;
	}
	t->fn = fn;
	t->user = user;
	t->loop = loop;
	t->pinned = pinned;
	t->sched_class = -1;   /* default (implicit) FIFO class */
	atomic_store_explicit(&t->state, XTC_TS_SCHEDULED,
	    memory_order_relaxed);   /* init, before the task is published */
	t->q_next = NULL;
	atomic_store_explicit(&t->park_timer, NULL,
	    memory_order_relaxed);
	t->park_fd = -1;
	atomic_store_explicit(&t->wake_revents, 0, memory_order_relaxed);
	t->cleanup = cleanup;
	t->cleanup_arg = cleanup_arg;

	if (out_task) *out_task = t;

	/* all_tasks and the run queue are single-owner state the scheduler
	 * on this thread also mutates: hold preemption off from the loop
	 * check to the enqueue (see xtc_waker_wake). */
	__xtc_unsafe_enter();
	if (__xtc_current_loop == loop) {
		t->all_next = loop->all_tasks;
		t->all_prev = NULL;
		if (loop->all_tasks != NULL)
			loop->all_tasks->all_prev = t;
		loop->all_tasks = t;
		atomic_fetch_add_explicit(&loop->n_alive, 1,
		    memory_order_relaxed);
		if (!pinned && loop->exec != NULL)
			__xtc_exec_mig_alive(loop, 1);
		(void)__xtc_loop_enqueue(loop, t);
		__xtc_unsafe_leave();
		return XTC_OK;
	}
	__xtc_unsafe_leave();

	/* Cross-thread publish.  Charge an inbox slot too; the drain
	 * releases exactly one per PUBLISH it takes. */
	if (loop->res != NULL) {
		rc = xtc_res_acquire(loop->res, XTC_RES_INBOX_MSGS, 1);
		if (rc != XTC_OK) {
			xtc_res_release(loop->res, XTC_RES_TASKS, 1);
			/* Not on all_tasks, and this is not loop's thread, so
			 * not its free list either: back to the spare (a
			 * recycled struct must stay type-stable). */
			if (t->typestable)
				__task_spare_push(loop, t);
			else
				__os_free(t);
			if (out_task) *out_task = NULL;
			return rc;
		}
	}

	atomic_fetch_add_explicit(&loop->n_alive, 1, memory_order_relaxed);
	if (!pinned && loop->exec != NULL)
		__xtc_exec_mig_alive(loop, 1);
	return __xtc_inbox_push(loop, XTC_INB_PUBLISH, t);
}

int
xtc_task_spawn(xtc_loop_t *loop, xtc_task_fn fn, void *user,
               xtc_task_t **out_task)
{
	/* Public entry: unpinned (stealable in executor mode). */
	return __xtc_task_spawn_ex(loop, fn, user, 0, out_task);
}

/*
 * PUBLIC: int xtc_task_waker __P((xtc_task_t *, xtc_waker_t *));
 */
int
xtc_task_waker(xtc_task_t *task, xtc_waker_t *out)
{
	if (task == NULL || out == NULL)
		return XTC_E_INVAL;
	/*
	 * The waker must name the loop the task is ACTUALLY runnable on
	 * right now, not task->loop (the loop it was spawned/counted on --
	 * an accounting field never updated by a steal; see loop.c's
	 * n_alive/xtc_res bookkeeping, which deliberately keys off the
	 * ORIGINAL loop and must not change).  Every caller of this
	 * function arms a waker for ITSELF (task == the running task), so
	 * __xtc_current_loop -- the thread-local set by whichever loop is
	 * presently stepping this task -- is always the correct answer,
	 * including after a migratable proc's coro has been work-stolen to
	 * a different loop than the one it was spawned on.
	 *
	 * Before this fix, a migrated task's waker still named its STALE
	 * spawn-time loop: xtc_waker_wake would push the wake into that
	 * loop's inbox, which enqueues the task on the WRONG loop (the
	 * inbox dispatcher has no way to know the task moved -- see
	 * __xtc_inbox_drain's XTC_INB_WAKE case) while the task is actually
	 * parked and expected on the loop it migrated to.  The task then
	 * never gets re-run there: a permanently stranded fiber, silent
	 * because xtc_proc_wake treats a resolve/enqueue as fire-and-forget
	 * (a cross-thread wake is a harmless best-effort by design).  Fall
	 * back to task->loop only for the (currently nonexistent, but
	 * defensively supported) case of arming a waker for a task from
	 * off any loop. */
	out->loop = __xtc_current_loop != NULL ? __xtc_current_loop : task->loop;
	out->task = task;
	return XTC_OK;
}

/*
 * Claim a wake of t: returns 1 if the caller moved t PARKED->SCHEDULED
 * and must now queue it, 0 if t was not parked (the wake is latched in
 * wake_pending for the PENDING verdict to consume, or t is already
 * queued / running / done).
 *
 * The latch alone is not enough when the waker runs on a different
 * thread from the task.  The PENDING verdict does exchange(wake_pending)
 * -> store PARKED -> exchange(wake_pending) again (loop.c, the ninth
 * surface), and a waker whose CAS failed while the task was RUNNING can
 * store its latch AFTER that second exchange -- leaving the task PARKED
 * with the latch set and nobody to consume it.  So after latching, look
 * again: if the task is PARKED now, race the verdict for the latch with
 * the same exchange; exactly one side takes it and does the CAS.
 */
static int
__task_wake_claim(xtc_task_t *t)
{
	int expect = XTC_TS_PARKED;
	if (atomic_compare_exchange_strong(&t->state, &expect,
	    XTC_TS_SCHEDULED))
		return 1;
	atomic_store(&t->wake_pending, 1);
	if (atomic_load(&t->state) != XTC_TS_PARKED ||
	    !atomic_exchange(&t->wake_pending, 0))
		return 0;
	expect = XTC_TS_PARKED;
	return atomic_compare_exchange_strong(&t->state, &expect,
	    XTC_TS_SCHEDULED);
}

/*
 * PUBLIC: int xtc_waker_wake __P((const xtc_waker_t *));
 *
 *   - PARKED   -> SCHEDULED + enqueue   (the normal path)
 *   - SCHEDULED-> no-op                 (Wk3 idempotent)
 *   - RUNNING  -> no-op                 (loop will observe verdict)
 *   - DONE     -> no-op                 (Wk4 safe-after-completion)
 *
 * Thread-aware: if the caller is on the owning loop's thread, do the
 * existing fast path.  Otherwise post to the inbox and ping the loop's
 * I/O backend so it wakes from io_poll.
 */
int
xtc_waker_wake(const xtc_waker_t *w)
{
	if (w == NULL || w->loop == NULL || w->task == NULL)
		return XTC_E_INVAL;

	/* The run queue is single-owner state that the scheduler on this
	 * thread also mutates: an involuntary preemption inside the enqueue
	 * lets the RESCHED re-queue of the preempted fiber relink through a
	 * stale q_tail and drop t (test_preempt_p2 /runq_lost_wake).  Defer
	 * preemption from the loop check (a migratable caller must not move
	 * carriers between it and the enqueue) to the end of the enqueue. */
	__xtc_unsafe_enter();
	if (__xtc_current_loop == w->loop) {
		int rc = XTC_OK;
		/* Same-loop wake.  Still claim the PARKED->SCHEDULED transition
		 * atomically: a migratable task can be running on another loop
		 * or targeted by a cross-loop wake concurrently, so a plain
		 * read-check-write would race it.  Only the winner enqueues.
		 *
		 * A lost claim is not automatically "already runnable": the
		 * dangerous case is the PREPARE/PARK WINDOW, where the task is
		 * still RUNNING (it has decided to park and armed its wake
		 * source, but the scheduler has not yet applied the PARKED
		 * verdict).  __task_wake_claim latches wake_pending for it, so
		 * the verdict re-schedules instead of parking a task whose wake
		 * already came and went.  Latching when the task is genuinely
		 * RUNNING-and-not-parking is harmless: the flag is consumed only
		 * by the PENDING verdict, and a stale flag is cleared by the
		 * exchange the next time it parks. */
		if (__task_wake_claim(w->task)) {
			XTC_PROBE_FIBER_WAKE((uintptr_t)w->task,
			    w->loop->exec_id, XTC_PROBE_WAKE_LOCAL);
			rc = __xtc_loop_enqueue(w->loop, w->task);
		} else {
			XTC_PROBE_FIBER_WAKE((uintptr_t)w->task,
			    w->loop->exec_id, XTC_PROBE_WAKE_UNPARKED);
		}
		__xtc_unsafe_leave();
		return rc;
	}
	__xtc_unsafe_leave();

	/* Cross-thread.  Claim the transition here, then hand the task to
	 * its loop's inbox.  Winning the claim is what entitles this waker
	 * to the task's single inbox slot (loop_int.h, inbox_next), so the
	 * push needs no allocation and cannot fail: a wake can no longer be
	 * dropped for ENOMEM after its message is already in the receiver's
	 * mailbox.
	 *
	 * Capture loop and task into locals before the push.  The waker *w
	 * may be embedded in a stack-allocated waiter owned by the parked
	 * task; once the task is in the inbox its loop can resume it on
	 * another thread and it may return, freeing *w.  The loop and its io
	 * are owned by the executor and outlive the task. */
	{
		xtc_loop_t *loop = w->loop;
		xtc_task_t *task = w->task;
		if (__task_wake_claim(task)) {
			XTC_PROBE_FIBER_WAKE((uintptr_t)task, loop->exec_id,
			    XTC_PROBE_WAKE_REMOTE);
			return __xtc_inbox_push(loop, XTC_INB_WAKE, task);
		}
		XTC_PROBE_FIBER_WAKE((uintptr_t)task, loop->exec_id,
		    XTC_PROBE_WAKE_UNPARKED);
		return XTC_OK;
	}
}

/*
 * PUBLIC: int xtc_task_park_on_timer __P((xtc_task_t *, int64_t));
 */
int
xtc_task_park_on_timer(xtc_task_t *self, int64_t delay_ns)
{
	int64_t now_ns;
	int rc;

	if (self == NULL || delay_ns < 0)
		return XTC_E_INVAL;
	if ((rc = __os_clock_mono(&now_ns)) != XTC_OK)
		return rc;
	return __xtc_task_park_on_timer_at(self, now_ns + delay_ns);
}

/*
 * Park on an ABSOLUTE mono deadline.  The internal timed waits already
 * hold a fresh `now` (they just checked the deadline), and re-reading
 * the clock here was one of three clock reads per timed park.
 */
int
__xtc_task_park_on_timer_at(xtc_task_t *self, int64_t deadline_ns)
{
	xtc_timer_t *t;
	int rc;

	if (self == NULL)
		return XTC_E_INVAL;
	if (atomic_load_explicit(&self->park_timer,
	    memory_order_relaxed) != NULL || self->park_fd >= 0)
		return XTC_E_INVAL;     /* already parked */

	if ((rc = __os_calloc(1, sizeof(*t), (void **)&t)) != XTC_OK)
		return rc;
	t->deadline_ns = deadline_ns;
	t->cb = NULL;
	t->user = NULL;
	t->waiter = self;
	t->heap_idx = -1;
	t->cancelled = 0;
	t->fired = 0;
	t->from_slab = 0;
	atomic_store_explicit(&t->refs, 2, memory_order_relaxed);  /* heap + park_timer slot */
	/*
	 * Arm the timer on the loop this fiber is RUNNING on, not its home
	 * loop (self->loop).  Under the multi-loop executor a migratable
	 * fiber can run stolen on a peer loop; loop->timers / loop->all_timers
	 * and the min-heap are single-owner (mutated by the owning loop's
	 * __xtc_drain_due_timers and pushes), so pushing onto self->loop from
	 * a thief thread races that home loop's owner and any other stolen
	 * fiber homed there -- the cross-loop timer-heap race (TSan-caught
	 * 2026-08-30, same family as the xtc_proc_wait_fd fd-registry fix).
	 * __xtc_current_loop is the running loop; the completion (timer fire)
	 * is drained on that same loop, which wakes the waiter.  Fall back to
	 * self->loop only off any loop (defensive; a park needs a loop). */
	{
		xtc_loop_t *wl;
		/* A migratable fiber preempted between reading the running loop
		 * and the push could resume on a thief and push onto this
		 * loop's single-owner heap from a foreign thread. */
		__xtc_unsafe_enter();
		wl = __xtc_current_loop != NULL ? __xtc_current_loop : self->loop;
		t->loop = wl;
		rc = __xtc_timer_heap_push(wl, t);
		__xtc_unsafe_leave();
		if (rc != XTC_OK) {
			__os_free(t);
			return rc;
		}
		XTC_PROBE_TIMER_ARM((uintptr_t)t, t->deadline_ns,
		    (uintptr_t)self);
		/*
		 * A park timer is refcounted (refs == 2): the heap holds one
		 * reference, the task's park_timer slot the other, and the last
		 * to release frees it (fired/cancelled park timers are reclaimed
		 * during the run, not retained on all_timers until loop_fini).
		 * So it is NOT spliced onto all_timers -- doing so would add a
		 * third owner that loop_fini would double-free.
		 */
	}
	atomic_store_explicit(&self->park_timer, t,
	    memory_order_relaxed);
	return XTC_OK;
}

/*
 * Cancel a task's park timer if one is armed, and clear the field.  A
 * no-op if none is armed.  Consolidates the cancel-and-NULL idiom that
 * every re-parking waiter loop (sync.c notify/sem/gate, lock_mgr.c
 * acquire, proc.c recv) would otherwise repeat inline.  Declared in
 * loop_int.h (internal; park_timer is a task-struct internal).
 */
void
__xtc_task_cancel_park_timer(xtc_task_t *self)
{
	if (self != NULL) {
		/*
		 * Exchange the slot to NULL so exactly one side (this cancel or
		 * a concurrent drain-fire) releases the slot's reference.  Mark
		 * the node cancelled first so the loop's heap-pop reclaims the
		 * heap reference; the last of the two frees it.
		 */
		xtc_timer_t *pt = atomic_exchange_explicit(&self->park_timer,
		    NULL, memory_order_relaxed);
		if (pt != NULL) {
			(void)xtc_timer_cancel(pt);
			__xtc_timer_park_release(pt);
		}
	}
}

/*
 * PUBLIC: int xtc_task_park_on_fd __P((xtc_task_t *, int, uint32_t));
 *
 * The tag we hand to xtc_io is the task pointer itself; the loop's
 * dispatcher recognises it and wakes the task.
 */
int
xtc_task_park_on_fd(xtc_task_t *self, int fd, uint32_t interest)
{
	int rc;
	if (self == NULL || fd < 0 || interest == 0)
		return XTC_E_INVAL;
	if (atomic_load_explicit(&self->park_timer,
	    memory_order_relaxed) != NULL || self->park_fd >= 0)
		return XTC_E_INVAL;
	if ((rc = xtc_io_reg_fd(self->loop->io, fd, interest, self)) != XTC_OK)
		return rc;
	self->park_fd = fd;
	return XTC_OK;
}

/*
 * Called by loop.c when an io event arrives.  The tag is either NULL
 * (XTC_IO_WAKEUP -- ignored in M3) or a task pointer registered by
 * xtc_task_park_on_fd.
 */
int
__xtc_loop_dispatch_event(xtc_loop_t *loop, xtc_io_event_t *ev)
{
	xtc_task_t *t;
	xtc_waker_t w;

	if (ev->flags & XTC_IO_WAKEUP)
		return XTC_OK;     /* M3: nothing parked on the wakeup */
	if (ev->tag == NULL)
		return XTC_OK;

	t = (xtc_task_t *)ev->tag;
	XTC_PROBE_IO_DISPATCH(loop->exec_id, ev->fd, ev->flags, (uintptr_t)t);
	/* Record the io flags so the parker knows what fired. */
	atomic_fetch_or_explicit(&t->wake_revents, ev->flags,
	    memory_order_relaxed);
	/*
	 * Drop the fd registration before waking; the task may register a
	 * fresh one when it runs.
	 *
	 * CLAIM it, do not test-then-use.  The parking fiber's own cleanup
	 * races this dispatch, and a plain "if (park_fd >= 0) del(park_fd)"
	 * let dispatch clear the field between the test and the use -- so the
	 * del was issued for a stale value (measured: del(-1)) and the
	 * registration SURVIVED.  See park_fd in loop_int.h for what an
	 * orphaned registration costs on a userspace-registry backend.
	 *
	 * Prefer ev->fd: it names the registration that actually fired, and
	 * loop->io is the registry that just delivered it, so neither half is
	 * a guess.  ev->fd < 0 (a wakeup, an AIO completion whose identity is
	 * the tag, or a backend that keeps its registry in the kernel) falls
	 * back to the claimed park_fd on its recorded io.
	 */
	if (ev->fd >= 0) {
		int claimed = atomic_load_explicit(&t->park_fd,
		    memory_order_acquire);
		if (claimed == ev->fd)
			(void)atomic_compare_exchange_strong_explicit(
			    &t->park_fd, &claimed, -1,
			    memory_order_acq_rel, memory_order_relaxed);
		(void)xtc_io_del_fd(loop->io, ev->fd);
	} else {
		int claimed = atomic_exchange_explicit(&t->park_fd, -1,
		    memory_order_acq_rel);
		if (claimed >= 0)
			(void)xtc_io_del_fd(
			    t->park_io != NULL ? t->park_io : loop->io,
			    claimed);
	}
	/*
	 * xtc_tail SCHED: record the DISPATCH itself, keyed by the task
	 * pointer.  A PARK-without-RUN tells you a fiber was never resumed but
	 * not whether its wake was ever generated; this closes that gap from
	 * the waker side.  A WAKE for task T with no subsequent RUN for T
	 * means dispatch ran and the loss is downstream of it; no WAKE at all
	 * means the completion never reached dispatch.
	 *
	 * pid carries the DISPATCHING loop id only -- dispatch has a task and
	 * there is no task-to-proc back pointer, so the task pointer goes in
	 * detail as the join key (match it against xtc-procs' task column, or
	 * against the task pointer an aio PARK records).  Gated, so a disabled
	 * tail is one branch.
	 */
	if (__xtc_tail_on(XTC_TAIL_SCHED)) {
		xtc_pid_t lp;
		memset(&lp, 0, sizeof lp);
		lp.loop_id = (uint32_t)(loop->exec_id >= 0 ? loop->exec_id : 0);
		__xtc_tail_emit(XTC_TAIL_SCHED, XTC_TAIL_WAKE, lp,
		    (uint64_t)(uintptr_t)t);
	}
	w.loop = loop;
	w.task = t;
	return xtc_waker_wake(&w);
}
