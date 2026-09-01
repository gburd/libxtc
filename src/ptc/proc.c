/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/proc.c
 *	BEAM-style processes built on top of the M4 coroutine substrate.
 *	Each process is a fiber with identity, a mailbox, and signal
 *	handling for links and monitors.  Selective receive uses the
 *	classic save-queue pattern.
 */

#include "xtc_int.h"
#include "xtc_inject.h"
#include "xtc_sim.h"
#include "xtc_dst_inject.h"
#include "xtc_proc.h"
#include "proc_int.h"   /* __xtc_proc_ctx_save/restore (internal) */
#include "xtc_loop.h"
#include "xtc_async.h"
#include "xtc_exec.h"
#include "loop_int.h"
#include "xtc_tail.h"     /* __xtc_tail_emit SCHED source (spawn/exit) */
#include "tail_int.h"     /* __xtc_tail_emit / __xtc_tail_on (internal) */
#include "coro_int.h"
#include "xtc_tailcall.h"
#include "xtc_slab.h"
#include "xtc_inspect.h"
#include "xtc_runtime.h"
#include "os_cpu.h"
#include "xtc_trace.h"
#include "xtc_slab.h"
#include "xtc_mctx.h"
#include "xtc_fs.h"     /* xtc_fs_close: portable fd close for recovery */
#include "preempt_int.h"   /* __xtc_unsafe_* / __xtc_mtx_*: internal preemption brackets */
#include <stdio.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>    /* sysconf(_SC_PAGESIZE) for stack-overflow detection */

/* ---------- envelope ---------- */

/*
 * Deferred cross-loop delivery (DST net-latency).  In a sim build the
 * sim I/O backend enqueues a callback on the target loop's event store
 * to run at a virtual-time deadline; a non-sim build has no such backend,
 * so a stub reports "unavailable" and the delay path is never taken
 * (xtc_sim_net_latency only fires under sim anyway).  This keeps proc.c
 * linkable in every build -- mirrors exec.c's __xtc_io_sim_next_due. */
#if defined(XTC_IO_BACKEND_SIM)
extern int __xtc_io_sim_defer_cb(xtc_io_t *io, int64_t due_ns,
    void (*fn)(void *), void *arg);
#else
static inline int __xtc_io_sim_defer_cb(xtc_io_t *io, int64_t due_ns,
    void (*fn)(void *), void *arg)
{ (void)io; (void)due_ns; (void)fn; (void)arg; return XTC_E_NOSYS; }
#endif

struct envelope {
	struct envelope *next;
	xtc_pid_t        from;
	size_t           size;
	uint64_t         hlc;        /* sender's HLC stamp (causal tracing) */
	unsigned char    data[];     /* flexible */
};

/* ---------- proc ---------- */

struct link_entry { struct link_entry *next; xtc_pid_t peer; };
struct mon_entry  { struct mon_entry *next; uint64_t ref; xtc_pid_t target; xtc_pid_t watcher; };

/* M11.5b: pools for link_entry / mon_entry (fixed-size, hot path). */
static _Atomic(xtc_slab_t *) __link_slab     = NULL;
static _Atomic(xtc_slab_t *) __mon_slab      = NULL;
static _Atomic(xtc_slab_t *) __env_slab      = NULL;
static pthread_mutex_t  __proc_slab_init_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Small-message envelope pool.  Message passing is the actor hot
 * path; allocating every envelope with malloc puts an allocator
 * round-trip on every send.  Envelopes whose payload fits in
 * ENV_POOL_PAYLOAD bytes are served from a fixed-size slab instead
 * (one magazine pop on the fast path, no global allocator call);
 * larger payloads fall back to malloc.  The discriminator on free is
 * the envelope's own size field -- a pooled envelope always has
 * size <= ENV_POOL_PAYLOAD -- so no per-envelope flag is needed.
 */
#define ENV_POOL_PAYLOAD  256

/*
 * Preemption-safe mutex brackets: proc-layer mutexes go through the
 * shared __xtc_mtx_lock/unlock (src/ptc/preempt.c) so a fiber holding a
 * proc mutex is never involuntarily preempted -- otherwise the loop
 * (many fibers, one OS thread) deadlocks when another same-loop fiber
 * blocks on the same mutex (captured on EC2 as a hang in
 * __notify_links_and_monitors on tbl->lock).  These thin aliases keep
 * the proc.c call sites readable and route every acquire/release
 * through the one library-wide helper.
 */
#define __proc_mtx_lock(m)    __xtc_mtx_lock(m)
#define __proc_mtx_unlock(m)  __xtc_mtx_unlock(m)

static void
__proc_slabs_ensure(void)
{
	/* Double-checked lazy init.  The pointers are _Atomic so the
	 * fast-path check loads them race-free (acquire) rather than a
	 * plain read that TSan flags against the release store under the
	 * lock -- the same fix applied to xtc_rcu's slab init.  Each pool
	 * is published exactly once. */
	if (atomic_load_explicit(&__link_slab, memory_order_acquire) != NULL &&
	    atomic_load_explicit(&__mon_slab, memory_order_acquire) != NULL &&
	    atomic_load_explicit(&__env_slab, memory_order_acquire) != NULL)
		return;
	(void) __proc_mtx_lock(&__proc_slab_init_lock);
	if (atomic_load_explicit(&__link_slab, memory_order_relaxed) == NULL) {
		xtc_slab_opts_t o = XTC_SLAB_OPTS_DEFAULT;
		xtc_slab_t *sl = NULL;
		o.name = "proc.link"; o.obj_size = sizeof(struct link_entry);
		if (xtc_slab_create(&o, &sl) == XTC_OK)
			atomic_store_explicit(&__link_slab, sl,
			    memory_order_release);
	}
	if (atomic_load_explicit(&__mon_slab, memory_order_relaxed) == NULL) {
		xtc_slab_opts_t o = XTC_SLAB_OPTS_DEFAULT;
		xtc_slab_t *sl = NULL;
		o.name = "proc.mon"; o.obj_size = sizeof(struct mon_entry);
		if (xtc_slab_create(&o, &sl) == XTC_OK)
			atomic_store_explicit(&__mon_slab, sl,
			    memory_order_release);
	}
	if (atomic_load_explicit(&__env_slab, memory_order_relaxed) == NULL) {
		xtc_slab_opts_t o = XTC_SLAB_OPTS_DEFAULT;
		xtc_slab_t *sl = NULL;
		o.name = "proc.env";
		o.obj_size = sizeof(struct envelope) + ENV_POOL_PAYLOAD;
		if (xtc_slab_create(&o, &sl) == XTC_OK)
			atomic_store_explicit(&__env_slab, sl,
			    memory_order_release);
	}
	(void) __proc_mtx_unlock(&__proc_slab_init_lock);
}

/* Allocate an envelope for a `size`-byte payload, from the pool when
 * it fits, else from malloc. */
static struct envelope *
__env_alloc(size_t size)
{
	void *p = NULL;
	if (size <= ENV_POOL_PAYLOAD) {
		__proc_slabs_ensure();
		if (XTC_LIKELY(__env_slab != NULL))
			return xtc_slab_alloc(__env_slab);
	}
	if (__os_malloc(sizeof(struct envelope) + size, &p) != XTC_OK)
		return NULL;
	return p;
}

/* Free an envelope, routing by its payload size (the same predicate
 * __env_alloc used). */
static void
__env_free(struct envelope *e)
{
	if (e == NULL) return;
	if (e->size <= ENV_POOL_PAYLOAD && __env_slab != NULL)
		xtc_slab_free(__env_slab, e);
	else
		__os_free(e);
}

/* ---------- causal tracing + hybrid logical clock (xtc_trace.h) ---------- */

#define XTC_TRACE_RING 8192u
static _Atomic uint64_t __g_hlc;
static _Atomic int      __trace_on;
static pthread_mutex_t  __trace_lock = PTHREAD_MUTEX_INITIALIZER;
static xtc_trace_rec_t  __trace_ring[XTC_TRACE_RING];
static uint64_t         __trace_seq;        /* total records ever written */

static uint64_t
__phys_us(void)
{
	int64_t ns = 0;
	(void)__os_clock_mono(&ns);
	return (uint64_t)(ns / 1000);
}

/* HLC: high 48 bits physical microseconds, low 16 bits logical. */
static uint64_t
__hlc_tick(void)
{
	uint64_t prev, next, pt = __phys_us();
	do {
		uint64_t pphys, plog;
		prev = atomic_load_explicit(&__g_hlc, memory_order_relaxed);
		pphys = prev >> 16; plog = prev & 0xFFFF;
		if (pt > pphys) {
			next = pt << 16;
		} else if (plog + 1 > 0xFFFF) {
			next = (pphys + 1) << 16;
		} else {
			next = (pphys << 16) | (plog + 1);
		}
	} while (!atomic_compare_exchange_weak_explicit(&__g_hlc, &prev, next,
	    memory_order_relaxed, memory_order_relaxed));
	return next;
}

/* Advance the clock past a received stamp `m`; return the new stamp. */
static uint64_t
__hlc_update(uint64_t m)
{
	uint64_t prev, next, pt = __phys_us();
	do {
		uint64_t pphys = 0, plog = 0, mphys = m >> 16, mlog = m & 0xFFFF;
		uint64_t nphys, nlog;
		prev = atomic_load_explicit(&__g_hlc, memory_order_relaxed);
		pphys = prev >> 16; plog = prev & 0xFFFF;
		nphys = pphys;
		if (mphys > nphys) nphys = mphys;
		if (pt > nphys) nphys = pt;
		if (nphys == pphys && nphys == mphys)
			nlog = (plog > mlog ? plog : mlog) + 1;
		else if (nphys == pphys)
			nlog = plog + 1;
		else if (nphys == mphys)
			nlog = mlog + 1;
		else
			nlog = 0;
		if (nlog > 0xFFFF) { nphys++; nlog = 0; }
		next = (nphys << 16) | (nlog & 0xFFFF);
	} while (!atomic_compare_exchange_weak_explicit(&__g_hlc, &prev, next,
	    memory_order_relaxed, memory_order_relaxed));
	return next;
}

static void
__trace_record(int kind, xtc_pid_t self, xtc_pid_t peer, uint64_t hlc,
    uint64_t cause, uint32_t detail)
{
	/* Hot-path guard: a single relaxed load when tracing is off. */
	if (!atomic_load_explicit(&__trace_on, memory_order_relaxed))
		return;
	(void) __proc_mtx_lock(&__trace_lock);
	if (atomic_load_explicit(&__trace_on, memory_order_relaxed)) {
		xtc_trace_rec_t *r = &__trace_ring[__trace_seq % XTC_TRACE_RING];
		r->hlc = hlc; r->cause = cause; r->kind = kind;
		r->self = self; r->peer = peer; r->detail = detail;
		__trace_seq++;
	}
	(void) __proc_mtx_unlock(&__trace_lock);
}

/* True iff tracing is currently enabled (hot-path predicate). */
static int
__trace_active(void)
{
	return atomic_load_explicit(&__trace_on, memory_order_relaxed) != 0;
}

static _Atomic uint64_t __mon_ref_seq = 0;

static struct link_entry *
__link_alloc(void)
{
	__proc_slabs_ensure();
	if (__link_slab == NULL) return NULL;
	return xtc_slab_alloc(__link_slab);
}
static void __link_free(struct link_entry *e) {
	if (e == NULL) return;
	if (__link_slab) xtc_slab_free(__link_slab, e); else free(e);
}
static struct mon_entry *
__mon_alloc(void)
{
	__proc_slabs_ensure();
	if (__mon_slab == NULL) return NULL;
	return xtc_slab_alloc(__mon_slab);
}
static void __mon_free(struct mon_entry *e) {
	if (e == NULL) return;
	if (__mon_slab) xtc_slab_free(__mon_slab, e); else free(e);
}

struct xtc_proc {
	xtc_pid_t   pid;
	xtc_loop_t *loop;
	xtc_task_t *task;            /* the underlying task */
	struct xtc_coro *coro;        /* the underlying fiber */

	/*
	 * Mailbox (singly-linked FIFO of envelopes).
	 *
	 * False-sharing fix: the producer-side fields (mbox_lock,
	 * mbox_tail) are written by senders on remote threads, while
	 * the consumer-side fields (mbox_head, mbox_n) are written
	 * by the owning proc.  We cache-line-separate them to avoid
	 * cache-line ping-pong under concurrent send/recv.
	 *
	 * Layout:
	 *   [consumer-side] mbox_head, mbox_n
	 *   <64-byte pad>
	 *   [producer-side] mbox_lock, mbox_tail, mbox_cap
	 */
	/* ---- consumer-side (owner writes) ---- */
	struct envelope  *mbox_head;
	size_t            mbox_n;

	char              __mbox_pad[XTC_CACHE_LINE
	                            - sizeof(struct envelope *)
	                            - sizeof(size_t)];

	/* ---- producer-side (senders write) ---- */
	pthread_mutex_t   mbox_lock;
	struct envelope  *mbox_tail;
	size_t            mbox_cap;

	/* Mailbox statistics (all under mbox_lock): peak depth ever
	 * reached, total messages accepted, total rejected (mailbox full
	 * or proc dead).  For xtc_proc_mailbox_stats / overload scrapes. */
	size_t            mbox_peak;
	uint64_t          mbox_recv_total;
	uint64_t          mbox_drop_total;

	/* Watermark hook: when an accepted message pushes the depth to or
	 * past mbox_wm_lvl (computed from a percent of cap at spawn), the
	 * callback fires once on the rising edge so the app can shed load
	 * before the hard cap.  mbox_wm_fired latches until depth falls
	 * back below the level. */
	size_t            mbox_wm_lvl;     /* 0 = disabled */
	int               mbox_wm_fired;
	void            (*mbox_wm_fn)(xtc_pid_t, size_t, size_t, void *);
	void             *mbox_wm_user;

	/* Selective-receive save queue (envelopes that the receiver
	 * already inspected and rejected).  mbox_saved counts them; it is
	 * written by the owning proc during recv and read by senders
	 * (under mbox_lock) so the admission cap bounds mailbox + save
	 * queue together -- otherwise a flood of non-matching messages
	 * drains into the unbounded save queue and defeats mailbox_cap. */
	struct envelope  *save_head;
	struct envelope  *save_tail;
	_Atomic size_t    mbox_saved;

	/* BEAM recv-mark optimization for selective receive.  When the
	 * caller invokes xtc_recv_match repeatedly with the same
	 * (match_fn, user) pair, we remember which save_queue entries
	 * have already been tested against that predicate so we don't
	 * re-walk them.  Cleared whenever the predicate changes. */
	xtc_match_fn      last_match_fn;
	void             *last_match_user;
	struct envelope  *recv_mark;       /* skip up to and including this entry */

	/* Receive coordination. */
	xtc_waker_t       recv_waker;
	int               waker_armed;

	/* Entry. */
	xtc_proc_fn fn;
	void       *arg;

	/* Exit handling. */
	jmp_buf     exit_jb;
	int         exit_jb_set;
	int         exit_reason;
	int         exit_kind;   /* xtc_down_kind_t: 0 clean, 1 exit, 2 signal */

	/* R1 fault containment: a recovery frame the proc arms with
	 * xtc_proc_recovery_arm().  A per-process SIGSEGV/SIGBUS handler,
	 * on a fault in this proc's fiber (identified via __current_proc),
	 * siglongjmps here -- but only outside a critical section
	 * (crit_depth == 0); inside one the fault escalates to process
	 * abort, preserving PG's critical-section PANIC semantics. */
	xtc_recovery_buf_t recovery_buf;
	volatile sig_atomic_t recovery_armed;
	volatile sig_atomic_t recovery_fired;   /* Windows: set by the VEH */
	volatile sig_atomic_t crit_depth;
	volatile sig_atomic_t fault_sig;

	/* Per-proc at-exit callbacks, run LIFO on the exit path -- both a
	 * normal return and a contained-fault recovery (xtc_exit_self).
	 * Where an embedder reclaims resources a faulted session held
	 * (lock-manager release-all, fd close, memory-context reset) so
	 * they never outlive the proc.  Runs outside signal context. */
#define XTC_PROC_MAX_ATEXIT 8
	struct { void (*fn)(void *); void *arg; } at_exit[XTC_PROC_MAX_ATEXIT];
	int           n_at_exit;
	struct xtc_mctx *proc_mctx;   /* lazily created by xtc_proc_mctx() */

	/* Recovery resource registry: the resources a proc holds that must
	 * be released if a contained fault unwinds its stack (the stack
	 * unwind via siglongjmp/VEH frees no locks, fds, or pins).  Each
	 * record names a built-in kind (fd close, mctx reset, lock-manager
	 * release-all) or a generic callback.  xtc_proc_recovery_cleanup()
	 * releases them LIFO; it is the default recovery action and is also
	 * callable from a custom recovery block to finish the standard
	 * bits.  Released records are also run on the normal exit path so a
	 * proc that returns without explicitly releasing still cleans up. */
#define XTC_PROC_MAX_RECOVERY 16
/* Recovery-resource kinds (struct xtc_recov_rec.kind). */
#define XTC_RECOV_FD    1   /* close(fd) */
#define XTC_RECOV_MCTX  2   /* xtc_mctx_reset(ptr) */
#define XTC_RECOV_LOCKS 3   /* lock-manager release-all (fn(ptr) with u64=locker) */
#define XTC_RECOV_CB    4   /* generic fn(ptr) */
	struct xtc_recov_rec {
		int   kind;       /* XTC_RECOV_* */
		int   fd;         /* XTC_RECOV_FD */
		void *ptr;        /* mctx (RESET) / lockmgr (LOCKS) / arg (CB) */
		uint64_t u64;     /* locker id (LOCKS) */
		void (*fn)(void *); /* XTC_RECOV_CB */
		void (*lock_release)(void *, uint64_t); /* XTC_RECOV_LOCKS */
	} recov[XTC_PROC_MAX_RECOVERY];
	int           n_recov;

	/* Asynchronous kill (cross-process exit signal).  Set by
	 * xtc_exit_pid; checked at every yield/recv parking point.
	 * The flag carries the reason+1 so 0 means "no kill pending". */
	_Atomic int kill_pending;

	/* A2 cancellation masking (MonadCancel uncancelable/poll).  While
	 * mask_depth > 0 an asynchronous kill (xtc_exit_pid) delivered at a
	 * yield/recv point is DEFERRED, not acted on: the reason+1 is latched
	 * in mask_deferred and the unwind runs only when mask_depth falls
	 * back to 0 (or when a poll region re-admits it).  This is what lets
	 * a resource acquired in a masked region always register its release
	 * before cancellation is observed -- A1's guaranteed-release has,
	 * without it, the finalizer-eating race Cats Effect had pre-masking.
	 * Owner-only fields (the proc reads/writes them on its own fiber),
	 * so no atomics are needed. */
	unsigned      mask_depth;
	int           mask_deferred;   /* latched reason+1; 0 = none pending */

