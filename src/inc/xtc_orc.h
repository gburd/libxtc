/*-
 * Copyright (c) 2026, The XTC Project
 * @stability stable
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_orc.h
 *	L4 orchestration: the supervisor, with the four OTP restart
 *	strategies (one_for_one, one_for_all, rest_for_one,
 *	simple_one_for_one) and restart intensity
 *	(max-N-restarts-per-W-seconds).
 *
 *	A supervisor is itself an xtc_proc that monitors its children
 *	and acts on their DOWN messages.  Restart intensity is enforced
 *	by counting child deaths in a sliding window; if the rate
 *	exceeds max_restarts/period_ns, the supervisor exits up the
 *	tree.
 */

#ifndef XTC_ORC_H
#define XTC_ORC_H

#include "xtc_export.h"

#include <stdint.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

/*
 * Restart strategies.  All four are implemented; xtc_sup_start returns
 * XTC_E_NOSYS for any other value.
 */
typedef enum xtc_restart_strategy {
	XTC_SUP_ONE_FOR_ONE = 0,
	XTC_SUP_ONE_FOR_ALL = 1,
	XTC_SUP_REST_FOR_ONE = 2,
	XTC_SUP_SIMPLE_OFO = 3
} xtc_restart_strategy_t;

typedef enum xtc_restart_policy {
	XTC_RESTART_PERMANENT  = 0,    /* always restart on exit */
	XTC_RESTART_TRANSIENT  = 1,    /* restart only on abnormal exit */
	XTC_RESTART_TEMPORARY  = 2     /* never restart */
} xtc_restart_policy_t;

/* The L2 multi-loop executor (xtc_exec.h).  A supervisor may be given
 * one so it places children across loops and owns the executor's stop on
 * its own exit. */
struct xtc_exec;
/* The process registry (xtc_reg.h), for child names. */
struct xtc_reg;

typedef struct xtc_child_spec {
	size_t                size;       /* sizeof(xtc_child_spec_t): XTC_CHILD_SPEC_INIT */
	const char           *name;       /* optional, for logs */
	xtc_proc_fn           fn;
	void                 *arg;
	xtc_restart_policy_t  policy;
	size_t                mailbox_cap; /* 0 = default */
	int                   loop;        /* exec loop index to place this
	                                    * child on (only when the supervisor
	                                    * has an exec); 0 = loop 0 */
	/*
	 * 2.2: the xtc_proc_opts_t knobs a plain spawn has, applied to every
	 * incarnation (the first start and each restart), so a child need not
	 * set them on itself.  See xtc_proc_opts_t for their meaning.
	 * sched_class must have been created on the loop the child is placed
	 * on (`loop`); a class from another loop is ignored, as for
	 * xtc_proc_spawn.
	 */
	void (*mailbox_watermark_fn)(xtc_pid_t self, size_t depth, size_t cap,
	    void *user);
	void *mailbox_watermark_user;
	xtc_exec_class_t sched_class; /* NULL = the plain FIFO class */
	/*
	 * 2.2: a name the supervisor registers for every incarnation of the
	 * child in its registry (xtc_sup_opts_t.registry; an xtc_app's own),
	 * replacing the previous incarnation's entry, and removes when the
	 * child dies.  Clients find the current incarnation with
	 * xtc_reg_whereis, or wait for the next one with
	 * xtc_reg_whereis_wait.  NULL = none.  Must stay valid for the
	 * supervisor's life; at most one child per name.
	 */
	const char *reg_name;
	int mailbox_watermark_pct;    /* 0 = off, 1..100 */
	int migratable;               /* 1 = work-stealable */
} xtc_child_spec_t;

/* Every spec, including each element of an xtc_sup_start / xtc_app_start
 * array, carries .size; the library strides an array by children[0].size
 * and requires every element to carry that same size. */
#define XTC_CHILD_SPEC_INIT { .size = sizeof(xtc_child_spec_t) }

typedef struct xtc_supervisor xtc_supervisor_t;

/*
 * A child of the supervisor went DOWN (2.2).  Called on the supervisor's
 * own fiber, BEFORE the supervisor decides whether to restart that child,
 * once for each DOWN of a child it owns:
 *   - a child that crashed, exited or was killed by someone else;
 *   - a sibling the supervisor itself killed in a one_for_all /
 *     rest_for_one restart, before that sibling is respawned (its DOWN has
 *     kind XTC_DOWN_KIND_KILLED or EXIT, by the reason it was killed with);
 *   - a child killed when the supervisor stops, before the stop completes.
 * `spec` is the supervisor's copy of the child's spec, borrowed for the
 * call: spec->name and spec->arg are a per-child identity that survives a
 * restart (the pid does not).  `info` is the classified DOWN
 * (xtc_down_decode_ex), borrowed.
 *
 * The hook observes; it cannot veto or change the restart.  It runs on the
 * supervisor, so it must be brief and MUST NOT park (no xtc_recv,
 * xtc_proc_sleep, blocking I/O, xtc_amutex): setting a flag, an atomic
 * store, xtc_proc_wake and xtc_sup_stop are fine.  xtc_sup_add_child on
 * this supervisor returns XTC_E_STATE from inside the hook.  Do not use a
 * plain xtc_send to forward the DOWN: it honours the target's mailbox cap
 * and can fail; record what you need and wake the reader instead.
 */
