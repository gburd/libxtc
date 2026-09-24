/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_app.h
 *	The L4 application container: a root supervisor + a process
 *	registry + lifecycle plumbing.  Models OTP's `application`
 *	concept.  An xtc_app owns a loop (creating one if not given
 *	or borrowing one passed in), starts a top-level supervisor
 *	with the configured children, and exposes the registry that
 *	those children can use to find each other by name.
 *
 *	Typical usage:
 *
 *	    xtc_app_t *app;
 *	    xtc_app_opts_t opts = XTC_APP_OPTS_DEFAULT;
 *	    xtc_app_create(&opts, &app);
 *	    xtc_app_start(app, child_specs, n_children);
 *	    xtc_app_run(app);              // blocks until app stops
 *	    xtc_app_destroy(app);
 */

#ifndef XTC_APP_H
#define XTC_APP_H

#include "xtc_export.h"

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_orc.h"
#include "xtc_reg.h"

typedef struct xtc_app xtc_app_t;

typedef struct xtc_app_opts {
	const char     *name;          /* optional, for logs */
	xtc_loop_t     *loop;          /* optional; NULL = create one */
	int             n_loops;       /* >1 = own an N-loop executor and run
	                                * the root supervisor on loop 0 (the
	                                * other loops host children placed via
	                                * xtc_child_spec.loop, and any work the
	                                * children spawn via xtc_app_exec); 0/1
	                                * = single loop.  Ignored when `loop`
	                                * is supplied. */
	xtc_sup_opts_t  sup;           /* root-supervisor settings */
	int             no_tuning_check; /* 1 = skip the __os_tuning_check()
	                                * power/kernel-tuning advisor probe
	                                * xtc_app_start runs by default
	                                * (PLAN.md 19.21); additive field,
	                                * appended last -- see
	                                * docs/abi-stability.md. */
} xtc_app_opts_t;

#define XTC_APP_OPTS_DEFAULT { \
	.name = NULL, \
	.loop = NULL, \
	.n_loops = 1, \
	.sup  = XTC_SUP_OPTS_DEFAULT, \
	.no_tuning_check = 0 \
}

/*
 * Graceful drain (PLAN.md 19.20 / 19.27.10).
 *
 * xtc_app_shutdown(app, drain_ns, force_ns, report):
 *   1. sends XTC_APP_SHUTDOWN_MSG to every static child (the children
 *      passed to xtc_app_start) -- test for it with
 *      xtc_app_is_shutdown_msg;
 *   2. waits up to drain_ns for each to finish on its own (return from
 *      its body, or exit);
 *   3. stops the supervisor (no more restarts) and force-cancels every
 *      child still there with xtc_exit_pid(pid, XTC_APP_SHUTDOWN_REASON);
 *   4. waits up to force_ns for each to LEAVE THE PROC TABLE -- the
 *      cleanup-complete signal (at-exit hooks and scope finalizers have
 *      returned), not the weaker "no longer alive";
 *   5. stops the app's loop/executor so xtc_app_run returns, and fills
 *      *report.
 * It ALWAYS returns within about drain_ns + force_ns (+ a few ms), even
 * if a child is masked forever: such a child is reported, not waited
 * for.  Returns XTC_OK when every child left the table, XTC_E_AGAIN when
 * at least one survived (report->survivor_mask), XTC_E_INVAL for a
 * NULL/unstarted app, a negative deadline, a report whose .size is too
 * small, a second shutdown, or a call from one of the app's own
 * supervised children.  Call it from a plain thread or from a proc that
 * is not a supervised child.  Dynamic children (xtc_sup_add_child) are
 * not drained or reported: the supervisor stop in step 3 kills them.
 */
#define XTC_APP_SHUTDOWN_MSG     "$xtc_shutdown"
#define XTC_APP_SHUTDOWN_REASON  1     /* force-cancel exit reason */

/*
 * Size-versioned (PLAN.md 18.1): set .size = sizeof(xtc_app_drain_report_t)
 * before the call (XTC_APP_DRAIN_REPORT_INIT does).  The library writes
 * only the first .size bytes, so fields appended in a later minor release
 * are invisible to, and harmless for, a caller built against this one.
 */
