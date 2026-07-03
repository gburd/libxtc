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
#include "xtc_preempt.h"   /* __xtc_mtx_lock/unlock: preemption-safe locks */
#include "xtc_sync.h"
#include "xtc_proc.h"
#include "xtc_sim.h"       /* XTC_SIM_BUGGIFY / xtc_sim_fault (DST) */
#include "loop_int.h"
#include "coro_int.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Off-loop condvar wait helpers.
 *
 * Every blocking primitive here (notify, semaphore, mutex, rwlock,
 * latch, ...) shares the same timed-wait shape for callers outside any
 * process: compute an absolute deadline from a relative timeout, then
 * loop pthread_cond_timedwait while the wake predicate is unmet,
 * returning XTC_E_AGAIN once the deadline passes.  Factor the two
 * mechanical pieces out so each primitive keeps only its own predicate.
 *
 * deadline_from_timeout fills *ts with now + timeout_ns (CLOCK_REALTIME,
 * the clock pthread_cond_timedwait uses).  cv_wait_until does one
 * bounded wait against that deadline and returns 0 if the cond was
 * signaled (the caller re-tests its predicate) or 1 if the deadline
 * has passed (the caller maps that to XTC_E_AGAIN).
 * ----------------------------------------------------------------------- */
static void
deadline_from_timeout(int64_t timeout_ns, struct timespec *ts)
{
	int64_t now = 0;
	(void)__os_clock_real(&now);
	now += timeout_ns;
	ts->tv_sec  = (time_t)(now / 1000000000LL);
	ts->tv_nsec = (long)(now % 1000000000LL);
}

static int
cv_wait_until(pthread_cond_t *cv, pthread_mutex_t *lock,
    const struct timespec *ts)
{
	/* 0 == signaled (re-test the predicate); 1 == deadline reached. */
	return pthread_cond_timedwait(cv, lock, ts) == 0 ? 0 : 1;
}

/* -------------------------------------------------------------------------
 * Fiber-park path for the blocking primitives (semaphore / gate / barrier
 * / notify).
 *
 * A caller running inside a fiber on a loop (__xtc_current_task() != NULL)
 * MUST NOT block the OS thread on a condvar: under the single-thread DST
 * scheduler that would freeze every other fiber (no other thread can
 * signal it), and in production it wedges the loop.  Instead such a caller
 * PARKS the fiber -- it arms a waker, enqueues on the primitive's fiber
 * wait queue, drops the primitive's lock, and xtc_yield()s to the loop --
 * and is re-woken when a post / leave / close / signal (or the barrier's
 * last arrival) wakes its waker.  On wake it re-acquires the lock and
 * re-tests its own predicate; a wake without the predicate met (spurious /
 * timer) simply re-parks.  This mirrors the xtc_amutex / lock-manager
 * fiber-park discipline exactly.
 *
 * The path is PURELY ADDITIVE and gated on __xtc_current_task() != NULL:
 * a caller NOT on a loop (OS threads, the blocking pool, tooling) takes
 * the ORIGINAL pthread_cond_wait / pthread_cond_timedwait path byte for
 * byte.  A signaller wakes BOTH kinds of waiter (broadcast the condvar for
 * threads, wake every queued fiber waker), so mixed waiters are correct.
 *
 * Unlike the amutex hand-off queue, these waiters use wake-and-recheck
 * (no direct grant): the shared state (a count, a generation) is not a
 * single ownable token, so a woken fiber simply re-evaluates its predicate
 * under the lock.  A queued waker is unlinked before its stack frame goes
 * away (on grant or timeout), so a later wake never touches a dead frame.
 * ----------------------------------------------------------------------- */
struct fiber_waiter {
	xtc_waker_t          waker;
	struct fiber_waiter *next;
};

/* Enqueue w at the tail of a FIFO fiber wait queue (under the lock). */
static void
fw_enqueue(struct fiber_waiter **head, struct fiber_waiter **tail,
    struct fiber_waiter *w)
{
	w->next = NULL;
	if (*tail != NULL) (*tail)->next = w;
	else *head = w;
	*tail = w;
}

/* Unlink w from the queue if still present (under the lock). */
static void
fw_remove(struct fiber_waiter **head, struct fiber_waiter **tail,
    struct fiber_waiter *w)
{
	struct fiber_waiter *p = *head, *prev = NULL;
	while (p != NULL) {
		if (p == w) {
			if (prev != NULL) prev->next = p->next;
			else *head = p->next;
			if (*tail == p) *tail = prev;
			return;
		}
		prev = p;
		p = p->next;
	}
}

/* Detach the whole queue and wake every waker.  Caller passes a detached
 * list (already spliced out under the lock) so the wakes can fire after
 * the lock is dropped -- each waiter's stack frame stays alive until it
 * re-acquires the lock to unlink itself. */
static void
fw_wake_all(struct fiber_waiter *list)
{
	while (list != NULL) {
		struct fiber_waiter *n = list->next;
		(void)xtc_waker_wake(&list->waker);
		list = n;
	}
}

