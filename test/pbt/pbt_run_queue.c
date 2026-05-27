/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/pbt/pbt_run_queue.c
 *	Property-based test for the L2 task run queue (M3).
 *
 *	Hegel draws a list of (target_resched_count) values, one per
 *	task.  Each spawned task increments its own counter on every
 *	call and returns DONE when counter == target.  Property:
 *	after xtc_loop_run, every task's counter equals its target.
 */

#include <stdint.h>
#include <stdlib.h>

#include "pbt_common.h"
#include "xtc_loop.h"
#include "xtc.h"

#if defined(__GNUC__) || defined(__clang__)
# define MAYBE_UNUSED __attribute__((unused))
#else
# define MAYBE_UNUSED
#endif

struct task_state {
	int counter;
	int target;
};

MAYBE_UNUSED static int
task_counter(xtc_task_t *self, void *u)
{
	struct task_state *t = u;
	(void)self;
	t->counter++;
	if (t->counter >= t->target) return XTC_TASK_DONE;
	return XTC_TASK_RESCHED;
}

#if defined(XTC_HAVE_HEGEL)

static void
prop_each_task_runs_target_times(hegel_test_case *tc, void *u)
{
	xtc_loop_t *loop;
	int n, i;
	struct task_state *st;
	(void)u;

	n = (int)hegel_draw_int(tc, hegel_integers(1, 32));

	hegel_assume(xtc_loop_init(&loop) == XTC_OK);
	st = calloc((size_t)n, sizeof *st);
	hegel_assume(st != NULL);

	for (i = 0; i < n; i++) {
		st[i].counter = 0;
		st[i].target  = (int)hegel_draw_int(tc, hegel_integers(1, 50));
		hegel_assume(xtc_task_spawn(loop, task_counter, &st[i], NULL)
		    == XTC_OK);
	}

	hegel_assume(xtc_loop_run(loop) == XTC_OK);

	for (i = 0; i < n; i++)
		hegel_assume(st[i].counter == st[i].target);

	free(st);
	hegel_assume(xtc_loop_fini(loop) == XTC_OK);
}

static const pbt_entry_t tests[] = {
	{ "each_task_runs_target_times",
	    prop_each_task_runs_target_times, 50 },
	{ NULL, NULL, 0 }
};

#else
static const pbt_entry_t tests[] = {
	{ "each_task_runs_target_times", NULL, 0 },
	{ NULL, NULL, 0 }
};
#endif

PBT_MAIN("run_queue", tests)
