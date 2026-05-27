/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/sync.c
 *	M9 sync primitives.  All of these compose with the existing
 *	xtc_proc receive infrastructure: a waiter parks via an internal
 *	mailbox-shaped signal and is woken either by an explicit signal
 *	or by a timeout.  We piggyback on xtc_recv when called from
 *	inside a process, and on a thread-condvar for callers from
 *	outside any process (test main threads, tooling).
 */

#include "xtc_int.h"
#include "xtc_sync.h"
#include "xtc_proc.h"
#include "loop_int.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ----- notify ----------------------------------------------------- */

struct xtc_notify {
	pthread_mutex_t lock;
	pthread_cond_t  cv;
	int             stored;       /* one-shot stored signal */
};

int
xtc_notify_create(xtc_notify_t **out)
{
	xtc_notify_t *n;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *n, (void **)&n)) != XTC_OK)
		return rc;
	(void)pthread_mutex_init(&n->lock, NULL);
	(void)pthread_cond_init(&n->cv, NULL);
	*out = n;
	return XTC_OK;
}

void
xtc_notify_destroy(xtc_notify_t *n)
{
	if (n == NULL) return;
	(void)pthread_cond_destroy(&n->cv);
	(void)pthread_mutex_destroy(&n->lock);
	__os_free(n);
}

int
xtc_notify_signal(xtc_notify_t *n)
{
	if (n == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&n->lock);
	n->stored = 1;
	(void)pthread_cond_broadcast(&n->cv);
	(void)pthread_mutex_unlock(&n->lock);
	return XTC_OK;
}

int
xtc_notify_wait(xtc_notify_t *n, int64_t timeout_ns)
{
	int rc = XTC_OK;
	if (n == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&n->lock);
	if (n->stored) {
		n->stored = 0;
		(void)pthread_mutex_unlock(&n->lock);
		return XTC_OK;
	}
	if (timeout_ns == 0) {
		(void)pthread_mutex_unlock(&n->lock);
		return XTC_E_AGAIN;
	}
	if (timeout_ns < 0) {
		while (!n->stored)
			(void)pthread_cond_wait(&n->cv, &n->lock);
		n->stored = 0;
	} else {
		struct timespec ts;
		int64_t now;
		(void)__os_clock_real(&now);
		now += timeout_ns;
		ts.tv_sec  = (time_t)(now / 1000000000LL);
		ts.tv_nsec = (long)(now % 1000000000LL);
		while (!n->stored) {
			int e = pthread_cond_timedwait(&n->cv, &n->lock, &ts);
			if (e == 0) continue;
			rc = XTC_E_AGAIN;
			break;
		}
		if (n->stored) { n->stored = 0; rc = XTC_OK; }
	}
	(void)pthread_mutex_unlock(&n->lock);
	return rc;
}

/* ----- semaphore -------------------------------------------------- */

struct xtc_sem {
	pthread_mutex_t lock;
	pthread_cond_t  cv;
	unsigned        count;
};

int
xtc_sem_create(unsigned initial, xtc_sem_t **out)
{
	xtc_sem_t *s;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *s, (void **)&s)) != XTC_OK)
		return rc;
	(void)pthread_mutex_init(&s->lock, NULL);
	(void)pthread_cond_init(&s->cv, NULL);
	s->count = initial;
	*out = s;
	return XTC_OK;
}

void
xtc_sem_destroy(xtc_sem_t *s)
{
	if (s == NULL) return;
	(void)pthread_cond_destroy(&s->cv);
	(void)pthread_mutex_destroy(&s->lock);
	__os_free(s);
}

int
xtc_sem_post(xtc_sem_t *s, unsigned n)
{
	if (s == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&s->lock);
	s->count += n;
	(void)pthread_cond_broadcast(&s->cv);
	(void)pthread_mutex_unlock(&s->lock);
	return XTC_OK;
}

int
xtc_sem_try_acquire(xtc_sem_t *s, unsigned n)
{
	int rc = XTC_E_AGAIN;
	if (s == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&s->lock);
	if (s->count >= n) { s->count -= n; rc = XTC_OK; }
	(void)pthread_mutex_unlock(&s->lock);
	return rc;
}

