/* Feature off/on with no installed policy must preserve dispatch order.
 * Record caller-assigned IDs on EVERY proc entry/resumption, including DONE;
 * aggregate run counts and xtc_sim_state_hash alone cannot prove this. */
#include "xtc_int.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sim.h"
#include <assert.h>
#include <stdio.h>

#define WORKERS 6
#define QUANTA 80

struct trace {
	unsigned ids[WORKERS * QUANTA];
	unsigned count[WORKERS], n, done;
};

struct worker {
	struct trace *trace;
	unsigned id;
};

static void
work(void *arg)
{
	struct worker *w = arg;
	struct trace *t = w->trace;
	unsigned i;

	for (i = 0; i < QUANTA; i++) {
		assert(t->n < WORKERS * QUANTA);
		t->ids[t->n++] = w->id;
		t->count[w->id]++;
		if (i + 1 < QUANTA)
			xtc_yield();
	}
	t->done++;
}

static void
run(uint64_t seed, int classes, int faults)
{
	struct trace t = {0};
	struct worker w[WORKERS];
	xtc_proc_opts_t opts = XTC_PROC_OPTS_INIT;
	xtc_exec_class_t lanes[2] = {0};
	xtc_exec_t *exec;
	xtc_loop_t *loop;
	unsigned i;

	assert(xtc_exec_init(&exec, 1) == XTC_OK);
	loop = xtc_exec_loop(exec, 0);
	if (classes) {
		assert(xtc_exec_class_create(loop, 3, 0, &lanes[0]) == XTC_OK);
		assert(xtc_exec_class_create(loop, 1, 0, &lanes[1]) == XTC_OK);
	}
	for (i = 0; i < WORKERS; i++) {
		w[i].trace = &t;
		w[i].id = i; /* Stable spawn ID, never first-run order or address. */
		/* Two default workers remain alongside two workers per class. */
		opts.sched_class = i < 2 ? NULL : lanes[(i - 2) / 2];
		assert(xtc_proc_spawn(loop, work, &w[i], &opts, NULL) == XTC_OK);
	}
	if (faults)
		xtc_sim_buggify_enable(1000);
	else
		xtc_sim_buggify_disable();
	assert(xtc_sim_exec_run(exec, seed, 100000) == XTC_OK);
	assert(t.done == WORKERS && t.n == WORKERS * QUANTA);
	printf("seed=%llu classes=%d faults=%d done=%u runs=%u state=%llu",
	    (unsigned long long)seed, classes, faults, t.done, t.n,
	    (unsigned long long)xtc_sim_state_hash(exec));
	for (i = 0; i < WORKERS; i++) {
		assert(t.count[i] == QUANTA); /* Includes default-lane progress. */
		printf(" %u", t.count[i]);
	}
	printf(" class-runs=%llu,%llu order=",
	    (unsigned long long)xtc_exec_class_runs(lanes[0]),
	    (unsigned long long)xtc_exec_class_runs(lanes[1]));
	for (i = 0; i < t.n; i++)
		printf("%u", t.ids[i]);
	putchar('\n');
	assert(xtc_exec_fini(exec) == XTC_OK);
	xtc_sim_buggify_disable();
}

int
main(void)
{
	uint64_t seed;
	int classes, faults;

	for (seed = 1; seed <= 32; seed++)
		for (classes = 0; classes <= 1; classes++)
			for (faults = 0; faults <= 1; faults++)
				run(seed, classes, faults);
	return 0;
}