	/* A3 async causal trace: a small per-fiber ring of the recent
	 * suspend/resume boundaries (park reason / resume) with a static
	 * site label, so xtc_dump can splice "how did this fiber get here"
	 * onto its current-state line.  Owner-only / core-private: the ring
	 * is written ONLY on this proc's own fiber, one index bump + a store
	 * per event, no lock and no atomic on the record (Invariant 1: no
	 * shared-line write on the hot path).  Written only when the causal
	 * trace is enabled, so a default build never touches it; a reader
	 * (xtc_dump / xtc_trace_causal_dump) snapshots it best-effort.
	 * ct_seq is the total events ever recorded; the live window is the
	 * last min(ct_seq, XTC_PROC_CAUSAL_RING) entries. */
#define XTC_PROC_CAUSAL_RING 16u
	struct { int kind; const char *site; } ct_ring[XTC_PROC_CAUSAL_RING];
	unsigned      ct_seq;

	/* Lifecycle. */
	int         alive;

	/*
	 * Teardown-safety refcount.  A resolver (__table_lookup) takes a
	 * ref WHILE HOLDING the owning table lock -- atomic with the
	 * detach in __notify_links_and_monitors -- so a proc cannot be
	 * freed out from under an in-flight cross-thread send / wake / DOWN
	 * delivery.  The owner (spawn) holds one ref; __notify drops it
	 * after detaching from the table, and the struct is freed only when
	 * the count reaches zero (see __proc_ref / __proc_release).  This
	 * closes the resolve-then-deliver use-after-free race class
	 * (KNOWN_ISSUES: DOWN-send vs teardown, blocking-pool wake). */
	_Atomic int refs;

	/* Consumer-owned opaque per-proc pointer (xtc_proc_set_userdata /
	 * xtc_proc_userdata).  NULL by default (the struct is calloc'd).
	 * Rides with the proc, so it survives work-stealing migration
	 * exactly like the mailbox and __current_proc.  The runtime never
	 * dereferences or frees it -- same lifetime contract as
	 * xtc_proc_at_exit's arg (the consumer owns whatever it points to);
	 * it is simply dropped when the proc is freed. */
	void             *userdata;

	/* L1 proportional-share scheduler: the class handle from
	 * xtc_proc_opts_t.sched_class, stashed at spawn and applied by
	 * __proc_entry on the proc's OWN loop thread (setting the task's
	 * class there avoids racing a cross-loop spawn that begins running
	 * before the spawner's next statement).  NULL = default class. */
	xtc_exec_class_t  spawn_class;

	/* Links & monitors. */
	struct link_entry *links;
	struct mon_entry  *monitors;     /* monitors WE created (we are watcher) */
	struct mon_entry  *monitored_by; /* monitors others created on us */
};

/*
 * Static assertion: mbox producer/consumer fields are cache-line separated.
 * offsetof(mbox_lock) should be at least 64 bytes beyond offsetof(mbox_head).
 */
_Static_assert(
    offsetof(struct xtc_proc, mbox_lock) - offsetof(struct xtc_proc, mbox_head)
        >= XTC_CACHE_LINE,
    "mbox producer/consumer fields must be cache-line separated");

/*
 * Per-loop slot table.  A loop owns its own proc_slots array; we
 * grow on demand (powers of two).  Slot reuse bumps the generation
 * so stale pids can be detected.
 */
struct xtc_proc_slot {
	struct xtc_proc *proc;       /* NULL if free */
	uint32_t         gen;
};

/*
 * Write striping for the per-loop proc table (PLAN.md 19.5c).  The old
 * design had ONE mutex per table (t->lock), so every xtc_send to any
 * proc on a carrier -- including same-carrier fiber-to-fiber hand-offs,
 * the PG fiber-per-session hot path -- serialized on it (measured
 * ~367k futex-waits/10s, flat in carrier count).  A key's stripe is
 * derived from its local_id ONLY (stripe = local_id & (NSTRIPES-1)),
 * so a lookup and the matching detach/teardown for the SAME proc always
 * take the SAME stripe -- mandatory for the "see-live-and-pin OR
 * see-NULL, never a freed pointer" atomicity between __table_lookup and
 * __table_release.  Two procs with different local_ids that share a
 * stripe only pay a false-sharing serialization, never a correctness
 * bug.  A grow (slot-array realloc) or a whole-table scan claims ALL
 * stripes in ASCENDING order (the sole multi-stripe hold -> the one
 * deadlock-avoidance rule), exactly as xtc_chash does.
 */
#define XTC_PT_NSTRIPES  16u

struct xtc_proc_table {
	struct xtc_proc_slot *slots;
	size_t                cap;
	size_t                n_used;
	pthread_mutex_t       stripes[XTC_PT_NSTRIPES];
	int                   inited;
};

static inline unsigned
__pt_stripe(uint16_t local_id)
{
	return (unsigned)local_id & (XTC_PT_NSTRIPES - 1u);
}

/* Lock/unlock EVERY stripe (ascending / descending), for a grow or a
 * whole-table scan.  Ascending lock order is the only deadlock rule. */
static void
__pt_lock_all(struct xtc_proc_table *t)
{
	unsigned s;
	for (s = 0; s < XTC_PT_NSTRIPES; s++)
		(void) __proc_mtx_lock(&t->stripes[s]);
}
static void
__pt_unlock_all(struct xtc_proc_table *t)
{
	unsigned s;
	for (s = XTC_PT_NSTRIPES; s-- > 0; )
		(void) __proc_mtx_unlock(&t->stripes[s]);
}

/* Each loop carries one of these (lazy-allocated on first proc spawn). */
static XTC_THREAD_LOCAL struct xtc_proc *__current_proc = NULL;
static void __xtc_proc_kill_check(void);   /* installed as the resume kill hook */

/*
 * The table lives in a side struct hung off xtc_loop->user_data... but
 * we don't have that field.  Use a per-loop pointer hashed by loop
 * pointer in a tiny global table.  For M8 four-loop typical use this
 * is fine; M9 adds a proper field.
 */
#define LOOP_TABLE_MAX 64
/*
 * Loop registry.  Read-mostly: written only when a loop is created or
 * finalized (rare), read on EVERY cross-thread wake/send that falls to
 * __resolve's Strategy 2 (hot -- one per command boundary under a
 * fiber-per-session server).  The slot's `loop` and `tbl` pointers are
 * therefore ATOMIC so the read path scans them lock-free with acquire
 * loads; only the writers (register/unregister, which also mutate the
 * table contents and free memory) serialize on __lt_lock.  A reader
 * that races a concurrent unregister either sees the old loop pointer
 * (and then finds its table, or a NULL slot) or the NULL -- never a
 * torn pointer; the proc-header refcount + generation check on the
 * resolved proc handles the rest of the lifetime race, exactly as it
 * did when this scan held the lock.
 */
struct lt_entry {
	_Atomic(xtc_loop_t *)            loop;
	_Atomic(struct xtc_proc_table *) tbl;
};
static pthread_mutex_t __lt_lock = PTHREAD_MUTEX_INITIALIZER;
static struct lt_entry __lt[LOOP_TABLE_MAX];

/* M11.5b: invalidate the registry entry for `loop` and free its
 * table.  Called from xtc_loop_fini.  Idempotent. */
void
__xtc_proc_loop_unregister(xtc_loop_t *loop)
{
	int i;
	struct xtc_proc_table *tbl = NULL;
	(void) __proc_mtx_lock(&__lt_lock);
	for (i = 0; i < LOOP_TABLE_MAX; i++) {
		if (atomic_load_explicit(&__lt[i].loop,
		    memory_order_relaxed) == loop) {
			tbl = atomic_load_explicit(&__lt[i].tbl,
			    memory_order_relaxed);
			/* Publish the slot vacancy with release stores so a
			 * lock-free reader sees loop==NULL (skip) rather than a
			 * live loop with a NULL/freed tbl.  Clear loop FIRST so
			 * a concurrent reader that still sees the old loop then
			 * reads tbl gets the (still-valid-until-freed-below)
			 * table; the actual free happens after the unlock, and
			 * the contract is that a loop being finalized has no
			 * live procs and no in-flight wakes to it. */
			atomic_store_explicit(&__lt[i].loop, NULL,
			    memory_order_release);
			atomic_store_explicit(&__lt[i].tbl, NULL,
			    memory_order_release);
			break;
		}
	}
	(void) __proc_mtx_unlock(&__lt_lock);
	if (tbl != NULL) {
		/* Free still-live process headers.  By contract a loop
		 * being finalized has no live procs left, but we defend
		 * the path anyway. */
		if (tbl->slots != NULL) {
			size_t k;
			for (k = 0; k < tbl->cap; k++) {
				struct xtc_proc *p = tbl->slots[k].proc;
				if (p != NULL) {
					(void)pthread_mutex_destroy(&p->mbox_lock);
					__os_free(p);
					tbl->slots[k].proc = NULL;
				}
			}
			__os_free(tbl->slots);
		}
		(void)pthread_mutex_destroy(&tbl->stripes[0]);
		{
			unsigned s;
			for (s = 1; s < XTC_PT_NSTRIPES; s++)
				(void)pthread_mutex_destroy(&tbl->stripes[s]);
		}
		__os_free(tbl);
	}
}

static struct xtc_proc_table *
__table_for(xtc_loop_t *loop, int create)
{
	int i;
	struct xtc_proc_table *t = NULL;

	/* Lock-free fast path: an existing loop's table is found by an
	 * acquire scan of the atomic slots -- no __lt_lock.  This is the
	 * path every cross-thread wake/send takes (the table already
	 * exists), so it must not serialize on the global lock. */
	for (i = 0; i < LOOP_TABLE_MAX; i++) {
		if (atomic_load_explicit(&__lt[i].loop,
		    memory_order_acquire) == loop) {
			return atomic_load_explicit(&__lt[i].tbl,
			    memory_order_acquire);
		}
	}
	if (!create) return NULL;

	/* Slow path: create + register a new table.  Serialized on
	 * __lt_lock (rare -- first proc spawned on a loop).  Re-scan under
	 * the lock in case another thread created it between our lock-free
	 * scan and acquiring the lock. */
	(void) __proc_mtx_lock(&__lt_lock);
	for (i = 0; i < LOOP_TABLE_MAX; i++) {
		if (atomic_load_explicit(&__lt[i].loop,
		    memory_order_relaxed) == loop) {
			t = atomic_load_explicit(&__lt[i].tbl,
			    memory_order_relaxed);
			goto out;
		}
	}
	if (__os_calloc(1, sizeof *t, (void **)&t) != XTC_OK)
		goto out;
	{
		unsigned s;
		for (s = 0; s < XTC_PT_NSTRIPES; s++)
			(void)pthread_mutex_init(&t->stripes[s], NULL);
	}
	t->inited = 1;
	for (i = 0; i < LOOP_TABLE_MAX; i++) {
		if (atomic_load_explicit(&__lt[i].loop,
		    memory_order_relaxed) == NULL) {
			/* Publish tbl BEFORE loop (release), so a lock-free
			 * reader that sees a non-NULL loop is guaranteed to
			 * then see the fully-initialized table, never NULL. */
			atomic_store_explicit(&__lt[i].tbl, t,
			    memory_order_release);
			atomic_store_explicit(&__lt[i].loop, loop,
			    memory_order_release);
			goto out;
		}
	}
	__os_free(t); t = NULL;
out:
	(void) __proc_mtx_unlock(&__lt_lock);
	return t;
}

static int
__table_alloc_slot(struct xtc_proc_table *t, struct xtc_proc *p,
                   uint16_t *out_local, uint32_t *out_gen)
{
	size_t i;
	int rc = XTC_OK;
	size_t new_cap, idx;
	struct xtc_proc_slot *ns = NULL;
	/* Spawn path: may grow (realloc) the slot array, so it claims ALL
	 * stripes -- no lookup/release on any local_id can be mid-flight
	 * while the array base moves.  Spawns are far rarer than sends, so
	 * serializing them is fine. */
	__pt_lock_all(t);
	for (i = 0; i < t->cap; i++) {
		if (t->slots[i].proc == NULL) {
			t->slots[i].proc = p;
			*out_local = (uint16_t)i;
			*out_gen   = ++t->slots[i].gen;
			t->n_used++;
			goto out;
		}
	}
	/* Grow. */
	new_cap = t->cap == 0 ? 16 : t->cap * 2;
	if (__os_realloc(t->slots, new_cap * sizeof *ns,
	    (void **)&ns) != XTC_OK || ns == NULL) {
		rc = XTC_E_NOMEM; goto out;
	}
	for (i = t->cap; i < new_cap; i++) {
		ns[i].proc = NULL;
		ns[i].gen = 0;
	}
	t->slots = ns;
	t->cap = new_cap;

	idx = t->n_used;     /* first new slot */
	t->slots[idx].proc = p;
	*out_local = (uint16_t)idx;
	*out_gen   = ++t->slots[idx].gen;
	t->n_used++;
out:
	__pt_unlock_all(t);
	return rc;
}

static void
__proc_free(struct xtc_proc *p);   /* final teardown; see __notify_links_and_monitors */

/* Drop a reference; free the struct when the last one goes.  Safe to
 * call from any thread. */
static void
__proc_release(struct xtc_proc *p)
{
	if (p == NULL) return;
	if (atomic_fetch_sub_explicit(&p->refs, 1, memory_order_acq_rel) == 1)
		__proc_free(p);
}

static struct xtc_proc *
__table_lookup(struct xtc_proc_table *t, uint16_t local_id, uint32_t gen)
{
	struct xtc_proc *p = NULL;
	unsigned st = __pt_stripe(local_id);
	(void) __proc_mtx_lock(&t->stripes[st]);
	if (local_id < t->cap &&
	    t->slots[local_id].proc != NULL &&
	    t->slots[local_id].gen == gen) {
		p = t->slots[local_id].proc;
		/* Take a ref WHILE this local_id's stripe is held.  The
		 * teardown path (__table_release) detaches the slot
		 * (proc = NULL) under the SAME stripe before dropping the
		 * owner ref, so a resolver either sees the live proc and pins
		 * it here, or sees NULL -- never a freed pointer in between.
		 * The caller must __proc_release when done. */
		atomic_fetch_add_explicit(&p->refs, 1, memory_order_relaxed);
	}
	(void) __proc_mtx_unlock(&t->stripes[st]);
	return p;
}

static void
__table_release(struct xtc_proc_table *t, uint16_t local_id)
{
	unsigned st = __pt_stripe(local_id);
	(void) __proc_mtx_lock(&t->stripes[st]);
	if (local_id < t->cap) {
		if (t->slots[local_id].proc != NULL) {
			t->slots[local_id].proc = NULL;
			t->n_used--;
		}
	}
	(void) __proc_mtx_unlock(&t->stripes[st]);
}

/* ---------- mailbox plumbing ---------- */

static void
__mbox_push_locked(struct xtc_proc *p, struct envelope *e)
{
	e->next = NULL;
	if (p->mbox_tail == NULL) p->mbox_head = p->mbox_tail = e;
	else { p->mbox_tail->next = e; p->mbox_tail = e; }
	p->mbox_n++;
	if (p->mbox_n > p->mbox_peak) {
		p->mbox_peak = p->mbox_n;
		/* xtc_tail MSG source: a new mailbox depth high-water -- the
		 * signal that a consumer is falling behind its producers. */
		__xtc_tail_emit(XTC_TAIL_MSG, XTC_TAIL_MBOX_HWM, p->pid,
		    (uint64_t)p->mbox_peak);
	}
	p->mbox_recv_total++;
}

static struct envelope *
__mbox_pop_locked(struct xtc_proc *p)
{
	struct envelope *e = p->mbox_head;
	if (e == NULL) return NULL;
	p->mbox_head = e->next;
	if (p->mbox_head == NULL) p->mbox_tail = NULL;
	p->mbox_n--;
	/* Drop the watermark latch once the backlog drains below the
	 * level, so a later refill fires the callback again. */
	if (p->mbox_wm_lvl > 0 && p->mbox_n < p->mbox_wm_lvl)
		p->mbox_wm_fired = 0;
	return e;
}

static int
__mbox_deliver_locked(struct xtc_proc *p, struct envelope *e)
{
	int armed;
	int wm_fire = 0;
	size_t wm_depth = 0, wm_cap = 0;
	xtc_waker_t wk = { 0 };
	(void) __proc_mtx_lock(&p->mbox_lock);
	/* Reject if proc is dead, or capped and full.
	 * Note: precedence bug fix -- the original used
	 *   if (!alive || cap > 0 ? mbox_n >= cap : 0)
	 * which parses as (!alive || cap>0) ? ... and produced
	 * surprising behaviour on platforms where alive timing
	 * differed.  Explicit parens. */
	if (!p->alive ||
	    (p->mbox_cap > 0 &&
	     (p->mbox_n +
	      atomic_load_explicit(&p->mbox_saved, memory_order_relaxed))
	     >= p->mbox_cap) ||
	    /* Buggify: under sim, when the "spurious_full" site is activated
	     * for this run (a once-per-run seeded gate), report the mailbox
	     * full on SOME sends (a fresh per-call seeded coin) even with
	     * room to spare.  Reporting XTC_E_AGAIN early is a LEGAL outcome
	     * (the cap is a soft backpressure limit), so this exercises every
	     * sender's drop/retry handling under the seeded schedule --
	     * FoundationDB's "smallest legal buffer" buggify.  The per-call
	     * coin (25%) keeps sends eventually succeeding so senders that
	     * retry make progress.  A no-op in production. */
	    (p->alive && XTC_SIM_BUGGIFY("proc.mbox.spurious_full") &&
	     xtc_sim_fault(250))) {
		p->mbox_drop_total++;
		(void) __proc_mtx_unlock(&p->mbox_lock);
		__env_free(e);
		return XTC_E_AGAIN;
	}
	/* Race window: the alive/capacity check passed but the envelope
	 * is not yet linked.  A test pauses a sender here, exits or
	 * fills the target, then releases to confirm the delivery still
	 * observes a consistent mailbox state under the lock. */
	XTC_INJECTION_POINT("proc.mbox.pre_push");
	__mbox_push_locked(p, e);
	/* Watermark rising edge: fire once when the depth first reaches
	 * the level; capture under the lock, invoke after releasing it
	 * (the callback must be free to call back into proc APIs). */
	if (p->mbox_wm_lvl > 0 && !p->mbox_wm_fired &&
	    p->mbox_n >= p->mbox_wm_lvl) {
		p->mbox_wm_fired = 1;
		wm_fire = (p->mbox_wm_fn != NULL);
		wm_depth = p->mbox_n;
		wm_cap = p->mbox_cap;
	}
	armed = p->waker_armed;
	/* Copy the waker OUT under the lock: xtc_waker_wake itself must run
	 * OUTSIDE mbox_lock (it may re-enter proc/loop APIs), but reading
	 * p->recv_waker after unlocking races the receiver re-arming it on
	 * its next recv cycle.  A local copy taken under the lock is
	 * race-free and the wake fires from the copy. */
	if (armed)
		wk = p->recv_waker;
	(void) __proc_mtx_unlock(&p->mbox_lock);
	if (wm_fire)
		p->mbox_wm_fn(p->pid, wm_depth, wm_cap, p->mbox_wm_user);
	if (armed) {
		/* Record the wake cause so xtc_proc_wait_fd / etc. can
		 * tell why we resumed. */
		if (p->task != NULL)
			atomic_fetch_or_explicit(&p->task->wake_revents,
			    XTC_WAIT_MAILBOX, memory_order_relaxed);
#if !XTC_DST_BUG(XTC_DST_BUG_LOSTWAKE)
		(void)xtc_waker_wake(&wk);
#endif   /* planted bug LOSTWAKE: drop this wake -> parked receiver hangs */
	}
	return XTC_OK;
}

