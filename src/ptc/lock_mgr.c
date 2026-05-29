/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/lock_mgr.c
 *	BDB-parity lock manager with 9-mode RIW lattice, configurable
 *	conflict matrix, 8 victim policies, lock-vec compound ops,
 *	upgrade/downgrade primitives, per-locker timeouts, statistics,
 *	failchk, and a slab pool for entry allocation.
 *
 *	Architecture mirrors libdb's lock manager (~/ws/libdb/src/lock/):
 *	  - N hash partitions; each owns a mutex + table of objects
 *	  - Each object owns granted-list + wait-list of entries
 *	  - Each entry is a (locker, mode, condvar, status) triple
 *	  - Lockers tracked in their own table (timestamps, n_held,
 *	    n_write_held, optional timeout, aborted flag)
 *	  - Detector (periodic / on-block / none) walks wait-for graph
 *	    and aborts victims per the configured policy
 *	  - Slab pool for lock_entry: fixed-size cache, falls back to
 *	    malloc on exhaustion
 */

#include "xtc_int.h"
#include "xtc_lockmgr.h"
#include "xtc_slab.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

/* ----- conflict matrix --------------------------------------- */

/*
 * Default 9-mode RIW conflict matrix, verbatim from libdb's
 * `db_riw_conflicts` (src/lock/lock_region.c).  1 = conflict.
 *
 * IMPORTANT: row/column ordering must match xtc_lock_mode_t.
 */
static const uint8_t XTC_DEFAULT_CONFLICTS_9[9 * 9] = {
	/*         NL  S   X   WT  IX  IS  IWR RU  WW  */
	/* NL  */   0,  0,  0,  0,  0,  0,  0,  0,  0,
	/* S   */   0,  0,  1,  0,  1,  0,  1,  0,  1,
	/* X   */   0,  1,  1,  1,  1,  1,  1,  1,  1,
	/* WT  */   0,  0,  0,  0,  0,  0,  0,  0,  0,
	/* IX  */   0,  1,  1,  0,  0,  0,  0,  1,  1,
	/* IS  */   0,  0,  1,  0,  0,  0,  0,  0,  1,
	/* IWR */   0,  1,  1,  0,  0,  0,  0,  1,  1,
	/* RU  */   0,  0,  1,  0,  1,  0,  1,  0,  0,
	/* WW  */   0,  1,  1,  0,  1,  1,  1,  0,  1
};

/* ----- structures -------------------------------------------- */

struct lock_entry {
	xtc_locker_t       locker;
	xtc_lock_mode_t    mode;
	pthread_cond_t     cv;
	int                granted;
	_Atomic int        aborted;
	int                from_pool;     /* 1 = recycle to slab, 0 = free() */
	struct lock_entry *prev;
	struct lock_entry *next;
	struct lock_obj   *obj;
};

struct lock_obj {
	void              *key;
	size_t             key_size;
	uint32_t           hash;
	struct lock_entry *granted;
	struct lock_entry *waiting;
	struct lock_entry *waiting_tail;
	struct lock_obj   *next;
};

struct lock_partition {
	pthread_mutex_t  lock;
	struct lock_obj *table;
};

struct locker_rec {
	xtc_locker_t       id;
	int64_t            ctime_ns;
	int64_t            timeout_ns;       /* -1 = no expiry */
	int64_t            deadline_ns;      /* ctime_ns + timeout_ns; -1 if none */
	_Atomic int        n_held;
	_Atomic int        n_write_held;     /* X / IX / IWR / WW */
	_Atomic int        aborted;
	_Atomic int        failed;
	struct locker_rec *next;
};

/* Slab pool for lock_entry. */
struct lock_pool {
	xtc_slab_t          *slab;          /* M11.5b: replaces hand-rolled free list */
	pthread_mutex_t      lock;          /* legacy hwm tracking */
	int                  hwm;           /* high-water mark for stats */
};

struct xtc_lockmgr {
	xtc_lockmgr_opts_t  opts;
	const uint8_t      *conflicts;       /* either default or user-supplied */
	int                 n_modes;

	struct lock_partition *parts;
	int                    n_parts;

	pthread_mutex_t        locker_lock;
	struct locker_rec     *locker_table[256];
	_Atomic uint64_t       next_locker;
	_Atomic int            n_lockers;

	struct lock_pool       pool;

	_Atomic int            n_held;
	_Atomic int            n_waiting;
	_Atomic int            n_objects;
	_Atomic uint64_t       n_acquires;
	_Atomic uint64_t       n_releases;
	_Atomic uint64_t       n_deadlocks_found;
	_Atomic uint64_t       n_timeouts;

	pthread_t              detector_thread;
	int                    detector_running;
	_Atomic int            detector_stop;
};

/* ----- helpers ----------------------------------------------- */

static int
__conflicts(xtc_lockmgr_t *m, xtc_lock_mode_t held, xtc_lock_mode_t req)
{
	int n = m->n_modes;
	if ((int)held >= n || (int)req >= n) return 1;
	return m->conflicts[(int)held * n + (int)req];
}

static int
__is_write_mode(xtc_lock_mode_t mode)
{
	switch (mode) {
	case XTC_LOCK_X:
	case XTC_LOCK_IX:
	case XTC_LOCK_IWR:
	case XTC_LOCK_WW:
		return 1;
	default:
		return 0;
	}
}

static int64_t
__lockmgr_now_ns(void)
{
	int64_t v = 0;
	(void)__os_clock_mono(&v);
	return v;
}

