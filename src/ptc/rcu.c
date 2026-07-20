/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/rcu.c
 *	Epoch-based reclamation, single-global-epoch flavour.
 *
 *	Per-thread state ('struct rcu_tls') tracks the epoch we
 *	last entered a read-side at, plus a nesting count.  At
 *	read_lock time we publish the global epoch into the slot;
 *	at read_unlock time we clear it.  A writer calling
 *	synchronize advances the epoch and waits until every
 *	thread is either at the new epoch or has no active read.
 *
 *	Retired objects sit on a 3-bucket ring keyed on epoch & 3.
 *	Bucket E gets freed when the global epoch reaches E+2 (one
 *	full grace period of buffer ahead).
 *
 *	DST (deterministic simulation): under the single-thread sim
 *	every fiber shares one OS thread and hence one thread-local
 *	__rcu_self slot, so overlapping read-sides from different
 *	fibers would corrupt each other's active_epoch and could free
 *	a node a concurrent reader still holds.  So when running
 *	INSIDE a fiber (__xtc_current_task() != NULL) the reader slot
 *	is keyed on the CURRENT TASK instead of the thread (each fiber
 *	gets its own struct rcu_tls, still registered into the same
 *	global registry synchronize scans), and synchronize's drain
 *	loop yields to the loop (xtc_yield) instead of sched_yield so
 *	reader fibers actually run and drain.  Both paths are PURELY
 *	ADDITIVE and gated on __xtc_current_task(): off a fiber (OS
 *	threads, the blocking pool, non-sim builds) the code is
 *	byte-identical to the original -- __rcu_self + sched_yield.
 */

#include "xtc_int.h"
#include "preempt_int.h"   /* __xtc_unsafe_* / __xtc_mtx_*: internal preemption brackets */
#include "xtc_rcu.h"
#include "xtc_slab.h"
#include "xtc_async.h"     /* xtc_yield (DST drain) */
#include "xtc_proc.h"      /* __xtc_proc_ctx_save/restore */
#include "proc_int.h"   /* __xtc_proc_ctx_save/restore (internal) */
#include "coro_int.h"      /* __xtc_current_task */

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>

#define XTC_RCU_NBUCKETS  4

struct rcu_tls {
	_Atomic uint64_t  active_epoch;   /* 0 = not in a read-side */
	int               nest;
	struct rcu_tls   *next;           /* registered list (synchronize scans) */
	/*
	 * Owning fiber for slots keyed on a task (DST path).  NULL for
	 * the OS-thread slot pointed at by __rcu_self.  Also used as the
	 * hash-table key so read_lock can find "my" slot again and so a
	 * recycled task pointer reuses its slot (keeps the fiber-slot
	 * table bounded to peak concurrent fibers, not total spawned).
	 */
	xtc_task_t       *owner;
	struct rcu_tls   *tbl_next;       /* fiber-slot hash bucket chain */
};

struct retired {
	void              *p;
	xtc_rcu_free_fn    fn;
	struct retired    *next;
};

struct rcu_state {
	_Atomic uint64_t   epoch;
	pthread_mutex_t    lock;          /* guards: registry list, buckets */
	struct rcu_tls    *registry;
	struct retired    *buckets[XTC_RCU_NBUCKETS];
	int                inited;
};

static struct rcu_state __rcu = { 0, PTHREAD_MUTEX_INITIALIZER, NULL, {0}, 0 };
static XTC_THREAD_LOCAL struct rcu_tls *__rcu_self = NULL;

