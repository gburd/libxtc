/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/loop_int.h
 *	Internal definitions for the L2 event loop.  Not part of the
 *	public ABI.
 */

#ifndef XTC_LOOP_INT_H
#define XTC_LOOP_INT_H

#include <stdatomic.h>
#include <stdint.h>

#include "xtc_loop.h"
#include "xtc_io.h"
#include "xtc_res.h"
#include "deque.h"
#include "os_thread.h"

/*
 * Task state machine.  Transitions:
 *	SCHEDULED -> RUNNING            loop pops from queue
 *	RUNNING   -> SCHEDULED          fn returned RESCHED
 *	RUNNING   -> PARKED             fn returned PENDING
 *	RUNNING   -> DONE               fn returned DONE
 *	PARKED    -> SCHEDULED          waker fired (or timer / fd ready)
 *	PARKED    -> DONE               (not reachable; PENDING tasks
 *	                                 are reaped only after they
 *	                                 next return DONE)
 */
enum xtc_task_state {
	XTC_TS_SCHEDULED = 0,
	XTC_TS_RUNNING   = 1,
	XTC_TS_PARKED    = 2,
	XTC_TS_DONE      = 3
};

struct xtc_task {
	xtc_task_fn  fn;
	void        *user;
	xtc_loop_t  *loop;
	_Atomic int  state;   /* xtc_task_state; atomic + CAS on PARKED->SCHEDULED
	                       * so a cross-loop XTC_INB_WAKE drained on a peer
	                       * loop cannot race the owning loop's dispatch
	                       * write (the concurrent-commit task->state race,
	                       * TSan-reported 2026-08-30). */
	/* L1 proportional-share scheduler: index into loop->classes of the
	 * run-class this task belongs to, or -1 for the default (implicit,
	 * plain-FIFO) class.  Set at spawn from xtc_proc_opts_t.sched_class
	 * or xtc_proc_set_class; survives a work-steal (a stolen task keeps
	 * its tag and is placed into the same class index on the thief).
	 * -1 by default, so a loop with no classes created is byte-for-byte
	 * the old FIFO+deque path.  INSPIRED BY Glommio's task-queue handle
	 * (executor/mod.rs TaskQueue). */
	int          sched_class;
	/* Monotonic time (ns) this run quantum was dispatched, recorded
	 * by the scheduler when loop->yield_budget_ns > 0.  xtc_yield_check
	 * compares against it; 0 means not yet recorded this quantum. */
	int64_t      run_start_ns;
	/* Pinned tasks run only on their home loop -- they go on the
	 * owner-only FIFO, never the stealable deque.  Used for explicit
	 * placement (xtc_exec_spawn_on) and for processes, which keep a
	 * shard-style affinity to one loop.  Unpinned tasks (the general
	 * pool) go on the Chase-Lev deque and may be work-stolen. */
	int          pinned;
	/* Run-queue intrusive next pointer. */
	struct xtc_task *q_next;

	/* Park bookkeeping.  At most one of these is active at a time
	 * while the task is in PARKED state. */
	xtc_timer_t *park_timer;
	int          park_fd;       /* -1 when not parked on fd */
	/* Voluntary park: when set by a primitive (e.g. xtc_amutex) just
	 * before yielding, the coro step returns PENDING instead of
	 * RESCHED, so the task sleeps until a waker re-enqueues it rather
	 * than busy-spinning.  Read-and-cleared by the step. */
	int          park_requested;

	/* Wakeup-cause flags, set by the dispatcher / timer callback /
	 * mbox_deliver when the task is unparked.  Sampled and cleared
	 * by the parker on resume (e.g. xtc_proc_wait_fd).  Encodes
	 * XTC_IO_* flags from the dispatched event plus the synthetic
	 * XTC_WAIT_MAILBOX (set by __mbox_deliver) and XTC_WAIT_TIMEOUT
	 * (set by the timer callback).  Atomic: a cross-thread xtc_send
	 * ORs XTC_WAIT_MAILBOX in from a FOREIGN thread (proc.c) while the
	 * owning loop ORs/reads/zeros it (task.c/loop.c/proc.c), so a plain
	 * uint32_t RMW here is a data race (TSan-reportable).  All accesses
	 * use relaxed atomics -- ordering is provided by the waker/mailbox
	 * lock; the atomic only makes the OR itself race-free. */
	_Atomic uint32_t wake_revents;