/*
 * DST net-latency: a deferred cross-loop delivery.  The sim I/O backend
 * runs this on the TARGET loop's poll drain at now + a seeded latency
 * (enqueued by __mbox_deliver below).  It pushes into the mailbox via
 * __mbox_deliver_locked -- the identical production path -- so a delayed
 * message behaves exactly like an on-time one, just observed later in
 * virtual time.
 *
 * The target is captured by PID, not by pointer: between scheduling the
 * deferral and it firing (a virtual-time window the seeded schedule may
 * fill with anything) the target proc can EXIT -- normally, or killed by
 * xtc_exit_pid -- and be reaped/freed, its slot possibly reused.  So the
 * callback re-resolves the pid at fire time and DROPS the message (frees
 * the envelope) if the proc is gone; the generation check in __resolve
 * also rejects a reused slot.  Capturing a raw struct xtc_proc * here was
 * a use-after-free (surfaced by DST machine-death + net-latency: a
 * deferred delivery to a proc killed inside the deferral window).
 */
struct __mbox_deferred { xtc_pid_t pid; struct envelope *e; };

/* Forward decl (definition at __resolve below): the deferred callback
 * re-resolves the target by pid at fire time. */
static struct xtc_proc *__resolve(xtc_pid_t pid, xtc_loop_t **out);

static void
__mbox_deferred_run(void *arg)
{
	struct __mbox_deferred *d = arg;
	struct xtc_proc *p = __resolve(d->pid, NULL);
	if (p != NULL) {
		(void)__mbox_deliver_locked(p, d->e);
		__proc_release(p);
	} else
		__env_free(d->e);   /* target gone: drop the delayed message */
	__os_free(d);
}

static int
__mbox_deliver(struct xtc_proc *p, struct envelope *e)
{
	/*
	 * DST cross-loop network model (sim only; a single relaxed load
	 * gates it out of production).  Loops are identified by pid.loop_id
	 * (== exec_id + 1; 0 == standalone).  A partition cut DROPS the
	 * message via the sender's existing soft-full path (XTC_E_AGAIN); a
	 * net-latency window DEFERS the delivery to now + a seeded latency so
	 * delivery ORDER is part of the replayable schedule.  Off by default.
	 */
	if (__xtc_sim_active()) {
		xtc_loop_t *src = __xtc_current_loop;
		xtc_loop_t *dst = p->loop;
		int src_id = (src == NULL) ? -1
		    : (src->exec_id < 0 ? 0 : src->exec_id + 1);
		int dst_id = (dst == NULL) ? -1
		    : (dst->exec_id < 0 ? 0 : dst->exec_id + 1);
		int cross = (src != NULL && dst != NULL && src != dst);

		if (cross && __xtc_sim_partition_blocked(src_id, dst_id)) {
			/* Partitioned: drop like a soft-full mailbox.  The
			 * sender already handles XTC_E_AGAIN, so a partitioned
			 * peer never deadlocks the sim. */
			(void) __proc_mtx_lock(&p->mbox_lock);
			p->mbox_drop_total++;
			(void) __proc_mtx_unlock(&p->mbox_lock);
			__env_free(e);
			return XTC_E_AGAIN;
		}
		if (cross && dst->io != NULL) {
			int64_t lat = __xtc_sim_net_latency();
			if (lat > 0) {
				struct __mbox_deferred *d = NULL;
				if (__os_malloc(sizeof *d, (void **)&d) == XTC_OK) {
					int64_t now = 0;
					(void)__os_clock_mono(&now);
					d->pid = p->pid;
					d->e = e;
					if (__xtc_io_sim_defer_cb(dst->io,
					    now + lat, __mbox_deferred_run,
					    d) == XTC_OK)
						return XTC_OK;
					__os_free(d);
				}
				/* alloc/defer failed: deliver inline (correct,
				 * just not delayed). */
			}
		}
	}
	return __mbox_deliver_locked(p, e);
}

/* ---------- spawn entry trampoline ---------- */

/* Forward decls. */
static void __notify_links_and_monitors(struct xtc_proc *p);
static void __run_proc_at_exit(struct xtc_proc *p);
static void __recov_release_all(struct xtc_proc *p);

static intptr_t
__proc_entry(void *arg)
{
	struct xtc_proc *p = arg;
	int reason;
	__current_proc = p;

	/*
	 * Bind the proc to its task/coro HERE, not in xtc_proc_spawn.  On a
	 * cross-loop spawn the target loop may already be running, so this
	 * coroutine can begin executing the instant xtc_async enqueues it --
	 * before the spawning thread runs its next statement.  If the
	 * spawner set p->task afterward, two bugs followed: (1) this proc's
	 * own self->task uses (xtc_proc_sleep, recv, wait_fd) would read an
	 * unset p->task, and (2) a short-lived proc could run to completion
	 * and be reaped/freed before the spawner's "p->task = t", a
	 * use-after-free.  Self-binding from the running coroutine closes
	 * both: we are the task, and p is live for as long as we run.
	 */
	p->task = __xtc_current_task();
	p->coro = __xtc_current_coro;

	/* L1: apply the spawn-time scheduling class on THIS loop's thread
	 * (the class array is per-loop; the handle was created on this same
	 * loop by the consumer).  Recover the index and tag the task, so
	 * the run queue places it in the class from its first reschedule. */
	if (p->spawn_class != NULL && p->task != NULL &&
	    p->task->loop != NULL) {
		xtc_loop_t *l = p->task->loop;
		ptrdiff_t ci = p->spawn_class - &l->classes[0];
		if (ci >= 0 && ci < l->n_classes && l->classes[ci].in_use)
			p->task->sched_class = (int)ci;
	}

	/*
	 * Auto-arm a DEFAULT fault-recovery frame before running the body,
	 * so a contained fault ANYWHERE in the proc -- including in its very
	 * first statement, before the app calls xtc_proc_recovery_arm() --
	 * still unwinds this one proc and delivers a DOWN to its monitors,
	 * rather than escalating (and, as a carrier team observed, sometimes
	 * delivering no DOWN at all for an early fault).  The app's own
	 * xtc_proc_recovery_arm() simply re-arms this same frame with its
	 * custom cleanup; this default is the floor.  On a fault the handler
	 * siglongjmp's here with the (positive) signal number; we release
	 * the proc's tracked recovery resources and record the fault as the
	 * exit reason (the positive signal, consistent with an app that does
	 * xtc_exit_self(sig)), then fall through to the normal exit +
	 * monitor-notify path.  crit_depth still governs escalation: a fault
	 * inside a critical section is NOT caught here (recovery is gated on
	 * crit_depth == 0 in the handler), preserving PANIC semantics. */
	{
		int fsig = xtc_proc_recovery_arm();   /* POSIX sigsetjmp / Win CONTEXT */
		if (fsig != 0) {
			/* A contained fault fired the default frame. */
			p->recovery_armed = 0;
			__recov_release_all(p);
			p->exit_reason = fsig;   /* positive signal / exception code */
			p->exit_kind = 2;        /* XTC_DOWN_KIND_SIGNAL */
			goto proc_exit;
		}
	}

	if ((reason = setjmp(p->exit_jb)) == 0) {
		p->exit_jb_set = 1;
		p->fn(p->arg);
		p->exit_reason = 0;        /* normal */
		p->exit_kind = 0;          /* XTC_DOWN_KIND_CLEAN */
	} else {
		p->exit_reason = reason - 1;  /* offset so 0 is reachable */
		/* xtc_exit_self(0) is a clean exit; any nonzero code is an
		 * app EXIT status, kept in a field distinct from a signal so
		 * a monitor never confuses xtc_exit_self(1) with SIGHUP. */
		p->exit_kind = (p->exit_reason == 0) ? 0 : 1;
	}
	p->recovery_armed = 0;   /* past the body; no more auto-recovery */

proc_exit:

	p->alive = 0;
	/* Run the proc's at-exit callbacks (release locks, reset memory,
	 * close fds) before anyone observes the exit.  __current_proc is
	 * still this proc, and we are past the fault handler's longjmp,
	 * so callbacks run as ordinary code.  Done before notifying
	 * monitors so a replacement spawned in reaction sees no resource
	 * (e.g. a lock) still held by the dead proc. */
	__run_proc_at_exit(p);
	/* __notify_links_and_monitors frees p (releases the slot,
	 * destroys the mailbox lock, frees the struct), so snapshot the
	 * exit reason before the call -- reading p->exit_reason after it
	 * is a use-after-free. */
	reason = p->exit_reason;
	__xtc_tail_emit(XTC_TAIL_SCHED, XTC_TAIL_EXIT, p->pid,
	    (uint64_t)(unsigned)reason);
	__notify_links_and_monitors(p);

	__current_proc = NULL;
	return reason;
}

static int __proc_spawn_core(xtc_loop_t *loop, xtc_proc_fn fn, void *arg,
    const xtc_proc_opts_t *opts, xtc_pid_t *out_pid, int rel,
    uint64_t *out_ref);

/*
 * PUBLIC: int xtc_proc_spawn __P((xtc_loop_t *, xtc_proc_fn, void *, const xtc_proc_opts_t *, xtc_pid_t *));
 */
int
xtc_proc_spawn(xtc_loop_t *loop, xtc_proc_fn fn, void *arg,
               const xtc_proc_opts_t *opts, xtc_pid_t *out_pid)
{
	return __proc_spawn_core(loop, fn, arg, opts, out_pid, 0, NULL);
}

/*
 * Shared spawn core.  `rel` selects the parent<->child relationship
 * established ATOMICALLY, before the child is enqueued and can run:
 *   0 -- none (plain spawn)
 *   1 -- link (bidirectional fate, like Erlang spawn_link)
 *   2 -- monitor (unidirectional DOWN to the caller, like spawn_monitor)
 * For rel != 0 the caller MUST be a process (__current_proc != NULL);
 * the relationship is installed on both the child's and the parent's
 * lists while we still hold the child `p` and its slot, so there is no
 * window in which the child exists but is not yet linked/monitored --
 * closing the race that a spawn-then-link idiom leaves open.
 * out_ref (rel == 2) receives the monitor reference.
 */
static int
__proc_spawn_core(xtc_loop_t *loop, xtc_proc_fn fn, void *arg,
               const xtc_proc_opts_t *opts, xtc_pid_t *out_pid,
               int rel, uint64_t *out_ref)
{
	struct xtc_proc *p;
	struct xtc_proc_table *tbl;
	xtc_task_t *t;
	xtc_pid_t spawned_pid;
	struct xtc_proc *self = (rel != 0) ? __current_proc : NULL;
	uint16_t local;
	uint32_t gen;
	int rc;

	if (XTC_UNLIKELY(loop == NULL || fn == NULL)) return XTC_E_INVAL;
	/* link/monitor require a calling process to be the other end. */
	if (rel != 0 && self == NULL) return XTC_E_INVAL;

	/* Install the fiber-context preservation hooks so a yield/await
	 * inside a proc keeps __current_proc pointing at this proc on
	 * resume.  Idempotent: every spawn writes the same stable function
	 * addresses.  Done here (not at a global init) because the proc
	 * layer has no other mandatory entry point. */
	__xtc_fiber_ctx_save = __xtc_proc_ctx_save;
	__xtc_fiber_ctx_restore = __xtc_proc_ctx_restore;
	__xtc_fiber_kill_check = __xtc_proc_kill_check;
	__xtc_loop_fini_hook = __xtc_proc_loop_unregister;

	if (XTC_UNLIKELY((tbl = __table_for(loop, 1)) == NULL)) return XTC_E_NOMEM;

	if (XTC_UNLIKELY((rc = __os_calloc(1, sizeof *p, (void **)&p)) != XTC_OK))
		return rc;
	(void)pthread_mutex_init(&p->mbox_lock, NULL);
	p->loop = loop;
	p->fn = fn;
	p->arg = arg;
	p->alive = 1;
	p->spawn_class = (opts != NULL) ? opts->sched_class : NULL;
	atomic_store_explicit(&p->refs, 1, memory_order_relaxed);   /* owner ref */
	p->mbox_cap = (opts != NULL && opts->mailbox_cap > 0)
	    ? opts->mailbox_cap : 4096;
	/* Watermark level: a percent of cap (rounded down), clamped so a
	 * positive percent yields at least 1.  0 percent disables it. */
	if (opts != NULL && opts->mailbox_watermark_pct > 0 &&
	    opts->mailbox_watermark_pct <= 100) {
		p->mbox_wm_lvl = (p->mbox_cap * (size_t)
		    opts->mailbox_watermark_pct) / 100;
		if (p->mbox_wm_lvl == 0) p->mbox_wm_lvl = 1;
		p->mbox_wm_fn = opts->mailbox_watermark_fn;
		p->mbox_wm_user = opts->mailbox_watermark_user;
	}

	if ((rc = __table_alloc_slot(tbl, p, &local, &gen)) != XTC_OK) {
		(void)pthread_mutex_destroy(&p->mbox_lock);
		__os_free(p);
		return rc;
	}
	p->pid.loop_id  = (uint16_t)(loop->exec_id < 0 ? 0 : loop->exec_id + 1);
	p->pid.local_id = local;
	p->pid.gen      = gen;
	/* Capture the pid before the spawn: once xtc_async enqueues the
	 * task, p may be reaped on another thread, so reading p->pid for
	 * out_pid afterward would be a use-after-free. */
	spawned_pid = p->pid;

	/*
	 * ATOMIC link/monitor: establish the parent<->child relationship
	 * NOW, before xtc_async makes the child runnable.  We hold p and
	 * its slot, and self is the calling process; adding to both lists
	 * here means that even if the child runs and exits the instant it
	 * is enqueued, its __notify_links_and_monitors already sees the
	 * link/monitor and delivers the EXIT/DOWN -- no XTC_DOWN_NOPROC
	 * race, unlike spawn-then-link.  Best-effort on the entry allocs:
	 * a NULL alloc degrades to a one-sided or missing relationship
	 * (same failure mode as xtc_link), never a crash.
	 */
	if (rel == 1) {   /* link: symmetric on both link lists */
		struct link_entry *lc = __link_alloc();  /* on child */
		struct link_entry *lp = __link_alloc();  /* on parent */
		if (lc != NULL) {
			lc->peer = self->pid;
			lc->next = p->links;
			p->links = lc;
		}
		if (lp != NULL) {
			lp->peer = spawned_pid;
			lp->next = self->links;
			self->links = lp;
		}
	} else if (rel == 2) {   /* monitor: DOWN flows child -> parent */
		uint64_t ref = atomic_fetch_add_explicit(&__mon_ref_seq, 1,
		    memory_order_relaxed) + 1;
		struct mon_entry *mw = __mon_alloc();  /* on watcher (parent) */
		struct mon_entry *mt = __mon_alloc();  /* on target (child) */
		if (mw != NULL) {
			mw->ref = ref;
			mw->target = spawned_pid;
			mw->watcher = self->pid;
			mw->next = self->monitors;
			self->monitors = mw;
		}
		if (mt != NULL) {
			mt->ref = ref;
			mt->target = spawned_pid;
			mt->watcher = self->pid;
			mt->next = p->monitored_by;
			p->monitored_by = mt;
		}
		if (out_ref) *out_ref = ref;
	}

	/* Spawn the underlying coroutine.  Once xtc_async enqueues the
	 * task it may run immediately on another loop's thread, so we must
	 * NOT touch p afterward: the proc binds itself to its task/coro in
	 * __proc_entry (see there).  Touching p here would race a reap.
	 *
	 * pinned = !migratable: a migratable proc's coro goes on the
	 * stealable deque so an idle loop can rebalance it (Phase 1 wires
	 * the mechanism; the migration-safety guarantees are proven by the
	 * cross-loop-steal DST test).  Default (opts == NULL or
	 * migratable == 0) stays pinned -- byte-identical to prior
	 * behavior. */
	int pinned = (opts != NULL && opts->migratable) ? 0 : 1;
	if ((rc = __xtc_async_ex(loop, __proc_entry, p, pinned, &t)) != XTC_OK) {
		/* Undo the parent-side link/monitor we added above; the
		 * child-side entries die with p.  Remove by the child pid
		 * (spawned_pid) which never got to run. */
		if (rel == 1) {
			struct link_entry **pp = &self->links;
			while (*pp != NULL) {
				if (xtc_pid_eq((*pp)->peer, spawned_pid)) {
					struct link_entry *e = *pp;
					*pp = e->next; __link_free(e); break;
				}
				pp = &(*pp)->next;
			}
		} else if (rel == 2) {
			struct mon_entry **pp = &self->monitors;
			while (*pp != NULL) {
				if (xtc_pid_eq((*pp)->target, spawned_pid)) {
					struct mon_entry *e = *pp;
					*pp = e->next; __mon_free(e); break;
				}
				pp = &(*pp)->next;
			}
		}
		__table_release(tbl, local);
		(void)pthread_mutex_destroy(&p->mbox_lock);
		__os_free(p);
		return rc;
	}
	(void)t;   /* p->task / p->coro are set by __proc_entry, not here */

	__xtc_tail_emit(XTC_TAIL_SCHED, XTC_TAIL_SPAWN, spawned_pid, 0);
	if (out_pid) *out_pid = spawned_pid;
	return XTC_OK;
}

/* PUBLIC: int xtc_proc_spawn_link __P((xtc_loop_t *, xtc_proc_fn, void *, const xtc_proc_opts_t *, xtc_pid_t *)); */
int
xtc_proc_spawn_link(xtc_loop_t *loop, xtc_proc_fn fn, void *arg,
                    const xtc_proc_opts_t *opts, xtc_pid_t *out_pid)
{
	return __proc_spawn_core(loop, fn, arg, opts, out_pid, 1, NULL);
}