/*
 * DST reader-slot re-keying.  Under the single-thread sim ALL fibers
 * share one OS thread, hence one __rcu_self slot -- fiber A's
 * read_lock (which publishes the global epoch into active_epoch)
 * would collide with fiber B's, corrupting the per-reader epoch that
 * synchronize() scans, and could free a node a concurrent reader
 * still holds.  So when running INSIDE a fiber (__xtc_current_task()
 * != NULL) the reader slot is keyed on the CURRENT TASK instead of
 * the thread: each fiber gets its own struct rcu_tls.  That slot
 * still registers into the SAME global __rcu.registry, so nothing in
 * synchronize's scan changes -- only WHERE "my slot" pointer lives.
 *
 * Off a fiber (OS threads, the blocking pool, non-sim builds) the
 * path is byte-identical to before: __rcu_self, sched_yield().
 *
 * The fiber slots are held in a tiny open-chained hash table keyed on
 * the xtc_task_t pointer, guarded by __rcu.lock (same lock that
 * guards the registry).  A slot is created on the fiber's first
 * read_lock and REUSED if the same task pointer reappears (the loop
 * recycles task structs, so the live-pointer set -- and thus this
 * table -- stays bounded by peak concurrent fibers, not total
 * spawned).  A leftover slot from an exited fiber is always at
 * active_epoch==0 (read_unlock cleared it), so it never blocks
 * reclamation; all slots are freed at xtc_rcu_fini.
 */
#define XTC_RCU_FTBL_SIZE 64
static struct rcu_tls *__rcu_ftbl[XTC_RCU_FTBL_SIZE];  /* guarded by __rcu.lock */

static size_t
__rcu_ftbl_hash(const xtc_task_t *t)
{
	uintptr_t x = (uintptr_t)t;
	x ^= x >> 16;
	return (size_t)(x % XTC_RCU_FTBL_SIZE);
}

/* M11.5b: pools for rcu_tls registry entries and retired records.
 * These are published exactly once under __rcu_slab_init_lock; they are
 * _Atomic so the double-checked fast path in __rcu_slabs_ensure (and
 * every other reader below) loads them race-free with acquire ordering
 * rather than a plain read that TSan (correctly) flags as a data race
 * against the release store under the lock. */
static _Atomic(xtc_slab_t *) __rcu_tls_slab     = NULL;
static _Atomic(xtc_slab_t *) __rcu_retired_slab = NULL;
static pthread_mutex_t __rcu_slab_init_lock = PTHREAD_MUTEX_INITIALIZER;

static void
__rcu_slabs_ensure(void)
{
	if (atomic_load_explicit(&__rcu_tls_slab, memory_order_acquire) != NULL &&
	    atomic_load_explicit(&__rcu_retired_slab, memory_order_acquire) != NULL)
		return;
	(void)__xtc_mtx_lock(&__rcu_slab_init_lock);
	if (atomic_load_explicit(&__rcu_tls_slab, memory_order_relaxed) == NULL) {
		xtc_slab_opts_t o = XTC_SLAB_OPTS_DEFAULT;
		xtc_slab_t *sl = NULL;
		o.name = "rcu.tls"; o.obj_size = sizeof(struct rcu_tls);
		if (xtc_slab_create(&o, &sl) == XTC_OK)
			atomic_store_explicit(&__rcu_tls_slab, sl,
			    memory_order_release);
	}
	if (atomic_load_explicit(&__rcu_retired_slab, memory_order_relaxed) == NULL) {
		xtc_slab_opts_t o = XTC_SLAB_OPTS_DEFAULT;
		xtc_slab_t *sl = NULL;
		o.name = "rcu.retired"; o.obj_size = sizeof(struct retired);
		if (xtc_slab_create(&o, &sl) == XTC_OK)
			atomic_store_explicit(&__rcu_retired_slab, sl,
			    memory_order_release);
	}
	(void)__xtc_mtx_unlock(&__rcu_slab_init_lock);
}

/* Lazy registration of the per-thread slot. */
static int
__rcu_register(void)
{
	struct rcu_tls *t;
	if (__rcu_self != NULL) return XTC_OK;
	__rcu_slabs_ensure();
	if (__rcu_tls_slab == NULL) return XTC_E_RESOURCE;
	t = xtc_slab_alloc(__rcu_tls_slab);
	if (t == NULL) return XTC_E_RESOURCE;
	memset(t, 0, sizeof *t);
	(void)__xtc_mtx_lock(&__rcu.lock);
	t->next = __rcu.registry;
	__rcu.registry = t;
	(void)__xtc_mtx_unlock(&__rcu.lock);
	__rcu_self = t;
	return XTC_OK;
}

/*
 * Return the reader slot for the CURRENT fiber (DST path), creating
 * and registering it on first use, reusing an existing slot for a
 * recycled task pointer.  Callers hold nothing; this takes __rcu.lock
 * internally.  Returns NULL only on OOM (caller degrades to no-op,
 * matching the OS-thread OOM behaviour).
 */