static uint32_t
__hash(const void *data, size_t size)
{
	const uint8_t *p = data;
	uint32_t h = 2166136261u;
	size_t i;
	for (i = 0; i < size; i++) { h ^= p[i]; h *= 16777619u; }
	return h ? h : 1;
}

/* ----- entry pool -------------------------------------------- */

#define POOL_HWM_LIMIT  4096   /* cap pool size to bound memory */

static struct lock_entry *
__entry_alloc(xtc_lockmgr_t *m)
{
	struct lock_entry *e = xtc_slab_alloc(m->pool.slab);
	if (e == NULL) return NULL;
	memset(e, 0, sizeof *e);
	(void)pthread_cond_init(&e->cv, NULL);
	return e;
}

static void
__entry_release(xtc_lockmgr_t *m, struct lock_entry *e)
{
	(void)pthread_cond_destroy(&e->cv);
	xtc_slab_free(m->pool.slab, e);
}

/* ----- locker IDs ------------------------------------------- */

static struct locker_rec *
__locker_find(xtc_lockmgr_t *m, xtc_locker_t id)
{
	struct locker_rec *r;
	int b = (int)(id & 0xff);
	for (r = m->locker_table[b]; r != NULL; r = r->next)
		if (r->id == id) return r;
	return NULL;
}

int
xtc_lockmgr_id(xtc_lockmgr_t *m, xtc_locker_t *out)
{
	struct locker_rec *r;
	int rc, b;
	if (m == NULL || out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *r, (void **)&r)) != XTC_OK) return rc;
	r->id = atomic_fetch_add_explicit(&m->next_locker, 1,
	    memory_order_relaxed) + 1;
	r->ctime_ns = __lockmgr_now_ns();
	r->timeout_ns = -1;
	r->deadline_ns = -1;
	b = (int)(r->id & 0xff);
	(void)pthread_mutex_lock(&m->locker_lock);
	r->next = m->locker_table[b];
	m->locker_table[b] = r;
	(void)pthread_mutex_unlock(&m->locker_lock);
	atomic_fetch_add_explicit(&m->n_lockers, 1, memory_order_relaxed);
	*out = r->id;
	return XTC_OK;
}

int
xtc_lockmgr_id_set_timeout(xtc_lockmgr_t *m, xtc_locker_t id, int64_t timeout_ns)
{
	struct locker_rec *r;
	if (m == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&m->locker_lock);
	r = __locker_find(m, id);
	if (r != NULL) {
		r->timeout_ns = timeout_ns;
		r->deadline_ns = (timeout_ns < 0) ? -1 : r->ctime_ns + timeout_ns;
	}
	(void)pthread_mutex_unlock(&m->locker_lock);
	return r ? XTC_OK : XTC_E_INVAL;
}

int
xtc_lockmgr_id_free(xtc_lockmgr_t *m, xtc_locker_t id)
{
	struct locker_rec *r, **link;
	int b;
	if (m == NULL) return XTC_E_INVAL;
	(void)xtc_lock_release_all(m, id);
	b = (int)(id & 0xff);
	(void)pthread_mutex_lock(&m->locker_lock);
	for (link = &m->locker_table[b]; (r = *link) != NULL; link = &r->next) {
		if (r->id == id) {
			*link = r->next;
			(void)pthread_mutex_unlock(&m->locker_lock);
			__os_free(r);
			atomic_fetch_sub_explicit(&m->n_lockers, 1,
			    memory_order_relaxed);
			return XTC_OK;
		}
	}
	(void)pthread_mutex_unlock(&m->locker_lock);
	return XTC_E_INVAL;
}

/* ----- object lookup --------------------------------------- */

static struct lock_obj *
__obj_lookup(struct lock_partition *p, const void *key, size_t key_size,
             uint32_t hash)
{
	struct lock_obj *o;
	for (o = p->table; o != NULL; o = o->next)
		if (o->hash == hash && o->key_size == key_size &&
		    memcmp(o->key, key, key_size) == 0) return o;
	return NULL;
}

static struct lock_obj *
__obj_get(xtc_lockmgr_t *m, struct lock_partition *p, const void *key,
          size_t key_size, uint32_t hash, int create)
{
	struct lock_obj *o = __obj_lookup(p, key, key_size, hash);
	if (o != NULL || !create) return o;
	if (__os_calloc(1, sizeof *o, (void **)&o) != XTC_OK) return NULL;
	if (key_size > 0) {
		o->key = malloc(key_size);
		if (o->key == NULL) { __os_free(o); return NULL; }
		memcpy(o->key, key, key_size);
	}
	o->key_size = key_size;
	o->hash = hash;
	o->next = p->table;
	p->table = o;
	atomic_fetch_add_explicit(&m->n_objects, 1, memory_order_relaxed);
	return o;
}

static void
__obj_maybe_free(xtc_lockmgr_t *m, struct lock_partition *p, struct lock_obj *o)
{
	struct lock_obj **link;
	if (o->granted != NULL || o->waiting != NULL) return;
	for (link = &p->table; *link != NULL; link = &(*link)->next) {
		if (*link == o) { *link = o->next; break; }
	}
	__os_free(o->key);
	__os_free(o);
	atomic_fetch_sub_explicit(&m->n_objects, 1, memory_order_relaxed);
}

/* ----- conflict checks ------------------------------------- */

static int
__has_conflict_granted(xtc_lockmgr_t *m, struct lock_obj *o,
                       xtc_locker_t locker, xtc_lock_mode_t mode)
{
	struct lock_entry *e;
	for (e = o->granted; e != NULL; e = e->next) {
		if (e->locker == locker) continue;
		if (__conflicts(m, e->mode, mode)) return 1;
	}
	return 0;
}