/* PUBLIC: int xtc_proc_spawn_monitor __P((xtc_loop_t *, xtc_proc_fn, void *, const xtc_proc_opts_t *, xtc_pid_t *, uint64_t *)); */
int
xtc_proc_spawn_monitor(xtc_loop_t *loop, xtc_proc_fn fn, void *arg,
                       const xtc_proc_opts_t *opts, xtc_pid_t *out_pid,
                       uint64_t *out_ref)
{
	return __proc_spawn_core(loop, fn, arg, opts, out_pid, 2, out_ref);
}

/* PUBLIC: xtc_pid_t xtc_self __P((void)); */
xtc_pid_t
xtc_self(void)
{
	return __current_proc != NULL ? __current_proc->pid : XTC_PID_NONE;
}

/* PUBLIC: int   xtc_proc_set_userdata __P((void *)); */
/* PUBLIC: void *xtc_proc_userdata __P((void)); */
/*
 * Consumer-owned opaque per-proc pointer, on the CALLING proc.  Set
 * once, read from any context on that proc; it rides with the proc
 * across work-stealing migration (it lives on the proc struct, like the
 * mailbox and __current_proc), so a migrated proc reading it always
 * sees its own value regardless of which loop it resumed on.  O(1),
 * allocation-free, lock-free (a plain field write/read on the calling
 * proc, which by definition is running on this thread).  The runtime
 * never dereferences or frees the pointer -- same contract as
 * xtc_proc_at_exit's arg.
 */
int
xtc_proc_set_userdata(void *ud)
{
	if (__current_proc == NULL)
		return XTC_E_INVAL;   /* not on a proc */
	__current_proc->userdata = ud;
	return XTC_OK;
}

void *
xtc_proc_userdata(void)
{
	return __current_proc != NULL ? __current_proc->userdata : NULL;
}

/* PUBLIC: int   xtc_proc_set_class __P((xtc_exec_class_t)); */
/*
 * L1 proportional-share scheduler: place the CALLING proc's task in the
 * scheduling class `cls`.  Recovers the class index as (cls -
 * loop->classes) and stores it on the task, which the run queue reads
 * in __queue_pop / __xtc_loop_enqueue.  The handle must belong to the
 * calling proc's loop (classes are per-loop); a foreign handle is
 * rejected.  A NULL handle resets the proc to the default (implicit)
 * class.  INSPIRED BY Glommio's task-queue placement.
 */
int
xtc_proc_set_class(xtc_exec_class_t cls)
{
	struct xtc_proc *p = __current_proc;
	xtc_loop_t *loop;
	ptrdiff_t idx;

	if (p == NULL || p->task == NULL)
		return XTC_E_INVAL;   /* not on a proc */
	loop = p->task->loop;
	if (cls == NULL) {
		p->task->sched_class = -1;
		return XTC_OK;
	}
	if (loop == NULL)
		return XTC_E_INVAL;
	idx = cls - &loop->classes[0];
	if (idx < 0 || idx >= loop->n_classes || !loop->classes[idx].in_use)
		return XTC_E_INVAL;   /* handle not on this proc's loop */
	p->task->sched_class = (int)idx;
	return XTC_OK;
}

/*
 * Save / restore the current-proc TLS across a yield performed by a
 * lower-level primitive (e.g. xtc_amutex parking the fiber).  When a
 * proc yields, the scheduler may run other procs that overwrite
 * __current_proc; on resume the primitive restores it so the proc
 * still sees itself.  Opaque void* so the sync layer need not know
 * the proc struct.
 *
 * PUBLIC: void *__xtc_proc_ctx_save __P((void));
 * PUBLIC: void  __xtc_proc_ctx_restore __P((void *));
 */
void *
__xtc_proc_ctx_save(void)
{
	return __current_proc;
}

void
__xtc_proc_ctx_restore(void *ctx)
{
	__current_proc = (struct xtc_proc *)ctx;
}

/*
 * Post-resume kill check (installed as __xtc_fiber_kill_check).  Called
 * from xtc_yield's resume point: if the current proc had a kill/cancel
 * requested while it was away from a cooperative point -- notably a
 * pure CPU loop that was involuntarily preempted, whose trampoline
 * yield resumes here -- honor it now by unwinding via xtc_exit_self.
 * This is what lets xtc_launch's deadline cancel a runaway fn: the
 * preemption slice yields, the launcher's xtc_exit_pid set kill_pending,
 * and on the fiber's next resume (which the slice guarantees) this
 * fires.  No-op when nothing is pending or off a proc.  Never called
 * inside a critical section (xtc_yield is not; and a kill inside one is
 * already deferred by the recovery gate).
 */
/*
 * Deliver a pending asynchronous kill to `self` at a yield/recv point,
 * respecting the A2 cancellation mask.  If kill_pending is set:
 *   - mask_depth == 0: unwind now via xtc_exit_self (does not return).
 *   - mask_depth  > 0: DEFER -- latch the reason into mask_deferred and
 *     return, so a resource acquired in a masked region can still
 *     register its release.  The latched kill fires when the mask drops
 *     to 0 (xtc_uncancelable / xtc_cancel_poll drain it).
 * Owner-only: called on self's own fiber, so mask_* need no atomics.
 */
static void
__xtc_proc_kill_deliver(struct xtc_proc *self)
{
	int kp;
	if (self == NULL)
		return;
	kp = atomic_load_explicit(&self->kill_pending, memory_order_acquire);
	if (kp == 0)
		return;
	if (self->mask_depth > 0) {
		if (self->mask_deferred == 0)
			self->mask_deferred = kp;   /* latch reason+1 */
		return;
	}
	xtc_exit_self(kp - 1);
}

static void
__xtc_proc_kill_check(void)
{
	__xtc_proc_kill_deliver(__current_proc);
}

/* ---------- A3: async causal trace ---------- */

/*
 * The single runtime toggle.  A relaxed atomic so the hot-path hook
 * (__xtc_trace_causal) is one relaxed load + branch when off; a default
 * build never records, so the per-fiber ring stays untouched.  Mirrors
 * the xtc_tail enable/disable discipline (observability must not tax
 * production).
 */
static _Atomic int __ct_enabled;   /* 0 = off (default), 1 = on */

/* PUBLIC: int xtc_trace_causal_enable __P((int)); */
int
xtc_trace_causal_enable(int on)
{
	return atomic_exchange_explicit(&__ct_enabled, on ? 1 : 0,
	    memory_order_release);
}

/*
 * Record one suspend/resume boundary on the calling proc's ring.  A
 * no-op fast path -- one relaxed load + branch -- when the trace is off
 * or we are not on a proc, so a disabled trace writes nothing on the
 * park/resume path.  Single-writer / core-private: called only on the
 * proc's own fiber, so the index bump + store need no atomic and cannot
 * race a reader's snapshot in a way that misreports (a reader may see a
 * torn window, never a corrupt struct).
 */
void
__xtc_trace_causal(int kind, const char *site)
{
	struct xtc_proc *self;

	if (atomic_load_explicit(&__ct_enabled, memory_order_relaxed) == 0)
		return;                 /* off: one branch, done */
	self = __current_proc;
	if (self == NULL)
		return;                 /* not on a proc */
	self->ct_ring[self->ct_seq % XTC_PROC_CAUSAL_RING].kind = kind;
	self->ct_ring[self->ct_seq % XTC_PROC_CAUSAL_RING].site = site;
	self->ct_seq++;
}

/* PUBLIC: int xtc_trace_causal_dump __P((xtc_pid_t, xtc_causal_fn, void *)); */
int
xtc_trace_causal_dump(xtc_pid_t pid, xtc_causal_fn cb, void *user)
{
	struct xtc_proc *p;
	xtc_causal_rec_t snap[XTC_PROC_CAUSAL_RING];
	unsigned seq, n, start, i;

	if (cb == NULL)
		return XTC_E_INVAL;
	p = __resolve(pid, NULL);
	if (p == NULL)
		return XTC_E_NOTFOUND;

	/*
	 * Snapshot the ring into a local copy, then release the proc
	 * ref and visit outside any hold (like xtc_inspect / xtc_tail).
	 * The writer is the proc's own fiber; a concurrent write can only
	 * make the window we copy slightly stale, never corrupt -- kind and
	 * site are word-sized and the ring is fixed.
	 */
	seq = p->ct_seq;
	n = seq < XTC_PROC_CAUSAL_RING ? seq : XTC_PROC_CAUSAL_RING;
	start = seq < XTC_PROC_CAUSAL_RING ? 0u
	    : seq % XTC_PROC_CAUSAL_RING;
	for (i = 0; i < n; i++) {
		snap[i].kind = p->ct_ring[(start + i) % XTC_PROC_CAUSAL_RING].kind;
		snap[i].site = p->ct_ring[(start + i) % XTC_PROC_CAUSAL_RING].site;
	}
	__proc_release(p);

	for (i = 0; i < n; i++)
		if (cb(&snap[i], user) != 0)
			break;
	return (int)n;
}

/* PUBLIC: int xtc_proc_sleep __P((int64_t)); */
int
xtc_proc_sleep(int64_t ns)
{
	struct xtc_proc *self = __current_proc;
	int64_t deadline = 0, now = 0;

	if (self == NULL)
		return XTC_E_INVAL;        /* not on a proc */
	if (ns <= 0)
		return XTC_OK;
	(void)__os_clock_mono(&deadline);
	deadline += ns;
	for (;;) {
		void *ctx;
		(void)__os_clock_mono(&now);
		if (now >= deadline)
			return XTC_OK;
		(void)xtc_task_park_on_timer(self->task, deadline - now);
		ctx = __xtc_proc_ctx_save();
		__xtc_trace_causal(XTC_CAUSAL_PARK_TIMER, __func__);
		xtc_yield();
		__xtc_proc_ctx_restore(ctx);
		__current_proc = self;
		__xtc_trace_causal(XTC_CAUSAL_RESUME, __func__);
		__xtc_proc_kill_deliver(self);
		/* Spurious / early wake: loop until the deadline. */
	}
}

/* PUBLIC: int xtc_proc_mailbox_stats __P((xtc_pid_t, xtc_mailbox_stats_t *)); */
static struct xtc_proc *__resolve(xtc_pid_t pid, xtc_loop_t **out);
int
xtc_proc_mailbox_stats(xtc_pid_t pid, xtc_mailbox_stats_t *out)
{
	struct xtc_proc *p;
	if (out == NULL || xtc_pid_is_none(pid))
		return XTC_E_INVAL;
	p = __resolve(pid, NULL);
	if (p == NULL)
		return XTC_E_INVAL;
	(void) __proc_mtx_lock(&p->mbox_lock);
	out->depth = p->mbox_n;
	out->saved = atomic_load_explicit(&p->mbox_saved,
	    memory_order_relaxed);
	out->peak = p->mbox_peak;
	out->cap = p->mbox_cap;
	out->recv_total = p->mbox_recv_total;
	out->drop_total = p->mbox_drop_total;
	(void) __proc_mtx_unlock(&p->mbox_lock);
	__proc_release(p);
	return XTC_OK;
}

/* ---------- send ---------- */

static struct xtc_proc *
__resolve(xtc_pid_t pid, xtc_loop_t **out_loop_for_send)
{
	xtc_loop_t *target_loop = NULL;
	struct xtc_proc_table *tbl;
	struct xtc_proc *p;

	/* Strategy 1: if we are running on a loop, prefer same-loop and
	 * exec-sibling lookups (cheapest). */
	if (__xtc_current_loop != NULL) {
		uint16_t my_id = (uint16_t)(__xtc_current_loop->exec_id < 0
		    ? 0 : __xtc_current_loop->exec_id + 1);
		if (pid.loop_id == my_id)
			target_loop = __xtc_current_loop;
		else if (__xtc_current_loop->exec != NULL) {
			struct xtc_exec *e = __xtc_current_loop->exec;
			int idx = (int)pid.loop_id - 1;
			if (idx >= 0 && idx < xtc_exec_n_loops(e))
				target_loop = xtc_exec_loop(e, idx);
		}
	}

	/*
	 * Strategy 2 (fallback for senders called from the main thread
	 * before xtc_loop_run, or a cross-thread wake to a loop outside
	 * the caller's exec): scan the global loop table for the loop
	 * whose encoded ID matches this pid.  LOCK-FREE: the slots are
	 * atomic, read with acquire loads (see struct lt_entry) -- this is
	 * the hot cross-thread-wake path and must not serialize on
	 * __lt_lock.  Linear in number of loops (<=64).
	 */
	if (target_loop == NULL) {
		int i;
		for (i = 0; i < LOOP_TABLE_MAX; i++) {
			xtc_loop_t *l = atomic_load_explicit(&__lt[i].loop,
			    memory_order_acquire);
			uint16_t lid;
			if (l == NULL) continue;
			lid = (uint16_t)(l->exec_id < 0 ? 0 : l->exec_id + 1);
			if (lid == pid.loop_id) {
				target_loop = l;
				break;
			}
		}
	}

	if (target_loop == NULL) return NULL;
	if (out_loop_for_send) *out_loop_for_send = target_loop;
	if ((tbl = __table_for(target_loop, 0)) == NULL) return NULL;
	p = __table_lookup(tbl, pid.local_id, pid.gen);
	return p;
}

/* PUBLIC: int xtc_send __P((xtc_pid_t, const void *, size_t)); */
int
xtc_send(xtc_pid_t to, const void *data, size_t size)
{
	struct xtc_proc *p;
	xtc_loop_t *target;
	struct envelope *e;
	int rc;

	if (XTC_UNLIKELY(size > 0 && data == NULL)) return XTC_E_INVAL;
	if (XTC_UNLIKELY(xtc_pid_is_none(to))) return XTC_E_INVAL;

	p = __resolve(to, &target);
	if (XTC_UNLIKELY(p == NULL)) return XTC_E_INVAL;
	/* p is pinned by the resolver ref; release on every exit below so a
	 * concurrent exit cannot free it mid-delivery. */
	if (XTC_UNLIKELY(!p->alive)) { __proc_release(p); return XTC_E_INVAL; }

	/* Guard against size_t overflow in the envelope allocation:
	 * a size near SIZE_MAX would wrap sizeof *e + size to a small
	 * value, malloc would succeed, and the memcpy below would
	 * overflow the heap.  Reject before allocating. */
	if (XTC_UNLIKELY(size > SIZE_MAX - sizeof *e)) {
		__proc_release(p); return XTC_E_INVAL;
	}

	e = __env_alloc(size);
	if (XTC_UNLIKELY(e == NULL)) { __proc_release(p); return XTC_E_NOMEM; }
	e->next = NULL;
	e->from = xtc_self();
	e->size = size;
	if (size > 0) memcpy(e->data, data, size);

	/* Causal tracing: stamp the envelope with the sender's HLC and
	 * record the send.  Off the hot path entirely when tracing is
	 * disabled (one relaxed load); the stamp is then left 0. */
	if (XTC_UNLIKELY(__trace_active())) {
		e->hlc = __hlc_tick();
		__trace_record(XTC_TRACE_SEND, e->from, to, e->hlc, 0,
		    (uint32_t)size);
	} else {
		e->hlc = 0;
	}

	/* xtc_tail MSG source: record the send (target pid, payload bytes).
	 * One relaxed mask load when the source is off. */
	__xtc_tail_emit(XTC_TAIL_MSG, XTC_TAIL_SEND, to, (uint64_t)size);

	rc = __mbox_deliver(p, e);
	__proc_release(p);
	return rc;
}

/* PUBLIC: int xtc_exit_pid __P((xtc_pid_t, int)); */
/*
 * Asynchronous, cross-process exit signal.  The target proc raises
 * an exit at its next yield/recv/wakeup point with the supplied
 * reason.  Idempotent: a second xtc_exit_pid before the first is
 * processed is a no-op (first one wins).
 *
 * This is the BEAM-style "kill" used by supervisors implementing
 * one_for_all and rest_for_one strategies, where the supervisor
 * needs to terminate sibling children that haven't crashed on
 * their own.
 */
int
xtc_exit_pid(xtc_pid_t target, int reason)
{
	struct xtc_proc *p;
	int expected = 0, desired;
	if (XTC_UNLIKELY(xtc_pid_is_none(target))) return XTC_E_INVAL;
	p = __resolve(target, NULL);
	if (XTC_UNLIKELY(p == NULL)) return XTC_E_INVAL;
	if (XTC_UNLIKELY(!p->alive)) { __proc_release(p); return XTC_E_INVAL; }

	/* Encode reason so 0 means "no kill pending".  Negative reasons
	 * are clamped to -1 so the encoded value stays nonzero. */
	desired = (reason == 0) ? 1 : reason + 1;
	(void)atomic_compare_exchange_strong_explicit(&p->kill_pending,
	    &expected, desired, memory_order_release, memory_order_relaxed);

	/* Best-effort: if the target is parked on its recv waker, wake
	 * it so it can observe the kill.  If it's runnable already this
	 * is a no-op. */
	(void) __proc_mtx_lock(&p->mbox_lock);
	if (p->waker_armed) {
		xtc_waker_wake(&p->recv_waker);
		p->waker_armed = 0;
	}
	(void) __proc_mtx_unlock(&p->mbox_lock);
	__proc_release(p);
	return XTC_OK;
}

/*
 * PUBLIC: int xtc_proc_wake __P((xtc_pid_t));
 *
 * Resume a process parked in xtc_proc_wait_fd / xtc_recv, from ANY OS
 * thread -- including a thread libxtc knows nothing about (an embedder's
 * I/O-worker thread, a foreign carrier).  This is the explicit "poke the
 * target loop" primitive: after a foreign thread does something that
 * makes a watched condition true (writes a self-pipe an fd-park watches,
 * sets an embedder latch, completes an async read), calling
 * xtc_proc_wake(pid) guarantees the target proc's loop is nudged out of
 * its I/O wait and the parked proc re-checks its condition.
 *
 * It fires the proc's receive waker, which -- when the caller is not on
 * the owning loop's thread -- posts to that loop's MPSC inbox and pings
 * its I/O backend (the wakeup fd), so a loop blocked in epoll_wait /
 * io_uring / kqueue / IOCP wait returns and re-polls.  It does NOT
 * deliver a message; the woken proc simply resumes and re-evaluates
 * (its wait_fd re-checks fd readiness, its recv re-checks the mailbox).
 * Spurious wakes are therefore always safe.
 *
 * Relying on fd readiness alone to wake a loop is correct for a
 * condition libxtc itself produces, but when the readiness is produced
 * by a fully foreign thread an embedder should pair it with
 * xtc_proc_wake for a guaranteed, race-free resume.  Returns XTC_OK
 * (including the no-op cases: target not parked, already runnable, or
 * gone), XTC_E_INVAL for XTC_PID_NONE.
 */