/* ----- notify ----------------------------------------------------- */

struct xtc_notify {
	pthread_mutex_t lock;
	pthread_cond_t  cv;
	int             stored;       /* one-shot stored signal */
	struct fiber_waiter *wq_head; /* fiber waiters, FIFO */
	struct fiber_waiter *wq_tail;
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
	struct fiber_waiter *woke;
	if (n == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&n->lock);
	n->stored = 1;
	(void)pthread_cond_broadcast(&n->cv);
	woke = n->wq_head;
	n->wq_head = n->wq_tail = NULL;
	(void)__xtc_mtx_unlock(&n->lock);
	fw_wake_all(woke);
	return XTC_OK;
}

int
xtc_notify_wait(xtc_notify_t *n, int64_t timeout_ns)
{
	int rc = XTC_OK;
	xtc_task_t *cur;
	if (n == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&n->lock);
	if (n->stored) {
		n->stored = 0;
		(void)__xtc_mtx_unlock(&n->lock);
		return XTC_OK;
	}
	if (timeout_ns == 0) {
		(void)__xtc_mtx_unlock(&n->lock);
		return XTC_E_AGAIN;
	}

	cur = __xtc_current_task();
	if (cur != NULL) {
		/* Fiber waiter: park and re-check on wake (purely additive). */
		struct fiber_waiter w;
		void *proc_ctx;
		int64_t deadline = -1;
		(void)xtc_task_waker(cur, &w.waker);
		w.next = NULL;
		fw_enqueue(&n->wq_head, &n->wq_tail, &w);
		if (timeout_ns > 0) {
			int64_t now;
			(void)__os_clock_mono(&now);
			deadline = now + timeout_ns;
		}
		for (;;) {
			int64_t now;
			if (n->stored) { n->stored = 0; rc = XTC_OK; break; }
			if (deadline >= 0) {
				(void)__os_clock_mono(&now);
				if (now >= deadline) { rc = XTC_E_AGAIN; break; }
				if (cur->park_timer != NULL) {
					(void)xtc_timer_cancel(cur->park_timer);
					cur->park_timer = NULL;
				}
				(void)xtc_task_park_on_timer(cur, deadline - now);
			} else {
				cur->park_requested = 1;
			}
			(void)__xtc_mtx_unlock(&n->lock);
			proc_ctx = __xtc_proc_ctx_save();
			xtc_yield();
			__xtc_proc_ctx_restore(proc_ctx);
			(void)__xtc_mtx_lock(&n->lock);
		}
		fw_remove(&n->wq_head, &n->wq_tail, &w);
		if (cur->park_timer != NULL) {
			(void)xtc_timer_cancel(cur->park_timer);
			cur->park_timer = NULL;
		}
		(void)__xtc_mtx_unlock(&n->lock);
		return rc;
	}

	if (timeout_ns < 0) {
		while (!n->stored)
			(void)pthread_cond_wait(&n->cv, &n->lock);
		n->stored = 0;
	} else {
		struct timespec ts;
		deadline_from_timeout(timeout_ns, &ts);
		while (!n->stored) {
			if (cv_wait_until(&n->cv, &n->lock, &ts)) {
				rc = XTC_E_AGAIN;
				break;
			}
		}
		if (n->stored) { n->stored = 0; rc = XTC_OK; }
	}
	(void)__xtc_mtx_unlock(&n->lock);
	return rc;
}

/* ----- semaphore -------------------------------------------------- */

struct xtc_sem {
	pthread_mutex_t lock;
	pthread_cond_t  cv;
	unsigned        count;
	struct fiber_waiter *wq_head;   /* fiber waiters, FIFO */
	struct fiber_waiter *wq_tail;
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
	struct fiber_waiter *woke;
	if (s == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&s->lock);
	s->count += n;
	(void)pthread_cond_broadcast(&s->cv);
	/* Wake every parked fiber; each re-checks count under the lock and
	 * re-parks if it lost the race, so no over-admission past count. */
	woke = s->wq_head;
	s->wq_head = s->wq_tail = NULL;
	(void)__xtc_mtx_unlock(&s->lock);
	fw_wake_all(woke);
	return XTC_OK;
}

int
xtc_sem_try_acquire(xtc_sem_t *s, unsigned n)
{
	int rc = XTC_E_AGAIN;
	if (s == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&s->lock);
	if (s->count >= n) { s->count -= n; rc = XTC_OK; }
	(void)__xtc_mtx_unlock(&s->lock);
	return rc;
}