int
xtc_sem_acquire(xtc_sem_t *s, unsigned n, int64_t timeout_ns)
{
	int rc = XTC_OK;
	if (s == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&s->lock);
	if (timeout_ns == 0) {
		if (s->count < n) { rc = XTC_E_AGAIN; goto out; }
		s->count -= n;
		goto out;
	}
	if (timeout_ns < 0) {
		while (s->count < n)
			(void)pthread_cond_wait(&s->cv, &s->lock);
		s->count -= n;
	} else {
		struct timespec ts;
		int64_t now;
		(void)__os_clock_real(&now);
		now += timeout_ns;
		ts.tv_sec  = (time_t)(now / 1000000000LL);
		ts.tv_nsec = (long)(now % 1000000000LL);
		while (s->count < n) {
			int e = pthread_cond_timedwait(&s->cv, &s->lock, &ts);
			if (e == 0) continue;
			rc = XTC_E_AGAIN;
			goto out;
		}
		s->count -= n;
	}
out:
	(void)pthread_mutex_unlock(&s->lock);
	return rc;
}

int
xtc_sem_count(const xtc_sem_t *s)
{
	int v;
	if (s == NULL) return 0;
	(void)pthread_mutex_lock((pthread_mutex_t *)&s->lock);
	v = (int)s->count;
	(void)pthread_mutex_unlock((pthread_mutex_t *)&s->lock);
	return v;
}

/* ----- abort_source ---------------------------------------------- */

struct xtc_abort_source {
	_Atomic int  fired;
	_Atomic int  reason;
};

int
xtc_abort_source_create(xtc_abort_source_t **out)
{
	xtc_abort_source_t *s;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *s, (void **)&s)) != XTC_OK)
		return rc;
	atomic_store_explicit(&s->fired, 0, memory_order_relaxed);
	atomic_store_explicit(&s->reason, 0, memory_order_relaxed);
	*out = s;
	return XTC_OK;
}

void
xtc_abort_source_destroy(xtc_abort_source_t *s)
{
	__os_free(s);
}

int
xtc_abort_source_fire(xtc_abort_source_t *s, int reason)
{
	if (s == NULL) return XTC_E_INVAL;
	atomic_store_explicit(&s->reason, reason, memory_order_relaxed);
	atomic_store_explicit(&s->fired, 1, memory_order_release);
	return XTC_OK;
}

int
xtc_abort_source_token(xtc_abort_source_t *s, xtc_abort_token_t *out)
{
	if (s == NULL || out == NULL) return XTC_E_INVAL;
	out->src = s;
	return XTC_OK;
}

int
xtc_abort_token_is_aborted(const xtc_abort_token_t *t)
{
	if (t == NULL || t->src == NULL) return 0;
	return atomic_load_explicit(&t->src->fired, memory_order_acquire);
}

int
xtc_abort_token_reason(const xtc_abort_token_t *t)
{
	if (t == NULL || t->src == NULL) return 0;
	return atomic_load_explicit(&t->src->reason, memory_order_relaxed);
}

/* ----- amutex (parking mutex) ----------------------------------- */

struct xtc_amutex {
	pthread_mutex_t lock;
	pthread_cond_t  cv;
	int             held;
};

int
xtc_amutex_create(xtc_amutex_t **out)
{
	xtc_amutex_t *m;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *m, (void **)&m)) != XTC_OK) return rc;
	(void)pthread_mutex_init(&m->lock, NULL);
	(void)pthread_cond_init(&m->cv, NULL);
	*out = m;
	return XTC_OK;
}

void
xtc_amutex_destroy(xtc_amutex_t *m)
{
	if (m == NULL) return;
	(void)pthread_cond_destroy(&m->cv);
	(void)pthread_mutex_destroy(&m->lock);
	__os_free(m);
}

int
xtc_amutex_try_lock(xtc_amutex_t *m)
{
	int rc = XTC_E_AGAIN;
	if (m == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&m->lock);
	if (!m->held) { m->held = 1; rc = XTC_OK; }
	(void)pthread_mutex_unlock(&m->lock);
	return rc;
}