static int
__has_waiters(struct lock_obj *o)
{
	return o->waiting != NULL;
}

static struct lock_entry *
__find_granted(struct lock_obj *o, xtc_locker_t locker)
{
	struct lock_entry *e;
	for (e = o->granted; e != NULL; e = e->next)
		if (e->locker == locker) return e;
	return NULL;
}

/* ----- create/destroy -------------------------------------- */

static void *__detector_thread(void *arg);

int
xtc_lockmgr_create(const xtc_lockmgr_opts_t *opts, xtc_lockmgr_t **out)
{
	xtc_lockmgr_t *m;
	xtc_lockmgr_opts_t defaults = XTC_LOCKMGR_OPTS_DEFAULT;
	int rc, i;
	if (out == NULL) return XTC_E_INVAL;
	if (opts == NULL) opts = &defaults;
	if ((rc = __os_calloc(1, sizeof *m, (void **)&m)) != XTC_OK) return rc;
	m->opts = *opts;
	if (m->opts.n_partitions <= 0) m->opts.n_partitions = 64;
	if (m->opts.detect_interval_ns <= 0)
		m->opts.detect_interval_ns = 100LL * 1000 * 1000;
	m->n_parts = m->opts.n_partitions;
	(void)pthread_mutex_init(&m->locker_lock, NULL);
	(void)pthread_mutex_init(&m->pool.lock, NULL);

	/* M11.5b: lock_entry pool now backed by xtc_slab. */
	{
		xtc_slab_opts_t so = XTC_SLAB_OPTS_DEFAULT;
		so.name = "lockmgr.lock_entry";
		so.obj_size = sizeof(struct lock_entry);
		if ((rc = xtc_slab_create(&so, &m->pool.slab)) != XTC_OK) {
			(void)pthread_mutex_destroy(&m->pool.lock);
			(void)pthread_mutex_destroy(&m->locker_lock);
			__os_free(m);
			return rc;
		}
	}

	if (m->opts.conflicts != NULL && m->opts.n_modes > 0) {
		m->conflicts = m->opts.conflicts;
		m->n_modes = m->opts.n_modes;
	} else {
		m->conflicts = XTC_DEFAULT_CONFLICTS_9;
		m->n_modes = XTC_LOCK_NMODES;
	}

	if ((rc = __os_calloc((size_t)m->n_parts, sizeof *m->parts,
	    (void **)&m->parts)) != XTC_OK) {
		(void)pthread_mutex_destroy(&m->pool.lock);
		(void)pthread_mutex_destroy(&m->locker_lock);
		__os_free(m);
		return rc;
	}
	for (i = 0; i < m->n_parts; i++)
		(void)pthread_mutex_init(&m->parts[i].lock, NULL);

	if (m->opts.detect_mode == XTC_LOCK_DETECT_PERIODIC) {
		if (pthread_create(&m->detector_thread, NULL,
		    __detector_thread, m) == 0)
			m->detector_running = 1;
	}
	*out = m;
	return XTC_OK;
}

void
xtc_lockmgr_destroy(xtc_lockmgr_t *m)
{
	int i;
	struct locker_rec *r, *nx;
	struct lock_entry *e;
	if (m == NULL) return;
	if (m->detector_running) {
		atomic_store_explicit(&m->detector_stop, 1,
		    memory_order_release);
		(void)pthread_join(m->detector_thread, NULL);
	}
	for (i = 0; i < m->n_parts; i++) {
		struct lock_obj *o, *no;
		for (o = m->parts[i].table; o != NULL; o = no) {
			struct lock_entry *ne;
			no = o->next;
			for (e = o->granted; e != NULL; e = ne) {
				ne = e->next;
				(void)pthread_cond_destroy(&e->cv);
				__os_free(e);
			}
			for (e = o->waiting; e != NULL; e = ne) {
				ne = e->next;
				(void)pthread_cond_destroy(&e->cv);
				__os_free(e);
			}
			__os_free(o->key);
			__os_free(o);
		}
		(void)pthread_mutex_destroy(&m->parts[i].lock);
	}
	for (i = 0; i < 256; i++) {
		for (r = m->locker_table[i]; r != NULL; r = nx) {
			nx = r->next;
			__os_free(r);
		}
	}
	/* Drain the pool. */
	xtc_slab_destroy(m->pool.slab);
	(void)pthread_mutex_destroy(&m->pool.lock);
	(void)pthread_mutex_destroy(&m->locker_lock);
	__os_free(m->parts);
	__os_free(m);
}

/* ----- acquire / release / upgrade / downgrade ------------ */