int
xtc_sem_acquire(xtc_sem_t *s, unsigned n, int64_t timeout_ns)
{
	int rc = XTC_OK;
	xtc_task_t *cur;
	if (s == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&s->lock);
	if (s->count >= n) { s->count -= n; goto out; }
	if (timeout_ns == 0) { rc = XTC_E_AGAIN; goto out; }

	cur = __xtc_current_task();
	if (cur != NULL) {
		/* Fiber waiter: park (yield to the loop) and re-check on wake,
		 * never blocking the OS thread.  Purely additive; gated on
		 * running inside a fiber. */
		struct fiber_waiter w;
		void *proc_ctx;
		int64_t deadline = -1;
		(void)xtc_task_waker(cur, &w.waker);
		w.next = NULL;
		fw_enqueue(&s->wq_head, &s->wq_tail, &w);
		if (timeout_ns > 0) {
			int64_t now;
			(void)__os_clock_mono(&now);
			deadline = now + timeout_ns;
		}
		for (;;) {
			int64_t now;
			if (s->count >= n) {
				/* Buggify: under DST, occasionally decline a
				 * satisfiable acquire and report a spurious
				 * timeout instead -- a legal pessimal outcome
				 * ONLY when the caller passed a finite timeout
				 * (XTC_E_AGAIN is documented there and the
				 * caller must retry).  A fresh per-call fault
				 * draw so a retrying caller eventually
				 * succeeds; the site coin gates liveness. */
				if (deadline >= 0 &&
				    XTC_SIM_BUGGIFY("sync.sem.spurious_timeout") &&
				    xtc_sim_fault(250)) {
					rc = XTC_E_AGAIN;
					break;
				}
				s->count -= n; rc = XTC_OK; break;
			}
			if (deadline >= 0) {
				(void)__os_clock_mono(&now);
				if (now >= deadline) { rc = XTC_E_AGAIN; break; }
				/* Re-arm a fresh timer each park; cancel a
				 * prior one first (else park_on_timer returns
				 * INVAL and the orphan advances the sim clock),
				 * mirroring lock_mgr.c. */
				if (cur->park_timer != NULL) {
					(void)xtc_timer_cancel(cur->park_timer);
					cur->park_timer = NULL;
				}
				(void)xtc_task_park_on_timer(cur, deadline - now);
			} else {
				cur->park_requested = 1;
			}
			(void)__xtc_mtx_unlock(&s->lock);
			proc_ctx = __xtc_proc_ctx_save();
			xtc_yield();
			__xtc_proc_ctx_restore(proc_ctx);
			(void)__xtc_mtx_lock(&s->lock);
		}
		/* Unlink and cancel any lingering timer before returning. */
		fw_remove(&s->wq_head, &s->wq_tail, &w);
		if (cur->park_timer != NULL) {
			(void)xtc_timer_cancel(cur->park_timer);
			cur->park_timer = NULL;
		}
		goto out;
	}

	/* Off-loop: block the OS thread on the condvar, as before. */
	if (timeout_ns < 0) {
		while (s->count < n)
			(void)pthread_cond_wait(&s->cv, &s->lock);
		s->count -= n;
	} else {
		struct timespec ts;
		deadline_from_timeout(timeout_ns, &ts);
		while (s->count < n) {
			if (cv_wait_until(&s->cv, &s->lock, &ts)) {
				rc = XTC_E_AGAIN;
				goto out;
			}
		}
		s->count -= n;
	}
out:
	(void)__xtc_mtx_unlock(&s->lock);
	return rc;
}

int
xtc_sem_count(const xtc_sem_t *s)
{
	int v;
	if (s == NULL) return 0;
	(void)__xtc_mtx_lock((pthread_mutex_t *)&s->lock);
	v = (int)s->count;
	(void)__xtc_mtx_unlock((pthread_mutex_t *)&s->lock);
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

/* ----- amutex (parking mutex) -----------------------------------
 *
 * Contended waiters that are running inside a coroutine / process
 * park the fiber (yield to the loop) instead of blocking the OS
 * thread, so a process can hold the lock across its own park (e.g.
 * a blocking-pool offload) without wedging the loop the moment
 * another process on that loop contends.  Off a loop (cur == NULL)
 * the caller blocks on a condvar, as before.
 *
 * Fiber waiters use a FIFO queue with direct hand-off: unlock grants
 * the lock to the head waiter (held stays 1) and wakes it, so there
 * is no thundering herd and ordering is fair.  Thread waiters share
 * the same `held` flag via the condvar.  When both kinds wait, unlock
 * prefers a fiber waiter; a later release with no fiber waiters wakes
 * a thread waiter.
 */

struct amutex_waiter {
	xtc_waker_t           waker;
	struct amutex_waiter *next;
	int                   granted;   /* set by unlock's hand-off */
};

struct xtc_amutex {
	pthread_mutex_t       lock;
	pthread_cond_t        cv;        /* thread (non-fiber) waiters */
	int                   held;
	int                   recursive; /* XTC_AMUTEX_RECURSIVE */
	struct amutex_owner {
		int       kind;          /* 0 none, 1 fiber, 2 thread */
		void     *task;          /* fiber identity (kind 1) */
		pthread_t thr;           /* thread identity (kind 2) */
	}                     owner;     /* valid while held; under lock */
	int                   count;     /* recursion depth */
	struct amutex_waiter *wq_head;   /* fiber waiters, FIFO */
	struct amutex_waiter *wq_tail;
};

/* Identity of the current execution context: the running fiber's task
 * on a loop, else the OS thread.  Tracked by FIBER on a loop so two
 * fibers sharing one OS thread are distinct owners. */
static struct amutex_owner
amutex_self_owner(void)
{
	struct amutex_owner o;
	xtc_task_t *t = __xtc_current_task();
	o.task = NULL;
	if (t != NULL) { o.kind = 1; o.task = t; }
	else           { o.kind = 2; o.thr = pthread_self(); }
	return o;
}

static int
amutex_owner_eq(const struct amutex_owner *a, const struct amutex_owner *b)
{
	if (a->kind != b->kind || a->kind == 0) return 0;
	if (a->kind == 1) return a->task == b->task;
	return pthread_equal(a->thr, b->thr);
}

int
xtc_amutex_create_ex(xtc_amutex_t **out, unsigned flags)
{
	xtc_amutex_t *m;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *m, (void **)&m)) != XTC_OK) return rc;
	(void)pthread_mutex_init(&m->lock, NULL);
	(void)pthread_cond_init(&m->cv, NULL);
	m->recursive = (flags & XTC_AMUTEX_RECURSIVE) ? 1 : 0;
	*out = m;
	return XTC_OK;
}

