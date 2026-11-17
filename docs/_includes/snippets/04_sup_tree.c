/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/04_sup_tree.c -- a supervision tree: a server
 * and a nested supervisor as children of one supervisor (2.2).
 *
 * The root supervisor has two children.  "kv" is a gen_server
 * (xtc_svr_child_main): the child process IS the server, found by name
 * with xtc_svr_call_name.  "pool" is a nested supervisor
 * (xtc_sup_child_main) with its own restart budget over one worker
 * that crashes on start: the crashes stay inside the pool until its
 * budget is spent, then the pool exits with XTC_E_RESOURCE and the root
 * restarts the whole subtree.  wait_children makes xtc_sup_start return
 * only once kv's init has run and the pool's worker has started.
 */

/* !region full */
#include <stdio.h>
#include <string.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_orc.h"
#include "xtc_proc.h"
#include "xtc_reg.h"
#include "xtc_svr.h"

#define MS (1000LL * 1000)

static int kv_value = 7;

static int
kv_call(void *st, const void *req, size_t n, xtc_svr_call_t *call)
{
	(void)req;
	(void)n;
	xtc_svr_reply(call, st, sizeof(int));
	return XTC_SVR_CONTINUE;
}

static int crashes;

static void
worker(void *arg)
{
	(void)arg;
	crashes++;
	xtc_exit_self(1);            /* every start fails */
}

static xtc_svr_callbacks_t kv_cb = { .size = sizeof(xtc_svr_callbacks_t),
	.handle_call = kv_call };
static xtc_svr_child_t kv = { .size = sizeof(xtc_svr_child_t),
	.cb = &kv_cb, .state = &kv_value };

static xtc_sup_opts_t pool_opts = { .size = sizeof(xtc_sup_opts_t),
	.strategy = XTC_SUP_ONE_FOR_ONE, .max_restarts = 2,
	.period_ns = 5000 * MS };
static xtc_child_spec_t pool_kids[1] = { { .size = sizeof(xtc_child_spec_t),
	.name = "worker", .fn = worker, .policy = XTC_RESTART_PERMANENT } };
static xtc_sup_child_t pool = { .size = sizeof(xtc_sup_child_t),
	.opts = &pool_opts, .children = pool_kids, .n_children = 1 };

struct run {
	xtc_loop_t       *loop;
	xtc_reg_t        *reg;
	xtc_supervisor_t *root;
};

static void
client(void *arg)
{
	struct run *r = arg;
	xtc_child_spec_t kids[2] = { XTC_CHILD_SPEC_INIT,
		XTC_CHILD_SPEC_INIT };
	xtc_sup_opts_t o = XTC_SUP_OPTS_DEFAULT;
	void *reply;
	size_t len;

	kids[0].name = kids[0].reg_name = "kv";
	kids[0].fn = xtc_svr_child_main;
	kids[0].arg = &kv;
	kids[0].policy = XTC_RESTART_TRANSIENT;
	kids[1].name = "pool";
	kids[1].fn = xtc_sup_child_main;
	kids[1].arg = &pool;
	kids[1].policy = XTC_RESTART_TRANSIENT;
	o.registry = r->reg;
	o.max_restarts = 1;          /* the subtree comes back once */
	o.wait_children = 1;
	if (xtc_sup_start(r->loop, &o, kids, 2, &r->root) != XTC_OK)
		return;

	if (xtc_svr_call_name(r->reg, "kv", "get", 3, &reply, &len,
	        1000 * MS) == XTC_OK) {
		printf("kv answered %d\n", *(int *)reply);
		xtc_free(reply);
	}
	/* Meanwhile the pool spends its budget (3 crashes), the root
	 * restarts it, and it spends it again: the root's own budget is
	 * then gone too, so the root stops and the loop drains. */
	xtc_proc_sleep(20 * MS);
	printf("worker crashes: %d\n", crashes);
	xtc_sup_stop(r->root);
}

int
main(void)
{
	struct run r = { 0 };

	xtc_loop_create(&r.loop);
	xtc_reg_create(&r.reg);
	xtc_proc_spawn(r.loop, client, &r, NULL, NULL);
	xtc_loop_run(r.loop);
	if (r.root != NULL)
		xtc_sup_join(r.root, -1);
	xtc_reg_destroy(r.reg);
	xtc_loop_destroy(r.loop);
	return 0;
}
/* !endregion */
