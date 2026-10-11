/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/15_continuous_batching.c -- one continuous
 * batch group with variable-length requests.  Four requests need 5, 1,
 * 2 and 1 rounds.  At most two are active at once; each waiting request
 * takes a freed slot at the next iteration boundary, so the three short
 * requests finish while the long one is still running.
 *
 * The batch API exists only in builds configured with
 * --enable-sched-policy.  A default build returns XTC_E_NOSYS from
 * xtc_batch_create; this program reports that and exits 0.
 */
#include <stdio.h>

#include "xtc.h"
#include "xtc_batch.h"
#include "xtc_exec.h"
#include "xtc_loop.h"
#include "xtc_sched.h"

#define NREQ 4

struct req {
	unsigned length, rounds, order;
	int outcome;
	uint64_t handle;
};

struct demo {
	xtc_batch_t *group;
	struct req reqs[NREQ];
	unsigned finished, deadlk;
};

/* !region step */
/* One call per iteration, with the whole ready vector. */
static int
step(const xtc_batch_item_t *items, xtc_batch_result_t *results,
    size_t count, void *user)
{
	struct demo *d = user;
	uint64_t h;
	size_t i;

	/* Callbacks cannot mutate their own group. */
	if (xtc_batch_submit(d->group, NULL, 1, &h) == XTC_E_DEADLK)
		d->deadlk++;
	for (i = 0; i < count; i++) {
		struct req *r = items[i].request;

		r->rounds++;
		results[i].state = r->rounds == r->length ?
		    XTC_BATCH_DONE : XTC_BATCH_CONTINUE;
	}
	return XTC_OK;
}

/* Exactly once per accepted request. */
static void
complete(uint64_t handle, void *request, int outcome, void *user)
{
	struct demo *d = user;
	struct req *r = request;

	(void)handle;
	r->outcome = outcome;
	r->order = ++d->finished;
}
/* !endregion step */

int
main(void)
{
	static const unsigned lengths[NREQ] = { 5, 1, 2, 1 };
	struct demo d = { 0 };
	xtc_loop_t *loop;
	xtc_exec_class_t cls;
	xtc_batch_stats_t st = { .size = sizeof(st),
	    .version = XTC_BATCH_VERSION };
	xtc_batch_opts_t opts;
	int i, rc;

	if (xtc_loop_init(&loop) != XTC_OK ||
	    xtc_exec_class_create(loop, 100, 0, &cls) != XTC_OK)
		return 1;

	/* !region create */
	opts = (xtc_batch_opts_t){ .size = sizeof(opts),
	    .version = XTC_BATCH_VERSION,
	    .capacity = NREQ,        /* waiting + active slots */
	    .max_active = 2,         /* members per vector, at most */
	    .resource_capacity = 2,  /* units shared by active members */
	    .sched_class = cls,
	    .step = step, .complete = complete, .user = &d };

	/* Before the loop's first run. */
	rc = xtc_batch_create(loop, &opts, &d.group);
	if (rc == XTC_E_NOSYS) {
		puts("batching not built (configure --enable-sched-policy)");
		(void)xtc_loop_fini(loop);
		return 0;
	}
	if (rc != XTC_OK ||
	    xtc_sched_continuous_set(loop, 3) != XTC_OK) /* optional */
		return 1;
	for (i = 0; i < NREQ; i++) {
		d.reqs[i].length = lengths[i];
		if (xtc_batch_submit(d.group, &d.reqs[i], 1,
		    &d.reqs[i].handle) != XTC_OK)
			return 1;
	}
	/* !endregion create */

	/* !region lifecycle */
	/* Close, run until the driver ends, destroy, then fini. */
	if (xtc_batch_close(d.group, XTC_BATCH_DRAIN) != XTC_OK ||
	    xtc_loop_run(loop) != XTC_OK ||
	    xtc_batch_get_stats(d.group, &st) != XTC_OK ||
	    xtc_batch_destroy(d.group) != XTC_OK ||
	    xtc_loop_fini(loop) != XTC_OK)
		return 1;
	/* !endregion lifecycle */

	printf("iterations=%llu members=%llu completed=%llu order=%u,%u,%u,%u "
	    "deadlk=%u\n", (unsigned long long)st.iterations,
	    (unsigned long long)st.total_members,
	    (unsigned long long)st.completed, d.reqs[0].order,
	    d.reqs[1].order, d.reqs[2].order, d.reqs[3].order, d.deadlk);

	/* 9 rounds of work in 5 iterations: the vector stayed full until
	 * only the long request was left, and every short request finished
	 * before it.  A static batch of two would hold requests 2 and 3
	 * until request 0 finished. */
	for (i = 0; i < NREQ; i++)
		if (d.reqs[i].outcome != XTC_OK ||
		    d.reqs[i].rounds != d.reqs[i].length)
			return 1;
	return st.iterations == 5 && st.total_members == 9 &&
	    st.max_vector == 2 && st.completed == NREQ &&
	    d.deadlk == st.iterations && d.reqs[1].order == 1 &&
	    d.reqs[2].order == 2 && d.reqs[3].order == 3 &&
	    d.reqs[0].order == 4 ? 0 : 1;
}