int
xtc_proc_wake(xtc_pid_t target)
{
	struct xtc_proc *p;

	if (XTC_UNLIKELY(xtc_pid_is_none(target)))
		return XTC_E_INVAL;
	p = __resolve(target, NULL);
	if (p == NULL) return XTC_OK;   /* gone: a wake is a harmless no-op */
	if (!p->alive) { __proc_release(p); return XTC_OK; }

	(void) __proc_mtx_lock(&p->mbox_lock);
	if (p->waker_armed) {
		/* Fire the waker without asserting a specific cause: the woken
		 * proc re-evaluates its own condition on resume (wait_fd
		 * re-checks fd readiness + the mailbox; recv re-checks the
		 * mailbox), so a spurious wake simply re-parks.  Setting a false
		 * MAILBOX/fd bit here would mislead out_revents. */
		xtc_waker_wake(&p->recv_waker);
	}
	(void) __proc_mtx_unlock(&p->mbox_lock);
	__proc_release(p);
	return XTC_OK;
}

/* ---------- receive ---------- */

static int
__match_first(const void *data, size_t size, void *u)
{
	(void)data; (void)size; (void)u;
	return 1;
}

static int
__do_recv(xtc_match_fn match, void *u, void **out, size_t *out_size,
          int64_t timeout_ns)
{
	struct xtc_proc *self = __current_proc;
	struct envelope *e, **link;
	int64_t deadline = -1;

	if (self == NULL) return XTC_E_INVAL;
	if (out == NULL || out_size == NULL) return XTC_E_INVAL;

	/* Defensive zero: the contract is that callers may pass
	 * uninitialised storage and check for non-NULL after a
	 * successful return.  Without this, a recv that returns
	 * XTC_E_AGAIN (timeout) would leave *out as garbage stack
	 * memory, and a caller doing `if (m) free(m)` would crash. */
	*out = NULL;
	*out_size = 0;

	/* Asynchronous kill check.  If another proc has signalled us via
	 * xtc_exit_pid, raise the exit now (longjmp) instead of receiving
	 * any more messages. */
	__xtc_proc_kill_deliver(self);

	if (timeout_ns >= 0) {
		int64_t now;
		(void)__os_clock_mono(&now);
		deadline = now + timeout_ns;
	}

	/* BEAM recv-mark: if the predicate has changed since the last
	 * call, invalidate the mark and re-test the whole save queue.
	 * If unchanged, we'll skip past `recv_mark` on the walk below. */
	if (self->last_match_fn != match || self->last_match_user != u) {
		self->last_match_fn = match;
		self->last_match_user = u;
		self->recv_mark = NULL;
	}

	for (;;) {
		struct envelope *skip_until = self->recv_mark;
		int past_mark = (skip_until == NULL);
		/* Walk save queue first.  Skip entries up to and including
		 * recv_mark (already tested with this predicate). */
		link = &self->save_head;
		while ((e = *link) != NULL) {
			if (!past_mark) {
				if (e == skip_until) past_mark = 1;
				link = &e->next;
				continue;
			}
			if (match(e->data, e->size, u)) {
				/* Unlink. */
				*link = e->next;
				atomic_fetch_sub_explicit(&self->mbox_saved, 1,
				    memory_order_relaxed);
				if (self->save_tail == e) {
					/* Walk to recompute tail.  O(N) but
					 * portable; the back-pointer trick using
					 * offsetof(envelope, next) was suspected of
					 * miscompilation on MinGW Windows. */
					struct envelope *t = self->save_head;
					if (t == NULL) self->save_tail = NULL;
					else {
						while (t->next != NULL) t = t->next;
						self->save_tail = t;
					}
				}
				goto deliver;
			}
			link = &e->next;
		}

		/* Pull from mailbox; for each, match or move to save. */
		(void) __proc_mtx_lock(&self->mbox_lock);
		for (;;) {
			e = __mbox_pop_locked(self);
			if (e == NULL) {
				/*
				 * Mailbox empty.  If we are about to park, arm the
				 * waker HERE, under the same lock that confirmed
				 * empty.  A sender delivering after this point takes
				 * mbox_lock, sees waker_armed, and wakes us.  Arming
				 * AFTER releasing the lock (as before) left a window
				 * in which a cross-thread sender pushed a message and
				 * saw waker_armed == 0, firing no waker -- the
				 * receiver then parked on an already-delivered
				 * message and stalled to its timeout.  Benign on a
				 * single loop (no concurrent sender in the gap),
				 * fatal to throughput across loops.
				 *
				 * Populate recv_waker HERE too, under the SAME lock,
				 * immediately before waker_armed = 1.  The wake side
				 * (__mbox_deliver) reads waker_armed under mbox_lock
				 * and only then, after unlocking, reads recv_waker to
				 * fire it; writing recv_waker before setting the flag
				 * (both under the lock) makes the write happen-before
				 * that read (release on our unlock, acquire on the
				 * sender's lock), so there is no data race on
				 * recv_waker.  (recv_waker's value is always the same
				 * fixed self->task/loop, so the old lockless write was
				 * benign in VALUE, but it was still a C11 data race
				 * TSan rightly flagged.)
				 */
				if (timeout_ns != 0) {
					(void)xtc_task_waker(self->task,
					    &self->recv_waker);
					self->waker_armed = 1;
				}
				(void) __proc_mtx_unlock(&self->mbox_lock);
				break;
			}
			(void) __proc_mtx_unlock(&self->mbox_lock);
			if (match(e->data, e->size, u))
				goto deliver;
			/* Append to save queue. */
			e->next = NULL;
			if (self->save_tail == NULL) self->save_head = self->save_tail = e;
			else { self->save_tail->next = e; self->save_tail = e; }
			atomic_fetch_add_explicit(&self->mbox_saved, 1,
			    memory_order_relaxed);
			(void) __proc_mtx_lock(&self->mbox_lock);
		}

		/* Update recv_mark: everything in save_queue has now been
		 * tested against this predicate.  Next call with the same
		 * predicate will skip past this point in the queue. */
		self->recv_mark = self->save_tail;

		/* Nothing to deliver.  Check timeout. */
		if (timeout_ns == 0) return XTC_E_AGAIN;

		if (deadline >= 0) {
			int64_t now;
			(void)__os_clock_mono(&now);
			if (now >= deadline) return XTC_E_AGAIN;
			(void)xtc_task_park_on_timer(self->task, deadline - now);
		} else {
			/* Infinite wait: park until a sender's waker
			 * re-enqueues us, rather than busy-rescheduling and
			 * burning a core (and starving same-loop peers).
			 * waker_armed is set above under mbox_lock, so a
			 * delivery cannot be lost against this park. */
			self->task->park_requested = 1;
		}
		{
			/* xtc_tail SCHED: record the park, and on resume the
			 * park->run latency (the off-CPU-while-blocked signal
			 * that exposes lost/late wakeups and long stalls).  Gated
			 * on the source being enabled so a disabled tail is one
			 * branch and reads no clock -- keeping the sim recv path
			 * side-effect-free by default. */
			int __tail_sched = __xtc_tail_on(XTC_TAIL_SCHED);
			int64_t __park_ns = 0;
			if (__tail_sched) {
				__xtc_tail_emit(XTC_TAIL_SCHED, XTC_TAIL_PARK,
				    self->pid, 0);
				(void)__os_clock_mono(&__park_ns);
			}
			__xtc_trace_causal(XTC_CAUSAL_PARK_MAILBOX, __func__);
			xtc_yield();
			__xtc_trace_causal(XTC_CAUSAL_RESUME, __func__);
			if (__tail_sched) {
				int64_t __run_ns = 0;
				(void)__os_clock_mono(&__run_ns);
				__xtc_tail_emit(XTC_TAIL_SCHED, XTC_TAIL_RUN,
				    self->pid,
				    (uint64_t)(__run_ns > __park_ns ?
				    __run_ns - __park_ns : 0));
			}
		}
		/*
		 * On resume we may have run inside another proc's fiber
		 * (which clobbered __current_proc).  Restore our pointer
		 * so post-yield code continues to see itself.
		 */
		__current_proc = self;

		/* Re-check kill flag after yielding back. */
		__xtc_proc_kill_deliver(self);

		(void) __proc_mtx_lock(&self->mbox_lock);
		self->waker_armed = 0;
		(void) __proc_mtx_unlock(&self->mbox_lock);
	}

deliver:
	/* Cancel any pending recv timer.  Safe no-op if it never armed
	 * or has already fired.  Without this, a recv that was woken by
	 * the waker leaves a stale timer in the heap, keeping the loop
	 * alive until the deadline (a needless wait of up to timeout_ns). */
	__xtc_task_cancel_park_timer(self->task);
	{
		void *buf = NULL;
		if (e->size > 0) {
			/* Handed to the caller as *out; the recv contract says free
			 * it with xtc_free (== __os_free == the installed alloc
			 * hook), so it MUST be __os_malloc'd or an embedder with a
			 * custom allocator hits a mismatched free. */
			if (__os_malloc(e->size, &buf) != XTC_OK) {
				/* Put it back at the head of save queue
				 * to preserve ordering. */
				e->next = self->save_head;
				self->save_head = e;
				if (self->save_tail == NULL) self->save_tail = e;
				atomic_fetch_add_explicit(&self->mbox_saved, 1,
				    memory_order_relaxed);
				return XTC_E_NOMEM;
			}
			memcpy(buf, e->data, e->size);
		}
		*out = buf;
		*out_size = e->size;
		/* Causal tracing: advance past the sender's stamp and record
		 * the receive, linking back to the send via `cause`. */
		if (XTC_UNLIKELY(__trace_active())) {
			uint64_t rs = __hlc_update(e->hlc);
			__trace_record(XTC_TRACE_RECV, self->pid, e->from, rs,
			    e->hlc, (uint32_t)e->size);
		}
		/* xtc_tail MSG source: record the receive (receiver pid, bytes). */
		__xtc_tail_emit(XTC_TAIL_MSG, XTC_TAIL_RECV, self->pid,
		    (uint64_t)e->size);
		__env_free(e);
	}
	return XTC_OK;
}

/* PUBLIC: int xtc_recv __P((void **, size_t *, int64_t)); */
int
xtc_recv(void **out, size_t *out_size, int64_t timeout_ns)
{
	/* Wrapper around __do_recv with the always-match predicate.
	 * Annotated XTC_MUSTTAIL so the compiler emits a jmp, not a
	 * call+ret -- keeps the recv fast path single-frame. */
	return XTC_MUSTTAIL __do_recv(__match_first, NULL, out, out_size,
	    timeout_ns);
}

/* PUBLIC: int xtc_recv_match __P((xtc_match_fn, void *, void **, size_t *, int64_t)); */
int
xtc_recv_match(xtc_match_fn fn, void *u, void **out, size_t *out_size,
               int64_t timeout_ns)
{
	if (XTC_UNLIKELY(fn == NULL)) return XTC_E_INVAL;
	/* XTC_MUSTTAIL: delegate to __do_recv as a tail call. */
	return XTC_MUSTTAIL __do_recv(fn, u, out, out_size, timeout_ns);
}

/* Predicate context for xtc_recv_correlate. */
struct corr_ctx {
	const unsigned char *corr;
	size_t               corr_size;
};

static int
__corr_match(const void *data, size_t size, void *u)
{
	struct corr_ctx *c = u;
	if (c == NULL || size < c->corr_size) return 0;
	return memcmp(data, c->corr, c->corr_size) == 0;
}

/* PUBLIC: int xtc_recv_correlate __P((const void *, size_t, int, xtc_msg_t *, int *, int64_t)); */
int
xtc_recv_correlate(const void *corr_value, size_t corr_size,
                   int n_expected, xtc_msg_t *out_msgs,
                   int *out_n, int64_t timeout_ns)
{
	struct corr_ctx ctx;
	int64_t deadline = -1;
	int collected = 0;
	int rc;

	if (corr_value == NULL || corr_size == 0 ||
	    n_expected <= 0 || out_msgs == NULL || out_n == NULL)
		return XTC_E_INVAL;

	ctx.corr = (const unsigned char *)corr_value;
	ctx.corr_size = corr_size;
	*out_n = 0;

	if (timeout_ns >= 0) {
		int64_t now;
		(void)__os_clock_mono(&now);
		deadline = now + timeout_ns;
	}

	while (collected < n_expected) {
		int64_t per_call_to = -1;
		if (deadline >= 0) {
			int64_t now;
			(void)__os_clock_mono(&now);
			if (now >= deadline) break;
			per_call_to = deadline - now;
		}
		rc = __do_recv(__corr_match, &ctx,
		    &out_msgs[collected].data,
		    &out_msgs[collected].size,
		    per_call_to);
		if (rc == XTC_OK) {
			collected++;
		} else {
			/* AGAIN (timeout) or other error: stop. */
			break;
		}
	}

	*out_n = collected;
	return (collected == n_expected) ? XTC_OK : XTC_E_AGAIN;
}

/* PUBLIC: int xtc_proc_wait_fd __P((int, uint32_t, int64_t, uint32_t *)); */
int
xtc_proc_wait_fd(int fd, uint32_t interest, int64_t timeout_ns,
                 uint32_t *out_revents)
{
	struct xtc_proc *self = __current_proc;
	uint32_t revents;
	int had_timer = 0;
	int had_fd = 0;
	xtc_loop_t *wl = NULL;   /* the loop this fiber runs on (not its home) */

	if (out_revents == NULL || fd < 0 || interest == 0) return XTC_E_INVAL;
	if (self == NULL) return XTC_E_INVAL;

	*out_revents = 0;

	/* Check kill-pending up front (same convention as xtc_recv). */
	__xtc_proc_kill_deliver(self);

	/* Fast path: if a message is already queued or the fd is already
	 * ready, just return without yielding.  We can answer the mailbox
	 * question without an actual recv call by peeking the queue. */
	(void) __proc_mtx_lock(&self->mbox_lock);
	if (self->mbox_n > 0 || self->save_head != NULL) {
		*out_revents |= XTC_WAIT_MAILBOX;
	}
	(void) __proc_mtx_unlock(&self->mbox_lock);
	if (*out_revents & XTC_WAIT_MAILBOX) return XTC_OK;

	/* Slow path: arm the recv waker, register the fd, optionally
	 * arm a timeout timer, then yield.  We bypass
	 * xtc_task_park_on_fd / _on_timer because those wrappers reject
	 * having both set; for wait_fd we need fd + timer + waker
	 * simultaneously. */
	atomic_store_explicit(&self->task->wake_revents, 0,
	    memory_order_relaxed);

	/*
	 * Register on the loop this fiber is RUNNING on, not its home loop.
	 * Under the multi-loop executor a proc can run stolen on another
	 * loop, and self->task->loop is the (fixed) home loop; submitting to
	 * its io_uring ring from this thread would race that ring's single
	 * producer and silently drop the POLL_ADD -- a lost wakeup.  The
	 * completion is reaped + dispatched on this same loop, which deletes
	 * park_fd before waking, so the post-yield cleanup never touches
	 * another thread's ring either. */
	wl = __xtc_current_loop != NULL ? __xtc_current_loop : self->task->loop;

	if (xtc_io_reg_fd(wl->io, fd, interest,
	    self->task) != XTC_OK)
		return XTC_E_INTERNAL;
	self->task->park_fd = fd;
	had_fd = 1;

	if (timeout_ns >= 0) {
		/* Inline timer registration matching xtc_task_park_on_timer
		 * but without the mutual-exclusion check. */
		xtc_timer_t *t = NULL;
		int64_t now_ns = 0;
		int trc = __os_calloc(1, sizeof(*t), (void **)&t);
		if (trc != XTC_OK || t == NULL ||
		    __os_clock_mono(&now_ns) != XTC_OK) {
			if (t) __os_free(t);
			(void)xtc_io_del_fd(wl->io, fd);
			self->task->park_fd = -1;
			return XTC_E_INTERNAL;
		}
		t->deadline_ns = now_ns + timeout_ns;
		t->cb = NULL;
		t->user = NULL;
		t->waiter = self->task;
		t->heap_idx = -1;
		t->cancelled = 0;
		t->fired = 0;
		t->loop = wl;
		if (__xtc_timer_heap_push(wl, t) != XTC_OK) {
			__os_free(t);
			(void)xtc_io_del_fd(wl->io, fd);
			self->task->park_fd = -1;
			return XTC_E_INTERNAL;
		}
		t->all_next = wl->all_timers;
		wl->all_timers = t;
		self->task->park_timer = t;
		had_timer = 1;
	}

	(void) __proc_mtx_lock(&self->mbox_lock);
	/* Populate recv_waker + set waker_armed together under mbox_lock so
	 * the cross-thread wake (which reads waker_armed under this lock,
	 * then reads recv_waker after unlocking) sees a fully-written
	 * recv_waker with no data race -- see the matching note in
	 * __do_recv's park point. */
	(void)xtc_task_waker(self->task, &self->recv_waker);
	self->waker_armed = 1;
	(void) __proc_mtx_unlock(&self->mbox_lock);

	__xtc_trace_causal(XTC_CAUSAL_PARK_FD, __func__);
	xtc_yield();
	/* Restore __current_proc -- another fiber may have clobbered it. */
	__current_proc = self;
	__xtc_trace_causal(XTC_CAUSAL_RESUME, __func__);

	(void) __proc_mtx_lock(&self->mbox_lock);
	self->waker_armed = 0;
	(void) __proc_mtx_unlock(&self->mbox_lock);

	/* Re-check kill-pending after yielding back. */
	__xtc_proc_kill_deliver(self);

	/* Sample wake_revents.  The dispatcher / mbox_deliver / timer cb
	 * have set the bits we care about. */
	revents = atomic_load_explicit(&self->task->wake_revents,
	    memory_order_relaxed);
	atomic_store_explicit(&self->task->wake_revents, 0,
	    memory_order_relaxed);

	/* Cleanup: unregister fd if still parked, cancel timer.  Use wl (the
	 * loop we registered on), not the home loop.  If this fiber was woken
	 * via a NON-fd path (timeout / xtc_proc_wake / mailbox -- which do
	 * not clear park_fd, unlike the fd-completion dispatch) and then
	 * work-stolen, it is now resuming on a DIFFERENT OS thread than wl's
	 * owner.  Calling xtc_io_del_fd on wl->io from here would race wl's
	 * single-owner fd registry AND its single-producer SQ ring (the
	 * native-path concurrent-commit collapse, TSan-caught 2026-08-30).
	 * When we have migrated off wl, defer the unregister to wl's owning
	 * thread; when still on wl (the common case), do it directly. */
	if (self->task->park_fd >= 0) {
		extern int __xtc_io_defer_del_fd(xtc_io_t *, int);
		(void)had_fd;   /* documents intent: an fd was registered */
		/*
		 * ALWAYS route the unregister to wl->io's owning loop thread,
		 * never inline.  __xtc_current_loop is the fiber's LOGICAL loop
		 * binding, which is preserved across a work-steal migration
		 * (the coro carries it) -- so __xtc_current_loop == wl does NOT
		 * imply we are on wl's physical OS thread; a migrated fiber
		 * resuming on a peer thread still reads __xtc_current_loop ==
		 * wl.  Comparing them to decide "safe to del inline" is wrong
		 * (TSan showed one io mutated inline from 6 distinct threads).
		 * The only thread that may touch wl->io's fd registry + SQ ring
		 * is wl's own poll thread, so defer: __xtc_io_defer_del_fd
		 * queues the fd and nudges wl, and wl performs the real
		 * unregister when it next drains in xtc_io_poll.  For a backend
		 * whose registry is kernel-synchronized (epoll) the defer is a
		 * safe passthrough. */
		(void)__xtc_io_defer_del_fd(wl->io, self->task->park_fd);
		self->task->park_fd = -1;
	}
	if (had_timer && self->task->park_timer != NULL) {
		(void)xtc_timer_cancel(self->task->park_timer);
		self->task->park_timer = NULL;
	}

	/* Check the mailbox again -- a message may have arrived without
	 * tripping the waker race-window. */
	(void) __proc_mtx_lock(&self->mbox_lock);
	if (self->mbox_n > 0 || self->save_head != NULL) {
		revents |= XTC_WAIT_MAILBOX;
	}
	(void) __proc_mtx_unlock(&self->mbox_lock);

	*out_revents = revents;

	/* Decide return code: if only timeout fired, return XTC_E_AGAIN. */
	if ((revents & ~(uint32_t)XTC_WAIT_TIMEOUT) == 0 && timeout_ns >= 0)
		return XTC_E_AGAIN;
	return XTC_OK;
}

