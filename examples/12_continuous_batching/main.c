/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_async.h"
#include "xtc_batch.h"
#include "xtc_proc.h"
#include "xtc_sched.h"

struct outcome {
	unsigned terminals, exact, cancelled, blocked, retried, heartbeat;
	unsigned late_order, long_order, updates;
};

static void
check(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		exit(1);
	}
}

static void
must(int rc)
{
	if (rc != XTC_OK) {
		fprintf(stderr, "libxtc: %s (build with --enable-sched-policy)\n",
		    xtc_strerror(rc));
		exit(1);
	}
}

#define WIDTH 8
#define GROUPS 2
#define REQUESTS 4

enum { LONG, SHORT, CANCEL, LATE };
struct request {
	uint64_t values[WIDTH], handle, seed;
	unsigned length, rounds, terminals, order;
	int blocked, released, outcome;
};
struct group {
	xtc_batch_t *batch;
	struct request requests[REQUESTS];
	unsigned sequence, updates;
	uint64_t coefficient_seed;
};
struct demo {
	struct group groups[GROUPS];
	unsigned heartbeat;
	int continuous, finished;
};

/* One coefficient setup per vector, not one per request. No fake delays. */
static int
step(const xtc_batch_item_t *items, xtc_batch_result_t *results, size_t n,
    void *arg)
{
	struct group *g = arg;
	uint64_t coefficients[WIDTH];
	struct request *r;
	size_t i;
	unsigned j;

	for (j = 0; j < WIDTH; j++)
		coefficients[j] = (g->coefficient_seed + j) * UINT64_C(6364136223846793005) | 1;
	for (i = 0; i < n; i++) {
		r = items[i].request;
		check(r->terminals == 0, "no step after completion");
		if (r == &g->requests[LONG] && r->rounds == 2 && !r->released) {
			r->blocked = 1;
			results[i].state = XTC_BATCH_BLOCKED;
			continue;
		}
		for (j = 0; j < WIDTH; j++)
			r->values[j] = r->values[j] * coefficients[j] + r->rounds + 1;
		g->updates += WIDTH;
		r->rounds++;
		results[i].state = r->rounds == r->length ?
		    XTC_BATCH_DONE : XTC_BATCH_CONTINUE;
	}
	return XTC_OK;
}

/* Callbacks only record observations; all group mutations are outside them. */
static void
complete(uint64_t handle, void *request, int outcome, void *arg)
{
	struct group *g = arg;
	struct request *r = request;

	check(handle == r->handle && ++r->terminals == 1, "one terminal per handle");
	r->outcome = outcome;
	r->order = ++g->sequence;
}

static void
heartbeat(void *arg)
{
	struct demo *d = arg;

	while (!d->finished) {
		check(++d->heartbeat < 1000000, "bounded progress watchdog");
		xtc_yield();
	}
}

static void
owner(void *arg)
{
	struct demo *d = arg;
	struct group *g;
	struct request *r;
	unsigned i;

	for (i = 0; i < GROUPS; i++) {
		g = &d->groups[i];
		r = g->requests;
		while (!r[LONG].blocked || !r[SHORT].terminals || !d->heartbeat)
			xtc_yield();
		/*
		 * The late arrival is now available. Static batching holds it until
		 * the original cohort drains; continuous batching fills the hole.
		 */
		if (d->continuous)
			must(xtc_batch_submit(g->batch, &r[LATE], 1, &r[LATE].handle));
		r[LONG].released = 1;
		must(xtc_batch_resume(g->batch, r[LONG].handle));
		if (!d->continuous) {
			while (!r[LONG].terminals)
				xtc_yield();
			must(xtc_batch_submit(g->batch, &r[LATE], 1, &r[LATE].handle));
		}
		must(xtc_batch_close(g->batch, XTC_BATCH_DRAIN));
	}
	for (i = 0; i < GROUPS; i++) {
		while (d->groups[i].sequence != REQUESTS)
			xtc_yield();
	}
	d->finished = 1;
}

/* Independent scalar reference: component-major, no vector step calls. */
static void
reference(const struct group *g, const struct request *r)
{
	uint64_t value, coefficient;
	unsigned j, k;

	for (j = 0; j < WIDTH; j++) {
		value = r->seed + j;
		coefficient = ((g->coefficient_seed + j) * UINT64_C(6364136223846793005)) | 1;
		for (k = 1; k <= r->rounds; k++)
			value = coefficient * value + k;
		check(value == r->values[j], "exact unsigned scalar reference");
	}
}