	/* Latched cross-thread wake that arrived while this task was NOT yet
	 * PARKED (the prepare/park race: a foreign xtc_waker_wake fires
	 * between the parker arming its waker and the loop transitioning it
	 * to PARKED on yield).  The WAKE-drain sets this instead of dropping
	 * the wake; the RUNNING->PARKED transition consumes it and
	 * re-schedules rather than parking, so the wake is never lost.
	 * Set cross-thread, consumed on the owning loop's thread. */
	_Atomic int wake_pending;

	/* Doubly linked into loop->all_tasks so a completed task can be
	 * unlinked in O(1) and recycled to the loop's task_slab (instead of
	 * lingering until loop_fini).  all_prev == NULL means the head. */
	struct xtc_task *all_next;
	struct xtc_task *all_prev;

	/* 1 if this task struct is eligible to be recycled onto the loop's
	 * task free-list when it completes (a plain task on its home loop);
	 * cleared for tasks that must not be recycled.  All task structs
	 * are __os_calloc'd regardless -- this only gates the free-list
	 * push, not the allocation source. */
	int              recyclable;

	/* Optional cleanup hook invoked by xtc_loop_fini before the task
	 * struct is freed.  The coroutine layer sets this to release the
	 * fiber stack + coro struct that wrap a task; plain tasks leave
	 * it NULL.  Keeps task lifetime owned by the loop while letting
	 * higher layers reclaim what they attached. */
	void       (*cleanup)(void *cleanup_arg);
	void        *cleanup_arg;
};

/*
 * Timer record.  Kept in a binary min-heap inside the loop.
 *
 * Cancel is lazy: we mark cancelled and skip on pop.  Cancel is O(1)
 * by cost; the heap may carry up to N stale entries until the next
 * extraction reaches them.  For M3 this is good enough; M5 may
 * upgrade to a hierarchical wheel.
 */
struct xtc_timer {
	int64_t      deadline_ns;
	xtc_timer_fn cb;
	void        *user;
	xtc_task_t  *waiter;        /* task to wake when fired (NULL if pure cb) */
	int          heap_idx;      /* current position in heap (-1 if not in) */
	int          cancelled;
	int          fired;
	int          sim_late;      /* DST: 1 once a buggify late-fire bumped
	                             * this timer's deadline (bump at most
	                             * once so a late fire cannot spin);
	                             * always 0 outside sim. */
	xtc_loop_t  *loop;          /* back-pointer for cancel-by-handle */
	struct xtc_timer *all_next; /* per-loop linked list for cleanup */
};

/*
 * Inbox message kinds.  All inbox traffic is cross-thread; the loop
 * owner drains its inbox at the top of every step.
 */
enum xtc_inbox_kind {
	XTC_INB_WAKE = 0,        /* re-queue a parked task */
	XTC_INB_PUBLISH = 1,     /* publish a freshly-allocated task */
};

struct xtc_inbox_msg {
	enum xtc_inbox_kind  kind;
	xtc_task_t          *task;
	struct xtc_inbox_msg *next;
};

struct xtc_inbox {
	__os_mutex_t          lock;
	struct xtc_inbox_msg *head;
	struct xtc_inbox_msg *tail;
	int                   inited;
};