int
xtc_amutex_lock(xtc_amutex_t *m, int64_t timeout_ns)
{
	int rc = XTC_OK;
	if (m == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&m->lock);
	if (timeout_ns == 0) {
		if (m->held) { rc = XTC_E_AGAIN; goto out; }
		m->held = 1;
		goto out;
	}
	if (timeout_ns < 0) {
		while (m->held) (void)pthread_cond_wait(&m->cv, &m->lock);
		m->held = 1;
	} else {
		struct timespec ts;
		int64_t now;
		(void)__os_clock_real(&now);
		now += timeout_ns;
		ts.tv_sec  = (time_t)(now / 1000000000LL);
		ts.tv_nsec = (long)(now % 1000000000LL);
		while (m->held) {
			int e = pthread_cond_timedwait(&m->cv, &m->lock, &ts);
			if (e == 0) continue;
			rc = XTC_E_AGAIN; goto out;
		}
		m->held = 1;
	}
out:
	(void)pthread_mutex_unlock(&m->lock);
	return rc;
}

int
xtc_amutex_unlock(xtc_amutex_t *m)
{
	if (m == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&m->lock);
	m->held = 0;
	(void)pthread_cond_signal(&m->cv);
	(void)pthread_mutex_unlock(&m->lock);
	return XTC_OK;
}

/* ----- rwlock (writer-priority) -------------------------------- */

struct xtc_rwlock {
	pthread_mutex_t lock;
	pthread_cond_t  cv;
	int             readers;       /* current readers */
	int             writer;        /* 1 if a writer holds */
	int             waiting_writers; /* writers blocked */
};

int
xtc_rwlock_create(xtc_rwlock_t **out)
{
	xtc_rwlock_t *r;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *r, (void **)&r)) != XTC_OK) return rc;
	(void)pthread_mutex_init(&r->lock, NULL);
	(void)pthread_cond_init(&r->cv, NULL);
	*out = r;
	return XTC_OK;
}

void
xtc_rwlock_destroy(xtc_rwlock_t *r)
{
	if (r == NULL) return;
	(void)pthread_cond_destroy(&r->cv);
	(void)pthread_mutex_destroy(&r->lock);
	__os_free(r);
}

static int
__rwlock_wait(xtc_rwlock_t *r, int64_t timeout_ns,
              int (*ready)(xtc_rwlock_t *))
{
	if (timeout_ns == 0) return ready(r) ? XTC_OK : XTC_E_AGAIN;
	if (timeout_ns < 0) {
		while (!ready(r)) (void)pthread_cond_wait(&r->cv, &r->lock);
		return XTC_OK;
	}
	{
		struct timespec ts;
		int64_t now;
		(void)__os_clock_real(&now);
		now += timeout_ns;
		ts.tv_sec = (time_t)(now / 1000000000LL);
		ts.tv_nsec = (long)(now % 1000000000LL);
		while (!ready(r)) {
			int e = pthread_cond_timedwait(&r->cv, &r->lock, &ts);
			if (e == 0) continue;
			return XTC_E_AGAIN;
		}
		return XTC_OK;
	}
}

static int __rd_ready(xtc_rwlock_t *r) {
	return !r->writer && r->waiting_writers == 0;
}
static int __wr_ready(xtc_rwlock_t *r) {
	return !r->writer && r->readers == 0;
}

int
xtc_rwlock_rdlock(xtc_rwlock_t *r, int64_t timeout_ns)
{
	int rc;
	if (r == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&r->lock);
	rc = __rwlock_wait(r, timeout_ns, __rd_ready);
	if (rc == XTC_OK) r->readers++;
	(void)pthread_mutex_unlock(&r->lock);
	return rc;
}

int
xtc_rwlock_wrlock(xtc_rwlock_t *r, int64_t timeout_ns)
{
	int rc;
	if (r == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&r->lock);
	r->waiting_writers++;
	rc = __rwlock_wait(r, timeout_ns, __wr_ready);
	r->waiting_writers--;
	if (rc == XTC_OK) r->writer = 1;
	else (void)pthread_cond_broadcast(&r->cv); /* let other readers in */
	(void)pthread_mutex_unlock(&r->lock);
	return rc;
}

int
xtc_rwlock_unlock(xtc_rwlock_t *r)
{
	if (r == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&r->lock);
	if (r->writer) r->writer = 0;
	else if (r->readers > 0) r->readers--;
	(void)pthread_cond_broadcast(&r->cv);
	(void)pthread_mutex_unlock(&r->lock);
	return XTC_OK;
}

/* ----- barrier --------------------------------------------------- */

