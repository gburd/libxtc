/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/orc/sup.c
 *	The L4 supervisor.  Runs as an xtc_proc that monitors each
 *	child and applies the configured restart strategy on DOWN
 *	messages.  Restart intensity uses a sliding-window count: if
 *	more than `max_restarts` happen within `period_ns`, the
 *	supervisor itself exits up the tree.
 */

#include "xtc_int.h"
#include "xtc_orc.h"
#include "xtc_proc.h"
#include "xtc_sync.h"

#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct child {
	xtc_child_spec_t spec;
	xtc_pid_t        pid;
	uint64_t         monitor_ref;
	int              alive;
};

struct xtc_supervisor {
	xtc_loop_t            *loop;
	xtc_sup_opts_t         opts;
	pthread_mutex_t        lock;

	struct child          *children;
	int                    n_children;

	/* Restart-intensity tracking: a small ring of recent restart
	 * timestamps.  When the count of timestamps within period_ns
	 * exceeds max_restarts, we exit. */
	int64_t               *recent_restarts;
	int                    rr_cap;
	int                    rr_n;

	_Atomic int            n_restarts_total;
	_Atomic int            alive;
	_Atomic int            stop_requested;
	xtc_pid_t              sup_pid;
	xtc_notify_t          *stopped;
};

/* The DOWN signal envelope shape we emit in xtc_proc_exit cleanup. */
struct down_signal {
	uint8_t  kind;
	uint64_t ref;
	xtc_pid_t pid;
	int      reason;
} __attribute__((packed));

static int
__match_down(const void *data, size_t size, void *u)
{
	const struct down_signal *d;
	(void)u;
	if (size < sizeof *d) return 0;
	d = data;
	return d->kind == 'D';
}

static int
__should_restart(struct xtc_supervisor *sup, const struct child *c, int reason)
{
	(void)sup; (void)c;
	switch (c->spec.policy) {
	case XTC_RESTART_PERMANENT:  return 1;
	case XTC_RESTART_TEMPORARY:  return 0;
	case XTC_RESTART_TRANSIENT:  return reason != 0;
	}
	return 0;
}

static int
__intensity_exceeded(struct xtc_supervisor *sup, int64_t now)
{
	int i, in_window = 0;
	int64_t cutoff = now - sup->opts.period_ns;
	for (i = 0; i < sup->rr_n; i++)
		if (sup->recent_restarts[i] > cutoff) in_window++;
	return in_window > sup->opts.max_restarts;
}

static void
__record_restart(struct xtc_supervisor *sup, int64_t now)
{
	if (sup->rr_n < sup->rr_cap) {
		sup->recent_restarts[sup->rr_n++] = now;
	} else {
		/* Slide window: shift left, drop oldest. */
		memmove(&sup->recent_restarts[0],
		    &sup->recent_restarts[1],
		    (size_t)(sup->rr_cap - 1) * sizeof *sup->recent_restarts);
		sup->recent_restarts[sup->rr_cap - 1] = now;
	}
	(void)atomic_fetch_add_explicit(&sup->n_restarts_total, 1,
	    memory_order_relaxed);
}

static int
__spawn_child(struct xtc_supervisor *sup, struct child *c)
{
	xtc_proc_opts_t pop;
	int rc;
	memset(&pop, 0, sizeof pop);
	pop.name = c->spec.name;
	pop.mailbox_cap = c->spec.mailbox_cap;
	rc = xtc_proc_spawn(sup->loop, c->spec.fn, c->spec.arg, &pop, &c->pid);
	if (rc != XTC_OK) return rc;
	c->alive = 1;
	rc = xtc_monitor(c->pid, &c->monitor_ref);
	return rc;
}

/* Kill a still-alive sibling; we only mark it as not-alive once we
 * see its DOWN come back through the mailbox. */
static void
__kill_sibling(struct xtc_supervisor *sup, struct child *c)
{
	(void)sup;
	if (!c->alive) return;
	(void)xtc_exit_pid(c->pid, 1);   /* reason 1 = supervisor-shutdown */
}

/* For one_for_all: after a child crash, kill every sibling that's
 * still alive, drain pending DOWN signals, then respawn all in
 * original order. */
static void
__do_one_for_all(struct xtc_supervisor *sup, int dead_idx)
{
	int i;
	(void)dead_idx;
	for (i = 0; i < sup->n_children; i++)
		if (sup->children[i].alive) __kill_sibling(sup, &sup->children[i]);

	/* Drain DOWN messages from the doomed siblings.  We can't use
	 * xtc_recv_match here without recursing; instead we just mark
	 * them not-alive optimistically and let the next pass through
	 * the main recv loop reap any stragglers. */
	for (i = 0; i < sup->n_children; i++) sup->children[i].alive = 0;

	for (i = 0; i < sup->n_children; i++) {
		if (__spawn_child(sup, &sup->children[i]) != XTC_OK) {
			/* Spawn failure aborts the supervisor. */
			atomic_store_explicit(&sup->stop_requested, 1,
			    memory_order_release);
			return;
		}
	}
}