static struct outcome
run(int continuous, int policy)
{
	struct demo d = {0};
	struct outcome out = {0};
	struct group *g;
	struct request *r;
	xtc_loop_t *loop;
	xtc_exec_class_t cls;
	xtc_pid_t pid;
	xtc_batch_opts_t opts = { .size = sizeof(opts), .version = XTC_BATCH_VERSION,
	    .capacity = 3, .max_active = 2, .resource_capacity = 2,
	    .step = step, .complete = complete };
	xtc_batch_stats_t stats = { .size = sizeof(stats), .version = XTC_BATCH_VERSION };
	unsigned i, j, k;

	d.continuous = continuous;
	must(xtc_loop_init(&loop));
	if (policy)
		must(xtc_sched_continuous_set(loop, 3));
	must(xtc_proc_spawn(loop, heartbeat, &d, NULL, &pid));
	for (i = 0; i < GROUPS; i++) {
		g = &d.groups[i];
		g->coefficient_seed = i + 3;
		for (j = 0; j < REQUESTS; j++) {
			r = &g->requests[j];
			r->length = j == LONG ? 64 : j == SHORT ? 4 : 2;
			r->seed = UINT64_MAX - i * 16 - j * WIDTH;
			for (k = 0; k < WIDTH; k++)
				r->values[k] = r->seed + k;
		}
		/* Each group owns a distinct, explicitly supplied scheduling class. */
		must(xtc_exec_class_create(loop, 100, 0, &cls));
		opts.sched_class = cls;
		opts.user = g;
		must(xtc_batch_create(loop, &opts, &g->batch));
		r = g->requests;
		for (j = 0; j < LATE; j++)
			must(xtc_batch_submit(g->batch, &r[j], 1, &r[j].handle));
		check(xtc_batch_submit(g->batch, &r[LATE], 1, &r[LATE].handle) == XTC_E_AGAIN,
		    "full capacity rejects; caller retains request for retry");
		must(xtc_batch_cancel(g->batch, r[CANCEL].handle));
	}
	must(xtc_proc_spawn(loop, owner, &d, NULL, &pid));
	must(xtc_loop_run(loop)); /* Natural drain, not xtc_loop_stop. */
	for (i = 0; i < GROUPS; i++) {
		g = &d.groups[i];
		r = g->requests;
		for (j = 0; j < REQUESTS; j++) {
			check(r[j].terminals == 1, "every accepted request terminates once");
			check(r[j].outcome == (j == CANCEL ? XTC_E_ABORTED : XTC_OK),
			    "expected terminal status");
			check(r[j].rounds == (j == CANCEL ? 0 : r[j].length), "exact work count");
			reference(g, &r[j]);
			out.terminals++;
			out.exact++;
		}
		check(continuous ? r[LATE].order < r[LONG].order : r[LATE].order > r[LONG].order,
		    "each group's late short completion order");
		must(xtc_batch_get_stats(g->batch, &stats));
		check(stats.completed == 3 && stats.cancelled == 1 && stats.failed == 0 &&
		    stats.rejections == 1 && stats.max_vector == 2 &&
		    stats.queued == 0 && stats.ready == 0 && stats.blocked == 0 &&
		    stats.reserved_units == 0, "drained batch statistics");
		out.cancelled++;
		out.blocked += r[LONG].blocked && r[LONG].released;
		out.retried += r[LATE].handle != 0;
		out.updates += g->updates;
		must(xtc_batch_destroy(g->batch));
	}
	out.heartbeat = d.heartbeat;
	out.long_order = d.groups[0].requests[LONG].order;
	out.late_order = d.groups[0].requests[LATE].order;
	must(xtc_loop_fini(loop));
	printf("mode=%s policy=%s: terminals=%u updates=%u late=%u long=%u OK\n",
	    continuous ? "continuous" : "static", policy ? "continuous" : "default",
	    out.terminals, out.updates, out.late_order, out.long_order);
	return out;
}

static void
self_test(void)
{
	struct outcome o;
	int mode, policy;

	for (policy = 0; policy < 2; policy++) {
		for (mode = 0; mode < 2; mode++) {
			o = run(mode, policy);
			check(o.terminals == 8 && o.exact == 8, "terminal once / scalar reference");
			check(o.cancelled == 2 && o.blocked == 2 && o.retried == 2,
			    "cancellation, BLOCKED/resume, capacity retry");
			check(o.heartbeat > 0 && o.updates == 1120, "heartbeat / same arithmetic work");
			check(mode ? o.late_order < o.long_order : o.late_order > o.long_order,
			    "late short completion order");
		}
	}
	puts("continuous batching self-test: PASS (4 mode/policy combinations)");
}

int
main(int argc, char **argv)
{
	int i, continuous = 1, policy = 0, test = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0) {
			puts("usage: continuous_batching [--mode=static|continuous]\n"
			    "       [--policy=default|continuous] [--self-test] [--help]\n"
			    "Defaults: continuous mode, default policy. --self-test runs all four combinations.\n"
			    "Requires libxtc configured with --enable-sched-policy.");
			return 0;
		} else if (strcmp(argv[i], "--mode=static") == 0)
			continuous = 0;
		else if (strcmp(argv[i], "--mode=continuous") == 0)
			continuous = 1;
		else if (strcmp(argv[i], "--policy=default") == 0)
			policy = 0;
		else if (strcmp(argv[i], "--policy=continuous") == 0)
			policy = 1;
		else if (strcmp(argv[i], "--self-test") == 0)
			test = 1;
		else {
			fprintf(stderr, "unknown argument: %s (try --help)\n", argv[i]);
			return 2;
		}
	}
	if (test)
		self_test();
	else
		(void)run(continuous, policy);
	return 0;
}
