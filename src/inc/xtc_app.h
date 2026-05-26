/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
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

#include <stddef.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_orc.h"
#include "xtc_reg.h"

typedef struct xtc_app xtc_app_t;

typedef struct xtc_app_opts {
	const char     *name;          /* optional, for logs */
	xtc_loop_t     *loop;          /* optional; NULL = create one */
	xtc_sup_opts_t  sup;           /* root-supervisor settings */
} xtc_app_opts_t;

#define XTC_APP_OPTS_DEFAULT { \
	.name = NULL, \
	.loop = NULL, \
	.sup  = XTC_SUP_OPTS_DEFAULT \
}

/*
 * PUBLIC: int        xtc_app_create __P((const xtc_app_opts_t *, xtc_app_t **));
 * PUBLIC: void       xtc_app_destroy __P((xtc_app_t *));
 * PUBLIC: int        xtc_app_start __P((xtc_app_t *, const xtc_child_spec_t *, int));
 * PUBLIC: int        xtc_app_run __P((xtc_app_t *));
 * PUBLIC: int        xtc_app_stop __P((xtc_app_t *));
 * PUBLIC: xtc_loop_t *xtc_app_loop __P((const xtc_app_t *));
 * PUBLIC: xtc_reg_t  *xtc_app_registry __P((const xtc_app_t *));
 */

int        xtc_app_create(const xtc_app_opts_t *opts, xtc_app_t **out);
void       xtc_app_destroy(xtc_app_t *app);

/* Start the root supervisor with `n_children` initial children. */
int        xtc_app_start(xtc_app_t *app,
                         const xtc_child_spec_t *children,
                         int n_children);

/* Run the loop until the supervisor exits or xtc_app_stop is called.
 * On return, xtc_app_destroy may be called. */
int        xtc_app_run(xtc_app_t *app);

/* Asynchronously request the app to stop (kicks the root sup). */
int        xtc_app_stop(xtc_app_t *app);

xtc_loop_t *xtc_app_loop(const xtc_app_t *app);
xtc_reg_t  *xtc_app_registry(const xtc_app_t *app);

#endif /* XTC_APP_H */
