/*-
 * Copyright (c) 2026, The XTC Project
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
#include "preempt_int.h"   /* __xtc_unsafe_* / __xtc_mtx_*: internal preemption brackets */
#include "xtc_orc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sync.h"
#include "proc_int.h"      /* __xtc_proc_loop_contains: owner-qualified presence */

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
	int              dynamic;   /* added by xtc_sup_add_child */
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
	_Atomic int            abandon_wait;   /* __xtc_sup_abandon_wait */
	/* Packed xtc_pid_t. Stored by xtc_sup_start and again (same value)
	 * by the supervisor proc itself, read from any thread. */
	_Atomic uint64_t       sup_pid;
	xtc_notify_t          *stopped;
};

static uint64_t
__sup_pid_pack(xtc_pid_t p)
{
	uint64_t v;
	memcpy(&v, &p, sizeof v);
	return v;
}

static xtc_pid_t
__sup_pid_load(struct xtc_supervisor *sup)
{
	uint64_t v = atomic_load_explicit(&sup->sup_pid, memory_order_acquire);
	xtc_pid_t p;
	memcpy(&p, &v, sizeof p);
	return p;
}

/* The DOWN signal envelope shape we emit in xtc_proc_exit cleanup. */
XTC_PACK_PUSH
struct down_signal {
	uint8_t  kind;
	uint64_t ref;
	xtc_pid_t pid;
	int      reason;
} XTC_PACKED;

/* Control message: add a dynamic child (SIMPLE_ONE_FOR_ONE).  Sent to
 * the supervisor proc so the spawn + monitor are owned by it.  Same
 * address space, so the spec (incl. fn/arg pointers) is copied by
 * value; the caller must keep spec.name alive for the child's life. */
struct add_child_msg {
	uint8_t          kind;       /* 'A' */
	xtc_pid_t        reply;
	xtc_loop_t      *reply_loop; /* reply's owner: pids are loop-relative */
	uint32_t         tag;
	xtc_child_spec_t spec;
};
struct add_child_reply {
	uint32_t  tag;
	int       ok;
	xtc_pid_t pid;
};
XTC_PACK_POP