static int
__do_acquire_locked(xtc_lockmgr_t *m, struct lock_partition *p,
                    struct lock_obj *o, xtc_locker_t locker,
                    xtc_lock_mode_t mode, int64_t timeout_ns)
{
	struct lock_entry *e, *prior;
	struct locker_rec *lr;
	int rc;

	prior = __find_granted(o, locker);
	if (prior != NULL && !__conflicts(m, prior->mode, mode)) {
		if ((int)mode > (int)prior->mode) prior->mode = mode;
		return XTC_OK;
	}

	if (!__has_conflict_granted(m, o, locker, mode) && !__has_waiters(o)) {
		e = __entry_alloc(m);
		if (e == NULL) return XTC_E_NOMEM;
		e->locker = locker;
		e->mode = mode;
		e->granted = 1;
		e->obj = o;
		e->next = o->granted;
		if (o->granted) o->granted->prev = e;
		o->granted = e;
		atomic_fetch_add_explicit(&m->n_held, 1, memory_order_relaxed);
		atomic_fetch_add_explicit(&m->n_acquires, 1, memory_order_relaxed);
		(void)pthread_mutex_lock(&m->locker_lock);
		lr = __locker_find(m, locker);
		(void)pthread_mutex_unlock(&m->locker_lock);
		if (lr) {
			atomic_fetch_add_explicit(&lr->n_held, 1,
			    memory_order_relaxed);
			if (__is_write_mode(mode))
				atomic_fetch_add_explicit(&lr->n_write_held, 1,
				    memory_order_relaxed);
		}
		return XTC_OK;
	}

	if (timeout_ns == 0) return XTC_E_AGAIN;

	e = __entry_alloc(m);
	if (e == NULL) return XTC_E_NOMEM;
	e->locker = locker;
	e->mode = mode;
	e->granted = 0;
	e->obj = o;
	e->prev = o->waiting_tail;
	if (o->waiting_tail) o->waiting_tail->next = e;
	else                 o->waiting        = e;
	o->waiting_tail = e;
	atomic_fetch_add_explicit(&m->n_waiting, 1, memory_order_relaxed);

	/* If detect-on-block, kick the detector synchronously. */
	if (m->opts.detect_mode == XTC_LOCK_DETECT_ON_BLOCK) {
		(void)pthread_mutex_unlock(&p->lock);
		(void)xtc_lockmgr_check_deadlocks(m, NULL);
		(void)pthread_mutex_lock(&p->lock);
	}

	rc = XTC_OK;
	for (;;) {
		if (atomic_load_explicit(&e->aborted, memory_order_acquire)) {
			rc = XTC_E_DEADLK;
			break;
		}
		if (e->granted) { rc = XTC_OK; break; }

		if (timeout_ns < 0) {
			(void)pthread_cond_wait(&e->cv, &p->lock);
		} else {
			struct timespec ts;
			int64_t now;
			int wait_rc;
			(void)__os_clock_real(&now);
			now += timeout_ns;
			ts.tv_sec  = (time_t)(now / 1000000000LL);
			ts.tv_nsec = (long)(now % 1000000000LL);
			wait_rc = pthread_cond_timedwait(&e->cv, &p->lock, &ts);
			if (wait_rc != 0 && !e->granted &&
			    !atomic_load_explicit(&e->aborted,
			        memory_order_acquire)) {
				rc = XTC_E_AGAIN;
				atomic_fetch_add_explicit(&m->n_timeouts, 1,
				    memory_order_relaxed);
				break;
			}
		}
	}

	if (rc != XTC_OK || !e->granted) {
		if (e->prev) e->prev->next = e->next;
		else         o->waiting    = e->next;
		if (e->next) e->next->prev = e->prev;
		else         o->waiting_tail = e->prev;
		atomic_fetch_sub_explicit(&m->n_waiting, 1,
		    memory_order_relaxed);
		__entry_release(m, e);
		__obj_maybe_free(m, p, o);
	} else {
		atomic_fetch_sub_explicit(&m->n_waiting, 1,
		    memory_order_relaxed);
		atomic_fetch_add_explicit(&m->n_held, 1, memory_order_relaxed);
		atomic_fetch_add_explicit(&m->n_acquires, 1, memory_order_relaxed);
		(void)pthread_mutex_lock(&m->locker_lock);
		lr = __locker_find(m, locker);
		(void)pthread_mutex_unlock(&m->locker_lock);
		if (lr) {
			atomic_fetch_add_explicit(&lr->n_held, 1,
			    memory_order_relaxed);
			if (__is_write_mode(mode))
				atomic_fetch_add_explicit(&lr->n_write_held, 1,
				    memory_order_relaxed);
		}
	}
	return rc;
}

int
xtc_lock_get(xtc_lockmgr_t *m, xtc_locker_t locker,
             const void *obj, size_t obj_size,
             xtc_lock_mode_t mode, int64_t timeout_ns)
{
	uint32_t h;
	struct lock_partition *p;
	struct lock_obj *o;
	int rc;

	if (m == NULL || obj == NULL || obj_size == 0) return XTC_E_INVAL;
	if (mode <= XTC_LOCK_NL || mode >= (xtc_lock_mode_t)m->n_modes)
		return XTC_E_INVAL;
	if (mode == XTC_LOCK_WAIT) return XTC_E_INVAL;  /* never get'able */

	h = __hash(obj, obj_size);
	p = &m->parts[h % (uint32_t)m->n_parts];
	(void)pthread_mutex_lock(&p->lock);
	o = __obj_get(m, p, obj, obj_size, h, 1);
	if (o == NULL) {
		(void)pthread_mutex_unlock(&p->lock);
		return XTC_E_NOMEM;
	}
	rc = __do_acquire_locked(m, p, o, locker, mode, timeout_ns);
	(void)pthread_mutex_unlock(&p->lock);
	return rc;
}

static void
__release_entry_locked(xtc_lockmgr_t *m, struct lock_partition *p,
                       struct lock_obj *o, struct lock_entry *e)
{
	struct lock_entry *w, *nw;
	struct locker_rec *lr;

