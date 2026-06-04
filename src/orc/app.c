/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/orc/app.c
 *	The application container: glue between loop + supervisor +
 *	registry.  Thin by design.
 */

#include "xtc_int.h"
#include "xtc_app.h"
#include "xtc_exec.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct xtc_app {
	char              *name;
	xtc_loop_t        *loop;       /* loop 0 (the supervisor's loop) */
	xtc_exec_t        *exec;       /* non-NULL for a multi-loop app */
	int                owns_loop;
	xtc_reg_t         *reg;
	xtc_supervisor_t  *root;
	xtc_sup_opts_t     sup_opts;
};

int
xtc_app_create(const xtc_app_opts_t *opts, xtc_app_t **out)
{
	xtc_app_t *a;
	xtc_app_opts_t defaults = XTC_APP_OPTS_DEFAULT;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if (opts == NULL) opts = &defaults;
	if ((rc = __os_calloc(1, sizeof *a, (void **)&a)) != XTC_OK) return rc;

	if (opts->name != NULL)
		(void)__os_strdup(opts->name, &a->name);

	if (opts->loop != NULL) {
		a->loop = opts->loop;
		a->owns_loop = 0;
	} else if (opts->n_loops > 1) {
		/* Multi-loop app: own an executor; the root supervisor runs on
		 * loop 0 and (via sup_opts.exec) places children across loops
		 * and stops the executor when it exits. */
		if ((rc = xtc_exec_init(&a->exec, opts->n_loops)) != XTC_OK)
			goto fail;
		a->loop = xtc_exec_loop(a->exec, 0);
		a->owns_loop = 0;   /* the executor owns its loops */
	} else {
		if ((rc = xtc_loop_init(&a->loop)) != XTC_OK) goto fail;
		a->owns_loop = 1;
	}

	if ((rc = xtc_reg_create(&a->reg)) != XTC_OK) goto fail;

	a->sup_opts = opts->sup;
	a->sup_opts.exec = a->exec;   /* NULL for single-loop; drives stop */
	*out = a;
	return XTC_OK;

fail:
	if (a->reg) xtc_reg_destroy(a->reg);
	if (a->exec) (void)xtc_exec_fini(a->exec);
	if (a->loop && a->owns_loop) (void)xtc_loop_fini(a->loop);
	if (a->name) __os_free(a->name);
	__os_free(a);
	return rc;
}

int
xtc_app_start(xtc_app_t *a, const xtc_child_spec_t *children, int n)
{
	if (a == NULL) return XTC_E_INVAL;
	if (a->root != NULL) return XTC_E_INVAL;     /* already started */
	return xtc_sup_start(a->loop, &a->sup_opts, children, n, &a->root);
}

int
xtc_app_run(xtc_app_t *a)
{
	if (a == NULL || a->loop == NULL) return XTC_E_INVAL;
	/* A multi-loop app runs every loop until the supervisor exits and
	 * stops the executor; a single-loop app runs its one loop until the
	 * supervisor exits and the loop drains. */
	if (a->exec != NULL)
		return xtc_exec_run(a->exec);
	return xtc_loop_run(a->loop);
}

int
xtc_app_stop(xtc_app_t *a)
{
	if (a == NULL) return XTC_E_INVAL;
	if (a->root == NULL) return XTC_OK;
	return xtc_sup_stop(a->root);
}

void
xtc_app_destroy(xtc_app_t *a)
{
	if (a == NULL) return;
	/* Best-effort: if the supervisor is still alive we let the
	 * caller handle that.  Join with a generous timeout. */
	if (a->root != NULL)
		(void)xtc_sup_join(a->root, 5LL * 1000 * 1000 * 1000);
	if (a->reg)  xtc_reg_destroy(a->reg);
	if (a->exec) (void)xtc_exec_fini(a->exec);
	if (a->loop && a->owns_loop) (void)xtc_loop_fini(a->loop);
	if (a->name) __os_free(a->name);
	__os_free(a);
}

xtc_loop_t *xtc_app_loop(const xtc_app_t *a)     { return a ? a->loop : NULL; }
xtc_exec_t *xtc_app_exec(const xtc_app_t *a)     { return a ? a->exec : NULL; }
xtc_reg_t  *xtc_app_registry(const xtc_app_t *a) { return a ? a->reg  : NULL; }