static struct rcu_tls *
__rcu_fiber_slot(xtc_task_t *cur)
{
	struct rcu_tls *t;
	size_t h;

	__rcu_slabs_ensure();
	if (__rcu_tls_slab == NULL) return NULL;
	h = __rcu_ftbl_hash(cur);

	(void)__xtc_mtx_lock(&__rcu.lock);
	for (t = __rcu_ftbl[h]; t != NULL; t = t->tbl_next)
		if (t->owner == cur) {
			(void)__xtc_mtx_unlock(&__rcu.lock);
			return t;
		}
	(void)__xtc_mtx_unlock(&__rcu.lock);

	t = xtc_slab_alloc(__rcu_tls_slab);
	if (t == NULL) return NULL;
	memset(t, 0, sizeof *t);
	t->owner = cur;

	(void)__xtc_mtx_lock(&__rcu.lock);
	/*
	 * Recheck under the lock: a concurrent creation is impossible in
	 * the single-thread sim (only one fiber runs at a time and there
	 * is no preemption inside this critical section), but recheck
	 * anyway so the slab entry is not double-registered if this ever
	 * runs multi-threaded.
	 */
	{
		struct rcu_tls *e;
		for (e = __rcu_ftbl[h]; e != NULL; e = e->tbl_next)
			if (e->owner == cur) {
				(void)__xtc_mtx_unlock(&__rcu.lock);
				xtc_slab_free(__rcu_tls_slab, t);
				return e;
			}
	}
	t->tbl_next = __rcu_ftbl[h];
	__rcu_ftbl[h] = t;
	t->next = __rcu.registry;         /* synchronize scans this list */
	__rcu.registry = t;
	(void)__xtc_mtx_unlock(&__rcu.lock);
	return t;
}

/* The reader slot for the calling context: the fiber slot when on a
 * fiber (DST), else the OS-thread slot.  NULL on OOM. */
static struct rcu_tls *
__rcu_reader_slot(int create)
{
	xtc_task_t *cur = __xtc_current_task();
	if (cur != NULL)
		return __rcu_fiber_slot(cur);
	if (__rcu_self == NULL) {
		if (!create) return NULL;
		(void)__rcu_register();
	}
	return __rcu_self;
}

int
xtc_rcu_init(void)
{
	(void)__xtc_mtx_lock(&__rcu.lock);
	__rcu.inited = 1;
	(void)__xtc_mtx_unlock(&__rcu.lock);
	return XTC_OK;
}

void
xtc_rcu_fini(void)
{
	struct rcu_tls *t, *next_t;
	int b;
	(void)__xtc_mtx_lock(&__rcu.lock);
	for (b = 0; b < XTC_RCU_NBUCKETS; b++) {
		struct retired *r, *next_r;
		for (r = __rcu.buckets[b]; r != NULL; r = next_r) {
			next_r = r->next;
			r->fn(r->p);
			if (__rcu_retired_slab) xtc_slab_free(__rcu_retired_slab, r);
			else __os_free(r);
		}
		__rcu.buckets[b] = NULL;
	}
	for (t = __rcu.registry; t != NULL; t = next_t) {
		next_t = t->next;
		if (__rcu_tls_slab) xtc_slab_free(__rcu_tls_slab, t);
		else __os_free(t);
	}
	__rcu.registry = NULL;
	/* Fiber slots were registered into __rcu.registry (freed above);
	 * just drop the table's bucket heads and the thread slot pointer. */
	for (b = 0; b < XTC_RCU_FTBL_SIZE; b++)
		__rcu_ftbl[b] = NULL;
	__rcu_self = NULL;
	__rcu.inited = 0;
	atomic_store_explicit(&__rcu.epoch, 0, memory_order_relaxed);
	(void)__xtc_mtx_unlock(&__rcu.lock);
}

void
xtc_rcu_read_lock(void)
{
	struct rcu_tls *t = __rcu_reader_slot(1);
	if (t == NULL) return;            /* OOM at register; degrade */
	if (t->nest++ == 0) {
		uint64_t e = atomic_load_explicit(&__rcu.epoch,
		    memory_order_acquire);
		atomic_store_explicit(&t->active_epoch, e,
		    memory_order_release);
	}
}