int
xtc_amutex_create(xtc_amutex_t **out)
{
	return xtc_amutex_create_ex(out, 0);
}

/* Process-global recursive static mutexes, lazily created. */
static xtc_amutex_t   *g_static_amutex[XTC_AMUTEX_STATIC_MAX];
static pthread_mutex_t g_static_amutex_lock = PTHREAD_MUTEX_INITIALIZER;

xtc_amutex_t *
xtc_amutex_static(unsigned slot)
{
	xtc_amutex_t *m;
	if (slot >= XTC_AMUTEX_STATIC_MAX) return NULL;
	(void)__xtc_mtx_lock(&g_static_amutex_lock);
	m = g_static_amutex[slot];
	if (m == NULL) {
		if (xtc_amutex_create_ex(&m, XTC_AMUTEX_RECURSIVE) == XTC_OK)
			g_static_amutex[slot] = m;
		else
			m = NULL;
	}
	(void)__xtc_mtx_unlock(&g_static_amutex_lock);
	return m;
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
	(void)__xtc_mtx_lock(&m->lock);
	if (m->recursive) {
		struct amutex_owner self = amutex_self_owner();
		if (m->held && amutex_owner_eq(&m->owner, &self)) {
			m->count++;
			rc = XTC_OK;
		} else if (!m->held) {
			m->held = 1;
			m->owner = self;
			m->count = 1;
			rc = XTC_OK;
		}
	} else if (!m->held) {
		m->held = 1;
		rc = XTC_OK;
	}
	(void)__xtc_mtx_unlock(&m->lock);
	return rc;
}

/* Unlink w from the fiber wait queue if still present. */
static void
__amutex_wq_remove(xtc_amutex_t *m, struct amutex_waiter *w)
{
	struct amutex_waiter *p = m->wq_head, *prev = NULL;
	while (p != NULL) {
		if (p == w) {
			if (prev != NULL) prev->next = p->next;
			else m->wq_head = p->next;
			if (m->wq_tail == p) m->wq_tail = prev;
			return;
		}
		prev = p;
		p = p->next;
	}
}

/* Thread-waiter (off-loop) slow path: classic condvar wait. */
static int
__amutex_lock_thread(xtc_amutex_t *m, int64_t timeout_ns)
{
	int rc = XTC_OK;
	if (timeout_ns < 0) {
		while (m->held) (void)pthread_cond_wait(&m->cv, &m->lock);
		m->held = 1;
	} else {
		struct timespec ts;
		deadline_from_timeout(timeout_ns, &ts);
		while (m->held) {
			if (cv_wait_until(&m->cv, &m->lock, &ts))
				return XTC_E_AGAIN;
		}
		m->held = 1;
	}
	return rc;
}