struct xtc_barrier {
	pthread_mutex_t lock;
	pthread_cond_t  cv;
	unsigned        target;
	unsigned        arrived;
	unsigned        generation;
};

int
xtc_barrier_create(unsigned n, xtc_barrier_t **out)
{
	xtc_barrier_t *b;
	int rc;
	if (out == NULL || n == 0) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *b, (void **)&b)) != XTC_OK) return rc;
	(void)pthread_mutex_init(&b->lock, NULL);
	(void)pthread_cond_init(&b->cv, NULL);
	b->target = n;
	*out = b;
	return XTC_OK;
}

void
xtc_barrier_destroy(xtc_barrier_t *b)
{
	if (b == NULL) return;
	(void)pthread_cond_destroy(&b->cv);
	(void)pthread_mutex_destroy(&b->lock);
	__os_free(b);
}

int
xtc_barrier_wait(xtc_barrier_t *b)
{
	unsigned gen;
	if (b == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&b->lock);
	gen = b->generation;
	b->arrived++;
	if (b->arrived == b->target) {
		b->arrived = 0;
		b->generation++;
		(void)pthread_cond_broadcast(&b->cv);
	} else {
		while (gen == b->generation)
			(void)pthread_cond_wait(&b->cv, &b->lock);
	}
	(void)pthread_mutex_unlock(&b->lock);
	return XTC_OK;
}

/* ----- gate ------------------------------------------------------- */

struct xtc_gate {
	pthread_mutex_t lock;
	pthread_cond_t  cv;
	int             count;
	int             closed;
};

int
xtc_gate_create(xtc_gate_t **out)
{
	xtc_gate_t *g;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *g, (void **)&g)) != XTC_OK) return rc;
	(void)pthread_mutex_init(&g->lock, NULL);
	(void)pthread_cond_init(&g->cv, NULL);
	*out = g;
	return XTC_OK;
}

void
xtc_gate_destroy(xtc_gate_t *g)
{
	if (g == NULL) return;
	(void)pthread_cond_destroy(&g->cv);
	(void)pthread_mutex_destroy(&g->lock);
	__os_free(g);
}

int
xtc_gate_enter(xtc_gate_t *g)
{
	int rc = XTC_OK;
	if (g == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&g->lock);
	if (g->closed) rc = XTC_E_INVAL;
	else g->count++;
	(void)pthread_mutex_unlock(&g->lock);
	return rc;
}

int
xtc_gate_leave(xtc_gate_t *g)
{
	if (g == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&g->lock);
	if (g->count > 0) g->count--;
	if (g->closed && g->count == 0)
		(void)pthread_cond_broadcast(&g->cv);
	(void)pthread_mutex_unlock(&g->lock);
	return XTC_OK;
}

int
xtc_gate_close(xtc_gate_t *g)
{
	if (g == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&g->lock);
	g->closed = 1;
	(void)pthread_cond_broadcast(&g->cv);
	(void)pthread_mutex_unlock(&g->lock);
	return XTC_OK;
}

int
xtc_gate_drain(xtc_gate_t *g, int64_t timeout_ns)
{
	int rc = XTC_OK;
	if (g == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&g->lock);
	if (timeout_ns < 0) {
		while (g->count > 0)
			(void)pthread_cond_wait(&g->cv, &g->lock);
	} else if (timeout_ns == 0) {
		if (g->count > 0) rc = XTC_E_AGAIN;
	} else {
		struct timespec ts;
		int64_t now;
		(void)__os_clock_real(&now);
		now += timeout_ns;
		ts.tv_sec  = (time_t)(now / 1000000000LL);
		ts.tv_nsec = (long)(now % 1000000000LL);
		while (g->count > 0) {
			int e = pthread_cond_timedwait(&g->cv, &g->lock, &ts);
			if (e == 0) continue;
			rc = XTC_E_AGAIN; break;
		}
	}
	(void)pthread_mutex_unlock(&g->lock);
	return rc;
}

int
xtc_gate_count(const xtc_gate_t *g)
{
	int v;
	if (g == NULL) return 0;
	(void)pthread_mutex_lock((pthread_mutex_t *)&g->lock);
	v = g->count;
	(void)pthread_mutex_unlock((pthread_mutex_t *)&g->lock);
	return v;
}