	if (e->granted) {
		atomic_fetch_sub_explicit(&m->n_held, 1, memory_order_relaxed);
		atomic_fetch_add_explicit(&m->n_releases, 1, memory_order_relaxed);
		(void)pthread_mutex_lock(&m->locker_lock);
		lr = __locker_find(m, e->locker);
		(void)pthread_mutex_unlock(&m->locker_lock);
		if (lr) {
			atomic_fetch_sub_explicit(&lr->n_held, 1,
			    memory_order_relaxed);
			if (__is_write_mode(e->mode))
				atomic_fetch_sub_explicit(&lr->n_write_held, 1,
				    memory_order_relaxed);
		}
		if (e->prev) e->prev->next = e->next;
		else         o->granted    = e->next;
		if (e->next) e->next->prev = e->prev;
	} else {
		atomic_fetch_sub_explicit(&m->n_waiting, 1,
		    memory_order_relaxed);
		if (e->prev) e->prev->next = e->next;
		else         o->waiting    = e->next;
		if (e->next) e->next->prev = e->prev;
		else         o->waiting_tail = e->prev;
	}
	__entry_release(m, e);

	/* Promote head-of-line waiters that no longer conflict. */
	for (w = o->waiting; w != NULL; w = nw) {
		nw = w->next;
		if (__has_conflict_granted(m, o, w->locker, w->mode)) break;
		if (w->prev) w->prev->next = w->next;
		else         o->waiting    = w->next;
		if (w->next) w->next->prev = w->prev;
		else         o->waiting_tail = w->prev;
		w->prev = NULL;
		w->next = o->granted;
		if (o->granted) o->granted->prev = w;
		o->granted = w;
		w->granted = 1;
		(void)pthread_cond_signal(&w->cv);
	}

	__obj_maybe_free(m, p, o);
}

int
xtc_lock_put(xtc_lockmgr_t *m, xtc_locker_t locker,
             const void *obj, size_t obj_size)
{
	uint32_t h;
	struct lock_partition *p;
	struct lock_obj *o;
	struct lock_entry *e;
	int rc = XTC_E_INVAL;

	if (m == NULL || obj == NULL || obj_size == 0) return XTC_E_INVAL;
	h = __hash(obj, obj_size);
	p = &m->parts[h % (uint32_t)m->n_parts];
	(void)pthread_mutex_lock(&p->lock);
	o = __obj_lookup(p, obj, obj_size, h);
	if (o != NULL) {
		for (e = o->granted; e != NULL; e = e->next) {
			if (e->locker == locker) {
				__release_entry_locked(m, p, o, e);
				rc = XTC_OK;
				break;
			}
		}
	}
	(void)pthread_mutex_unlock(&p->lock);
	return rc;
}

int
xtc_lock_release_all(xtc_lockmgr_t *m, xtc_locker_t locker)
{
	int i, rel = 0;
	if (m == NULL) return XTC_E_INVAL;
	for (i = 0; i < m->n_parts; i++) {
		struct lock_partition *p = &m->parts[i];
		struct lock_obj *o, *no;
		(void)pthread_mutex_lock(&p->lock);
		for (o = p->table; o != NULL; o = no) {
			struct lock_entry *e, *ne;
			no = o->next;
			/* Abort this locker's waiters first.  This only sets
			 * the aborted flag and signals; it does not free o.
			 * The granted-release loop runs last because its final
			 * __release_entry_locked may free o via __obj_maybe_free
			 * -- so nothing may touch o afterward.  (Same ordering
			 * the deadlock-victim path uses.) */
			for (e = o->waiting; e != NULL; e = ne) {
				ne = e->next;
				if (e->locker == locker) {
					atomic_store_explicit(&e->aborted, 1,
					    memory_order_release);
					(void)pthread_cond_signal(&e->cv);
				}
			}
			for (e = o->granted; e != NULL; e = ne) {
				ne = e->next;
				if (e->locker == locker) {
					__release_entry_locked(m, p, o, e);
					rel++;
				}
			}
		}
		(void)pthread_mutex_unlock(&p->lock);
	}
	return rel;
}

int
xtc_lock_upgrade(xtc_lockmgr_t *m, xtc_locker_t locker,
                 const void *obj, size_t obj_size, xtc_lock_mode_t new_mode)
{
	uint32_t h;
	struct lock_partition *p;
	struct lock_obj *o;
	struct lock_entry *cur;
	int rc;

	if (m == NULL || obj == NULL || obj_size == 0) return XTC_E_INVAL;
	if (new_mode <= XTC_LOCK_NL || new_mode >= (xtc_lock_mode_t)m->n_modes)
		return XTC_E_INVAL;
	h = __hash(obj, obj_size);
	p = &m->parts[h % (uint32_t)m->n_parts];
	(void)pthread_mutex_lock(&p->lock);
	o = __obj_lookup(p, obj, obj_size, h);
	if (o == NULL) {
		(void)pthread_mutex_unlock(&p->lock);
		return XTC_E_INVAL;
	}
	cur = __find_granted(o, locker);
	if (cur == NULL) {
		(void)pthread_mutex_unlock(&p->lock);
		return XTC_E_INVAL;
	}
	if ((int)new_mode <= (int)cur->mode) {
		(void)pthread_mutex_unlock(&p->lock);
		return XTC_E_INVAL;     /* not an upgrade */
	}
	/* Check conflicts excluding self. */
	if (__has_conflict_granted(m, o, locker, new_mode)) {
		/* Block via the standard wait-queue path. */
		rc = __do_acquire_locked(m, p, o, locker, new_mode, -1);
		(void)pthread_mutex_unlock(&p->lock);
		return rc;
	}
	/* In-place upgrade. */
	{
		struct locker_rec *lr;
		(void)pthread_mutex_lock(&m->locker_lock);
		lr = __locker_find(m, locker);
		(void)pthread_mutex_unlock(&m->locker_lock);
		if (lr && !__is_write_mode(cur->mode) && __is_write_mode(new_mode))
			atomic_fetch_add_explicit(&lr->n_write_held, 1,
			    memory_order_relaxed);
		else if (lr && __is_write_mode(cur->mode) && !__is_write_mode(new_mode))
			atomic_fetch_sub_explicit(&lr->n_write_held, 1,
			    memory_order_relaxed);
		cur->mode = new_mode;
	}
	(void)pthread_mutex_unlock(&p->lock);
	return XTC_OK;
}