/* ---------- exit / link / monitor ---------- */

/* PUBLIC: int xtc_exit_self __P((int)); */
int
xtc_exit_self(int reason)
{
	struct xtc_proc *self = __current_proc;
	if (self == NULL || !self->exit_jb_set) return XTC_E_INVAL;
	longjmp(self->exit_jb, reason + 1);
	/* NOTREACHED */
	return XTC_OK;
}

/* ---------- R1: per-fiber fault containment ---------- */

static XTC_THREAD_LOCAL xtc_recovery_buf_t __recovery_dummy;

#if !defined(_WIN32)
static volatile sig_atomic_t __fault_guard_installed;

/* The synchronous, thread-directed, fiber-attributable faults we can
 * contain: a bad pointer, a misaligned/again bad access, an
 * arithmetic trap (integer divide-by-zero), and an illegal
 * instruction.  SIGABRT is deliberately excluded -- abort() is an
 * intentional, usually-unrecoverable signal. */
static const int __fault_signals[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL };

/*
 * Process-wide fault handler.  The faulting coroutine is the faulting
 * thread's current proc (__current_proc is thread-local and set while
 * a proc runs), so we identify it with no extra bookkeeping.  If that
 * proc armed a recovery frame and is not inside a critical section we
 * contain the fault by siglongjmp'ing back to the frame
 * (async-signal-safe); the proc then runs its cleanup and
 * xtc_exit_self, delivering DOWN to its monitors.  Otherwise the
 * fault escalates: restore the default disposition and re-raise so
 * the whole process dies (a torn critical section cannot be unwound).
 *
 * Async-signal-safety of the handler->siglongjmp window.  Every
 * operation here is async-signal-safe (POSIX.1 "Signal Concepts"):
 * reads of __current_proc (a thread-local pointer) and of the proc's
 * recovery_armed / crit_depth flags; writes of recovery_armed (the
 * one-shot clear) and fault_sig -- these flags are volatile
 * sig_atomic_t, so each access is single and untearable, and the proc
 * is the only other writer (a synchronous fault interrupts the proc's
 * own instruction stream, so no second agent is mutating them at
 * fault time); siglongjmp(), which is on the async-signal-safe list;
 * and on the escalation path memset() (pure computation),
 * sigemptyset(), sigaction(), and raise() -- all async-signal-safe.
 * No heap, stdio, or locking is touched.  The proc's cleanup (at-exit
 * hooks, lock release, xtc_exit_self) runs AFTER the siglongjmp, in
 * normal context -- never in the handler.  SA_NODEFER keeps a fault
 * that re-enters before the siglongjmp from looping: recovery_armed
 * is already cleared, so it escalates.
 */
static void
__xtc_fault_handler(int sig, siginfo_t *si, void *uctx)
{
	struct xtc_proc *p = __current_proc;
	struct xtc_coro *c = __xtc_current_coro;
	int stack_overflow = 0;
	(void)uctx;

	/*
	 * Stack-overflow detection.  A fault whose address lies in (or just
	 * below) the running fiber's guard page is a stack overflow: the
	 * guard is gone and the stack is unusable, so we must NOT contain
	 * it (siglongjmp/cleanup would run on the broken stack) -- it
	 * escalates like a critical-section fault.  Without this, the
	 * default recovery frame auto-armed in __proc_entry would wrongly
	 * swallow a genuine stack overflow.  The guard is the first page of
	 * the fiber's mmap (c->stack .. c->stack + one page); a fault at or
	 * just below it (within a page, covering a red-zone probe) is an
	 * overflow.  Only SIGSEGV/SIGBUS carry a meaningful si_addr here. */
	if (c != NULL && c->stack != NULL && si != NULL &&
	    (sig == SIGSEGV || sig == SIGBUS)) {
		long pg = sysconf(_SC_PAGESIZE);
		uintptr_t page = (pg > 0) ? (uintptr_t)pg : 4096u;
		uintptr_t guard_lo = (uintptr_t)c->stack;
		uintptr_t guard_hi = guard_lo + page;
		uintptr_t fa = (uintptr_t)si->si_addr;
		if (fa >= guard_lo - page && fa < guard_hi)
			stack_overflow = 1;
	}

	if (!stack_overflow && p != NULL && p->recovery_armed &&
	    p->crit_depth == 0) {
		/* One-shot: a fault during recovery/cleanup must escalate
		 * rather than loop back here. */
		p->recovery_armed = 0;
		p->fault_sig = sig;
		siglongjmp(p->recovery_buf, sig);
		/* NOTREACHED */
	}
	/* Escalate to process abort with the default disposition. */
	{
		struct sigaction sa;
		memset(&sa, 0, sizeof sa);
		sa.sa_handler = SIG_DFL;
		(void)sigemptyset(&sa.sa_mask);
		(void)sigaction(sig, &sa, NULL);
	}
	(void)raise(sig);
}
#endif /* !_WIN32 */

#if defined(_WIN32)
#include <windows.h>
static PVOID                 __veh_handle;
static volatile sig_atomic_t __fault_guard_installed_win;

/*
 * Windows fault containment via a Vectored Exception Handler -- the
 * SEH analogue of the POSIX signal path.  The VEH runs on the
 * faulting thread before SEH dispatch, so __current_proc still names
 * the faulting fiber.  For a synchronous, fiber-attributable hardware
 * fault (the parity set of the POSIX SIGSEGV/SIGBUS/SIGFPE/SIGILL) we
 * RESTORE the CONTEXT the proc captured at xtc_proc_recovery_arm()
 * and return EXCEPTION_CONTINUE_EXECUTION: the OS reloads the thread
 * registers and resumes at the capture point, where the arm helper
 * sees recovery_fired set and reports the fault code.  This does no
 * stack unwinding (unlike longjmp, which corrupts the CRT when driven
 * from a VEH on a fiber stack).
 *
 * STACK_OVERFLOW is excluded (the guard page is gone; resuming is
 * unsafe) and escalates, as does any fault with no armed frame or
 * inside a critical section -- EXCEPTION_CONTINUE_SEARCH preserves the
 * process's default crash disposition (PG's PANIC).
 */