/* For rest_for_one: kill children dead_idx+1..end (in reverse),
 * then respawn dead_idx..end in forward order. */
static void
__do_rest_for_one(struct xtc_supervisor *sup, int dead_idx)
{
	int i;
	for (i = sup->n_children - 1; i > dead_idx; i--)
		if (sup->children[i].alive) __kill_sibling(sup, &sup->children[i]);
	for (i = dead_idx; i < sup->n_children; i++) sup->children[i].alive = 0;
	for (i = dead_idx; i < sup->n_children; i++) {
		if (__spawn_child(sup, &sup->children[i]) != XTC_OK) {
			atomic_store_explicit(&sup->stop_requested, 1,
			    memory_order_release);
			return;
		}
	}
}

static int
__find_child_by_pid(struct xtc_supervisor *sup, xtc_pid_t p)
{
	int i;
	for (i = 0; i < sup->n_children; i++) {
		if (sup->children[i].alive &&
		    xtc_pid_eq(sup->children[i].pid, p))
			return i;
	}
	return -1;
}

static void
__sup_entry(void *arg)
{
	struct xtc_supervisor *sup = arg;
	int i;

	sup->sup_pid = xtc_self();

	for (i = 0; i < sup->n_children; i++) {
		if (__spawn_child(sup, &sup->children[i]) != XTC_OK) {
			atomic_store_explicit(&sup->alive, 0, memory_order_release);
			(void)xtc_notify_signal(sup->stopped);
			return;
		}
	}

	while (!atomic_load_explicit(&sup->stop_requested, memory_order_acquire)) {
		void *msg; size_t sz;
		int rc;
		struct down_signal d;
		int idx;
		int64_t now;

		rc = xtc_recv_match(__match_down, NULL, &msg, &sz,
		    100LL * 1000 * 1000);
		if (rc == XTC_E_AGAIN) continue;
		if (rc != XTC_OK) break;
		if (sz < sizeof d) { __os_free(msg); continue; }
		memcpy(&d, msg, sizeof d);
		__os_free(msg);

		(void)pthread_mutex_lock(&sup->lock);
		idx = __find_child_by_pid(sup, d.pid);
		if (idx < 0) {
			(void)pthread_mutex_unlock(&sup->lock);
			continue;
		}
		sup->children[idx].alive = 0;

		if (atomic_load_explicit(&sup->stop_requested,
		    memory_order_acquire)) {
			(void)pthread_mutex_unlock(&sup->lock);
			break;
		}

		if (!__should_restart(sup, &sup->children[idx], d.reason)) {
			(void)pthread_mutex_unlock(&sup->lock);
			continue;
		}

		(void)__os_clock_mono(&now);
		__record_restart(sup, now);
		if (__intensity_exceeded(sup, now)) {
			(void)pthread_mutex_unlock(&sup->lock);
			break;
		}

		switch (sup->opts.strategy) {
		case XTC_SUP_ONE_FOR_ONE:
			(void)__spawn_child(sup, &sup->children[idx]);
			break;
		case XTC_SUP_ONE_FOR_ALL:
			__do_one_for_all(sup, idx);
			break;
		case XTC_SUP_REST_FOR_ONE:
			__do_rest_for_one(sup, idx);
			break;
		case XTC_SUP_SIMPLE_OFO:
			/* simple_one_for_one is dynamic-only: children
			 * spawned via xtc_sup_add_child are automatically
			 * one-for-one'd here.  No-op for static children. */
			(void)__spawn_child(sup, &sup->children[idx]);
			break;
		}
		(void)pthread_mutex_unlock(&sup->lock);
	}

	atomic_store_explicit(&sup->alive, 0, memory_order_release);

	/* On exit, kill any still-alive children so the loop can drain. */
	{
		int k;
		(void)pthread_mutex_lock(&sup->lock);
		for (k = 0; k < sup->n_children; k++) {
			if (sup->children[k].alive) {
				(void)xtc_exit_pid(sup->children[k].pid, 1);
				sup->children[k].alive = 0;
			}
		}
		(void)pthread_mutex_unlock(&sup->lock);
	}

	(void)xtc_notify_signal(sup->stopped);
}