int
xtc_lock_downgrade(xtc_lockmgr_t *m, xtc_locker_t locker,
                   const void *obj, size_t obj_size, xtc_lock_mode_t new_mode)
{
	uint32_t h;
	struct lock_partition *p;
	struct lock_obj *o;
	struct lock_entry *cur, *w, *nw;
	struct locker_rec *lr;

	if (m == NULL || obj == NULL || obj_size == 0) return XTC_E_INVAL;
	if (new_mode < XTC_LOCK_NL || new_mode >= (xtc_lock_mode_t)m->n_modes)
		return XTC_E_INVAL;
	h = __hash(obj, obj_size);
	p = &m->parts[h % (uint32_t)m->n_parts];
	(void)pthread_mutex_lock(&p->lock);
	o = __obj_lookup(p, obj, obj_size, h);
	if (o == NULL) { (void)pthread_mutex_unlock(&p->lock); return XTC_E_INVAL; }
	cur = __find_granted(o, locker);
	if (cur == NULL) { (void)pthread_mutex_unlock(&p->lock); return XTC_E_INVAL; }
	if ((int)new_mode > (int)cur->mode) {
		(void)pthread_mutex_unlock(&p->lock);
		return XTC_E_INVAL;     /* not a downgrade */
	}

	(void)pthread_mutex_lock(&m->locker_lock);
	lr = __locker_find(m, locker);
	(void)pthread_mutex_unlock(&m->locker_lock);
	if (lr && __is_write_mode(cur->mode) && !__is_write_mode(new_mode))
		atomic_fetch_sub_explicit(&lr->n_write_held, 1,
		    memory_order_relaxed);

	cur->mode = new_mode;
	/* Promote head-of-line waiters now that we hold a weaker mode. */
	for (w = o->waiting; w != NULL; w = nw) {
		nw = w->next;
		if (__has_conflict_granted(m, o, w->locker, w->mode)) break;
		if (w->prev) w->prev->next = w->next;
		else         o->waiting    = w->next;
		if (w->next) w->next->prev = w->prev;
		else         o->waiting_tail = w->prev;
		w->prev = NULL;
		w->next = o->granted;
		if (o->granted) o->granted->prev = w;
		o->granted = w;
		w->granted = 1;
		(void)pthread_cond_signal(&w->cv);
	}
	(void)pthread_mutex_unlock(&p->lock);
	return XTC_OK;
}

/* ----- lock_vec compound ------------------------------------ */

int
xtc_lock_vec(xtc_lockmgr_t *m, xtc_locker_t locker,
             xtc_lock_req_t *reqs, int n_reqs, int *out_executed)
{
	int i, executed = 0, rc = XTC_OK;
	if (m == NULL || reqs == NULL || n_reqs <= 0) return XTC_E_INVAL;

	for (i = 0; i < n_reqs; i++) {
		xtc_lock_req_t *r = &reqs[i];
		switch (r->op) {
		case XTC_LOCK_OP_GET:
			rc = xtc_lock_get(m, locker, r->obj, r->obj_size,
			    r->mode, r->timeout_ns);
			break;
		case XTC_LOCK_OP_PUT:
			rc = xtc_lock_put(m, locker, r->obj, r->obj_size);
			break;
		case XTC_LOCK_OP_PUT_ALL:
			(void)xtc_lock_release_all(m, locker);
			rc = XTC_OK;
			break;
		case XTC_LOCK_OP_UPGRADE:
			rc = xtc_lock_upgrade(m, locker, r->obj, r->obj_size,
			    r->mode);
			break;
		case XTC_LOCK_OP_DOWNGRADE:
			rc = xtc_lock_downgrade(m, locker, r->obj, r->obj_size,
			    r->mode);
			break;
		default:
			rc = XTC_E_INVAL;
			break;
		}
		if (rc != XTC_OK) break;
		executed++;
	}
	if (out_executed) *out_executed = executed;
	return rc;
}

/* ----- failchk ---------------------------------------------- */

int
xtc_lockmgr_failchk(xtc_lockmgr_t *m, xtc_locker_t locker)
{
	struct locker_rec *r;
	if (m == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&m->locker_lock);
	r = __locker_find(m, locker);
	if (r) atomic_store_explicit(&r->failed, 1, memory_order_release);
	(void)pthread_mutex_unlock(&m->locker_lock);
	if (r) {
		(void)xtc_lock_release_all(m, locker);
		return XTC_OK;
	}
	return XTC_E_INVAL;
}

/* ----- deadlock detection ---------------------------------- */

#define MAX_LOCKERS  4096
struct dd_node {
	xtc_locker_t  id;
	int64_t       ctime;
	int64_t       deadline;
	int           n_held;
	int           n_write_held;
	int           expired;
	int           out_first;
};
struct dd_edge {
	int next;
	int to;
};
struct dd_state {
	struct dd_node nodes[MAX_LOCKERS];
	int            n_nodes;
	struct dd_edge edges[MAX_LOCKERS * 8];
	int            n_edges;
};