/*
 * L1 proportional-share (weighted-fair) run class.  INSPIRED BY Glommio
 * (Glauber Costa / ScyllaDB): Glommio's executor keeps a set of task
 * queues, each with SHARES (1..1000) and a virtual runtime, and a
 * min-vruntime pick gives each queue a weighted CPU fraction
 * (executor/mod.rs TaskQueue + account_vruntime; shares.rs
 * reciprocal_shares).
 *
 * A class is a per-loop overlay on the ready set: it carries its own
 * ready FIFO (q_head/q_tail through the task's q_next) alongside the
 * class's shares, virtual runtime, and the precomputed reciprocal used
 * by the account formula.  The default (implicit) class is index -1 on
 * a task and is NOT one of these entries -- it uses the loop's plain
 * q_head/q_tail + deque, so a loop with zero classes runs the exact old
 * path with zero overhead.  The vruntime pick activates only once a
 * class has been created on the loop (loop->n_classes > 0).
 */
struct xtc_run_class {
	int          shares;        /* 1..1000 (Glommio's range) */
	int64_t      latency_ns;    /* optional latency bound; 0 = none */
	uint64_t     reciprocal;    /* (1<<22)/shares, precomputed */
	uint64_t     vruntime;      /* accumulated weighted runtime */
	uint64_t     runs;          /* times a task from this class was run
	                             * (telemetry / CPU-share observability) */
	struct xtc_task *q_head;    /* this class's ready FIFO */
	struct xtc_task *q_tail;
	int          in_use;        /* 1 once created */
};

/* Max scheduling classes per loop.  Glommio apps use a handful of task
 * queues; a small fixed array keeps __queue_pop a tiny linear scan (no
 * heap, no alloc on the dispatch path).  Creating more than this many
 * classes on one loop returns XTC_E_AGAIN. */
#define XTC_LOOP_MAX_CLASSES 16

/* Shares weight of the implicit default lane (untagged tasks) when it
 * races the explicit classes in the min-vruntime pick.  Chosen at the
 * top of Glommio's 1..1000 range so untagged/background work gets a
 * fair-but-not-dominant slice against a small set of weighted classes;
 * a class must be created with shares > this to out-run the default. */
#define XTC_DEFAULT_CLASS_SHARES 100
#define XTC_DEFAULT_CLASS_RECIP  (((uint64_t)1 << 22) / XTC_DEFAULT_CLASS_SHARES)

/* Minimum per-run cost charged to a class's vruntime (reduction-style
 * floor).  Guarantees vruntime always advances so the min-vruntime pick
 * cannot degenerate -- essential under the deterministic simulator,
 * where virtual time does not advance within a compute run (elapsed 0).
 * In production a real run's elapsed exceeds this, so the accounting is
 * Glommio-faithful (weighted by measured CPU time). */
#define XTC_VRUNTIME_MIN_QUANTUM_NS 1000

struct xtc_loop {
	xtc_io_t *io;

	/* Local run queue (Chase-Lev deque, owner pushes/pops). */
	xtc_deque_t deque;

	/* Slow-path overflow when the deque is full.  Owner-only. */
	struct xtc_task *q_head;
	struct xtc_task *q_tail;

	/* Timer min-heap. */
	xtc_timer_t **timers;
	int           n_timers;
	int           cap_timers;

	/* All tasks ever spawned, for cleanup.  Owner-only after init. */
	struct xtc_task *all_tasks;

	/* All timers ever created, for cleanup at fini. */
	xtc_timer_t *all_timers;

	/* M11.5b: per-loop slab cache for xtc_timer_t.  Created lazily
	 * by xtc_timer_set; freed in loop_fini.  Per-loop = single-
	 * threaded ownership = magazine fast path is lock-free. */
	struct xtc_slab *timer_slab;

	/* Per-loop task-struct free-list: a plain single-threaded LIFO of
	 * recycled task structs (linked through their q_next while free).
	 * xtc_task_spawn pops from it instead of malloc'ing, and a
	 * completed plain task on its home loop is pushed back instead of
	 * freed -- the spawn-heavy hot path, with no allocator call and no
	 * accumulation.  Only the owning loop thread touches it, so it is
	 * lock-free.  Drained (structs __os_free'd) at loop_fini. */
	struct xtc_task *task_free;
	int              task_free_n;