int
xtc_amutex_lock(xtc_amutex_t *m, int64_t timeout_ns)
{
	xtc_task_t *cur;
	struct amutex_waiter w;
	void *proc_ctx;
	int64_t deadline = -1;

	if (m == NULL) return XTC_E_INVAL;

	(void)__xtc_mtx_lock(&m->lock);
	if (m->recursive) {
		struct amutex_owner self = amutex_self_owner();
		if (m->held && amutex_owner_eq(&m->owner, &self)) {
			m->count++;                 /* recursive re-entry */
			(void)__xtc_mtx_unlock(&m->lock);
			return XTC_OK;
		}
		if (!m->held) {
			m->held = 1;
			m->owner = self;
			m->count = 1;
			(void)__xtc_mtx_unlock(&m->lock);
			return XTC_OK;
		}
	} else if (!m->held) {
		m->held = 1;
		(void)__xtc_mtx_unlock(&m->lock);
		return XTC_OK;
	}
	if (timeout_ns == 0) {
		(void)__xtc_mtx_unlock(&m->lock);
		return XTC_E_AGAIN;
	}

	cur = __xtc_current_task();
	if (cur == NULL) {
		/* Not on a loop: block the thread on the condvar. */
		int rc = __amutex_lock_thread(m, timeout_ns);
		(void)__xtc_mtx_unlock(&m->lock);
		return rc;
	}

	/* Fiber waiter: enqueue and park. */
	(void)xtc_task_waker(cur, &w.waker);
	w.granted = 0;
	w.next = NULL;
	if (m->wq_tail != NULL) m->wq_tail->next = &w;
	else m->wq_head = &w;
	m->wq_tail = &w;
	(void)__xtc_mtx_unlock(&m->lock);

	if (timeout_ns > 0) {
		int64_t now;
		(void)__os_clock_mono(&now);
		deadline = now + timeout_ns;
	}

	for (;;) {
		int64_t now;
		/* Re-arm the wakeup cause each iteration: a timer for the
		 * timeout, otherwise a plain voluntary park. */
		if (deadline >= 0) {
			(void)__os_clock_mono(&now);
			if (now >= deadline) {
				(void)__xtc_mtx_lock(&m->lock);
				if (w.granted) {
					if (m->recursive) {
						m->owner = amutex_self_owner();
						m->count = 1;
					}
					(void)__xtc_mtx_unlock(&m->lock);
					return XTC_OK;   /* raced with grant */
				}
				__amutex_wq_remove(m, &w);
				(void)__xtc_mtx_unlock(&m->lock);
				return XTC_E_AGAIN;
			}
			(void)xtc_task_park_on_timer(cur, deadline - now);
		} else {
			cur->park_requested = 1;
		}

		proc_ctx = __xtc_proc_ctx_save();
		xtc_yield();
		__xtc_proc_ctx_restore(proc_ctx);

		(void)__xtc_mtx_lock(&m->lock);
		if (w.granted) {
			if (m->recursive) {
				m->owner = amutex_self_owner();
				m->count = 1;
			}
			(void)__xtc_mtx_unlock(&m->lock);
			return XTC_OK;
		}
		(void)__xtc_mtx_unlock(&m->lock);
		/* Spurious / timer wake without grant: loop and re-park. */
	}
}

int
xtc_amutex_unlock(xtc_amutex_t *m)
{
	struct amutex_waiter *w = NULL;
	if (m == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&m->lock);
	if (m->recursive) {
		if (m->count > 1) {
			m->count--;            /* still held by this owner */
			(void)__xtc_mtx_unlock(&m->lock);
			return XTC_OK;
		}
		m->count = 0;
		m->owner.kind = 0;         /* release; next owner claims it */
	}
	if (m->wq_head != NULL) {
		/* Hand off to the head fiber waiter: keep held == 1. */
		w = m->wq_head;
		m->wq_head = w->next;
		if (m->wq_head == NULL) m->wq_tail = NULL;
		w->granted = 1;
		w->next = NULL;
	} else {
		/* No fiber waiter: release and wake a thread waiter. */
		m->held = 0;
		(void)pthread_cond_signal(&m->cv);
	}
	(void)__xtc_mtx_unlock(&m->lock);
	if (w != NULL)
		(void)xtc_waker_wake(&w->waker);
	return XTC_OK;
}

/* ----- arwlock (parking reader/writer latch) --------------------
 *
 * A shared/exclusive latch whose contended waiters PARK the fiber
 * (yield to the loop) rather than blocking the OS thread, so a holder
 * may park on I/O (e.g. a buffer-manager page fetch) while latched
 * without wedging the loop, and lock coupling can hold a parent latch
 * across a child fix.  Off a loop (cur == NULL) waiters block on a
 * condvar.
 *
 * Fiber waiters use a FIFO queue with direct hand-off: a release grants
 * the latch to the head waiter(s) -- a run of consecutive shared
 * waiters together, or one exclusive waiter -- and wakes them, so
 * ordering is fair and writers do not starve.  New acquirers that find
 * any waiter queued ahead of them queue too (FIFO), so a steady read
 * stream cannot starve a waiting writer.  Off-loop (condvar) waiters
 * re-check the grant condition themselves and yield to queued fiber
 * waiters; under heavy fiber contention an off-loop waiter may wait
 * (the documented tradeoff -- real contention here is between procs).
 */
#define ARW_SHARED 0
#define ARW_EXCL   1

struct arwlock_waiter {
	xtc_waker_t            waker;
	struct arwlock_waiter *next;
	int                    mode;
	int                    granted;
};

struct xtc_arwlock {
	pthread_mutex_t        lock;
	pthread_cond_t         cv;        /* off-loop waiters */
	int                    readers;   /* active shared holders */
	int                    writer;    /* 1 if exclusive held */
	int                    cv_waiters;
	struct arwlock_waiter *wq_head;   /* fiber waiters, FIFO */
	struct arwlock_waiter *wq_tail;
};