int
xtc_sup_start(xtc_loop_t *loop, const xtc_sup_opts_t *opts,
              const xtc_child_spec_t *children, int n_children,
              xtc_supervisor_t **out_sup)
{
	xtc_supervisor_t *sup;
	xtc_sup_opts_t   defaults = XTC_SUP_OPTS_DEFAULT;
	int rc;
	xtc_pid_t pid;
	int i;

	if (loop == NULL || children == NULL || n_children <= 0 || out_sup == NULL)
		return XTC_E_INVAL;

	if ((rc = __os_calloc(1, sizeof *sup, (void **)&sup)) != XTC_OK)
		return rc;
	sup->loop = loop;
	sup->opts = opts != NULL ? *opts : defaults;
	if (sup->opts.strategy != XTC_SUP_ONE_FOR_ONE &&
	    sup->opts.strategy != XTC_SUP_ONE_FOR_ALL &&
	    sup->opts.strategy != XTC_SUP_REST_FOR_ONE &&
	    sup->opts.strategy != XTC_SUP_SIMPLE_OFO) {
		__os_free(sup);
		return XTC_E_NOSYS;
	}
	(void)pthread_mutex_init(&sup->lock, NULL);
	atomic_store_explicit(&sup->alive, 1, memory_order_relaxed);

	sup->rr_cap = sup->opts.max_restarts + 4;
	if ((rc = __os_calloc((size_t)sup->rr_cap, sizeof *sup->recent_restarts,
	    (void **)&sup->recent_restarts)) != XTC_OK) goto fail0;

	if ((rc = __os_calloc((size_t)n_children, sizeof *sup->children,
	    (void **)&sup->children)) != XTC_OK) goto fail1;
	sup->n_children = n_children;
	for (i = 0; i < n_children; i++) sup->children[i].spec = children[i];

	if ((rc = xtc_notify_create(&sup->stopped)) != XTC_OK) goto fail2;

	if ((rc = xtc_proc_spawn(loop, __sup_entry, sup, NULL, &pid)) != XTC_OK)
		goto fail3;
	sup->sup_pid = pid;
	*out_sup = sup;
	return XTC_OK;

fail3:	xtc_notify_destroy(sup->stopped);
fail2:	__os_free(sup->children);
fail1:	__os_free(sup->recent_restarts);
fail0:	(void)pthread_mutex_destroy(&sup->lock);
	__os_free(sup);
	return rc;
}

/*
 * PUBLIC: int xtc_sup_stop __P((xtc_supervisor_t *));
 *
 * Non-blocking: requests stop and returns immediately.  The actual
 * wind-down happens on the supervisor's loop the next time it
 * processes its mailbox.  Use xtc_sup_join to synchronously wait
 * for the supervisor to finish (must be called from outside the
 * supervisor's loop thread).
 *
 * The previous implementation called xtc_notify_wait here, which
 * deadlocked when xtc_sup_stop was called from a process running
 * on the same loop as the supervisor: the wait blocks the loop
 * thread, preventing the supervisor from reaching its exit cleanup
 * to fire the notify.  Splitting stop and join is the correct shape.
 */
int
xtc_sup_stop(xtc_supervisor_t *sup)
{
	if (sup == NULL) return XTC_E_INVAL;
	atomic_store_explicit(&sup->stop_requested, 1, memory_order_release);
	/* Send a one-byte kick so the supervisor wakes from its 100ms
	 * recv-poll and observes stop_requested. */
	{
		uint8_t kick = 'X';
		(void)xtc_send(sup->sup_pid, &kick, 1);
	}
	return XTC_OK;
}

/*
 * PUBLIC: int xtc_sup_join __P((xtc_supervisor_t *, int64_t));
 *
 * Wait up to timeout_ns for the supervisor to exit, then free its
 * resources.  -1 = forever, 0 = poll once.  Must be called from
 * outside the supervisor's loop thread (otherwise the loop can't
 * make progress during the wait).
 *
 * After a successful join the sup pointer is invalid.
 */
int
xtc_sup_join(xtc_supervisor_t *sup, int64_t timeout_ns)
{
	if (sup == NULL) return XTC_E_INVAL;
	(void)xtc_notify_wait(sup->stopped, timeout_ns);

	xtc_notify_destroy(sup->stopped);
	__os_free(sup->children);
	__os_free(sup->recent_restarts);
	(void)pthread_mutex_destroy(&sup->lock);
	__os_free(sup);
	return XTC_OK;
}

int xtc_sup_n_children(const xtc_supervisor_t *sup) {
	return sup ? sup->n_children : 0;
}
int xtc_sup_n_restarts(const xtc_supervisor_t *sup) {
	return sup ? atomic_load_explicit(&((xtc_supervisor_t *)sup)->n_restarts_total,
	    memory_order_relaxed) : 0;
}
int xtc_sup_alive(const xtc_supervisor_t *sup) {
	return sup ? atomic_load_explicit(&((xtc_supervisor_t *)sup)->alive,
	    memory_order_relaxed) : 0;
}