	/* Live-task counter.  Atomic so cross-thread spawns/completions
	 * can update it without lock. */
	_Atomic int n_alive;

	/* Per-loop work statistics (executor observability).  tasks_run
	 * counts task steps executed on this loop; steals counts tasks
	 * this loop successfully stole from a peer.  Relaxed atomics:
	 * read-mostly counters, exactness across a concurrent read is
	 * not required. */
	_Atomic uint64_t n_tasks_run;
	_Atomic uint64_t n_steals;

	/* I/O fairness: counts task runs since the last I/O poll.  When the
	 * run queue never empties (busy-yielding fibers, e.g. a buffer
	 * manager spinning on eviction), the loop would otherwise never
	 * poll I/O and parked completions would starve.  Every
	 * IO_FAIRNESS_QUANTUM runs the step does a non-blocking poll. */
	unsigned int     runs_since_poll;

	/* Cooperative yield watchdog (opt-in).  When yield_budget_ns > 0
	 * the scheduler records each quantum's start time on the task and
	 * xtc_yield_check reports a task over budget; n_yield_due counts
	 * over-budget reports (telemetry). */
	int64_t          yield_budget_ns;
	_Atomic uint64_t n_yield_due;

	/* L1 proportional-share scheduler (opt-in).  n_classes == 0 (the
	 * default) means no class was ever created and the scheduler uses
	 * the plain q_head/q_tail FIFO + deque -- byte-for-byte the old
	 * path.  Once a class exists, __queue_pop picks the min-vruntime
	 * in-use class and pops its FIFO.  Owner-only (no lock): classes
	 * are created and picked only on the loop's own thread. */
	struct xtc_run_class classes[XTC_LOOP_MAX_CLASSES];
	int              n_classes;
	/* Virtual runtime of the IMPLICIT default lane (the plain
	 * q_head/q_tail FIFO + deque, for tasks with sched_class == -1).
	 * When classes exist, the default lane races in the same
	 * min-vruntime pick as a class with fixed default shares, so
	 * untagged/background work is never starved by always-ready class
	 * work -- it just gets a default weight.  Unused (stays 0) until a
	 * class is created.  INSPIRED BY Glommio's default task queue. */
	uint64_t         default_vruntime;
	/* Effective per-loop latency bound: the smallest latency_ns over
	 * all in-use classes (0 = none).  Shrinks the yield/preempt
	 * interval so a latency class is checked often, like Glommio's
	 * reevaluate_preempt_timer.  Recomputed on class create. */
	int64_t          class_latency_ns;

	/* L3 over-budget stall watchdog (opt-in, off by default).  When
	 * stall_budget_ns > 0 the run-end boundary in __xtc_loop_step
	 * compares the elapsed run time against it and, on an overrun,
	 * invokes stall_cb (or logs).  A single branch on stall_budget_ns
	 * == 0 when off, so zero cost unless enabled.  INSPIRED BY
	 * Glommio's stall detector (executor/stall.rs). */
	int64_t          stall_budget_ns;
	void           (*stall_cb)(xtc_loop_t *loop, xtc_task_t *task,
	                          int64_t ran_ns, int64_t budget_ns,
	                          void *user);
	void            *stall_cb_user;
	_Atomic uint64_t n_stalls;   /* over-budget reports (telemetry) */

	int stop_requested;

	/* Cross-thread inbox: wakers and remote spawns deposit here;
	 * the owner drains in __xtc_loop_drain_inbox. */
	struct xtc_inbox inbox;

	/* For the multi-loop executor: 0-based index in xtc_exec; -1 if
	 * this loop is standalone (M3 single-thread mode). */
	int exec_id;

	/* Back-pointer to the executor (NULL if standalone). */
	struct xtc_exec *exec;

