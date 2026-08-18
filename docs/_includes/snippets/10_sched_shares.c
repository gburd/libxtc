/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/10_sched_shares.c -- the L1 opt-in
 * proportional-share (weighted-fair) scheduler and the L3 over-budget
 * stall watchdog.  INSPIRED BY Glommio (Glauber Costa / ScyllaDB):
 * Glommio's executor gives each task queue SHARES and picks the next by
 * a CFS-style virtual-runtime heap, and its stall detector reports
 * which task monopolized a core.  libxtc adds both as opt-in, zero-
 * overhead-when-off features on the run queue.
 *
 * We create two scheduling classes on one loop -- class A weighted 3x
 * class B -- run equally busy workers in each, and confirm A gets about
 * 3x the CPU (measured as run count).  We also arm a stall budget so a
 * hog would be reported.
 */

/* !region full */
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"

#define BUDGET 900

static atomic_int g_total;   /* shared work budget across both classes */

/* A well-behaved worker: equal cooperative rounds until the shared
 * budget is spent.  The higher-shares class is picked more often, so it
 * burns more of the budget -- proportional CPU. */
static void
worker(void *arg)
{
	(void)arg;
	while (atomic_fetch_add_explicit(&g_total, 1, memory_order_relaxed)
	    < BUDGET)
		xtc_yield();
}

struct ctx {
	xtc_loop_t      *loop;
	xtc_exec_class_t a;
	xtc_exec_class_t b;
};

static void
spawner(void *arg)
{
	struct ctx *c = arg;
	xtc_proc_opts_t oa, ob;
	int i;

	memset(&oa, 0, sizeof oa);
	memset(&ob, 0, sizeof ob);
	oa.sched_class = c->a;   /* place these tasks in class A */
	ob.sched_class = c->b;   /* and these in class B */
	for (i = 0; i < 3; i++) {
		(void)xtc_proc_spawn(c->loop, worker, NULL, &oa, NULL);
		(void)xtc_proc_spawn(c->loop, worker, NULL, &ob, NULL);
	}
}

int
main(void)
{
	xtc_loop_t *loop;
	xtc_exec_class_t a = NULL, b = NULL;
	struct ctx c;
	double ratio;

	if (xtc_loop_init(&loop) != XTC_OK)
		return 1;

	/* Class A: 3 shares.  Class B: 1 share.  Optional latency bound 0
	 * (none).  With no class created the loop would be a plain FIFO. */
	if (xtc_exec_class_create(loop, 3, 0, &a) != XTC_OK ||
	    xtc_exec_class_create(loop, 1, 0, &b) != XTC_OK)
		return 1;

	/* L3: report any run that hogs the loop for more than 50 ms.  Off
	 * by default; this arms it (a NULL callback logs + backtraces). */
	xtc_loop_set_stall_budget(loop, 50 * 1000 * 1000LL);

	c.loop = loop;
	c.a = a;
	c.b = b;
	if (xtc_proc_spawn(loop, spawner, &c, NULL, NULL) != XTC_OK)
		return 1;

	(void)xtc_loop_run(loop);

	/* Class A (3 shares) should have been picked about 3x as often as
	 * class B (1 share): weighted-fair CPU. */
	ratio = xtc_exec_class_runs(b)
	    ? (double)xtc_exec_class_runs(a) / (double)xtc_exec_class_runs(b)
	    : 0.0;
	{
		unsigned long long stalls = xtc_loop_stall_count(loop);
		printf("class A runs=%llu, class B runs=%llu, ratio=%.2f:1 "
		    "(want ~3:1), stalls=%llu\n",
		    (unsigned long long)xtc_exec_class_runs(a),
		    (unsigned long long)xtc_exec_class_runs(b),
		    ratio, stalls);

		(void)xtc_loop_fini(loop);
		/* The well-behaved workers never trip the stall watchdog, and
		 * A beats B by roughly its share ratio. */
		return (stalls == 0 && ratio >= 2.0) ? 0 : 1;
	}
}
/* !endregion full */
