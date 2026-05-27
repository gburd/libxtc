/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_orc.h
 *	L4 orchestration: supervisor.  M10 ships ONE_FOR_ONE strategy
 *	with restart intensity (max-N-restarts-per-W-seconds).  The
 *	other strategies (one_for_all, rest_for_one, simple_one_for_one)
 *	and the gen_server / app / registry layer arrive in M10.5.
 *
 *	A supervisor is itself an xtc_proc that monitors its children
 *	and acts on their DOWN messages.  Restart intensity is enforced
 *	by counting child deaths in a sliding window; if the rate
 *	exceeds max_restarts/period_ns, the supervisor exits up the
 *	tree.
 */

#ifndef XTC_ORC_H
#define XTC_ORC_H

#include <stdint.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

/*
 * Restart strategies.  M10 implements ONE_FOR_ONE; the others have
 * named slots so callers can configure for the future and get a
 * clear XTC_E_NOSYS today.
 */
typedef enum xtc_restart_strategy {
	XTC_SUP_ONE_FOR_ONE  = 0,
	XTC_SUP_ONE_FOR_ALL  = 1,    /* M10.5 */
	XTC_SUP_REST_FOR_ONE = 2,    /* M10.5 */
	XTC_SUP_SIMPLE_OFO   = 3     /* M10.5 */
} xtc_restart_strategy_t;

typedef enum xtc_restart_policy {
	XTC_RESTART_PERMANENT  = 0,    /* always restart on exit */
	XTC_RESTART_TRANSIENT  = 1,    /* restart only on abnormal exit */
	XTC_RESTART_TEMPORARY  = 2     /* never restart */
} xtc_restart_policy_t;

typedef struct xtc_child_spec {
	const char           *name;       /* optional, for logs */
	xtc_proc_fn           fn;
	void                 *arg;
	xtc_restart_policy_t  policy;
	size_t                mailbox_cap; /* 0 = default */
} xtc_child_spec_t;

typedef struct xtc_sup_opts {
	xtc_restart_strategy_t strategy;
	int                    max_restarts;     /* default 3 */
	int64_t                period_ns;        /* default 5 s */
} xtc_sup_opts_t;

#define XTC_SUP_OPTS_DEFAULT { \
	.strategy     = XTC_SUP_ONE_FOR_ONE, \
	.max_restarts = 3, \
	.period_ns    = 5LL * 1000 * 1000 * 1000 \
}

typedef struct xtc_supervisor xtc_supervisor_t;

/*
 * PUBLIC: int  xtc_sup_start __P((xtc_loop_t *, const xtc_sup_opts_t *, const xtc_child_spec_t *, int, xtc_supervisor_t **));
 * PUBLIC: int  xtc_sup_stop __P((xtc_supervisor_t *));
 * PUBLIC: int  xtc_sup_n_children __P((const xtc_supervisor_t *));
 * PUBLIC: int  xtc_sup_n_restarts __P((const xtc_supervisor_t *));
 * PUBLIC: int  xtc_sup_alive __P((const xtc_supervisor_t *));
 */

/*
 * Start a supervisor on `loop` with `n_children` initial children.
 * The supervisor itself runs as a process; the returned handle lets
 * callers query/stop it from outside.
 */
int  xtc_sup_start(xtc_loop_t *loop,
                   const xtc_sup_opts_t *opts,
                   const xtc_child_spec_t *children,
                   int n_children,
                   xtc_supervisor_t **out_sup);

/* Ask the supervisor to terminate.  Non-blocking; returns immediately
 * after setting the flag and sending a kick.  Children get DOWN'd as
 * the supervisor processes its mailbox; supervisor exits.
 *
 * Safe to call from any thread, multiple times. */
int  xtc_sup_stop(xtc_supervisor_t *sup);

/* Wait for the supervisor to actually exit, then free its handle.
 * Must be called from outside the supervisor's loop thread.
 * timeout_ns < 0 = forever, 0 = poll-once.  After a successful join
 * the handle is invalid. */
int  xtc_sup_join(xtc_supervisor_t *sup, int64_t timeout_ns);

int  xtc_sup_n_children(const xtc_supervisor_t *sup);
int  xtc_sup_n_restarts(const xtc_supervisor_t *sup);
int  xtc_sup_alive(const xtc_supervisor_t *sup);

#endif /* XTC_ORC_H */