int
xtc_arwlock_create(xtc_arwlock_t **out)
{
	xtc_arwlock_t *r;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *r, (void **)&r)) != XTC_OK) return rc;
	(void)pthread_mutex_init(&r->lock, NULL);
	(void)pthread_cond_init(&r->cv, NULL);
	*out = r;
	return XTC_OK;
}

void
xtc_arwlock_destroy(xtc_arwlock_t *r)
{
	if (r == NULL) return;
	(void)pthread_cond_destroy(&r->cv);
	(void)pthread_mutex_destroy(&r->lock);
	__os_free(r);
}

/* Can `mode` be granted right now, ignoring queue order? */
static int
__arw_compatible(xtc_arwlock_t *r, int mode)
{
	return mode == ARW_EXCL ? (!r->writer && r->readers == 0)
	                        : (!r->writer);
}

/* Grant as many head fiber waiters as the current state allows; return
 * the granted waiters (linked via ->next) to wake after unlocking.
 * Wakes off-loop waiters too when no fiber waiter is queued ahead. */
static struct arwlock_waiter *
__arw_grant_locked(xtc_arwlock_t *r)
{
	struct arwlock_waiter *woke = NULL;
	while (r->wq_head != NULL) {
		struct arwlock_waiter *w = r->wq_head;
		if (!__arw_compatible(r, w->mode))
			break;
		r->wq_head = w->next;
		if (r->wq_head == NULL) r->wq_tail = NULL;
		if (w->mode == ARW_EXCL) r->writer = 1; else r->readers++;
		w->granted = 1;
		w->next = woke;
		woke = w;
		if (w->mode == ARW_EXCL)
			break;          /* exclusive: nothing after can join */
	}
	if (r->cv_waiters > 0 && r->wq_head == NULL)
		(void)pthread_cond_broadcast(&r->cv);
	return woke;
}

static void
__arw_wq_remove(xtc_arwlock_t *r, struct arwlock_waiter *w)
{
	struct arwlock_waiter *p = r->wq_head, *prev = NULL;
	while (p != NULL) {
		if (p == w) {
			if (prev != NULL) prev->next = p->next;
			else r->wq_head = p->next;
			if (r->wq_tail == p) r->wq_tail = prev;
			return;
		}
		prev = p; p = p->next;
	}
}

static int
__arwlock_lock(xtc_arwlock_t *r, int mode, int64_t timeout_ns)
{
	xtc_task_t *cur;
	struct arwlock_waiter w;
	void *proc_ctx;
	int64_t deadline = -1;

	if (r == NULL) return XTC_E_INVAL;

	(void)__xtc_mtx_lock(&r->lock);
	/* Fast path: compatible AND nobody queued ahead (FIFO fairness). */
	if (r->wq_head == NULL && __arw_compatible(r, mode)) {
		if (mode == ARW_EXCL) r->writer = 1; else r->readers++;
		(void)__xtc_mtx_unlock(&r->lock);
		return XTC_OK;
	}
	if (timeout_ns == 0) {
		(void)__xtc_mtx_unlock(&r->lock);
		return XTC_E_AGAIN;
	}

	cur = __xtc_current_task();
	if (cur == NULL) {
		/* Off-loop: condvar wait, yielding to any queued fiber waiter. */
		int rc = XTC_OK;
		r->cv_waiters++;
		if (timeout_ns < 0) {
			while (!(r->wq_head == NULL && __arw_compatible(r, mode)))
				(void)pthread_cond_wait(&r->cv, &r->lock);
		} else {
			struct timespec ts;
			deadline_from_timeout(timeout_ns, &ts);
			while (!(r->wq_head == NULL && __arw_compatible(r, mode))) {
				if (cv_wait_until(&r->cv, &r->lock, &ts)) {
					rc = XTC_E_AGAIN; break;
				}
			}
		}
		if (rc == XTC_OK) {
			if (mode == ARW_EXCL) r->writer = 1; else r->readers++;
		}
		r->cv_waiters--;
		(void)__xtc_mtx_unlock(&r->lock);
		return rc;
	}

	/* Fiber waiter: enqueue and park until granted. */
	(void)xtc_task_waker(cur, &w.waker);
	w.mode = mode;
	w.granted = 0;
	w.next = NULL;
	if (r->wq_tail != NULL) r->wq_tail->next = &w;
	else r->wq_head = &w;
	r->wq_tail = &w;
	(void)__xtc_mtx_unlock(&r->lock);

	if (timeout_ns > 0) {
		int64_t now;
		(void)__os_clock_mono(&now);
		deadline = now + timeout_ns;
	}
	for (;;) {
		int64_t now;
		if (deadline >= 0) {
			(void)__os_clock_mono(&now);
			if (now >= deadline) {
				(void)__xtc_mtx_lock(&r->lock);
				if (w.granted) { (void)__xtc_mtx_unlock(&r->lock); return XTC_OK; }
				__arw_wq_remove(r, &w);
				/* Removing us may unblock waiters behind us. */
				{
					struct arwlock_waiter *woke = __arw_grant_locked(r);
					while (woke != NULL) {
						struct arwlock_waiter *n = woke->next;
						(void)xtc_waker_wake(&woke->waker);
						woke = n;
					}
					(void)__xtc_mtx_unlock(&r->lock);
				}
				return XTC_E_AGAIN;
			}
			(void)xtc_task_park_on_timer(cur, deadline - now);
		} else {
			cur->park_requested = 1;
		}
		proc_ctx = __xtc_proc_ctx_save();
		xtc_yield();
		__xtc_proc_ctx_restore(proc_ctx);

		(void)__xtc_mtx_lock(&r->lock);
		if (w.granted) { (void)__xtc_mtx_unlock(&r->lock); return XTC_OK; }
		(void)__xtc_mtx_unlock(&r->lock);
	}
}