typedef void (*xtc_sup_down_fn)(xtc_supervisor_t *sup,
                                const xtc_child_spec_t *spec,
                                const xtc_down_info_t *info, void *user);

typedef struct xtc_sup_opts {
	size_t                 size;             /* sizeof(xtc_sup_opts_t) */
	xtc_restart_strategy_t strategy;
	int                    max_restarts;     /* default 3 */
	int64_t                period_ns;        /* default 5 s */
	struct xtc_exec       *exec;             /* optional: place children
	                                          * across its loops and stop
	                                          * it when the supervisor exits;
	                                          * NULL = single-loop */
	int                    max_children;     /* 0 = unbounded; > 0 caps the
	                                          * dynamic child count
	                                          * (xtc_sup_add_child returns
	                                          * XTC_E_RESOURCE at the cap) --
	                                          * the simple_one_for_one pool
	                                          * bound */
	xtc_sup_down_fn        on_child_down;    /* NULL = none (2.2) */
	void                  *on_child_down_user;
	/* 2.2: the registry for child specs' reg_name.  xtc_app sets its
	 * own; NULL = no names.  Must outlive the supervisor. */
	struct xtc_reg *registry;
} xtc_sup_opts_t;

#define XTC_SUP_OPTS_DEFAULT { \
	.size         = sizeof(xtc_sup_opts_t), \
	.strategy     = XTC_SUP_ONE_FOR_ONE, \
	.max_restarts = 3, \
	.period_ns    = 5LL * 1000 * 1000 * 1000, \
	.exec         = NULL, \
	.max_children = 0 \
}

/*
 * PUBLIC: int  xtc_sup_start __P((xtc_loop_t *, const xtc_sup_opts_t *, const xtc_child_spec_t *, int, xtc_supervisor_t **));
 * PUBLIC: int  xtc_sup_add_child __P((xtc_supervisor_t *, const xtc_child_spec_t *, xtc_pid_t *));
 * PUBLIC: int  xtc_sup_stop __P((xtc_supervisor_t *));
 * PUBLIC: int  xtc_sup_n_children __P((const xtc_supervisor_t *));
 * PUBLIC: int  xtc_sup_n_alive __P((const xtc_supervisor_t *));
 * PUBLIC: int  xtc_sup_n_restarts __P((const xtc_supervisor_t *));
 * PUBLIC: int  xtc_sup_alive __P((const xtc_supervisor_t *));
 * PUBLIC: int  xtc_sup_child_restarts __P((const xtc_supervisor_t *, int, int *));
 */

/*
 * Start a supervisor on `loop` with `n_children` initial children.
 * The supervisor itself runs as a process; the returned handle lets
 * callers query/stop it from outside.
 */
XTC_API int  xtc_sup_start(xtc_loop_t *loop,
                           const xtc_sup_opts_t *opts,
                           const xtc_child_spec_t *children,
                           int n_children,
                           xtc_supervisor_t **out_sup);

/* Ask the supervisor to terminate.  Non-blocking; returns immediately
 * after setting the flag and sending a kick.  Children get DOWN'd as
 * the supervisor processes its mailbox; supervisor exits.
 *
 * Safe to call from any thread, multiple times. */
XTC_API int  xtc_sup_stop(xtc_supervisor_t *sup);

/* Dynamically add a child to a running supervisor (the
 * SIMPLE_ONE_FOR_ONE pattern: a pool of identical children spawned on
 * demand).  Routed through the supervisor proc so it owns the
 * monitor; returns the new child's pid.  Must be called from within a
 * proc (it awaits the supervisor's reply).  Keep spec->name alive for
 * the child's lifetime. */
XTC_API int  xtc_sup_add_child(xtc_supervisor_t *sup, const xtc_child_spec_t *spec,
                               xtc_pid_t *out_pid);

/* Wait for the supervisor to actually exit, then free its handle.
 * Must be called from outside the supervisor's loop thread.
 * timeout_ns < 0 = forever, 0 = poll-once.  After a successful join
 * the handle is invalid. */
XTC_API int xtc_sup_join(xtc_supervisor_t *sup, int64_t timeout_ns);

XTC_API int  xtc_sup_n_children(const xtc_supervisor_t *sup);
XTC_API int  xtc_sup_n_alive(const xtc_supervisor_t *sup);
XTC_API int  xtc_sup_n_restarts(const xtc_supervisor_t *sup);
XTC_API int  xtc_sup_alive(const xtc_supervisor_t *sup);

/*
 * Restarts of one static child (2.2): the child at `index` in the array
 * given to xtc_sup_start (or xtc_app_start), counted the way
 * xtc_sup_n_restarts counts them all (one per restart decision; a
 * one_for_all or rest_for_one restart counts for the child that died,
 * not for the siblings restarted with it).  Any thread, any time while
 * the handle is valid.  XTC_E_INVAL for a NULL sup or out, XTC_E_NOTFOUND
 * for an index outside the static array.
 */
XTC_API int
xtc_sup_child_restarts(const xtc_supervisor_t *sup, int index, int *out);

#endif /* XTC_ORC_H */