void
xtc_rcu_read_unlock(void)
{
	struct rcu_tls *t = __rcu_reader_slot(0);
	if (t == NULL) return;
	if (--t->nest == 0)
		atomic_store_explicit(&t->active_epoch, 0,
		    memory_order_release);
}

void
xtc_rcu_retire(void *p, xtc_rcu_free_fn fn)
{
	struct retired *r;
	uint64_t e;
	int b;
	if (p == NULL || fn == NULL) return;
	__rcu_slabs_ensure();
	if (__rcu_retired_slab == NULL) {
		fn(p); return;
	}
	r = xtc_slab_alloc(__rcu_retired_slab);
	if (r == NULL) {
		/* Couldn't even record; fall back to immediate free.
		 * Caller has lost the safety guarantee but at least
		 * we don't leak.  This is best-effort. */
		fn(p);
		return;
	}
	r->p = p;
	r->fn = fn;
	(void)__xtc_mtx_lock(&__rcu.lock);
	e = atomic_load_explicit(&__rcu.epoch, memory_order_relaxed);
	b = (int)(e % XTC_RCU_NBUCKETS);
	r->next = __rcu.buckets[b];
	__rcu.buckets[b] = r;
	(void)__xtc_mtx_unlock(&__rcu.lock);
}

/* Wait for all readers in the OLD epoch (the one before we advanced)
 * to leave.  Then reclaim the bucket two epochs back. */
void
xtc_rcu_synchronize(void)
{
	uint64_t old, new_e;
	struct rcu_tls *t;
	struct retired *to_free, *r, *next;
	int reclaim_bucket;

	old = atomic_load_explicit(&__rcu.epoch, memory_order_acquire);
	new_e = old + 1;
	atomic_store_explicit(&__rcu.epoch, new_e, memory_order_release);

	/* Wait for all readers active in epoch <= old to drain.
	 * (Readers see the new epoch on their next read_lock.) */
	for (;;) {
		int still_in_old = 0;
		(void)__xtc_mtx_lock(&__rcu.lock);
		for (t = __rcu.registry; t != NULL; t = t->next) {
			uint64_t a = atomic_load_explicit(&t->active_epoch,
			    memory_order_acquire);
			if (a != 0 && a <= old) { still_in_old = 1; break; }
		}
		(void)__xtc_mtx_unlock(&__rcu.lock);
		if (!still_in_old) break;
		/*
		 * Hand control to a reader so it can leave the old epoch.
		 * On a fiber (DST) sched_yield does NOT reach the sim
		 * scheduler -- the writer fiber would spin forever and the
		 * reader fibers would never run to drain -- so yield
		 * cooperatively to the loop instead.  __current_proc is
		 * saved/restored across the yield (another proc runs while
		 * we are parked).  Off a fiber the original sched_yield()
		 * spin is unchanged.
		 */
		if (__xtc_current_task() != NULL) {
			void *proc_ctx = __xtc_proc_ctx_save();
			xtc_yield();
			__xtc_proc_ctx_restore(proc_ctx);
		} else {
			sched_yield();
		}
	}

	/* Reclaim the bucket that's now safe.  An object retired at
	 * epoch E sits in bucket (E % N).  By the time global epoch
	 * has advanced to E+2, all readers that could see E have left,
	 * so bucket (E % N) at epoch E+2 is recycle-safe. */
	if (new_e < 2) return;
	reclaim_bucket = (int)((new_e - 2) % XTC_RCU_NBUCKETS);
	(void)__xtc_mtx_lock(&__rcu.lock);
	to_free = __rcu.buckets[reclaim_bucket];
	__rcu.buckets[reclaim_bucket] = NULL;
	(void)__xtc_mtx_unlock(&__rcu.lock);

	for (r = to_free; r != NULL; r = next) {
		next = r->next;
		r->fn(r->p);
		if (__rcu_retired_slab) xtc_slab_free(__rcu_retired_slab, r);
		else __os_free(r);
	}
}

uint64_t
xtc_rcu_current_epoch(void)
{
	return atomic_load_explicit(&__rcu.epoch, memory_order_acquire);
}