static int
__dd_node_idx(struct dd_state *st, xtc_locker_t id, struct locker_rec *lr,
              int64_t now)
{
	int i;
	for (i = 0; i < st->n_nodes; i++)
		if (st->nodes[i].id == id) return i;
	if (st->n_nodes >= MAX_LOCKERS) return -1;
	st->nodes[st->n_nodes].id = id;
	st->nodes[st->n_nodes].ctime = lr ? lr->ctime_ns : 0;
	st->nodes[st->n_nodes].deadline = lr ? lr->deadline_ns : -1;
	st->nodes[st->n_nodes].expired = (lr && lr->deadline_ns > 0 &&
	    now > lr->deadline_ns) ? 1 : 0;
	st->nodes[st->n_nodes].n_held = lr ?
	    atomic_load_explicit(&lr->n_held, memory_order_relaxed) : 0;
	st->nodes[st->n_nodes].n_write_held = lr ?
	    atomic_load_explicit(&lr->n_write_held, memory_order_relaxed) : 0;
	st->nodes[st->n_nodes].out_first = -1;
	return st->n_nodes++;
}

static void
__dd_add_edge(struct dd_state *st, int from, int to)
{
	int i;
	if (from < 0 || to < 0 || st->n_edges >= MAX_LOCKERS * 8) return;
	for (i = st->nodes[from].out_first; i >= 0; i = st->edges[i].next)
		if (st->edges[i].to == to) return;
	st->edges[st->n_edges].next = st->nodes[from].out_first;
	st->edges[st->n_edges].to   = to;
	st->nodes[from].out_first   = st->n_edges;
	st->n_edges++;
}

static int
__dd_dfs(struct dd_state *st, int u, char *color, int *parent, int *cycle_in)
{
	int e_idx;
	color[u] = 1;
	for (e_idx = st->nodes[u].out_first; e_idx >= 0;
	     e_idx = st->edges[e_idx].next) {
		int v = st->edges[e_idx].to;
		if (color[v] == 1) { *cycle_in = u; return 1; }
		if (color[v] == 0) {
			parent[v] = u;
			if (__dd_dfs(st, v, color, parent, cycle_in)) return 1;
		}
	}
	color[u] = 2;
	return 0;
}

/* Score a node for victim selection.  Higher = more likely victim. */
static int64_t
__dd_score(xtc_lockmgr_t *m, struct dd_node *n)
{
	switch (m->opts.victim) {
	case XTC_LOCK_VICTIM_OLDEST:    return -n->ctime;
	case XTC_LOCK_VICTIM_YOUNGEST:  return  n->ctime;
	case XTC_LOCK_VICTIM_MIN_LOCKS: return -n->n_held;
	case XTC_LOCK_VICTIM_MAX_LOCKS: return  n->n_held;
	case XTC_LOCK_VICTIM_MIN_WRITE: return -n->n_write_held;
	case XTC_LOCK_VICTIM_MAX_WRITE: return  n->n_write_held;
	case XTC_LOCK_VICTIM_EXPIRE:    return n->expired ? 1 : -1;
	case XTC_LOCK_VICTIM_RANDOM:
	case XTC_LOCK_VICTIM_DEFAULT:
	default:
		return rand();
	}
}

static int
__dd_pick_victim(xtc_lockmgr_t *m, struct dd_state *st,
                 int cycle_in, const int *parent)
{
	int u = cycle_in, victim = u;
	int64_t best_score;
	int seen_any = 0;

	/* Custom-picker mode: collect candidates around the cycle, then
	 * delegate.  We pass locker IDs (the user-visible identifier) so
	 * the callback can render decisions in stable terms. */
	if (m->opts.victim == XTC_LOCK_VICTIM_CUSTOM &&
	    m->opts.victim_pick_fn != NULL) {
		uint64_t cand[64];
		int n = 0;
		u = cycle_in;
		for (;;) {
			if (n < (int)(sizeof cand / sizeof cand[0]))
				cand[n++] = (uint64_t)u;
			if (parent[u] < 0) break;
			u = parent[u];
			if (u == cycle_in) break;
		}
		if (n > 0) {
			int pick = m->opts.victim_pick_fn(cand, n,
			    m->opts.victim_pick_user);
			if (pick >= 0 && pick < n)
				return (int)cand[pick];
		}
		/* Fall through to default scoring on empty / bad pick. */
	}

	best_score = __dd_score(m, &st->nodes[u]);
	for (;;) {
		int64_t s = __dd_score(m, &st->nodes[u]);
		if (m->opts.victim == XTC_LOCK_VICTIM_EXPIRE) {
			/* Only pick expired nodes; if none in the cycle,
			 * fall through to default below. */
			if (st->nodes[u].expired && (!seen_any || s > best_score))
				{ victim = u; best_score = s; seen_any = 1; }
		} else {
			if (s > best_score) { victim = u; best_score = s; }
		}
		if (parent[u] < 0) break;
		u = parent[u];
		if (u == cycle_in) break;
	}
	if (m->opts.victim == XTC_LOCK_VICTIM_EXPIRE && !seen_any)
		return -1;       /* no expired victim; leave cycle alone */
	return victim;
}

