/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * Real-driver DST: default picker, two loops, owner-routed command messages.
 * Each seed scripts FCFS reservation pressure, variable lengths, capacity
 * rejection, block/resume, cancellation, slot reuse and shutdown. No shadow
 * batch scheduler: the oracle counts application borrows and vector members.
 * Liveness is bounded by scheduler steps and owner turns, never wall time.
 * argv: <base seed> <count>; defaults to seeds 1..64, each replayed twice.
 */
#include "xtc_config.h"
#include "xtc.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef XTC_ENABLE_SCHED_POLICY
int
main(void)
{
	puts("SKIP: test_sim_batch requires --enable-sched-policy");
	return 0;
}
#else
#include "xtc_async.h"
#include "xtc_batch.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

#define NREQ 12
#define CAPACITY 4
#define ACTIVE 2
#define RESOURCES 5
#define TURNS 1024
#define STEPS 20000
#define TRACE_WORDS 32768

struct request {
	uint64_t handle, charge;
	unsigned id, left, calls;
	int accepted, active, blocked, block_once, terminal, done_result;
	int expected;
};
struct trace {
	uint64_t words[TRACE_WORDS];
	size_t used;
};
struct fixture {
	xtc_batch_t *batch;
	xtc_pid_t owner, sender;
	uint64_t seed;
	unsigned phase, ticks, accepted, terminals, completed, cancelled;
	struct request req[NREQ];
	struct trace *trace;
};

static void
require(struct fixture *f, int ok, const char *why)
{
	if (!ok) {
		fprintf(stderr, "FAIL: batch seed=%" PRIu64 " phase=%u tick=%u: %s\n",
		    f->seed, f->phase, f->ticks, why);
		exit(1);
	}
}

static void
event(struct fixture *f, uint64_t kind, uint64_t id, uint64_t a, uint64_t b)
{
	struct trace *t = f->trace;
	require(f, t->used + 4 <= TRACE_WORDS, "trace capacity");
	t->words[t->used++] = kind;
	t->words[t->used++] = id;
	t->words[t->used++] = a;
	t->words[t->used++] = b;
}

/* Only at committed boundaries, not within terminal delivery accounting. */
static void
check(struct fixture *f)
{
	xtc_batch_stats_t s = {0};
	uint64_t reserved = 0;
	unsigned i, active = 0, blocked = 0, queued = 0;
	s.size = sizeof(s);
	s.version = XTC_BATCH_VERSION;
	require(f, xtc_batch_get_stats(f->batch, &s) == XTC_OK, "stats");
	for (i = 0; i < NREQ; ++i) {
		struct request *r = &f->req[i];
		if (!r->accepted || r->terminal)
			continue;
		if (r->active) {
			active++;
			reserved += r->charge;
			blocked += r->blocked != 0;
		} else
			queued++;
	}
	require(f, active <= ACTIVE && s.ready + s.blocked <= ACTIVE,
	    "active cap exceeded");
	require(f, reserved <= RESOURCES && s.reserved_units == reserved,
	    "reservation conservation");
	require(f, s.queued == queued && s.blocked == blocked &&
	    s.ready == active - blocked, "membership conservation");
	require(f, f->accepted == f->terminals + queued + active &&
	    queued + active <= CAPACITY, "borrow conservation/capacity");
	require(f, s.completed == f->completed && s.cancelled == f->cancelled &&
	    s.failed == 0, "terminal stats conservation");
	require(f, s.max_vector <= ACTIVE, "vector cap exceeded");
	event(f, 1, s.queued, s.ready, s.blocked);
	event(f, 2, s.reserved_units, s.completed, s.cancelled);
}

static int
step(const xtc_batch_item_t *items, xtc_batch_result_t *results, size_t n,
    void *arg)
{
	struct fixture *f = arg;
	struct request *r;
	size_t i, j;
	require(f, n > 0 && n <= ACTIVE, "vector cap exceeded");
	for (i = 0; i < n; ++i) {
		r = items[i].request;
		for (j = 0; j < NREQ && r != &f->req[j]; ++j)
			;
		require(f, j < NREQ, "foreign request");
		require(f, r->accepted && !r->terminal && !r->done_result,
		    "completed member retained in vector");
		require(f, r->handle == items[i].handle, "vector handle identity");
		require(f, !r->blocked && r->expected == XTC_OK,
		    "blocked/cancelled member in vector");
		for (j = 0; j < i; ++j)
			require(f, items[j].handle != items[i].handle, "duplicate member");
		r->active = 1;
	}
	check(f);
	event(f, 3, f->phase, n, 0);
	for (i = 0; i < n; ++i) {
		r = items[i].request;
		r->calls++;
		if (r->block_once) {
			r->block_once = 0;
			r->blocked = 1;
			results[i].state = XTC_BATCH_BLOCKED;
		} else {
			require(f, r->left != 0, "request over-executed");
			results[i].state = --r->left == 0 ?
			    XTC_BATCH_DONE : XTC_BATCH_CONTINUE;
			r->done_result = r->left == 0;
		}
		event(f, 4, r->id, results[i].state, r->left);
	}
	return XTC_OK;
}

