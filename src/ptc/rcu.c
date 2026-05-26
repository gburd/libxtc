/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
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
 */

#include "xtc_int.h"
#include "xtc_rcu.h"
#include "xtc_slab.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>

#define XTC_RCU_NBUCKETS  4

struct rcu_tls {
	_Atomic uint64_t  active_epoch;   /* 0 = not in a read-side */
	int               nest;
	struct rcu_tls   *next;           /* registered list */
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
static __thread struct rcu_tls *__rcu_self = NULL;

/* M11.5b: pools for rcu_tls registry entries and retired records. */
static xtc_slab_t   *__rcu_tls_slab     = NULL;
static xtc_slab_t   *__rcu_retired_slab = NULL;
static pthread_mutex_t __rcu_slab_init_lock = PTHREAD_MUTEX_INITIALIZER;

static void
__rcu_slabs_ensure(void)
{
	if (__rcu_tls_slab != NULL && __rcu_retired_slab != NULL) return;
	(void)pthread_mutex_lock(&__rcu_slab_init_lock);
	if (__rcu_tls_slab == NULL) {
		xtc_slab_opts_t o = XTC_SLAB_OPTS_DEFAULT;
		o.name = "rcu.tls"; o.obj_size = sizeof(struct rcu_tls);
		(void)xtc_slab_create(&o, &__rcu_tls_slab);
	}
	if (__rcu_retired_slab == NULL) {
		xtc_slab_opts_t o = XTC_SLAB_OPTS_DEFAULT;
		o.name = "rcu.retired"; o.obj_size = sizeof(struct retired);
		(void)xtc_slab_create(&o, &__rcu_retired_slab);
	}
	(void)pthread_mutex_unlock(&__rcu_slab_init_lock);
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
	(void)pthread_mutex_lock(&__rcu.lock);
	t->next = __rcu.registry;
	__rcu.registry = t;
	(void)pthread_mutex_unlock(&__rcu.lock);
	__rcu_self = t;
	return XTC_OK;
}

int
xtc_rcu_init(void)
{
	(void)pthread_mutex_lock(&__rcu.lock);
	__rcu.inited = 1;
	(void)pthread_mutex_unlock(&__rcu.lock);
	return XTC_OK;
}

void
xtc_rcu_fini(void)
{
	struct rcu_tls *t, *next_t;
	int b;
	(void)pthread_mutex_lock(&__rcu.lock);
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
	__rcu.inited = 0;
	atomic_store_explicit(&__rcu.epoch, 0, memory_order_relaxed);
	(void)pthread_mutex_unlock(&__rcu.lock);
}

void
xtc_rcu_read_lock(void)
{
	struct rcu_tls *t;
	if (__rcu_self == NULL) (void)__rcu_register();
	t = __rcu_self;
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
	struct rcu_tls *t = __rcu_self;
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
	(void)pthread_mutex_lock(&__rcu.lock);
	e = atomic_load_explicit(&__rcu.epoch, memory_order_relaxed);
	b = (int)(e % XTC_RCU_NBUCKETS);
	r->next = __rcu.buckets[b];
	__rcu.buckets[b] = r;
	(void)pthread_mutex_unlock(&__rcu.lock);
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
		(void)pthread_mutex_lock(&__rcu.lock);
		for (t = __rcu.registry; t != NULL; t = t->next) {
			uint64_t a = atomic_load_explicit(&t->active_epoch,
			    memory_order_acquire);
			if (a != 0 && a <= old) { still_in_old = 1; break; }
		}
		(void)pthread_mutex_unlock(&__rcu.lock);
		if (!still_in_old) break;
		/* Spin briefly then yield. */
		sched_yield();
	}

	/* Reclaim the bucket that's now safe.  An object retired at
	 * epoch E sits in bucket (E % N).  By the time global epoch
	 * has advanced to E+2, all readers that could see E have left,
	 * so bucket (E % N) at epoch E+2 is recycle-safe. */
	if (new_e < 2) return;
	reclaim_bucket = (int)((new_e - 2) % XTC_RCU_NBUCKETS);
	(void)pthread_mutex_lock(&__rcu.lock);
	to_free = __rcu.buckets[reclaim_bucket];
	__rcu.buckets[reclaim_bucket] = NULL;
	(void)pthread_mutex_unlock(&__rcu.lock);

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