	/*
	 * Resource accountant.  Either owned by the loop (allocated and
	 * freed at init/fini) or borrowed from the executor.  Tracks
	 * tasks-alive, inbox messages, channels, etc.
	 */
	xtc_res_t *res;
	int        owns_res;

#if defined(XTC_DIAGNOSTIC)
	/*
	 * DIAGNOSTIC owner-thread guard.  A loop's owner-only structures
	 * (the Chase-Lev deque, the q_head/q_tail slow FIFO, the timer
	 * min-heap + all_timers, the task_slab free-list) must be mutated
	 * ONLY by the OS thread that runs this loop.  Four bugs in the
	 * v1.40.1..v1.40.4 arc were exactly a cross-loop mutation of one of
	 * these from a work-stolen fiber resuming on the wrong thread.
	 * owner_tid is recorded when the loop begins running on its thread
	 * (the exec worker, or xtc_loop_run); XTC_ASSERT_LOOP_OWNER aborts
	 * the instant a non-owner touches an owner-only structure, turning
	 * that whole race category from a probabilistic eventual wedge into
	 * a deterministic, immediate, located abort in the first offending
	 * run.  Compiled out entirely in a normal build (zero cost). */
	pthread_t  owner_tid;
	int        owner_set;
#endif
};

#if defined(XTC_DIAGNOSTIC)
/*
 * Abort if the calling thread is not this loop's owner.  `site` names the
 * owner-only structure being mutated, for a legible message.  A loop with
 * no recorded owner yet (owner_set == 0: before it has started running on
 * a thread, e.g. spawn-time init) is exempt -- there is no concurrent
 * owner to race.  __xtc_current_loop is deliberately NOT used to decide
 * ownership: it is the fiber's LOGICAL loop binding, preserved across a
 * work-steal migration, so it does not identify the physical OS thread
 * (this is the exact trap that caused a regression in the v1.40.3 fix).
 * pthread_self() is the authoritative physical-thread identity.
 */
void __xtc_loop_owner_violation(const struct xtc_loop *loop, const char *site);
extern XTC_THREAD_LOCAL struct xtc_loop *__xtc_current_loop;
extern int __xtc_sim_active(void);
#define XTC_ASSERT_LOOP_OWNER(loop, site)                                    \
	do {                                                                 \
		const struct xtc_loop *_l = (loop);                          \
		if (_l == NULL)                                              \
			break;                                               \
		if (__xtc_sim_active()) {                                    \
			/* Single-thread DST: the physical-thread check can   \
			 * never fire (one thread drives all loops), but the  \
			 * VIOLATION still executes -- a fiber mutating a loop \
			 * other than the one currently being stepped is the  \
			 * exact cross-loop bug, just serialized so it cannot  \
			 * corrupt.  __xtc_current_loop is the loop being      \
			 * stepped, i.e. the only legitimate mutation target.  \
			 * This makes the whole category DETERMINISTICALLY     \
			 * reproducible under a seed. */                       \
			if (__xtc_current_loop != NULL &&                    \
			    __xtc_current_loop != _l)                        \
				__xtc_loop_owner_violation(_l, (site));      \
		} else if (_l->owner_set &&                                  \
		    !pthread_equal(pthread_self(), _l->owner_tid)) {         \
			__xtc_loop_owner_violation(_l, (site));              \
		}                                                            \
	} while (0)
#else
#define XTC_ASSERT_LOOP_OWNER(loop, site)  ((void)0)
#endif

/* Internal helpers shared between loop.c, task.c, timer.c. */
int  __xtc_loop_enqueue(xtc_loop_t *loop, xtc_task_t *t);
/* L1: create a run class on `loop`.  shares in 1..1000; latency_ns >= 0
 * (0 = none).  Returns the class index in *out_idx (>= 0), or an error
 * (XTC_E_INVAL bad args, XTC_E_AGAIN if the per-loop class cap is hit).
 * Owner-only. */
int  __xtc_loop_class_create(xtc_loop_t *loop, int shares,
                            int64_t latency_ns, int *out_idx);
/* Spawn with explicit pinned-ness: pinned tasks stay on `loop` (FIFO,
 * never work-stolen); unpinned tasks may migrate via the deque. */