static void
complete(uint64_t handle, void *request, int outcome, void *arg)
{
	struct fixture *f = arg;
	struct request *r = request;
	unsigned i;
	for (i = 0; i < NREQ && r != &f->req[i]; ++i)
		;
	require(f, i < NREQ, "foreign terminal request");
	require(f, r->accepted && !r->terminal, "terminal delivered twice");
	require(f, handle == r->handle, "terminal handle identity");
	require(f, outcome == r->expected, "terminal outcome");
	require(f, outcome != XTC_OK || (r->done_result && r->left == 0),
	    "premature completion");
	r->terminal = 1;
	f->terminals++;
	if (outcome == XTC_OK)
		f->completed++;
	else
		f->cancelled++;
	event(f, 5, r->id, handle, (uint64_t)(int64_t)outcome);
}

static void
submit(struct fixture *f, unsigned id, unsigned charge, int block)
{
	struct request *r = &f->req[id];
	r->charge = charge;
	r->block_once = block;
	require(f, xtc_batch_submit(f->batch, r, charge, &r->handle) == XTC_OK,
	    "submission");
	require(f, r->handle != 0, "zero handle");
	r->accepted = 1;
	f->accepted++;
	event(f, 6, id, r->handle, charge);
}

static void
cancel(struct fixture *f, unsigned id)
{
	struct request *r = &f->req[id];
	r->expected = XTC_E_ABORTED;
	require(f, xtc_batch_cancel(f->batch, r->handle) == XTC_OK, "cancel");
	event(f, 7, id, r->handle, 0);
}

static void
resume(struct fixture *f, unsigned id)
{
	struct request *r = &f->req[id];
	require(f, r->blocked && !r->terminal, "resume precondition");
	require(f, xtc_batch_resume(f->batch, r->handle) == XTC_OK, "resume");
	r->blocked = 0;
	require(f, xtc_batch_resume(f->batch, r->handle) == XTC_OK,
	    "duplicate ready resume");
	event(f, 8, id, r->handle, 0);
}

static void
tick(struct fixture *f)
{
	require(f, ++f->ticks <= TURNS, "logical turn bound (lost wake/progress)");
	xtc_yield();
	check(f);
}

static void
wait_terminal(struct fixture *f, unsigned total)
{
	while (f->terminals != total)
		tick(f);
	check(f);
}

static void
full(struct fixture *f)
{
	uint64_t h = 0;
	require(f, xtc_batch_submit(f->batch, &f->req[11], 1, &h) == XTC_E_AGAIN,
	    "full admission accepted");
	require(f, h == 0, "rejected submission changed handle");
	event(f, 9, f->phase, 0, 0);
}

static unsigned
receive(struct fixture *f)
{
	void *msg = NULL;
	size_t size = 0;
	unsigned command;
	require(f, xtc_recv(&msg, &size, -1) == XTC_OK &&
	    size == sizeof(command), "command receive");
	memcpy(&command, msg, sizeof(command));
	xtc_free(msg);
	return command;
}

static void
owner(void *arg)
{
	struct fixture *f = arg;
	unsigned p, i;
	uint64_t h = 0;
	for (p = 1; p <= 3; ++p) {
		require(f, receive(f) == p, "command order");
		f->phase = p;
		event(f, 10, p, 0, 0);
		if (p == 1) {
			while (!f->req[0].blocked || !f->req[3].terminal)
				tick(f);
			/* 3,3,2 FCFS: the cheap third request cannot bypass second. */
			for (i = 0; i < 4; ++i)
				tick(f);
			require(f, f->req[1].calls == 0 && f->req[2].calls == 0,
			    "FCFS reservation bypass");
			resume(f, 0);
			wait_terminal(f, 4);
		} else if (p == 2) {
			/* With no live work, allow the real driver to park. */
			require(f, xtc_proc_sleep(1000 + (int64_t)(f->seed % 17)) == XTC_OK,
			    "virtual empty-park timer");
			submit(f, 4, 3, 1);
			submit(f, 5, 2, 1);
			submit(f, 6, 1, 0);
			submit(f, 7, 1, 1);
			full(f);
			/* The old slot has been reused; old identity must not touch it. */
			require(f, xtc_batch_cancel(f->batch, f->req[0].handle) == XTC_E_INVAL,
			    "stale handle cancelled successor");
			require(f, xtc_batch_resume(f->batch, f->req[0].handle) == XTC_E_INVAL,
			    "stale handle resumed successor");
			while (!f->req[4].blocked || !f->req[5].blocked)
				tick(f);
			require(f, f->req[6].calls == 0 && f->req[7].calls == 0,
			    "blocked members did not retain active cap");
			cancel(f, 6);
			cancel(f, 4);
			resume(f, 5);
			while (!f->req[7].blocked)
				tick(f);
			resume(f, 7);
			wait_terminal(f, 8);
			require(f, f->req[6].calls == 0, "cancelled waiting member ran");
		} else {
			submit(f, 8, 2, 1);
			submit(f, 9, 2, 1);
			while (!f->req[8].blocked || !f->req[9].blocked)
				tick(f);
			/* There is resource space but NO active slot. Submit only after
			 * both block: a cap mutant is caught without vector overflow. */
			submit(f, 10, 1, 0);
			require(f, xtc_batch_close(f->batch, XTC_BATCH_DRAIN) == XTC_OK,
			    "drain close");
			for (i = 0; i < 4; ++i)
				tick(f);
			require(f, f->terminals == 8 && f->req[10].calls == 0,
			    "DRAIN discarded blocked work/active cap");
			for (i = 8; i <= 10; ++i)
				f->req[i].expected = XTC_E_ABORTED;
			require(f, xtc_batch_close(f->batch, XTC_BATCH_CANCEL) == XTC_OK &&
			    xtc_batch_close(f->batch, XTC_BATCH_DRAIN) == XTC_OK,
			    "cancel escalation/idempotence");
			require(f, xtc_batch_submit(f->batch, &f->req[11], 1, &h) == XTC_E_AGAIN,
			    "closed admission accepted");
			wait_terminal(f, 11);
		}
		require(f, xtc_send(f->sender, &p, sizeof(p)) == XTC_OK, "ack send");
	}
}