int
xtc_lockmgr_check_deadlocks(xtc_lockmgr_t *m, int *n_aborted)
{
	struct dd_state *st;
	char *color;
	int  *parent;
	int  i, cycle_in, victim;
	int  rc, n_killed = 0;
	int64_t now = __lockmgr_now_ns();

	if (m == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *st, (void **)&st)) != XTC_OK) return rc;
	if ((rc = __os_calloc(MAX_LOCKERS, 1, (void **)&color)) != XTC_OK) {
		__os_free(st); return rc;
	}
	if ((rc = __os_calloc(MAX_LOCKERS, sizeof(int), (void **)&parent)) != XTC_OK) {
		__os_free(color); __os_free(st); return rc;
	}

	for (i = 0; i < m->n_parts; i++) {
		struct lock_partition *p = &m->parts[i];
		struct lock_obj *o;
		(void)pthread_mutex_lock(&p->lock);
		for (o = p->table; o != NULL; o = o->next) {
			struct lock_entry *w, *g;
			for (w = o->waiting; w != NULL; w = w->next) {
				struct locker_rec *wl;
				int from;
				(void)pthread_mutex_lock(&m->locker_lock);
				wl = __locker_find(m, w->locker);
				(void)pthread_mutex_unlock(&m->locker_lock);
				from = __dd_node_idx(st, w->locker, wl, now);
				for (g = o->granted; g != NULL; g = g->next) {
					struct locker_rec *gl;
					int to;
					if (g->locker == w->locker) continue;
					if (!__conflicts(m, g->mode, w->mode))
						continue;
					(void)pthread_mutex_lock(&m->locker_lock);
					gl = __locker_find(m, g->locker);
					(void)pthread_mutex_unlock(&m->locker_lock);
					to = __dd_node_idx(st, g->locker, gl, now);
					__dd_add_edge(st, from, to);
				}
			}
		}
	}

	for (;;) {
		memset(color, 0, MAX_LOCKERS);
		for (i = 0; i < st->n_nodes; i++) parent[i] = -1;
		cycle_in = -1;
		for (i = 0; i < st->n_nodes; i++) {
			if (color[i] == 0 && __dd_dfs(st, i, color, parent,
			    &cycle_in)) break;
		}
		if (cycle_in < 0) break;
		victim = __dd_pick_victim(m, st, cycle_in, parent);
		if (victim < 0) {
			/* EXPIRE policy with no expired victim -- break out. */
			st->nodes[cycle_in].out_first = -1;
			continue;
		}
		{
			xtc_locker_t vid = st->nodes[victim].id;
			int j;
			for (j = 0; j < m->n_parts; j++) {
				struct lock_obj *o, *no;
				for (o = m->parts[j].table; o != NULL; o = no) {
					struct lock_entry *e, *ne;
					no = o->next;
					for (e = o->waiting; e != NULL; e = ne) {
						ne = e->next;
						if (e->locker != vid) continue;
						atomic_store_explicit(&e->aborted, 1,
						    memory_order_release);
						(void)pthread_cond_signal(&e->cv);
					}
					for (e = o->granted; e != NULL; e = ne) {
						ne = e->next;
						if (e->locker == vid)
							__release_entry_locked(m,
							    &m->parts[j], o, e);
					}
				}
			}
			st->nodes[victim].out_first = -1;
			n_killed++;
			atomic_fetch_add_explicit(&m->n_deadlocks_found, 1,
			    memory_order_relaxed);
		}
	}

	for (i = m->n_parts - 1; i >= 0; i--)
		(void)pthread_mutex_unlock(&m->parts[i].lock);

	__os_free(parent);
	__os_free(color);
	__os_free(st);
	if (n_aborted) *n_aborted = n_killed;
	return XTC_OK;
}

/* ----- detector thread ------------------------------------ */

static void *
__detector_thread(void *arg)
{
	xtc_lockmgr_t *m = arg;
	struct timespec ts;
	int64_t  iv = m->opts.detect_interval_ns;
	ts.tv_sec  = (time_t)(iv / 1000000000LL);
	ts.tv_nsec = (long)(iv % 1000000000LL);
	while (!atomic_load_explicit(&m->detector_stop, memory_order_acquire)) {
		(void)nanosleep(&ts, NULL);
		(void)xtc_lockmgr_check_deadlocks(m, NULL);
	}
	return NULL;
}

/* ----- stats ---------------------------------------------- */

int
xtc_lockmgr_stat(const xtc_lockmgr_t *m, xtc_lockmgr_stat_t *out)
{
	xtc_lockmgr_t *mm = (xtc_lockmgr_t *)m;
	if (m == NULL || out == NULL) return XTC_E_INVAL;
	out->n_held    = atomic_load_explicit(&mm->n_held,    memory_order_relaxed);
	out->n_waiting = atomic_load_explicit(&mm->n_waiting, memory_order_relaxed);
	out->n_objects = atomic_load_explicit(&mm->n_objects, memory_order_relaxed);
	out->n_lockers = atomic_load_explicit(&mm->n_lockers, memory_order_relaxed);
	out->n_acquires = atomic_load_explicit(&mm->n_acquires,
	    memory_order_relaxed);
	out->n_releases = atomic_load_explicit(&mm->n_releases,
	    memory_order_relaxed);
	out->n_deadlocks_found = atomic_load_explicit(&mm->n_deadlocks_found,
	    memory_order_relaxed);
	out->n_timeouts = atomic_load_explicit(&mm->n_timeouts,
	    memory_order_relaxed);
	return XTC_OK;
}

int xtc_lockmgr_n_held(const xtc_lockmgr_t *m) {
	return m ? atomic_load_explicit(&((xtc_lockmgr_t *)m)->n_held,
	    memory_order_relaxed) : 0;
}
int xtc_lockmgr_n_waiting(const xtc_lockmgr_t *m) {
	return m ? atomic_load_explicit(&((xtc_lockmgr_t *)m)->n_waiting,
	    memory_order_relaxed) : 0;
}