int  __xtc_task_spawn_ex(xtc_loop_t *loop, xtc_task_fn fn, void *user,
                         int pinned, xtc_task_t **out_task);
int  __xtc_timer_heap_push(xtc_loop_t *loop, xtc_timer_t *t);
xtc_timer_t *__xtc_timer_heap_pop_due(xtc_loop_t *loop, int64_t now_ns);
int64_t      __xtc_timer_heap_next_deadline(xtc_loop_t *loop);
void __xtc_task_cancel_park_timer(xtc_task_t *self);
int  __xtc_loop_dispatch_event(xtc_loop_t *loop, xtc_io_event_t *ev);

/*
 * L2 io_uring ring-pointer preempt seam (INSPIRED BY Glommio's
 * need_preempt).  Real on the io_uring backend (src/io/io_uring.c),
 * portable no-op stubs elsewhere, so loop.c/exec.c call them
 * unconditionally.  _arm returns XTC_OK only when the ring is now the
 * preempt trigger (uring backend); _due is the two-load Glommio check
 * (1 = a preempt slice elapsed); _disarm tears the ring down. */
int  __xtc_io_uring_preempt_arm(xtc_io_t *io, int64_t interval_ns);
int  __xtc_io_uring_preempt_due(xtc_io_t *io);
void __xtc_io_uring_preempt_disarm(xtc_io_t *io);

/* Implemented in proc.c.  Called from loop_fini to release the
 * proc-table side struct hashed against this loop pointer.  Must be
 * idempotent. */
void __xtc_proc_loop_unregister(xtc_loop_t *loop);
/* Inbox API.  Producer-side functions are thread-safe. */
int  __xtc_inbox_init(struct xtc_inbox *ib);
void __xtc_inbox_fini(struct xtc_inbox *ib);
int  __xtc_inbox_push(struct xtc_inbox *ib, enum xtc_inbox_kind k, xtc_task_t *t);
int  __xtc_inbox_drain(xtc_loop_t *loop);  /* owner-only; drains into local queue */

/* Per-thread cursor: which loop the calling thread is running.
 * NULL on threads that aren't loop owners. */
extern XTC_THREAD_LOCAL xtc_loop_t *__xtc_current_loop;

/*
 * Fiber-context preservation hook.  The L3 process layer keeps a
 * per-thread "current proc" pointer that must survive a yield (the
 * scheduler runs other fibers in between, which overwrite it).  The
 * L2 coro layer cannot depend on L3, so every yield/await jump saves
 * the opaque context before jumping to the scheduler and restores it
 * on resume through these hooks.  proc.c installs them on first
 * spawn; while NULL (no process layer in use) the calls are no-ops.
 * Set once to stable function addresses, so a plain pointer load is
 * safe without synchronization.
 */
extern void *(*__xtc_fiber_ctx_save)(void);
extern void  (*__xtc_fiber_ctx_restore)(void *);

/* Post-resume cancellation hook.  Installed by the process layer
 * (proc.c).  Called at the universal fiber resume point (after a yield
 * returns) so a fiber that had a kill/cancel requested while it was
 * NOT at a cooperative point -- e.g. a pure CPU loop that was
 * involuntarily preempted -- honors the kill the instant the scheduler
 * resumes it, by unwinding via xtc_exit_self.  NULL when no process
 * layer is present (bare coroutine use).  Returns without effect if no
 * kill is pending. */
extern void  (*__xtc_fiber_kill_check)(void);

/* Loop-fini hook.  Installed by the process layer (proc.c) on first
 * spawn; lets xtc_loop_fini release the loop's per-loop proc table
 * without the L2 loop depending on the L3 proc layer directly (the
 * loop calls the hook, proc.c points it at __xtc_proc_loop_unregister).
 * NULL when no process layer is in use, so a bare-coroutine loop has
 * nothing to clean up. */
extern void  (*__xtc_loop_fini_hook)(xtc_loop_t *loop);

/* Forward declaration for back-pointer in xtc_loop. */
struct xtc_exec;

#endif /* XTC_LOOP_INT_H */