static void
sender(void *arg)
{
	struct fixture *f = arg;
	unsigned p;
	for (p = 1; p <= 3; ++p) {
		require(f, xtc_proc_sleep(100 + (int64_t)((f->seed >> p) % 101)) == XTC_OK,
		    "virtual command timer");
		require(f, xtc_send(f->owner, &p, sizeof(p)) == XTC_OK, "command send");
		require(f, receive(f) == p, "ack order");
	}
}

static void
run(uint64_t seed, struct trace *trace)
{
	struct fixture f = {0};
	xtc_batch_opts_t opts = {0};
	xtc_exec_t *exec = NULL;
	unsigned i;
	f.seed = seed;
	f.trace = trace;
	trace->used = 0;
	for (i = 0; i < NREQ; ++i) {
		f.req[i].id = i;
		f.req[i].left = 1 + (unsigned)((seed * (2 * i + 1) + i) % 7);
	}
	require(&f, xtc_exec_init(&exec, 2) == XTC_OK, "exec init");
	opts.size = sizeof(opts);
	opts.version = XTC_BATCH_VERSION;
	opts.capacity = CAPACITY;
	opts.max_active = ACTIVE;
	opts.resource_capacity = RESOURCES;
	opts.step = step;
	opts.complete = complete;
	opts.user = &f;
	require(&f, xtc_batch_create(xtc_exec_loop(exec, 0), &opts, &f.batch) == XTC_OK,
	    "batch create (enabled run mandatory)");
	submit(&f, 0, 3, 1);
	submit(&f, 1, 3, 0);
	submit(&f, 2, 2, 0);
	submit(&f, 3, 1, 0);
	full(&f);
	cancel(&f, 3);
	require(&f, xtc_proc_spawn(xtc_exec_loop(exec, 0), owner, &f, NULL,
	    &f.owner) == XTC_OK, "owner spawn");
	require(&f, xtc_proc_spawn(xtc_exec_loop(exec, 1), sender, &f, NULL,
	    &f.sender) == XTC_OK, "sender spawn");
	require(&f, xtc_sim_exec_run(exec, seed, STEPS) == XTC_OK,
	    "logical scheduler step budget/determinism guard");
	check(&f);
	require(&f, f.phase == 3 && f.accepted == 11 && f.terminals == 11 &&
	    f.completed == 5 && f.cancelled == 6, "final terminal conservation");
	require(&f, xtc_sim_check(exec) == XTC_OK, "sim structural invariants");
	require(&f, xtc_batch_destroy(f.batch) == XTC_OK, "driver did not exit");
	require(&f, xtc_exec_fini(exec) == XTC_OK, "exec fini");
}

static uint64_t
number(const char *text)
{
	char *end;
	unsigned long long n;
	errno = 0;
	n = strtoull(text, &end, 0);
	if (errno || *text == '-' || *text == '\0' || *end != '\0') {
		fprintf(stderr, "invalid seed/count: %s\n", text);
		exit(2);
	}
	return (uint64_t)n;
}

int
main(int argc, char **argv)
{
	static struct trace first, replay;
	uint64_t base = 1, count = 64, i;
	if (argc > 3) {
		fprintf(stderr, "usage: %s [base-seed [count]]\n", argv[0]);
		return 2;
	}
	if (argc > 1)
		base = number(argv[1]);
	if (argc > 2)
		count = number(argv[2]);
	if (count == 0 || count - 1 > UINT64_MAX - base)
		return 2;
	for (i = 0; i < count; ++i) {
		run(base + i, &first);
		run(base + i, &replay);
		if (first.used != replay.used || memcmp(first.words, replay.words,
		    first.used * sizeof(first.words[0])) != 0) {
			fprintf(stderr, "FAIL: batch seed=%" PRIu64 " event replay differs\n",
			    base + i);
			return 1;
		}
	}
	printf("OK: batch seeds=%" PRIu64 "..%" PRIu64
	    " replayed twice; real driver, two-loop commands, bounded progress\n",
	    base, base + count - 1);
	return 0;
}
#endif