int
xtc_arwlock_rdlock(xtc_arwlock_t *r, int64_t timeout_ns)
{
	return __arwlock_lock(r, ARW_SHARED, timeout_ns);
}

int
xtc_arwlock_wrlock(xtc_arwlock_t *r, int64_t timeout_ns)
{
	return __arwlock_lock(r, ARW_EXCL, timeout_ns);
}

int
xtc_arwlock_unlock(xtc_arwlock_t *r)
{
	struct arwlock_waiter *woke;
	if (r == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&r->lock);
	if (r->writer) r->writer = 0;
	else if (r->readers > 0) r->readers--;
	woke = __arw_grant_locked(r);
	/*
	 * Wake granted waiters WHILE HOLDING r->lock.  Each waiter is a
	 * stack object on its own fiber; that fiber may wake on its own park
	 * timer, observe w->granted, and return -- reclaiming the waiter's
	 * stack -- the instant r->lock becomes free.  To return it must
	 * re-acquire r->lock, so firing the wake before we release keeps the
	 * waiter (and its waker) alive across xtc_waker_wake.  The wake only
	 * posts to the target loop (no lock ordered against this leaf lock),
	 * so this cannot deadlock.
	 */
	while (woke != NULL) {
		struct arwlock_waiter *n = woke->next;
		(void)xtc_waker_wake(&woke->waker);
		woke = n;
	}
	(void)__xtc_mtx_unlock(&r->lock);
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
		deadline_from_timeout(timeout_ns, &ts);
		while (!ready(r)) {
			if (cv_wait_until(&r->cv, &r->lock, &ts))
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
	(void)__xtc_mtx_lock(&r->lock);
	rc = __rwlock_wait(r, timeout_ns, __rd_ready);
	if (rc == XTC_OK) r->readers++;
	(void)__xtc_mtx_unlock(&r->lock);
	return rc;
}

int
xtc_rwlock_wrlock(xtc_rwlock_t *r, int64_t timeout_ns)
{
	int rc;
	if (r == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&r->lock);
	r->waiting_writers++;
	rc = __rwlock_wait(r, timeout_ns, __wr_ready);
	r->waiting_writers--;
	if (rc == XTC_OK) r->writer = 1;
	else (void)pthread_cond_broadcast(&r->cv); /* let other readers in */
	(void)__xtc_mtx_unlock(&r->lock);
	return rc;
}

int
xtc_rwlock_unlock(xtc_rwlock_t *r)
{
	if (r == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&r->lock);
	if (r->writer) r->writer = 0;
	else if (r->readers > 0) r->readers--;
	(void)pthread_cond_broadcast(&r->cv);
	(void)__xtc_mtx_unlock(&r->lock);
	return XTC_OK;
}

/* ----- barrier --------------------------------------------------- */

struct xtc_barrier {
	pthread_mutex_t lock;
	pthread_cond_t  cv;
	unsigned        target;
	unsigned        arrived;
	unsigned        generation;
	struct fiber_waiter *wq_head; /* fiber parties, FIFO */
	struct fiber_waiter *wq_tail;
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
	xtc_task_t *cur;
	if (b == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&b->lock);
	gen = b->generation;
	b->arrived++;
	if (b->arrived == b->target) {
		/* Last party: release everyone (all parties together). */
		struct fiber_waiter *woke;
		b->arrived = 0;
		b->generation++;
		(void)pthread_cond_broadcast(&b->cv);
		woke = b->wq_head;
		b->wq_head = b->wq_tail = NULL;
		(void)__xtc_mtx_unlock(&b->lock);
		fw_wake_all(woke);
		return XTC_OK;
	}

	cur = __xtc_current_task();
	if (cur != NULL) {
		/* Fiber party: park until the generation advances (purely
		 * additive; the barrier has no timeout, so no park timer). */
		struct fiber_waiter w;
		void *proc_ctx;
		(void)xtc_task_waker(cur, &w.waker);
		w.next = NULL;
		fw_enqueue(&b->wq_head, &b->wq_tail, &w);
		while (gen == b->generation) {
			cur->park_requested = 1;
			(void)__xtc_mtx_unlock(&b->lock);
			proc_ctx = __xtc_proc_ctx_save();
			xtc_yield();
			__xtc_proc_ctx_restore(proc_ctx);
			(void)__xtc_mtx_lock(&b->lock);
		}
		fw_remove(&b->wq_head, &b->wq_tail, &w);
		(void)__xtc_mtx_unlock(&b->lock);
		return XTC_OK;
	}

	while (gen == b->generation)
		(void)pthread_cond_wait(&b->cv, &b->lock);
	(void)__xtc_mtx_unlock(&b->lock);
	return XTC_OK;
}

/* ----- gate ------------------------------------------------------- */

struct xtc_gate {
	pthread_mutex_t lock;
	pthread_cond_t  cv;
	int             count;
	int             closed;
	struct fiber_waiter *wq_head; /* fiber drainers, FIFO */
	struct fiber_waiter *wq_tail;
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
	(void)__xtc_mtx_lock(&g->lock);
	if (g->closed) rc = XTC_E_INVAL;
	else g->count++;
	(void)__xtc_mtx_unlock(&g->lock);
	return rc;
}

int
xtc_gate_leave(xtc_gate_t *g)
{
	struct fiber_waiter *woke = NULL;
	if (g == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&g->lock);
	if (g->count > 0) g->count--;
	if (g->closed && g->count == 0) {
		(void)pthread_cond_broadcast(&g->cv);
		woke = g->wq_head;
		g->wq_head = g->wq_tail = NULL;
	}
	(void)__xtc_mtx_unlock(&g->lock);
	fw_wake_all(woke);
	return XTC_OK;
}

int
xtc_gate_close(xtc_gate_t *g)
{
	struct fiber_waiter *woke = NULL;
	if (g == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&g->lock);
	g->closed = 1;
	(void)pthread_cond_broadcast(&g->cv);
	if (g->count == 0) {                 /* already drained */
		woke = g->wq_head;
		g->wq_head = g->wq_tail = NULL;
	}
	(void)__xtc_mtx_unlock(&g->lock);
	fw_wake_all(woke);
	return XTC_OK;
}

int
xtc_gate_drain(xtc_gate_t *g, int64_t timeout_ns)
{
	int rc = XTC_OK;
	xtc_task_t *cur;
	if (g == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&g->lock);
	if (g->count == 0) { (void)__xtc_mtx_unlock(&g->lock); return XTC_OK; }
	if (timeout_ns == 0) {
		(void)__xtc_mtx_unlock(&g->lock);
		return XTC_E_AGAIN;
	}

	cur = __xtc_current_task();
	if (cur != NULL) {
		/* Fiber drainer: park until count hits 0 (purely additive). */
		struct fiber_waiter w;
		void *proc_ctx;
		int64_t deadline = -1;
		(void)xtc_task_waker(cur, &w.waker);
		w.next = NULL;
		fw_enqueue(&g->wq_head, &g->wq_tail, &w);
		if (timeout_ns > 0) {
			int64_t now;
			(void)__os_clock_mono(&now);
			deadline = now + timeout_ns;
		}
		for (;;) {
			int64_t now;
			if (g->count == 0) { rc = XTC_OK; break; }
			if (deadline >= 0) {
				(void)__os_clock_mono(&now);
				if (now >= deadline) { rc = XTC_E_AGAIN; break; }
				if (cur->park_timer != NULL) {
					(void)xtc_timer_cancel(cur->park_timer);
					cur->park_timer = NULL;
				}
				(void)xtc_task_park_on_timer(cur, deadline - now);
			} else {
				cur->park_requested = 1;
			}
			(void)__xtc_mtx_unlock(&g->lock);
			proc_ctx = __xtc_proc_ctx_save();
			xtc_yield();
			__xtc_proc_ctx_restore(proc_ctx);
			(void)__xtc_mtx_lock(&g->lock);
		}
		fw_remove(&g->wq_head, &g->wq_tail, &w);
		if (cur->park_timer != NULL) {
			(void)xtc_timer_cancel(cur->park_timer);
			cur->park_timer = NULL;
		}
		(void)__xtc_mtx_unlock(&g->lock);
		return rc;
	}

	if (timeout_ns < 0) {
		while (g->count > 0)
			(void)pthread_cond_wait(&g->cv, &g->lock);
	} else {
		struct timespec ts;
		deadline_from_timeout(timeout_ns, &ts);
		while (g->count > 0) {
			if (cv_wait_until(&g->cv, &g->lock, &ts)) {
				rc = XTC_E_AGAIN; break;
			}
		}
	}
	(void)__xtc_mtx_unlock(&g->lock);
	return rc;
}

int
xtc_gate_count(const xtc_gate_t *g)
{
	int v;
	if (g == NULL) return 0;
	(void)__xtc_mtx_lock((pthread_mutex_t *)&g->lock);
	v = g->count;
	(void)__xtc_mtx_unlock((pthread_mutex_t *)&g->lock);
	return v;
}