static LONG CALLBACK
__xtc_veh(EXCEPTION_POINTERS *ep)
{
	struct xtc_proc *p;
	DWORD code;

	if (ep == NULL || ep->ExceptionRecord == NULL ||
	    ep->ContextRecord == NULL)
		return EXCEPTION_CONTINUE_SEARCH;
	code = ep->ExceptionRecord->ExceptionCode;
	switch (code) {
	case EXCEPTION_ACCESS_VIOLATION:
	case EXCEPTION_DATATYPE_MISALIGNMENT:
	case EXCEPTION_IN_PAGE_ERROR:
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
	case EXCEPTION_FLT_DIVIDE_BY_ZERO:
	case EXCEPTION_ILLEGAL_INSTRUCTION:
	case EXCEPTION_PRIV_INSTRUCTION:
		break;
	default:
		return EXCEPTION_CONTINUE_SEARCH;   /* not a contained fault */
	}

	p = __current_proc;
	if (p != NULL && p->recovery_armed && p->crit_depth == 0) {
		p->recovery_armed = 0;          /* one-shot */
		p->fault_sig = (int)code;
		p->recovery_fired = 1;          /* arm helper reads this */
		/* Resume at the captured arm point: no unwind, just a
		 * register/stack-pointer reload by the kernel. */
		*ep->ContextRecord = p->recovery_buf.ctx;
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	return EXCEPTION_CONTINUE_SEARCH;       /* escalate */
}
#endif /* _WIN32 */

/* PUBLIC: int xtc_fault_guard_install __P((void)); */
int
xtc_fault_guard_install(void)
{
#if defined(_WIN32)
	/* Containment via a Vectored Exception Handler (SEH).  Process-
	 * wide and installed once; no per-thread alt stack is needed
	 * because the VEH runs on the faulting thread and longjmps from
	 * there. */
	if (__fault_guard_installed_win)
		return XTC_OK;
	__veh_handle = AddVectoredExceptionHandler(1 /* call first */,
	    __xtc_veh);
	if (__veh_handle == NULL)
		return XTC_E_INTERNAL;
	__fault_guard_installed_win = 1;
	return XTC_OK;
#else
	struct sigaction sa;
	stack_t ss;
	size_t i;
	/* 64 KiB is ample for a handler that only siglongjmps; a fixed
	 * size avoids SIGSTKSZ (no longer a compile-time constant on
	 * recent glibc).  Thread-local so each loop thread that installs
	 * the guard gets its own stack. */
	static XTC_THREAD_LOCAL char altstack[65536];

	memset(&ss, 0, sizeof ss);
	ss.ss_sp = altstack;
	ss.ss_size = sizeof altstack;
	ss.ss_flags = 0;
	(void)sigaltstack(&ss, NULL);

	if (__fault_guard_installed)
		return XTC_OK;          /* process-wide sigaction: once */

	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = __xtc_fault_handler;
	(void)sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
	for (i = 0; i < sizeof __fault_signals / sizeof __fault_signals[0];
	    i++) {
		if (sigaction(__fault_signals[i], &sa, NULL) != 0)
			return XTC_E_INTERNAL;
	}
	__fault_guard_installed = 1;
	return XTC_OK;
#endif
}

/*
 * Arm-slot helper for the xtc_proc_recovery_arm() macro: marks the
 * calling proc's recovery frame armed and returns it, so the macro's
 * sigsetjmp records the caller's frame.  Off a proc it returns a
 * thread-local throwaway (the macro stays well-formed; nothing arms).
 */
xtc_recovery_buf_t *
__xtc_proc_recovery_slot(void)
{
	struct xtc_proc *p = __current_proc;
	if (p == NULL)
		return &__recovery_dummy;
	p->recovery_armed = 1;
	return &p->recovery_buf;
}

#if defined(_WIN32)
/* Windows recovery-arm helpers driving the CONTEXT capture/restore.
 * __xtc_recovery_prep arms and clears the fired flag; __xtc_recovery_ctx
 * is the CONTEXT to RtlCaptureContext into; __xtc_recovery_result is 0
 * on the arming pass and the fault code once the VEH restores the
 * captured context (which resumes execution just after the capture). */
void
__xtc_recovery_prep(void)
{
	struct xtc_proc *p = __current_proc;
	if (p == NULL)
		return;
	p->recovery_fired = 0;
	p->recovery_armed = 1;
}

CONTEXT *
__xtc_recovery_ctx(void)
{
	struct xtc_proc *p = __current_proc;
	if (p == NULL)
		return &__recovery_dummy.ctx;
	return &p->recovery_buf.ctx;
}

int
__xtc_recovery_result(void)
{
	struct xtc_proc *p = __current_proc;
	if (p == NULL)
		return 0;
	return p->recovery_fired ? (int)p->fault_sig : 0;
}
#endif /* _WIN32 */

/* PUBLIC: void xtc_proc_recovery_disarm __P((void)); */
void
xtc_proc_recovery_disarm(void)
{
	if (__current_proc != NULL)
		__current_proc->recovery_armed = 0;
}

/* PUBLIC: void xtc_proc_critical_enter __P((void)); */
void
xtc_proc_critical_enter(void)
{
	if (__current_proc != NULL)
		__current_proc->crit_depth++;
}

/* PUBLIC: void xtc_proc_critical_leave __P((void)); */
void
xtc_proc_critical_leave(void)
{
	if (__current_proc != NULL && __current_proc->crit_depth > 0)
		__current_proc->crit_depth--;
}

/* PUBLIC: int __xtc_proc_crit_depth __P((void)); */
/* The current proc's critical-section depth, or 0 off a proc.  Read from
 * the preemption timer signal handler to decide whether a signal-context
 * involuntary yield is safe (a proc inside a critical section must not
 * be preempted -- same rule the fault handler uses).  A plain read of a
 * sig_atomic_t field, async-signal-safe. */
int
__xtc_proc_crit_depth(void)
{
	return __current_proc != NULL ? (int)__current_proc->crit_depth : 0;
}

/* ---------- per-proc at-exit hooks + proc-scoped memory ---------- */

/* PUBLIC: int xtc_proc_at_exit __P((void (*)(void *), void *)); */
int
xtc_proc_at_exit(void (*fn)(void *), void *arg)
{
	struct xtc_proc *p = __current_proc;
	if (p == NULL || fn == NULL)
		return XTC_E_INVAL;
	if (p->n_at_exit >= XTC_PROC_MAX_ATEXIT)
		return XTC_E_RESOURCE;
	p->at_exit[p->n_at_exit].fn = fn;
	p->at_exit[p->n_at_exit].arg = arg;
	p->n_at_exit++;
	return XTC_OK;
}

/* Run a proc's at-exit callbacks (LIFO) and tear down its scoped
 * memory context.  Called once on the exit path, with __current_proc
 * still set and outside signal context. */
static void
__run_proc_at_exit(struct xtc_proc *p)
{
	int i;
	/* Release any resources the proc registered for recovery but did
	 * not explicitly release (LIFO), before the at-exit hooks. */
	__recov_release_all(p);
	for (i = p->n_at_exit - 1; i >= 0; i--)
		p->at_exit[i].fn(p->at_exit[i].arg);
	p->n_at_exit = 0;
	if (p->proc_mctx != NULL) {
		xtc_mctx_destroy(p->proc_mctx);
		p->proc_mctx = NULL;
	}
}

/* PUBLIC: struct xtc_mctx *xtc_proc_mctx __P((void)); */
struct xtc_mctx *
xtc_proc_mctx(void)
{
	struct xtc_proc *p = __current_proc;
	if (p == NULL)
		return NULL;
	if (p->proc_mctx == NULL)
		(void)xtc_mctx_create(NULL, "proc", 0, &p->proc_mctx);
	return p->proc_mctx;
}

/* ---------- recovery resource registry ----------
 *
 * A contained fault unwinds the faulting fiber's call stack (siglongjmp
 * on POSIX, a CONTEXT restore via the VEH on Windows) but frees NONE of
 * the resources the proc held -- locks stay locked, fds stay open,
 * memory contexts keep their arenas, buffer pins stay pinned -- and a
 * leaked lock can wedge every peer that later contends it.  Before the
 * fix, the recovery block had to remember and release each one by hand.
 *
 * These calls let a proc REGISTER the resources it acquires so the
 * runtime can release them automatically.  xtc_proc_recovery_cleanup()
 * releases everything registered, LIFO, and is:
 *   - the DEFAULT recovery action (xtc_proc_recovery_arm's recovered
 *     branch can just call it, or use the xtc_proc_recovery_arm_clean
 *     convenience that does cleanup + exit), and
 *   - callable from a CUSTOM recovery block to finish the standard
 *     bits after the block's own application-specific unwinding.
 * It also runs automatically on the normal exit path (before the
 * at-exit hooks), so a proc that simply returns still releases what it
 * registered.
 */

static int
__recov_push(struct xtc_proc *p, struct xtc_recov_rec r)
{
	if (p == NULL)
		return XTC_E_INVAL;
	if (p->n_recov >= XTC_PROC_MAX_RECOVERY)
		return XTC_E_RESOURCE;
	p->recov[p->n_recov++] = r;
	return XTC_OK;
}

/* PUBLIC: int xtc_proc_recovery_track_fd __P((int)); */
int
xtc_proc_recovery_track_fd(int fd)
{
	struct xtc_recov_rec r;
	if (fd < 0)
		return XTC_E_INVAL;
	memset(&r, 0, sizeof r);
	r.kind = XTC_RECOV_FD;
	r.fd = fd;
	return __recov_push(__current_proc, r);
}

/* PUBLIC: int xtc_proc_recovery_track_mctx __P((struct xtc_mctx *)); */
int
xtc_proc_recovery_track_mctx(struct xtc_mctx *mctx)
{
	struct xtc_recov_rec r;
	if (mctx == NULL)
		return XTC_E_INVAL;
	memset(&r, 0, sizeof r);
	r.kind = XTC_RECOV_MCTX;
	r.ptr = mctx;
	return __recov_push(__current_proc, r);
}

/* PUBLIC: int xtc_proc_recovery_track_locks __P((void *, uint64_t, void (*)(void *, uint64_t))); */
int
xtc_proc_recovery_track_locks(void *mgr, uint64_t locker,
                              void (*release_all)(void *, uint64_t))
{
	struct xtc_recov_rec r;
	if (mgr == NULL || release_all == NULL)
		return XTC_E_INVAL;
	memset(&r, 0, sizeof r);
	r.kind = XTC_RECOV_LOCKS;
	r.ptr = mgr;
	r.u64 = locker;
	r.lock_release = release_all;
	return __recov_push(__current_proc, r);
}

/* PUBLIC: int xtc_proc_recovery_track __P((void (*)(void *), void *)); */
int
xtc_proc_recovery_track(void (*fn)(void *), void *arg)
{
	struct xtc_recov_rec r;
	if (fn == NULL)
		return XTC_E_INVAL;
	memset(&r, 0, sizeof r);
	r.kind = XTC_RECOV_CB;
	r.fn = fn;
	r.ptr = arg;
	return __recov_push(__current_proc, r);
}

/* Release every tracked resource (LIFO) and forget them.  Shared by
 * the public cleanup call and the proc exit path. */
static void
__recov_release_all(struct xtc_proc *p)
{
	int i;
	if (p == NULL)
		return;
	for (i = p->n_recov - 1; i >= 0; i--) {
		struct xtc_recov_rec *r = &p->recov[i];
		switch (r->kind) {
		case XTC_RECOV_FD:
			if (r->fd >= 0) (void)xtc_fs_close(r->fd);
			break;
		case XTC_RECOV_MCTX:
			if (r->ptr != NULL)
				xtc_mctx_reset((struct xtc_mctx *)r->ptr);
			break;
		case XTC_RECOV_LOCKS:
			if (r->lock_release != NULL && r->ptr != NULL)
				r->lock_release(r->ptr, r->u64);
			break;
		case XTC_RECOV_CB:
			if (r->fn != NULL) r->fn(r->ptr);
			break;
		default:
			break;
		}
	}
	p->n_recov = 0;
}

/* PUBLIC: void xtc_proc_recovery_cleanup __P((void)); */
void
xtc_proc_recovery_cleanup(void)
{
	__recov_release_all(__current_proc);
}

/* PUBLIC: int xtc_proc_recovery_untrack_fd __P((int)); */
int
xtc_proc_recovery_untrack_fd(int fd)
{
	/* Drop the most-recent FD record matching `fd` so a resource the
	 * proc released NORMALLY is not double-released on recovery. */
	struct xtc_proc *p = __current_proc;
	int i;
	if (p == NULL)
		return XTC_E_INVAL;
	for (i = p->n_recov - 1; i >= 0; i--) {
		if (p->recov[i].kind == XTC_RECOV_FD && p->recov[i].fd == fd) {
			int j;
			for (j = i; j < p->n_recov - 1; j++)
				p->recov[j] = p->recov[j + 1];
			p->n_recov--;
			return XTC_OK;
		}
	}
	return XTC_E_NOTFOUND;
}

/* ---------- A2: cancellation masking (uncancelable / poll) ----------
 *
 * MonadCancel's masking discipline for C.  A per-proc mask_depth counter
 * (owner-only) makes structured cancellation composable: an asynchronous
 * kill (xtc_exit_pid) delivered at a yield/recv point while mask_depth>0
 * is DEFERRED, latched in mask_deferred, and observed only when the mask
 * drops to 0.  This is the guarantee A1's bracket leans on -- the release
 * acquired in a masked region always gets registered before cancellation
 * unwinds the fiber -- closing the finalizer-eating race a bare abort
 * flag has.
 */

/* Drain a latched-deferred kill once the mask is fully lifted; does not
 * return if one was pending (unwinds via xtc_exit_self). */
static void
__mask_drain(struct xtc_proc *p)
{
	int kp;
	if (p == NULL || p->mask_depth > 0)
		return;
	kp = p->mask_deferred;
	if (kp != 0) {
		p->mask_deferred = 0;
		xtc_exit_self(kp - 1);
	}
}

/* PUBLIC: int xtc_uncancelable __P((int (*)(void *), void *)); */
int
xtc_uncancelable(int (*body)(void *), void *ud)
{
	struct xtc_proc *p = __current_proc;
	int rc;
	if (body == NULL)
		return XTC_E_INVAL;
	if (p == NULL)
		return body(ud);        /* off a proc: nothing to mask */
	p->mask_depth++;
	rc = body(ud);
	if (p->mask_depth > 0)
		p->mask_depth--;
	/* Fully unmasked now: honor any kill that arrived while masked. */
	__mask_drain(p);
	return rc;
}

/* PUBLIC: int xtc_cancel_poll __P((int (*)(void *), void *)); */
int
xtc_cancel_poll(int (*body)(void *), void *ud)
{
	struct xtc_proc *p = __current_proc;
	unsigned saved;
	int rc;
	if (body == NULL)
		return XTC_E_INVAL;
	if (p == NULL || p->mask_depth == 0)
		return body(ud);       /* unmasked already: just run it */
	/* Temporarily re-admit cancellation for this sub-region: drop the
	 * mask to 0, honoring any already-latched kill BEFORE running the
	 * body (Cats Effect's poll observes cancellation at the poll site),
	 * then restore the caller's mask depth. */
	saved = p->mask_depth;
	p->mask_depth = 0;
	__mask_drain(p);            /* may not return */
	rc = body(ud);
	p->mask_depth = saved;
	return rc;
}

/* PUBLIC: int xtc_cancel_requested __P((void)); */
int
xtc_cancel_requested(void)
{
	struct xtc_proc *p = __current_proc;
	if (p == NULL)
		return 0;
	if (atomic_load_explicit(&p->kill_pending, memory_order_acquire) != 0)
		return 1;
	return p->mask_deferred != 0;
}

/* ---------- A1: resource scope / bracket ----------
 *
 * A blessed, runtime-enforced resource scope on top of the recovery
 * registry.  Cats Effect's Resource/bracket was a "paper door": a
 * convention a coding agent barges through.  xtc_scope makes
 * "this WILL be released on every exit path" a MECHANISM: a scope is
 * pushed as a recovery-registry marker (XTC_RECOV_CB), so a proc-level
 * unwind -- normal return, xtc_exit_self, an async kill, OR a
 * fault-guard-contained crash -- runs the same LIFO cleanup that
 * releases fds/locks/mctx, which now includes closing every still-open
 * scope's deferred finalizers.  xtc_scope_close on the happy path runs
 * them and detaches the marker.  Scopes nest: each is its own marker,
 * so an outer unwind closes inner-then-outer LIFO (the recovery stack
 * is walked high-index-first).
 */

struct xtc_scope {
	struct xtc_proc *owner;      /* proc this scope belongs to */
	int              closed;     /* guards double-run */
#define XTC_SCOPE_MAX_DEFER 32
	struct { xtc_finalizer_fn fn; void *arg; } fin[XTC_SCOPE_MAX_DEFER];
	int              n_fin;
};

/* Run a scope's deferred finalizers LIFO, once.  Shared by the happy-
 * path close and the recovery-marker unwind. */
static void
__scope_run(struct xtc_scope *s)
{
	int i;
	if (s == NULL || s->closed)
		return;
	s->closed = 1;
	for (i = s->n_fin - 1; i >= 0; i--)
		if (s->fin[i].fn != NULL)
			s->fin[i].fn(s->fin[i].arg);
	s->n_fin = 0;
}

/* Recovery-marker trampoline: the registry invokes fn(arg) on an
 * unwind.  Runs the finalizers but does NOT free the scope struct --
 * the marker's presence means the caller never reached xtc_scope_close,
 * so the struct is freed here too (the recovery path owns it now). */
static void
__scope_recover_cb(void *arg)
{
	struct xtc_scope *s = arg;
	if (s == NULL)
		return;
	__scope_run(s);
	__os_free(s);
}

/* Drop the recovery marker that referenced `s` (happy-path close), so a
 * later proc-level unwind does not re-run an already-closed scope.
 * Removes the most-recent matching XTC_RECOV_CB record. */
static void
__scope_drop_marker(struct xtc_proc *p, struct xtc_scope *s)
{
	int i, j;
	if (p == NULL)
		return;
	for (i = p->n_recov - 1; i >= 0; i--) {
		if (p->recov[i].kind == XTC_RECOV_CB &&
		    p->recov[i].fn == __scope_recover_cb &&
		    p->recov[i].ptr == s) {
			for (j = i; j < p->n_recov - 1; j++)
				p->recov[j] = p->recov[j + 1];
			p->n_recov--;
			return;
		}
	}
}

/* PUBLIC: xtc_scope_t *xtc_scope_open __P((void)); */
xtc_scope_t *
xtc_scope_open(void)
{
	struct xtc_proc *p = __current_proc;
	struct xtc_scope *s = NULL;
	struct xtc_recov_rec r;
	if (p == NULL)
		return NULL;            /* scopes are per-proc */
	if (__os_calloc(1, sizeof *s, (void **)&s) != XTC_OK)
		return NULL;
	s->owner = p;
	/* Push a recovery-registry marker so a proc-level unwind (fault,
	 * kill, exit) closes this scope LIFO alongside the standard fd/lock
	 * cleanup.  If the registry is full the scope cannot guarantee its
	 * release-on-crash contract, so refuse to open rather than lie. */
	memset(&r, 0, sizeof r);
	r.kind = XTC_RECOV_CB;
	r.fn = __scope_recover_cb;
	r.ptr = s;
	if (__recov_push(p, r) != XTC_OK) {
		__os_free(s);
		return NULL;
	}
	return s;
}

/* PUBLIC: int xtc_scope_defer __P((xtc_scope_t *, xtc_finalizer_fn, void *)); */
int
xtc_scope_defer(xtc_scope_t *s, xtc_finalizer_fn fn, void *arg)
{
	if (s == NULL || fn == NULL)
		return XTC_E_INVAL;
	if (s->closed)
		return XTC_E_INVAL;
	if (s->n_fin >= XTC_SCOPE_MAX_DEFER)
		return XTC_E_RESOURCE;
	s->fin[s->n_fin].fn = fn;
	s->fin[s->n_fin].arg = arg;
	s->n_fin++;
	return XTC_OK;
}

/* PUBLIC: void xtc_scope_close __P((xtc_scope_t *)); */
void
xtc_scope_close(xtc_scope_t *s)
{
	struct xtc_proc *p;
	if (s == NULL)
		return;
	p = s->owner;
	/* Detach the recovery marker FIRST so that if a finalizer itself
	 * faults or exits, the unwind does not re-enter this same scope. */
	__scope_drop_marker(p, s);
	__scope_run(s);
	__os_free(s);
}

/* PUBLIC: int xtc_bracket __P((int (*)(void **, void *), int (*)(void *, void *), void (*)(void *, void *), void *)); */
struct __bracket_rel {
	void  *res;
	void (*release)(void *, void *);
	void  *ud;
};
static void
__bracket_release_cb(void *arg)
{
	struct __bracket_rel *br = arg;
	if (br == NULL)
		return;
	if (br->release != NULL)
		br->release(br->res, br->ud);
	__os_free(br);
}
int
xtc_bracket(int (*acquire)(void **res, void *ud),
            int (*use)(void *res, void *ud),
            void (*release)(void *res, void *ud),
            void *ud)
{
	struct xtc_proc *p = __current_proc;
	void *res = NULL;
	int arc, urc;
	if (acquire == NULL || use == NULL || release == NULL)
		return XTC_E_INVAL;
	if (p == NULL) {
		/* Off a proc there is no scope/kill machinery: still honor the
		 * acquire/use/guaranteed-release contract directly. */
		if ((arc = acquire(&res, ud)) != XTC_OK)
			return arc;
		urc = use(res, ud);
		release(res, ud);
		return urc;
	}
	/* Acquire runs ABORT-MASKED so a kill cannot fire between the
	 * resource becoming live and its release being registered -- the
	 * A1+A2 pairing.  The mask is held across acquire + the defer, then
	 * dropped for use(). */
	p->mask_depth++;
	arc = acquire(&res, ud);
	if (arc != XTC_OK) {
		if (p->mask_depth > 0)
			p->mask_depth--;
		__mask_drain(p);
		return arc;
	}
	{
		struct xtc_scope *s = xtc_scope_open();
		struct __bracket_rel *br = NULL;
		if (s == NULL) {
			/* Cannot guarantee release via a scope; release now
			 * (still masked) and fail rather than leak. */
			release(res, ud);
			if (p->mask_depth > 0)
				p->mask_depth--;
			__mask_drain(p);
			return XTC_E_RESOURCE;
		}
		if (__os_calloc(1, sizeof *br, (void **)&br) != XTC_OK) {
			xtc_scope_close(s);
			release(res, ud);
			if (p->mask_depth > 0)
				p->mask_depth--;
			__mask_drain(p);
			return XTC_E_RESOURCE;
		}
		br->res = res;
		br->release = release;
		br->ud = ud;
		(void)xtc_scope_defer(s, __bracket_release_cb, br);
		/* Release is now registered on every exit path.  Drop the mask
		 * (honoring a deferred kill) and run use(). */
		if (p->mask_depth > 0)
			p->mask_depth--;
		__mask_drain(p);        /* if killed, scope unwind releases */
		urc = use(res, ud);
		xtc_scope_close(s);     /* runs release + frees br */
		return urc;
	}
}

/* PUBLIC: int xtc_down_decode __P((const void *, size_t, xtc_pid_t *, int *)); */
int
xtc_down_decode(const void *msg, size_t len, xtc_pid_t *out_pid,
                int *out_reason)
{
	/* Decode a DOWN signal without the caller hand-rolling the packed
	 * layout (a footgun: an unpacked mirror struct misreads reason).
	 * We read it through the same packed shape used to send it. */
	XTC_PACK_PUSH
	struct {
		uint8_t   kind;
		uint64_t  ref;
		xtc_pid_t pid;
		int       reason;
	} XTC_PACKED d;
	XTC_PACK_POP
	if (msg == NULL || len < sizeof d)
		return XTC_E_INVAL;
	memcpy(&d, msg, sizeof d);
	if (d.kind != 'D')
		return XTC_E_INVAL;     /* not a DOWN message */
	if (out_pid != NULL) *out_pid = d.pid;
	if (out_reason != NULL) *out_reason = d.reason;
	return XTC_OK;
}

/* PUBLIC: int xtc_down_decode_ex __P((const void *, size_t, xtc_down_info_t *)); */
int
xtc_down_decode_ex(const void *msg, size_t len, xtc_down_info_t *out)
{
	const uint8_t *k = msg;
	XTC_PACK_PUSH
	struct dmsg {
		uint8_t   kind;
		uint64_t  ref;
		xtc_pid_t pid;
		int       reason;
		uint8_t   exit_kind;
	} XTC_PACKED;
	struct emsg {
		uint8_t   kind;
		int       reason;
		xtc_pid_t pid;
		uint8_t   exit_kind;
	} XTC_PACKED;
	XTC_PACK_POP
	xtc_down_info_t info;
	int ek;

	if (msg == NULL || len < 1 || out == NULL)
		return XTC_E_INVAL;
	memset(&info, 0, sizeof info);

	if (*k == 'D') {
		struct dmsg d;
		/* Accept both the current layout (with exit_kind) and an
		 * older/shorter DOWN without it: if the extra byte is absent
		 * we derive the kind from the reason value. */
		size_t base = offsetof(struct dmsg, exit_kind);
		if (len < base) return XTC_E_INVAL;
		memset(&d, 0, sizeof d);
		memcpy(&d, msg, len < sizeof d ? len : sizeof d);
		info.pid    = d.pid;
		info.reason = d.reason;
		info.ref    = d.ref;
		ek = (len >= sizeof d) ? d.exit_kind : -1;
	} else if (*k == 'E') {
		struct emsg e;
		size_t base = offsetof(struct emsg, exit_kind);
		if (len < base) return XTC_E_INVAL;
		memset(&e, 0, sizeof e);
		memcpy(&e, msg, len < sizeof e ? len : sizeof e);
		info.pid    = e.pid;
		info.reason = e.reason;
		info.ref    = 0;   /* a link EXIT carries no monitor ref */
		ek = (len >= sizeof e) ? e.exit_kind : -1;
	} else {
		return XTC_E_INVAL;
	}

	/* Map the on-wire exit_kind byte (or a derived value for an older
	 * sender) to the classified fields. */
	if (xtc_down_is_noproc(info.reason)) {
		info.kind = XTC_DOWN_KIND_NOPROC;
	} else if (ek == 2) {
		info.kind = XTC_DOWN_KIND_SIGNAL;
		info.signal = info.reason;
	} else if (ek == 1) {
		info.kind = XTC_DOWN_KIND_EXIT;
		info.exit_code = info.reason;
	} else if (ek == 0) {
		info.kind = XTC_DOWN_KIND_CLEAN;
	} else {
		/* No exit_kind byte (older sender): fall back to the legacy
		 * numeric convention so a downgraded peer still classifies. */
		if (info.reason == 0)
			info.kind = XTC_DOWN_KIND_CLEAN;
		else if (info.reason >= 1 && info.reason <= 255) {
			info.kind = XTC_DOWN_KIND_SIGNAL;
			info.signal = info.reason;
		} else {
			info.kind = XTC_DOWN_KIND_EXIT;
			info.exit_code = info.reason;
		}
	}
	*out = info;
	return XTC_OK;
}

/*
 * Cross-loop link/monitor safety.
 *
 * self->links / self->monitors are mutated only by the owning fiber, so
 * they need no lock.  But xtc_link / xtc_monitor also push an entry onto
 * the PEER's list (peer->links / peer->monitored_by), and the peer may
 * live on another loop / OS thread and may be exiting concurrently --
 * __notify_links_and_monitors walks and frees those lists and then frees
 * the proc struct.  Two races follow if the peer push is unlocked: a
 * torn list (concurrent push vs. walk) and a use-after-free of the peer
 * struct itself (push vs. __os_free).
 *
 * The peer's TABLE lock (the per-loop slot table lock, which already
 * guards slot existence + generation) is the serialization point.  A
 * peer push takes that lock, re-validates the slot still holds the same
 * live proc (gen match), and pushes -- so it cannot run against a freed
 * or recycled struct.  __notify detaches the peer-visible lists AND
 * releases the slot under the same lock, so a push either lands before
 * the exit (and is notified) or finds the slot gone (and is dropped).
 * Only ONE table lock is ever held at a time (never nested with a send,
 * which takes a peer mbox_lock), so there is no lock-order cycle.
 *
 * Returns 1 if the entry was pushed (peer live), 0 if the peer is gone
 * (caller frees the entry).
 */
static int
__peer_push_link(xtc_pid_t peer_pid, struct link_entry *e)
{
	xtc_loop_t *lp = NULL;
	struct xtc_proc_table *tbl;
	struct xtc_proc *peer;
	int pushed = 0;
	peer = __resolve(peer_pid, &lp);
	if (peer == NULL || lp == NULL) { if (peer) __proc_release(peer); return 0; }
	tbl = __table_for(lp, 0);
	if (tbl == NULL) { __proc_release(peer); return 0; }
	(void) __proc_mtx_lock(&tbl->stripes[__pt_stripe(peer_pid.local_id)]);
	if (peer_pid.local_id < tbl->cap &&
	    tbl->slots[peer_pid.local_id].proc == peer &&
	    tbl->slots[peer_pid.local_id].gen == peer_pid.gen &&
	    peer->alive) {
		e->next = peer->links;
		peer->links = e;
		pushed = 1;
	}
	(void) __proc_mtx_unlock(&tbl->stripes[__pt_stripe(peer_pid.local_id)]);
	__proc_release(peer);
	return pushed;
}

static int
__peer_push_monitored_by(xtc_pid_t peer_pid, struct mon_entry *m)
{
	xtc_loop_t *lp = NULL;
	struct xtc_proc_table *tbl;
	struct xtc_proc *peer;
	int pushed = 0;
	peer = __resolve(peer_pid, &lp);
	if (peer == NULL || lp == NULL) { if (peer) __proc_release(peer); return 0; }
	tbl = __table_for(lp, 0);
	if (tbl == NULL) { __proc_release(peer); return 0; }
	(void) __proc_mtx_lock(&tbl->stripes[__pt_stripe(peer_pid.local_id)]);
	if (peer_pid.local_id < tbl->cap &&
	    tbl->slots[peer_pid.local_id].proc == peer &&
	    tbl->slots[peer_pid.local_id].gen == peer_pid.gen &&
	    peer->alive) {
		m->next = peer->monitored_by;
		peer->monitored_by = m;
		pushed = 1;
	}
	(void) __proc_mtx_unlock(&tbl->stripes[__pt_stripe(peer_pid.local_id)]);
	__proc_release(peer);
	return pushed;
}

/* PUBLIC: int xtc_link __P((xtc_pid_t)); */
int
xtc_link(xtc_pid_t other)
{
	struct xtc_proc *self = __current_proc;
	struct xtc_proc *peer;
	struct link_entry *le;
	if (self == NULL) return XTC_E_INVAL;
	peer = __resolve(other, NULL);
	if (peer == NULL || !peer->alive) { if (peer) __proc_release(peer); return XTC_E_INVAL; }
	__proc_release(peer);   /* only liveness was needed; the symmetric
	                         * push below re-resolves under the peer lock */
	le = __link_alloc();
	if (le == NULL) return XTC_E_NOMEM;
	le->peer = other;
	le->next = self->links;
	self->links = le;
	/* Symmetric: add ourselves to the peer's link list.  This may be a
	 * cross-loop push, so it goes through __peer_push_link, which holds
	 * the peer's table lock and re-validates liveness.  If the peer is
	 * already gone, free the entry (no link to maintain). */
	{
		struct link_entry *pe = __link_alloc();
		if (pe != NULL) {
			pe->peer = self->pid;
			if (!__peer_push_link(other, pe))
				__link_free(pe);
		}
	}
	return XTC_OK;
}

/* PUBLIC: int xtc_unlink __P((xtc_pid_t)); */
int
xtc_unlink(xtc_pid_t other)
{
	struct xtc_proc *self = __current_proc;
	struct link_entry **pp;
	if (self == NULL) return XTC_E_INVAL;
	for (pp = &self->links; *pp != NULL; pp = &(*pp)->next) {
		if (xtc_pid_eq((*pp)->peer, other)) {
			struct link_entry *e = *pp;
			*pp = e->next;
			__link_free(e);
			break;
		}
	}
	return XTC_OK;
}

/* PUBLIC: int xtc_monitor __P((xtc_pid_t, uint64_t *)); */
int
xtc_monitor(xtc_pid_t target, uint64_t *out_ref)
{
	struct xtc_proc *self = __current_proc;
	struct xtc_proc *peer;
	struct mon_entry *me;
	if (self == NULL) return XTC_E_INVAL;
	peer = __resolve(target, NULL);
	if (peer == NULL || !peer->alive) {
		/*
		 * Target is already gone (it exited and was reaped between
		 * the caller's spawn and this monitor -- a real race when the
		 * target is spawned cross-loop onto an already-running loop
		 * and finishes immediately).  Erlang semantics: a monitor of
		 * a dead process delivers an immediate DOWN rather than
		 * failing.  Deliver it to self so the caller's normal DOWN
		 * path runs; we missed the real exit reason (it is reaped),
		 * so report it as XTC_DOWN_NOPROC -- a DISTINCT reason (not a
		 * signal number and not an XTC_E_ code) so a supervisor can
		 * tell "target already gone" apart from a real fault exit
		 * (whose reason is the positive signal number).  A supervisor
		 * that restarts on a crash treats NOPROC as "re-establish if
		 * you would restart" but does NOT misclassify it as SIGSEGV.
		 */
		uint64_t ref = atomic_fetch_add_explicit(&__mon_ref_seq, 1,
		    memory_order_relaxed) + 1;
		XTC_PACK_PUSH
		struct {
			uint8_t kind;
			uint64_t ref;
			xtc_pid_t pid;
			int     reason;
			uint8_t exit_kind;
		} XTC_PACKED down = { 'D', ref, target, XTC_DOWN_NOPROC, 3 };
		XTC_PACK_POP
		(void)xtc_send(self->pid, &down, sizeof down);
		if (out_ref) *out_ref = ref;
		if (peer) __proc_release(peer);
		return XTC_OK;
	}
	/* Liveness confirmed; the symmetric push below re-resolves under the
	 * peer's table lock, so we no longer need to pin peer here. */
	__proc_release(peer);
	me = __mon_alloc();
	if (me == NULL) return XTC_E_NOMEM;
	me->ref = atomic_fetch_add_explicit(&__mon_ref_seq, 1,
	    memory_order_relaxed) + 1;
	me->target = target;
	me->watcher = self->pid;
	me->next = self->monitors;
	self->monitors = me;
	{
		struct mon_entry *m2 = __mon_alloc();
		if (m2 != NULL) {
			*m2 = *me;
			/* Cross-loop-safe push onto the peer's monitored_by list;
			 * drop the entry if the peer exited meanwhile. */
			if (!__peer_push_monitored_by(target, m2))
				__mon_free(m2);
		}
	}
	if (out_ref) *out_ref = me->ref;
	return XTC_OK;
}

/* On exit: notify links + monitors. */
static void
__notify_links_and_monitors(struct xtc_proc *p)
{
	struct link_entry *le, *next_le;
	struct mon_entry  *me, *next_me;
	XTC_PACK_PUSH
	struct {
		uint8_t kind;
		int     reason;
		xtc_pid_t pid;
		uint8_t exit_kind;
	} XTC_PACKED exit_signal = {
		.kind = 'E', .reason = p->exit_reason, .pid = p->pid,
		.exit_kind = (uint8_t)p->exit_kind
	};
	struct {
		uint8_t kind;
		uint64_t ref;
		xtc_pid_t pid;
		int     reason;
		uint8_t exit_kind;
	} XTC_PACKED down_signal;
	XTC_PACK_POP
	struct link_entry *links;
	struct mon_entry  *monitored_by, *monitors;

	/*
	 * Detach the peer-visible lists (links, monitored_by) AND release
	 * the slot under the table lock, so a concurrent cross-loop
	 * __peer_push_link / __peer_push_monitored_by either landed before
	 * this (and is in the detached list, so it gets notified) or finds
	 * the slot already gone (and drops its entry).  After the slot is
	 * released no __resolve can hand this proc out again.  monitors
	 * (the lists WE hold on others) is owner-only, but detach it here
	 * too for uniformity.  Sends/frees run OUTSIDE the lock (xtc_send
	 * takes a peer mbox_lock; never nest two proc locks).
	 */
	{
		struct xtc_proc_table *tbl = __table_for(p->loop, 0);
		unsigned pst = __pt_stripe(p->pid.local_id);
		if (tbl != NULL) (void) __proc_mtx_lock(&tbl->stripes[pst]);
		links = p->links;               p->links = NULL;
		monitored_by = p->monitored_by; p->monitored_by = NULL;
		monitors = p->monitors;         p->monitors = NULL;
		if (tbl != NULL) {
			if (p->pid.local_id < tbl->cap &&
			    tbl->slots[p->pid.local_id].proc == p) {
				tbl->slots[p->pid.local_id].proc = NULL;
				tbl->n_used--;
			}
			(void) __proc_mtx_unlock(&tbl->stripes[pst]);
		}
	}

	for (le = links; le != NULL; le = next_le) {
		next_le = le->next;
		(void)xtc_send(le->peer, &exit_signal, sizeof exit_signal);
		__link_free(le);
	}

	for (me = monitored_by; me != NULL; me = next_me) {
		next_me = me->next;
		down_signal.kind = 'D';
		down_signal.ref  = me->ref;
		down_signal.pid  = p->pid;
		down_signal.reason = p->exit_reason;
		down_signal.exit_kind = (uint8_t)p->exit_kind;
		(void)xtc_send(me->watcher, &down_signal, sizeof down_signal);
		__mon_free(me);
	}

	for (me = monitors; me != NULL; me = next_me) {
		next_me = me->next;
		__mon_free(me);
	}

	/* The slot was already released (under the table lock) above, so no
	 * NEW resolver can find this proc.  Drop the owner ref: if an
	 * in-flight cross-thread send / wake still holds a ref (took one in
	 * __table_lookup before we detached), the actual teardown -- mailbox
	 * drain, mbox_lock destroy, free -- is deferred to __proc_free when
	 * that last ref is released, so the sender never touches freed
	 * memory or a destroyed lock. */
	__proc_release(p);
}

/* Final proc teardown, run when the last reference is dropped (owner +
 * any in-flight resolver refs).  Drains the mailbox, destroys the lock,
 * frees the struct.  See the refcount discussion on struct xtc_proc. */
static void
__proc_free(struct xtc_proc *p)
{
	struct envelope *e, *n;
	(void) __proc_mtx_lock(&p->mbox_lock);
	for (e = p->mbox_head; e != NULL; e = n) { n = e->next; __env_free(e); }
	p->mbox_head = p->mbox_tail = NULL;
	(void) __proc_mtx_unlock(&p->mbox_lock);
	for (e = p->save_head; e != NULL; e = n) { n = e->next; __env_free(e); }
	p->save_head = p->save_tail = NULL;

	(void)pthread_mutex_destroy(&p->mbox_lock);
	__os_free(p);
}

/* ---------- live introspection (xtc_inspect.h) ---------- */

/*
 * Fill *info from a proc.  Called with no lock held by the caller; we
 * take the proc's mbox_lock for the mailbox counters (consistent with
 * push/pop, which hold it).  The run state is sampled from the task
 * without a lock -- a best-effort snapshot, as documented.  Link and
 * monitor lists are deliberately not traversed (the owner mutates them
 * lock-free; a cross-thread walk would race).
 */
static void
__fill_proc_info(struct xtc_proc *p, xtc_proc_info_t *info)
{
	memset(info, 0, sizeof *info);
	info->pid = p->pid;
	info->alive = p->alive;
	info->kill_pending =
	    atomic_load_explicit(&p->kill_pending, memory_order_relaxed) ? 1 : 0;
	if (p->task != NULL) {
		int st = atomic_load_explicit(&p->task->state,
		    memory_order_relaxed);   /* introspection snapshot */
		info->run_state = st;
		if (st == XTC_TS_PARKED) {
			if (p->task->park_fd >= 0)
				info->park_reason = XTC_PARK_FD;
			else if (p->task->park_timer != NULL)
				info->park_reason = XTC_PARK_TIMER;
			else if (p->task->park_requested)
				info->park_reason = XTC_PARK_MAILBOX;
		}
	}
	(void) __proc_mtx_lock(&p->mbox_lock);
	info->mbox_len = p->mbox_n;
	info->mbox_peak = p->mbox_peak;
	info->mbox_cap = p->mbox_cap;
	info->mbox_recv_total = p->mbox_recv_total;
	info->mbox_drop_total = p->mbox_drop_total;
	(void) __proc_mtx_unlock(&p->mbox_lock);
	info->mbox_saved =
	    atomic_load_explicit(&p->mbox_saved, memory_order_relaxed);
}

int
xtc_inspect_procs(xtc_inspect_proc_fn cb, void *user)
{
	xtc_proc_info_t *buf = NULL;
	size_t n = 0, cap = 0;
	int i, rc = XTC_OK;

	if (cb == NULL)
		return XTC_E_INVAL;

	/*
	 * Hold __lt_lock for the whole collection: it serializes against
	 * loop registration and table teardown (loop_fini), so no table
	 * is freed mid-walk.  Under it we take each table's lock; a
	 * non-NULL slot is a proc that has not reached __table_release
	 * (which needs that same lock), hence not yet freed.  We copy
	 * into a buffer and invoke the callback only AFTER every lock is
	 * dropped, so the callback may call back into the proc/loop APIs.
	 */
	(void) __proc_mtx_lock(&__lt_lock);
	for (i = 0; i < LOOP_TABLE_MAX; i++) {
		struct xtc_proc_table *tbl = atomic_load_explicit(&__lt[i].tbl,
		    memory_order_relaxed);
		size_t s;
		if (atomic_load_explicit(&__lt[i].loop,
		    memory_order_relaxed) == NULL || tbl == NULL)
			continue;
		(void) __pt_lock_all(tbl);
		for (s = 0; s < tbl->cap; s++) {
			struct xtc_proc *p = tbl->slots[s].proc;
			if (p == NULL)
				continue;
			if (n == cap) {
				size_t ncap = cap ? cap * 2 : 32;
				xtc_proc_info_t *nb;
				if (__os_realloc(buf, ncap * sizeof *nb,
				    (void **)&nb) != XTC_OK) {
					(void) __pt_unlock_all(tbl);
					(void) __proc_mtx_unlock(&__lt_lock);
					__os_free(buf);
					return XTC_E_NOMEM;
				}
				buf = nb;
				cap = ncap;
			}
			__fill_proc_info(p, &buf[n]);
			n++;
		}
		(void) __pt_unlock_all(tbl);
	}
	(void) __proc_mtx_unlock(&__lt_lock);

	for (i = 0; (size_t)i < n; i++)
		if (cb(&buf[i], user) != 0)
			break;
	__os_free(buf);
	(void)rc;
	return (int)n;
}

int
xtc_inspect_loops(xtc_inspect_loop_fn cb, void *user)
{
	xtc_loop_info_t infos[LOOP_TABLE_MAX];
	int n = 0, i;

	if (cb == NULL)
		return XTC_E_INVAL;

	(void) __proc_mtx_lock(&__lt_lock);
	for (i = 0; i < LOOP_TABLE_MAX; i++) {
		struct xtc_proc_table *tbl = atomic_load_explicit(&__lt[i].tbl,
		    memory_order_relaxed);
		xtc_loop_t *loop = atomic_load_explicit(&__lt[i].loop,
		    memory_order_relaxed);
		size_t s, procs = 0;
		if (loop == NULL || tbl == NULL)
			continue;
		(void) __pt_lock_all(tbl);
		for (s = 0; s < tbl->cap; s++)
			if (tbl->slots[s].proc != NULL)
				procs++;
		(void) __pt_unlock_all(tbl);
		infos[n].loop_id = loop->exec_id < 0 ? 0 : loop->exec_id + 1;
		infos[n].n_procs = (int)procs;
		infos[n].n_alive =
		    atomic_load_explicit(&loop->n_alive, memory_order_relaxed);
		infos[n].tasks_run =
		    atomic_load_explicit(&loop->n_tasks_run, memory_order_relaxed);
		infos[n].steals =
		    atomic_load_explicit(&loop->n_steals, memory_order_relaxed);
		n++;
	}
	(void) __proc_mtx_unlock(&__lt_lock);

	for (i = 0; i < n; i++)
		if (cb(&infos[i], user) != 0)
			break;
	return n;
}

int
xtc_proc_info(xtc_pid_t pid, xtc_proc_info_t *out)
{
	int i, found = 0;

	if (out == NULL)
		return XTC_E_INVAL;

	(void) __proc_mtx_lock(&__lt_lock);
	for (i = 0; i < LOOP_TABLE_MAX && !found; i++) {
		struct xtc_proc_table *tbl = atomic_load_explicit(&__lt[i].tbl,
		    memory_order_relaxed);
		size_t s;
		if (atomic_load_explicit(&__lt[i].loop,
		    memory_order_relaxed) == NULL || tbl == NULL)
			continue;
		(void) __pt_lock_all(tbl);
		for (s = 0; s < tbl->cap; s++) {
			struct xtc_proc *p = tbl->slots[s].proc;
			if (p != NULL && xtc_pid_eq(p->pid, pid)) {
				__fill_proc_info(p, out);
				found = 1;
				break;
			}
		}
		(void) __pt_unlock_all(tbl);
	}
	(void) __proc_mtx_unlock(&__lt_lock);
	return found ? XTC_OK : XTC_E_NOTFOUND;
}

/* ---------- process runtime introspection (xtc_runtime.h) ---------- */

int
xtc_runtime_info(xtc_runtime_info_t *out)
{
	int n;

	if (out == NULL)
		return XTC_E_INVAL;

	/* n_loops: the executor the caller runs on.  xtc_shard_count()
	 * returns that executor's loop count on a multi-loop loop, 1 on
	 * a standalone loop, and 0 off any loop -- map the off-loop case
	 * (no thread-local current executor to consult) to 1. */
	n = xtc_shard_count();
	out->n_loops = n > 0 ? n : 1;

	out->n_cpus_online = __os_ncpus();
	out->n_cpus_perf   = __os_ncpus_perf();
	out->n_cpus_effic  = __os_ncpus_effic();
	out->numa_nodes    = __os_numa_nnodes();

	/* Memory: libxtc has no process-global / default resource
	 * accountant, and these fields are not the OS RSS.  Report 0
	 * ("no cap / unknown"); see xtc_runtime.h. */
	out->mem_cap_bytes  = 0;
	out->mem_used_bytes = 0;

	return XTC_OK;
}

/* ---------- causal tracing public API (xtc_trace.h) ---------- */

int
xtc_trace_enable(int on)
{
	return atomic_exchange_explicit(&__trace_on, on ? 1 : 0,
	    memory_order_relaxed);
}

int
xtc_trace_reset(void)
{
	(void) __proc_mtx_lock(&__trace_lock);
	__trace_seq = 0;
	(void) __proc_mtx_unlock(&__trace_lock);
	return XTC_OK;
}

uint64_t
xtc_hlc_now(void)
{
	return atomic_load_explicit(&__g_hlc, memory_order_relaxed);
}

static int
__trace_rec_cmp(const void *a, const void *b)
{
	uint64_t x = ((const xtc_trace_rec_t *)a)->hlc;
	uint64_t y = ((const xtc_trace_rec_t *)b)->hlc;
	return (x > y) - (x < y);
}

int
xtc_trace_dump(xtc_trace_fn cb, void *user)
{
	xtc_trace_rec_t *snap = NULL;
	uint64_t n, start, i;

	if (cb == NULL)
		return XTC_E_INVAL;

	(void) __proc_mtx_lock(&__trace_lock);
	n = __trace_seq < XTC_TRACE_RING ? __trace_seq : XTC_TRACE_RING;
	if (n > 0) {
		if (__os_malloc((size_t)n * sizeof *snap, (void **)&snap)
		    != XTC_OK) {
			(void) __proc_mtx_unlock(&__trace_lock);
			return XTC_E_NOMEM;
		}
		start = __trace_seq - n;
		for (i = 0; i < n; i++)
			snap[i] = __trace_ring[(start + i) % XTC_TRACE_RING];
	}
	(void) __proc_mtx_unlock(&__trace_lock);

	/* Present in causal (HLC-ascending) order. */
	if (n > 1)
		qsort(snap, (size_t)n, sizeof *snap, __trace_rec_cmp);
	for (i = 0; i < n; i++)
		if (cb(&snap[i], user) != 0)
			break;
	__os_free(snap);
	return (int)n;
}