static int
__should_restart(struct xtc_supervisor *sup, const struct child *c, int reason)
{
	(void)sup; (void)c;
	switch (c->spec.policy) {
	case XTC_RESTART_PERMANENT:  return 1;
	case XTC_RESTART_TEMPORARY:  return 0;
	case XTC_RESTART_TRANSIENT:
		/*
		 * TRANSIENT restarts only on ABNORMAL termination (OTP
		 * semantics).  "Nonzero reason" is the test for that, with one
		 * exception: XTC_DOWN_NOPROC (-100000) means the monitor was
		 * registered on a target that had already been reaped, so the
		 * real reason is UNKNOWN -- it is explicitly documented as
		 * benign, not as abnormal.  Treating it as abnormal restarts
		 * children that exited cleanly and can fight an orderly
		 * shutdown, with restart-intensity escalation then taking down
		 * a healthy tree.
		 *
		 * __spawn_child no longer produces this (it spawns and monitors
		 * atomically), but xtc_monitor on an already-dead target is a
		 * public route to NOPROC, so the policy is made explicit here
		 * rather than left to depend on the spawn path being correct.
		 * Defence in depth: the atomicity fix removes the cause, this
		 * removes the misclassification.
		 */
		return reason != 0 && reason != XTC_DOWN_NOPROC;
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

/* The loop a child is spawned on -- its OWNER loop (p->loop, unchanged by
 * migration), a pure function of the spec, so every incarnation of a
 * child index has the same owner.  Multi-loop app: the requested
 * executor loop (clamped); the supervisor itself runs on loop 0 and
 * monitors it cross-loop, restarting it on the same loop on a DOWN. */
static xtc_loop_t *
__child_loop(struct xtc_supervisor *sup, const struct child *c)
{
	if (sup->opts.exec != NULL) {
		int n = xtc_exec_n_loops(sup->opts.exec);
		int idx = c->spec.loop;
		if (idx < 0 || idx >= n) idx = 0;
		return xtc_exec_loop(sup->opts.exec, idx);
	}
	return sup->loop;
}

static int
__spawn_child(struct xtc_supervisor *sup, struct child *c)
{
	xtc_proc_opts_t pop;
	xtc_loop_t *target = __child_loop(sup, c);
	int rc;
	memset(&pop, 0, sizeof pop);
	pop.name = c->spec.name;
	pop.mailbox_cap = c->spec.mailbox_cap;
	/*
	 * ATOMIC spawn+monitor, not spawn-then-monitor.
	 *
	 * xtc_proc_spawn followed by xtc_monitor leaves a window in which the
	 * child exists and is runnable but unmonitored.  A child that exits or
	 * faults inside that window is reaped before the monitor lands, so
	 * xtc_monitor delivers XTC_DOWN_NOPROC instead of the real reason --
	 * and the real reason is gone, not merely late.  Two consequences, both
	 * measured on this file before the fix:
	 *
	 *   - a child that FAULTS is reported as NOPROC (documented "benign")
	 *     rather than a signal, so a supervisor cannot tell a segfault from
	 *     a monitor that raced an already-dead target.  For a consumer whose
	 *     policy is "a crashed child means shared state may be corrupt, so
	 *     fail-stop", losing that distinction loses the fail-stop trigger.
	 *   - XTC_DOWN_NOPROC is -100000, which is nonzero, so the TRANSIENT
	 *     policy below ("restart on any nonzero reason") RESTARTS a child
	 *     that in fact exited cleanly.  That violates OTP transient
	 *     semantics and can fight an orderly shutdown.
	 *
	 * The window is not merely theoretical and not merely a cross-loop
	 * thread race: widening it by 5 ms turned a clean fast-exiting child
	 * from reason=0 into reason=-100000 in 40 of 40 runs on a SINGLE loop.
	 * Cross-loop placement (see the comment above) widens it further,
	 * because the child can be made runnable on another OS thread.
	 *
	 * xtc_proc_spawn_monitor establishes the monitor BEFORE the child is
	 * made runnable, so there is no such window.  It requires the caller to
	 * be a process; every path here runs inside __sup_entry (the supervisor
	 * proc), including the restart paths and __handle_add_child, so that
	 * holds at all call sites.
	 */
	rc = xtc_proc_spawn_monitor(target, c->spec.fn, c->spec.arg, &pop,
	    &c->pid, &c->monitor_ref);
	if (rc != XTC_OK) return rc;
	c->alive = 1;
	return XTC_OK;
}

/* Handle an ADD_CHILD control message inside the supervisor proc, so
 * the spawn + monitor are owned by it (the recv loop reaps the DOWN).
 * Grows the children array, spawns, and replies with the new pid. */
static void
__handle_add_child(struct xtc_supervisor *sup, const struct add_child_msg *a)
{
	struct add_child_reply rep;
	void *nc = NULL;
	int idx, rc;

	memset(&rep, 0, sizeof rep);
	rep.tag = a->tag;
	(void)__xtc_mtx_lock(&sup->lock);
	/* Bounded dynamic pool: enforce max_children (0 = unbounded).  Count
	 * only live children so a slot freed by a DOWN can be reused. */
	if (sup->opts.max_children > 0) {
		int live = 0, i;
		for (i = 0; i < sup->n_children; i++)
			if (sup->children[i].alive) live++;
		if (live >= sup->opts.max_children) {
			(void)__xtc_mtx_unlock(&sup->lock);
			rep.ok = 2;   /* at cap -> XTC_E_RESOURCE */
			(void)__xtc_send_on(a->reply_loop, a->reply, &rep, sizeof rep);
			return;
		}
	}
	if (__os_realloc(sup->children,
	    (size_t)(sup->n_children + 1) * sizeof(struct child), &nc)
	    != XTC_OK) {
		(void)__xtc_mtx_unlock(&sup->lock);
		rep.ok = 0;
		(void)__xtc_send_on(a->reply_loop, a->reply, &rep, sizeof rep);
		return;
	}
	sup->children = nc;
	idx = sup->n_children;
	memset(&sup->children[idx], 0, sizeof(struct child));
	sup->children[idx].spec = a->spec;
	sup->children[idx].dynamic = 1;
	rc = __spawn_child(sup, &sup->children[idx]);
	if (rc != XTC_OK) {
		(void)__xtc_mtx_unlock(&sup->lock);
		rep.ok = 0;
		(void)__xtc_send_on(a->reply_loop, a->reply, &rep, sizeof rep);
		return;
	}
	sup->n_children++;
	rep.ok = 1;
	rep.pid = sup->children[idx].pid;
	(void)__xtc_mtx_unlock(&sup->lock);
	(void)__xtc_send_on(a->reply_loop, a->reply, &rep, sizeof rep);
}

/*
 * How long a group restart waits for a killed sibling's cleanup to finish
 * before giving up on the restart.  Not an opts field on purpose:
 * xtc_sup_opts_t is caller-allocated, and appending to it is exactly the
 * 1.x struct-growth ABI break documented in PLAN.md 18.1.
 */
#define XTC_SUP_CLEANUP_WAIT_NS  (5LL * 1000 * 1000 * 1000)

/*
 * Has this pid's CLEANUP completed?  True only once it has left the proc
 * table, which happens after every at-exit callback has returned.  !alive
 * and XTC_KILL_DELIVERED are both documented (xtc_proc.h) as NOT meaning
 * cleanup finished -- the proc clears `alive` BEFORE running its exit
 * hooks -- so neither may gate a respawn.  Same test
 * xtc_arena_group_discard uses for the same reason.  Looked up in the
 * child's OWNER loop only: PIDs are executor-relative, and a global
 * (xtc_proc_info) scan matched an unrelated standalone loop's equal pid,
 * so a group restart waited out XTC_SUP_CLEANUP_WAIT_NS and stopped.
 */
static int
__child_gone(xtc_loop_t *owner, xtc_pid_t pid)
{
	return !__xtc_proc_loop_contains(owner, pid);
}

/*
 * Wait, yielding, until every child in [lo, hi) that we just asked to
 * exit has actually left the proc table.  Returns 1 if all are gone, 0 if
 * the deadline passed with at least one still cleaning up.
 *
 * This is what makes a group restart safe for children that share a C
 * resource.  The previous code killed siblings fire-and-forget, marked
 * them dead, and respawned immediately, so a replacement could run while
 * its predecessor was still inside an exit hook holding the resource --
 * measured as a hold count of 4 where the steady state is 2.
 *
 * Called from the supervisor's own fiber with sup->lock held; the only
 * other takers of that lock are this same proc (ADD_CHILD is handled in
 * this loop), so yielding while holding it cannot deadlock a foreign
 * thread.  It must yield rather than block: the dying siblings need
 * scheduler time, possibly on this very loop, to run their exit hooks.
 */
static int
__wait_children_gone(struct xtc_supervisor *sup, int lo, int hi,
                     const xtc_pid_t *old)
{
	int64_t start = 0, now = 0;
	int i, pending;

	(void)__os_clock_mono(&start);
	for (;;) {
		/* The owner (xtc_app_shutdown) has already accounted for the
		 * stragglers as survivors: stop waiting on them. */
		if (atomic_load_explicit(&sup->abandon_wait, memory_order_acquire))
			return 0;
		pending = 0;
		for (i = lo; i < hi; i++)
			if (!xtc_pid_is_none(old[i]) &&
			    !__child_gone(__child_loop(sup, &sup->children[i]),
			    old[i]))
				pending++;
		if (pending == 0)
			return 1;
		(void)__os_clock_mono(&now);
		if (now - start >= XTC_SUP_CLEANUP_WAIT_NS)
			return 0;
		(void)xtc_proc_sleep(1LL * 1000 * 1000);   /* 1 ms */
	}
}

/* Kill a still-alive sibling; we only mark it as not-alive once we
 * see its DOWN come back through the mailbox. */
static void
__kill_sibling(struct xtc_supervisor *sup, struct child *c)
{
	if (!c->alive) return;
	/* Owner-qualified: pids are loop-relative (reason 1 = shutdown). */
	(void)__xtc_exit_pid_on(__child_loop(sup, c), c->pid, 1);
}

/* For one_for_all: after a child crash, kill every sibling that's
 * still alive, drain pending DOWN signals, then respawn all in
 * original order. */
static void
__do_one_for_all(struct xtc_supervisor *sup, int dead_idx)
{
	int i;
	xtc_pid_t *old = NULL;
	(void)dead_idx;

	/* Snapshot every child's pid (the crashed one included: its slot may
	 * still be draining its own exit hooks) BEFORE killing, so the wait
	 * below names exactly the generation being replaced. */
	if (sup->n_children > 0 &&
	    __os_calloc((size_t)sup->n_children, sizeof *old,
	    (void **)&old) != XTC_OK) {
		atomic_store_explicit(&sup->stop_requested, 1,
		    memory_order_release);
		return;
	}
	for (i = 0; i < sup->n_children; i++)
		old[i] = sup->children[i].pid;
	for (i = 0; i < sup->n_children; i++)
		if (sup->children[i].alive) __kill_sibling(sup, &sup->children[i]);
	for (i = 0; i < sup->n_children; i++) sup->children[i].alive = 0;

	/* Do not respawn into an overlap: wait for every old child's cleanup
	 * to COMPLETE.  If one will not finish, give up on the restart rather
	 * than run a replacement beside it -- stopping the supervisor is the
	 * documented escalation, and its caller sees it exit. */
	if (!__wait_children_gone(sup, 0, sup->n_children, old)) {
		__os_free(old);
		atomic_store_explicit(&sup->stop_requested, 1,
		    memory_order_release);
		return;
	}
	__os_free(old);

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
	xtc_pid_t *old = NULL;

	if (sup->n_children > 0 &&
	    __os_calloc((size_t)sup->n_children, sizeof *old,
	    (void **)&old) != XTC_OK) {
		atomic_store_explicit(&sup->stop_requested, 1,
		    memory_order_release);
		return;
	}
	for (i = dead_idx; i < sup->n_children; i++)
		old[i] = sup->children[i].pid;
	for (i = sup->n_children - 1; i > dead_idx; i--)
		if (sup->children[i].alive) __kill_sibling(sup, &sup->children[i]);
	for (i = dead_idx; i < sup->n_children; i++) sup->children[i].alive = 0;

	/* As in one_for_all: no replacement until the old generation's
	 * cleanup has completed; escalate (stop) if it does not. */
	if (!__wait_children_gone(sup, dead_idx, sup->n_children, old)) {
		__os_free(old);
		atomic_store_explicit(&sup->stop_requested, 1,
		    memory_order_release);
		return;
	}
	__os_free(old);

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

	atomic_store_explicit(&sup->sup_pid, __sup_pid_pack(xtc_self()),
	    memory_order_release);

	for (i = 0; i < sup->n_children; i++) {
		if (__spawn_child(sup, &sup->children[i]) != XTC_OK) {
			atomic_store_explicit(&sup->alive, 0, memory_order_release);
			(void)xtc_notify_signal(sup->stopped);
			if (sup->opts.exec != NULL)
				(void)xtc_exec_stop(sup->opts.exec);
			return;
		}
	}

	while (!atomic_load_explicit(&sup->stop_requested, memory_order_acquire)) {
		void *msg; size_t sz;
		int rc;
		struct down_signal d;
		int idx;
		int64_t now;
		uint8_t kind;

		/* Receive ANY message (not a selective match): DOWN signals
		 * AND ADD_CHILD control messages share this mailbox, and a
		 * selective match would pile the others into the (capped)
		 * save queue. */
		rc = xtc_recv(&msg, &sz, 100LL * 1000 * 1000);
		if (rc == XTC_E_AGAIN) continue;
		if (rc != XTC_OK) break;
		if (sz < 1) { __os_free(msg); continue; }
		kind = *(const uint8_t *)msg;

		if (kind == 'A') {
			struct add_child_msg a;
			if (sz >= sizeof a) {
				memcpy(&a, msg, sizeof a);
				__os_free(msg);
				__handle_add_child(sup, &a);
			} else {
				__os_free(msg);
			}
			continue;
		}
		if (kind != 'D' || sz < sizeof d) { __os_free(msg); continue; }
		memcpy(&d, msg, sizeof d);
		__os_free(msg);

		(void)__xtc_mtx_lock(&sup->lock);
		idx = __find_child_by_pid(sup, d.pid);
		if (idx < 0) {
			(void)__xtc_mtx_unlock(&sup->lock);
			continue;
		}
		sup->children[idx].alive = 0;

		if (atomic_load_explicit(&sup->stop_requested,
		    memory_order_acquire)) {
			(void)__xtc_mtx_unlock(&sup->lock);
			break;
		}

		if (!__should_restart(sup, &sup->children[idx], d.reason)) {
			/* A dynamic child that is not coming back (TEMPORARY, or
			 * TRANSIENT after a normal exit) leaves the table, as in
			 * OTP.  Before 1.55.1 its slot stayed forever: 20000
			 * short-lived pool workers left n_children at 20000 and
			 * made every add and DOWN scan them.  Order-preserving
			 * removal, so rest_for_one ordering is unchanged. */
			if (sup->children[idx].dynamic) {
				memmove(&sup->children[idx],
				    &sup->children[idx + 1],
				    (size_t)(sup->n_children - idx - 1) *
				    sizeof(struct child));
				sup->n_children--;
			}
			(void)__xtc_mtx_unlock(&sup->lock);
			continue;
		}

		(void)__os_clock_mono(&now);
		__record_restart(sup, now);
		if (__intensity_exceeded(sup, now)) {
			(void)__xtc_mtx_unlock(&sup->lock);
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
		(void)__xtc_mtx_unlock(&sup->lock);
	}

	/*
	 * On exit, kill any still-alive children, then WAIT (bounded) for
	 * their cleanup to finish before announcing the stop.  Before 1.50
	 * the supervisor signalled `stopped` and, as the root of a
	 * multi-loop app, called xtc_exec_stop immediately after sending the
	 * kills -- freezing the executor while the children were still in
	 * their exit hooks, so a graceful shutdown left them half-cleaned
	 * (reported by the shutdown work, 19.27.10).  The same proc-table
	 * absence wait the group restarts use (__wait_children_gone,
	 * XTC_SUP_CLEANUP_WAIT_NS); a child that will not finish in time is
	 * abandoned, not waited on forever.
	 */
	{
		int k;
		xtc_pid_t *old = NULL;
		if (sup->n_children > 0 &&
		    __os_calloc((size_t)sup->n_children, sizeof *old,
		    (void **)&old) != XTC_OK)
			old = NULL;             /* no memory: kill, do not wait */
		(void)__xtc_mtx_lock(&sup->lock);
		for (k = 0; k < sup->n_children; k++) {
			if (old != NULL)
				old[k] = sup->children[k].alive
				    ? sup->children[k].pid : XTC_PID_NONE;
			if (sup->children[k].alive) {
				(void)__xtc_exit_pid_on(__child_loop(sup,
				    &sup->children[k]), sup->children[k].pid, 1);
				sup->children[k].alive = 0;
			}
		}
		(void)__xtc_mtx_unlock(&sup->lock);
		if (old != NULL) {
			(void)__wait_children_gone(sup, 0, sup->n_children, old);
			__os_free(old);
		}
	}

	/*
	 * `alive` drops only NOW, with the exit complete and `stopped` about
	 * to be signalled -- not before the child wait above.  Callers read
	 * !xtc_sup_alive as "the supervisor has exited, a join will succeed"
	 * (xtc_app_shutdown's force phase does); clearing it earlier let one
	 * stop the loop while this proc still slept in __wait_children_gone,
	 * so the join timed out and -- correctly, since f6637ef -- freed
	 * nothing: the supervisor leaked (LSan, 12_graceful_drain under ASan).
	 */
	atomic_store_explicit(&sup->alive, 0, memory_order_release);
	(void)xtc_notify_signal(sup->stopped);
	/* Root of a multi-loop app: stopping the supervisor stops the whole
	 * application -- release the executor so xtc_exec_run (driving all
	 * loops) returns.  This fires both on an intentional xtc_sup_stop
	 * and on a restart-intensity giveup. */
	if (sup->opts.exec != NULL)
		(void)xtc_exec_stop(sup->opts.exec);
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

	if (loop == NULL || out_sup == NULL || n_children < 0 ||
	    (n_children > 0 && children == NULL))
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

	if (n_children > 0) {
		if ((rc = __os_calloc((size_t)n_children, sizeof *sup->children,
		    (void **)&sup->children)) != XTC_OK) goto fail1;
	}
	sup->n_children = n_children;
	for (i = 0; i < n_children; i++) sup->children[i].spec = children[i];

	if ((rc = xtc_notify_create(&sup->stopped)) != XTC_OK) goto fail2;

	/*
	 * Plain spawn, deliberately: nothing monitors the supervisor proc from
	 * here, so there is no link/monitor window to close (RULE 5 in
	 * test_api_discipline.sh matches a spawn FOLLOWED BY monitor/link, and
	 * there is none).  xtc_proc_spawn_monitor would not even be available:
	 * it requires the caller to be a process, and xtc_sup_start is called
	 * from outside any proc (see the tests, which call it from the test
	 * function).  Callers wanting the supervisor's exit use xtc_sup_join.
	 */
	if ((rc = xtc_proc_spawn(loop, __sup_entry, sup, NULL, &pid)) != XTC_OK)
		goto fail3;
	atomic_store_explicit(&sup->sup_pid, __sup_pid_pack(pid),
	    memory_order_release);
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
/* PUBLIC: int xtc_sup_add_child __P((xtc_supervisor_t *, const xtc_child_spec_t *, xtc_pid_t *)); */
int
xtc_sup_add_child(xtc_supervisor_t *sup, const xtc_child_spec_t *spec,
                  xtc_pid_t *out_pid)
{
	static _Atomic uint32_t tagctr;
	struct add_child_msg a;
	struct add_child_reply rep;
	void *msg; size_t sz;

	if (sup == NULL || spec == NULL) return XTC_E_INVAL;
	if (!atomic_load_explicit(&sup->alive, memory_order_acquire))
		return XTC_E_INVAL;
	if (xtc_pid_is_none(xtc_self()))
		return XTC_E_INVAL;     /* must run inside a proc (awaits reply) */

	memset(&a, 0, sizeof a);
	a.kind = 'A';
	/* Record (pid, owner): a pid-only reply from the supervisor's loop
	 * would resolve loop_id 0 to ITS loop, not ours. */
	a.reply_loop = __xtc_proc_self_loop(&a.reply);
	a.tag = atomic_fetch_add_explicit(&tagctr, 1, memory_order_relaxed) + 1;
	a.spec = *spec;
	if (__xtc_send_on(sup->loop, __sup_pid_load(sup), &a, sizeof a)
	    != XTC_OK)
		return XTC_E_INTERNAL;

	for (;;) {
		if (xtc_recv(&msg, &sz, 5LL * 1000 * 1000 * 1000) != XTC_OK)
			return XTC_E_AGAIN;
		if (sz >= sizeof rep) {
			memcpy(&rep, msg, sizeof rep);
			__os_free(msg);
			if (rep.tag != a.tag) continue;   /* not our reply */
			if (rep.ok == 2) return XTC_E_RESOURCE;   /* pool at max_children */
			if (!rep.ok) return XTC_E_NOMEM;
			if (out_pid != NULL) *out_pid = rep.pid;
			return XTC_OK;
		}
		__os_free(msg);
	}
}

int
xtc_sup_stop(xtc_supervisor_t *sup)
{
	if (sup == NULL) return XTC_E_INVAL;
	atomic_store_explicit(&sup->stop_requested, 1, memory_order_release);
	/* Send a one-byte kick so the supervisor wakes from its 100ms
	 * recv-poll and observes stop_requested. */
	{
		uint8_t kick = 'X';
		/* Owner-qualified: off-loop, a pid-only send landed on the
		 * first standalone loop with an equal pid (stray message). */
		(void)__xtc_send_on(sup->loop, __sup_pid_load(sup), &kick, 1);
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
 * XTC_OK: the supervisor exited and is freed; the pointer is invalid.
 * XTC_E_AGAIN: it is STILL RUNNING -- nothing was freed, the pointer
 * stays valid, and the caller may stop it and join again.
 *
 * Before 1.50 the wait's result was discarded and the supervisor freed
 * either way, so a join that timed out freed sup, its child table and
 * its lock under the still-running supervisor proc: a heap-use-after-
 * free at its next mailbox iteration (ASan: READ in __sup_entry of
 * memory freed by xtc_sup_join), and XTC_OK returned for a supervisor
 * that had not stopped.
 */
/*
 * Internal, for xtc_app_shutdown: make the supervisor stop waiting for
 * its killed children's cleanup (__wait_children_gone) and finish exiting
 * now.  The app calls it once it has reported the children that did not
 * finish as survivors (e.g. masked forever); without it the supervisor
 * sat in its bounded 5 s wait after the app had stopped the loop, so it
 * never signalled `stopped`, xtc_app_destroy's join timed out, and the
 * supervisor leaked (LSan: /m10.5/app/shutdown_masked_bounded).
 *
 * PUBLIC: int __xtc_sup_abandon_wait __P((xtc_supervisor_t *));
 */
int
__xtc_sup_abandon_wait(xtc_supervisor_t *sup)
{
	if (sup == NULL) return XTC_E_INVAL;
	atomic_store_explicit(&sup->abandon_wait, 1, memory_order_release);
	return XTC_OK;
}

int
xtc_sup_join(xtc_supervisor_t *sup, int64_t timeout_ns)
{
	int rc;

	if (sup == NULL) return XTC_E_INVAL;
	if ((rc = xtc_notify_wait(sup->stopped, timeout_ns)) != XTC_OK)
		return (rc == XTC_E_AGAIN) ? XTC_E_AGAIN : rc;   /* still running */

	xtc_notify_destroy(sup->stopped);
	__os_free(sup->children);
	__os_free(sup->recent_restarts);
	(void)pthread_mutex_destroy(&sup->lock);
	__os_free(sup);
	return XTC_OK;
}

int xtc_sup_n_children(const xtc_supervisor_t *sup) {
	/* Unlocked on purpose: the supervisor holds sup->lock across
	 * yields during a group restart, so a same-loop caller blocking on
	 * it would deadlock the loop thread.  A snapshot. */
	return sup ? sup->n_children : 0;
}
int xtc_sup_n_alive(const xtc_supervisor_t *sup) {
	int live = 0, i;
	if (sup == NULL) return 0;
	(void)__xtc_mtx_lock(&((xtc_supervisor_t *)sup)->lock);
	for (i = 0; i < sup->n_children; i++)
		if (sup->children[i].alive) live++;
	(void)__xtc_mtx_unlock(&((xtc_supervisor_t *)sup)->lock);
	return live;
}
int xtc_sup_n_restarts(const xtc_supervisor_t *sup) {
	return sup ? atomic_load_explicit(&((xtc_supervisor_t *)sup)->n_restarts_total,
	    memory_order_relaxed) : 0;
}
int xtc_sup_alive(const xtc_supervisor_t *sup) {
	return sup ? atomic_load_explicit(&((xtc_supervisor_t *)sup)->alive,
	    memory_order_relaxed) : 0;
}