typedef struct xtc_app_drain_report {
	size_t    size;           /* IN: sizeof(xtc_app_drain_report_t) */
	int       n_children;     /* static children at shutdown */
	int       n_drained;      /* finished on their own within drain_ns */
	int       n_forced;       /* force-cancelled, then left the table */
	int       n_survivors;    /* still in the proc table at return */
	uint64_t  forced_mask;    /* bit i = child i was forced (i < 64) */
	uint64_t  survivor_mask;  /* bit i = child i survived (i < 64) */
	int64_t   elapsed_ns;     /* time spent in the shutdown */
} xtc_app_drain_report_t;

#define XTC_APP_DRAIN_REPORT_INIT { .size = sizeof(xtc_app_drain_report_t) }

/*
 * PUBLIC: int        xtc_app_create __P((const xtc_app_opts_t *, xtc_app_t **));
 * PUBLIC: void       xtc_app_destroy __P((xtc_app_t *));
 * PUBLIC: int        xtc_app_start __P((xtc_app_t *, const xtc_child_spec_t *, int));
 * PUBLIC: int        xtc_app_run __P((xtc_app_t *));
 * PUBLIC: int        xtc_app_stop __P((xtc_app_t *));
 * PUBLIC: xtc_loop_t *xtc_app_loop __P((const xtc_app_t *));
 * PUBLIC: xtc_exec_t *xtc_app_exec __P((const xtc_app_t *));
 * PUBLIC: xtc_reg_t  *xtc_app_registry __P((const xtc_app_t *));
 * PUBLIC: int        xtc_app_shutdown __P((xtc_app_t *, int64_t, int64_t, xtc_app_drain_report_t *));
 * PUBLIC: int        xtc_app_drain_on_signal __P((xtc_app_t *, int64_t, int64_t, xtc_app_drain_report_t *));
 * PUBLIC: int        xtc_app_is_shutdown_msg __P((const void *, size_t));
 */

XTC_API int        xtc_app_create(const xtc_app_opts_t *opts, xtc_app_t **out);
XTC_API void       xtc_app_destroy(xtc_app_t *app);

/* Start the root supervisor with `n_children` initial children. */
XTC_API int        xtc_app_start(xtc_app_t *app,
                                 const xtc_child_spec_t *children,
                                 int n_children);

/* Run the loop until the supervisor exits or xtc_app_stop is called.
 * On return, xtc_app_destroy may be called. */
XTC_API int        xtc_app_run(xtc_app_t *app);

/* Asynchronously request the app to stop (kicks the root sup).  NOT a
 * drain: the supervisor immediately xtc_exit_pid()s every child and
 * does not wait for their cleanup.  Use xtc_app_shutdown for that. */
XTC_API int        xtc_app_stop(xtc_app_t *app);

/* Bounded graceful drain; see the block comment above. */
XTC_API int        xtc_app_shutdown(xtc_app_t *app, int64_t drain_ns,
                                    int64_t force_ns,
                                    xtc_app_drain_report_t *report);

/*
 * OPT-IN: drain the app on SIGTERM or SIGINT.  The library installs no
 * signal handler unless this is called.  The handler only write()s one
 * byte to a non-blocking self-pipe (async-signal-safe, nothing else); a
 * watcher proc on the app's loop waits on the pipe with xtc_proc_wait_fd
 * and runs xtc_app_shutdown(app, drain_ns, force_ns, report) there.
 * `report` (may be NULL) must stay valid until xtc_app_run returns; it
 * is filled before that.  Call after xtc_app_start, before xtc_app_run.
 * xtc_app_destroy restores the previous dispositions.  One app per
 * process at a time: a second call while one is armed returns
 * XTC_E_INVAL.  XTC_E_NOSYS on Windows.
 */
XTC_API int        xtc_app_drain_on_signal(xtc_app_t *app, int64_t drain_ns,
                                           int64_t force_ns,
                                           xtc_app_drain_report_t *report);

/* 1 if (msg, len) is the XTC_APP_SHUTDOWN_MSG drain request, else 0. */
XTC_API int        xtc_app_is_shutdown_msg(const void *msg, size_t len);

XTC_API xtc_loop_t *xtc_app_loop(const xtc_app_t *app);

/* The executor backing a multi-loop app (n_loops > 1), or NULL for a
 * single-loop app.  Children use it to spawn work across loops -- e.g.
 * a listener spawning a proc per connection round-robin. */
XTC_API xtc_exec_t *xtc_app_exec(const xtc_app_t *app);

XTC_API xtc_reg_t  *xtc_app_registry(const xtc_app_t *app);

#endif /* XTC_APP_H */
