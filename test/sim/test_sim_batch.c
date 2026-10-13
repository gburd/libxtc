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
#include <stdatomic.h>
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
#include "xtc_future.h"
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

/* --------------------------------------------------------------------------
 * Async-offload (FUTURE) sim scenario. A model loop runs an async batch; a
 * producer loop resolves each outstanding future after a seeded virtual sleep.
 * Variable-length completion, one cancel-in-flight, and a cancel-close
 * teardown of the remainder. Everything is virtual-time + seeded RNG, so the
 * event trace replays byte-identically. FENCE is native-only (no sim fd
 * readiness); FUTURE is the sim-clean path. */
#define ANREQ 4
struct areq {
	uint64_t handle;
	unsigned id, left;
	int accepted, done_result, terminal, expect_abort;
};
struct afix {
	xtc_batch_t *batch;
	uint64_t seed;
	unsigned accepted, terminals, completed, cancelled;
	struct areq req[ANREQ];
	/* single outstanding op: the promise the producer resolves */
	xtc_promise_t *prom;
	xtc_future_t *fut;
	int cancel_target; /* id to cancel in flight once, else -1 */
	struct trace *trace;
};

static void
arequire(struct afix *f, int ok, const char *why)
{
	if (!ok) {
		fprintf(stderr, "FAIL: abatch seed=%" PRIu64 ": %s\n",
		    f->seed, why);
		exit(1);
	}
}

static void
aevent(struct afix *f, uint64_t k, uint64_t a, uint64_t b, uint64_t c)
{
	struct trace *t = f->trace;
	arequire(f, t->used + 4 <= TRACE_WORDS, "atrace capacity");
	t->words[t->used++] = k;
	t->words[t->used++] = a;
	t->words[t->used++] = b;
	t->words[t->used++] = c;
}

static int
asubmit(const xtc_batch_item_t *items, size_t n, void *user,
    xtc_batch_exec_t *exec)
{
	struct afix *f = user;
	struct areq *r;
	size_t i;
	arequire(f, n > 0 && n <= ACTIVE, "async vector cap");
	arequire(f, f->prom == NULL && f->fut == NULL, "one op at a time");
	if (xtc_future_new_pair(&f->prom, &f->fut) != XTC_OK)
		return XTC_E_NOMEM;
	exec->kind = XTC_BATCH_EXEC_FUTURE;
	exec->future = f->fut;
	for (i = 0; i < n; ++i) {
		r = items[i].request;
		r->done_result = (--r->left == 0);
		aevent(f, 100, r->id, r->left, (uint64_t)r->done_result);
	}
	return XTC_OK;
}

static int
acollect(const xtc_batch_item_t *items, xtc_batch_result_t *results, size_t n,
    int exec_status, void *user)
{
	struct afix *f = user;
	struct areq *r;
	size_t i;
	aevent(f, 101, n, (uint64_t)(int64_t)exec_status, 0);
	for (i = 0; i < n; ++i) {
		r = items[i].request;
		if (exec_status == XTC_BATCH_EXEC_OK && r->done_result)
			results[i].state = XTC_BATCH_DONE;
		else
			results[i].state = XTC_BATCH_CONTINUE;
	}
	/* The future is consumed by the engine; forget our handle to it. */
	f->fut = NULL;
	return XTC_OK;
}

static void
acomplete(uint64_t handle, void *request, int outcome, void *user)
{
	struct afix *f = user;
	struct areq *r = request;
	arequire(f, handle == r->handle, "async handle identity");
	arequire(f, !r->terminal, "async terminal twice");
	r->terminal = 1;
	f->terminals++;
	if (outcome == XTC_OK)
		f->completed++;
	else
		f->cancelled++;
	aevent(f, 102, r->id, handle, (uint64_t)(int64_t)outcome);
}

/* Producer: resolve each outstanding future after a seeded virtual delay,
 * cancelling one member in flight on the way. */
static void
aproducer(void *arg)
{
	struct afix *f = arg;
	int guard = 0;
	while (f->terminals < f->accepted) {
		arequire(f, ++guard < 100000, "async producer bound");
		if (f->prom != NULL) {
			int64_t d = 50 + (int64_t)((f->seed >> (guard % 13)) % 97);
			arequire(f, xtc_proc_sleep(d) == XTC_OK, "virtual delay");
			if (f->cancel_target >= 0) {
				if (!f->req[f->cancel_target].terminal) {
					arequire(f, xtc_batch_cancel(f->batch,
					    f->req[f->cancel_target].handle)
					    == XTC_OK, "async cancel");
					f->req[f->cancel_target].expect_abort = 1;
				}
				f->cancel_target = -1;
			}
			xtc_promise_set(f->prom, 0, XTC_OK);
			f->prom = NULL;
		} else
			xtc_yield();
	}
	arequire(f, xtc_batch_close(f->batch, XTC_BATCH_DRAIN) == XTC_OK,
	    "async drain close");
}

static void
arun(uint64_t seed, struct trace *trace)
{
	struct afix f = {0};
	xtc_batch_async_opts_t o = {0};
	xtc_exec_t *exec = NULL;
	xtc_pid_t pid;
	unsigned i;
	f.seed = seed;
	f.trace = trace;
	f.cancel_target = (int)(seed % ANREQ);
	trace->used = 0;
	for (i = 0; i < ANREQ; ++i) {
		f.req[i].id = i;
		f.req[i].left = 1 + (unsigned)((seed * (2 * i + 3) + i) % 4);
	}
	arequire(&f, xtc_exec_init(&exec, 2) == XTC_OK, "async exec init");
	o.size = sizeof(o);
	o.version = XTC_BATCH_VERSION;
	o.capacity = CAPACITY;
	o.max_active = ACTIVE;
	o.resource_capacity = RESOURCES;
	o.submit = asubmit;
	o.collect = acollect;
	o.complete = acomplete;
	o.user = &f;
	arequire(&f, xtc_batch_create_async(xtc_exec_loop(exec, 0), &o,
	    &f.batch) == XTC_OK, "async create");
	for (i = 0; i < ANREQ; ++i) {
		arequire(&f, xtc_batch_submit(f.batch, &f.req[i], 1,
		    &f.req[i].handle) == XTC_OK, "async submit");
		f.req[i].accepted = 1;
		f.accepted++;
	}
	/* Producer shares the batch loop: cancel/close are owner-loop
	 * mutations (cross-loop admission is S2). Promise resolution is
	 * thread-safe regardless. */
	arequire(&f, xtc_proc_spawn(xtc_exec_loop(exec, 0), aproducer, &f, NULL,
	    &pid) == XTC_OK, "async producer spawn");
	arequire(&f, xtc_sim_exec_run(exec, seed, STEPS) == XTC_OK,
	    "async step budget/determinism guard");
	arequire(&f, f.terminals == ANREQ && f.completed + f.cancelled == ANREQ,
	    "async terminal conservation");
	for (i = 0; i < ANREQ; ++i)
		arequire(&f, f.req[i].terminal, "async member finalized");
	arequire(&f, xtc_sim_check(exec) == XTC_OK, "async sim invariants");
	arequire(&f, xtc_batch_destroy(f.batch) == XTC_OK, "async destroy");
	arequire(&f, xtc_exec_fini(exec) == XTC_OK, "async exec fini");
}

/* --------------------------------------------------------------------------
 * Cross-loop admission (S2) sim scenario. A producer on a SIBLING loop feeds a
 * SYNC-step group on the model loop via submit_mt at seeded virtual times, and
 * cancel_mt's one member in-stream. req[0] is submitted first and its handle
 * handed (by message) to a canceller on the MODEL loop, a different producer:
 * its cancel_mt must abort req[0] before it ever runs, whether req[0] is
 * still in the ring, held behind a full group, or waiting in a slot. The
 * group's capacity is smaller than the hand-in burst, so held hand-ins are
 * exercised on every seed. The ring/free-pool/handle counter read no clock
 * and no unseeded RNG, so the event trace replays byte-identically. */
#define MNREQ 5
struct mreq {
	uint64_t handle;
	unsigned id, left, ran;
	int terminal, expect_abort, aborted;
};
struct mfix {
	xtc_batch_t *batch;
	xtc_pid_t canceller;
	uint64_t seed;
	unsigned accepted, terminals, completed, cancelled;
	struct mreq req[MNREQ];
	_Atomic int producer_done;
	struct trace *trace;
};

static void
mrequire(struct mfix *f, int ok, const char *why)
{
	if (!ok) {
		fprintf(stderr, "FAIL: mbatch seed=%" PRIu64 ": %s\n",
		    f->seed, why);
		exit(1);
	}
}

static void
mevent(struct mfix *f, uint64_t k, uint64_t a, uint64_t b, uint64_t c)
{
	struct trace *t = f->trace;
	mrequire(f, t->used + 4 <= TRACE_WORDS, "mtrace capacity");
	t->words[t->used++] = k;
	t->words[t->used++] = a;
	t->words[t->used++] = b;
	t->words[t->used++] = c;
}

static int
msubmit(const xtc_batch_item_t *items, size_t n, void *user,
    xtc_batch_exec_t *exec)
{
	struct mfix *f = user;
	struct mreq *r;
	size_t i;
	exec->kind = XTC_BATCH_EXEC_SYNC;
	for (i = 0; i < n; ++i) {
		r = items[i].request;
		r->ran++;
		if (r->left > 0)
			r->left--;
		mevent(f, 200, r->id, r->left, 0);
	}
	return XTC_OK;
}

static int
mcollect(const xtc_batch_item_t *items, xtc_batch_result_t *results, size_t n,
    int exec_status, void *user)
{
	struct mfix *f = user;
	struct mreq *r;
	size_t i;
	(void)exec_status;
	(void)f;
	for (i = 0; i < n; ++i) {
		r = items[i].request;
		results[i].state = r->left == 0 ? XTC_BATCH_DONE :
		                                 XTC_BATCH_CONTINUE;
	}
	return XTC_OK;
}

static void
mcomplete(uint64_t handle, void *request, int outcome, void *user)
{
	struct mfix *f = user;
	struct mreq *r = request;
	mrequire(f, !r->terminal, "mt terminal twice");
	(void)handle;
	r->terminal = 1;
	r->aborted = outcome == XTC_E_ABORTED;
	f->terminals++;
	if (outcome == XTC_OK)
		f->completed++;
	else
		f->cancelled++;
	mevent(f, 202, r->id, (uint64_t)(int64_t)outcome, 0);
}

/* Producer on a sibling loop: submit_mt each request after a seeded virtual
 * delay, cancel_mt one member mid-stream. The engine stamps the handle, so a
 * handle is known immediately; the member joins at the next boundary. */
static void
mproducer(void *arg)
{
	struct mfix *f = arg;
	unsigned i;
	/* req[0] is the cross-producer case: its handle goes to the canceller
	 * on the model loop. Pick the in-stream cancel from the rest so it
	 * never collides with req[0]. */
	int cancel_id = 1 + (int)(f->seed % (MNREQ - 1));
	for (i = 0; i < MNREQ; ++i) {
		int64_t d = 40 + (int64_t)((f->seed >> (i % 11)) % 73);
		uint64_t h = 0;
		int rc;
		/* Odd seeds: burst the first hand-ins back to back so they
		 * queue behind the full group (held). */
		if (i > 0 && (i > 2 || !(f->seed & 1)))
			mrequire(f, xtc_proc_sleep(d) == XTC_OK,
			    "mt virtual delay");
		for (;;) {
			rc = xtc_batch_submit_mt(f->batch, &f->req[i], 1, &h);
			if (rc == XTC_OK)
				break;
			mrequire(f, rc == XTC_E_AGAIN, "mt submit rc");
			xtc_yield();
		}
		f->req[i].handle = h;
		f->accepted++;
		mevent(f, 201, i, h, 0);
		if (i == 0)
			mrequire(f, xtc_send(f->canceller, &h, sizeof h) == XTC_OK,
			    "mt handle to canceller");
		if ((int)i == cancel_id) {
			mrequire(f, xtc_batch_cancel_mt(f->batch, h) == XTC_OK,
			    "mt cancel");
			f->req[i].expect_abort = 1;
		}
	}
	(void)atomic_store_explicit(&f->producer_done, 1,
	    memory_order_relaxed);
}

/* Canceller on the model loop: a different producer than the submitter. */
static void
mcanceller(void *arg)
{
	struct mfix *f = arg;
	void *msg = NULL;
	size_t size = 0;
	uint64_t h;
	mrequire(f, xtc_recv(&msg, &size, -1) == XTC_OK && size == sizeof h,
	    "mt canceller receive");
	memcpy(&h, msg, sizeof h);
	xtc_free(msg);
	mrequire(f, xtc_batch_cancel_mt(f->batch, h) == XTC_OK,
	    "mt cross-producer cancel");
	mevent(f, 203, 0, h, 0);
}

/* Controller on the model loop: wait for every member to terminate then close
 * so the driver exits. */
static void
mcontroller(void *arg)
{
	struct mfix *f = arg;
	unsigned ticks = 0;
	while (f->terminals < MNREQ
	    || !atomic_load_explicit(&f->producer_done, memory_order_relaxed)) {
		/* A clean run needs a few dozen ticks; an accepted hand-in that
		 * never completes keeps this loop going forever. */
		mrequire(f, ++ticks <= 1000,
		    "mt accepted hand-in never completed");
		mrequire(f, xtc_proc_sleep(50) == XTC_OK, "mt controller tick");
	}
	mrequire(f, xtc_batch_close(f->batch, XTC_BATCH_DRAIN) == XTC_OK,
	    "mt drain close");
}

static void
mrun(uint64_t seed, struct trace *trace)
{
	struct mfix f = {0};
	xtc_batch_async_opts_t o = {0};
	xtc_exec_t *exec = NULL;
	xtc_pid_t pid;
	unsigned i;
	f.seed = seed;
	f.trace = trace;
	trace->used = 0;
	for (i = 0; i < MNREQ; ++i) {
		f.req[i].id = i;
		f.req[i].left = 1 + (unsigned)((seed * (2 * i + 1) + i) % 3);
	}
	mrequire(&f, xtc_exec_init(&exec, 2) == XTC_OK, "mt exec init");
	o.size = sizeof(o);
	o.version = XTC_BATCH_VERSION;
	o.capacity = ACTIVE;
	o.max_active = ACTIVE;
	o.resource_capacity = RESOURCES + 2;
	o.mt_queue_cap = 8;
	o.submit = msubmit;
	o.collect = mcollect;
	o.complete = mcomplete;
	o.user = &f;
	mrequire(&f, xtc_batch_create_async(xtc_exec_loop(exec, 0), &o,
	    &f.batch) == XTC_OK, "mt create");
	f.req[0].expect_abort = 1;
	/* req[0] blocks once it runs, so it can only finish by the cancel. */
	f.req[0].left = 1000000;
	mrequire(&f, xtc_proc_spawn(xtc_exec_loop(exec, 0), mcanceller, &f,
	    NULL, &f.canceller) == XTC_OK, "mt canceller spawn");
	mrequire(&f, xtc_proc_spawn(xtc_exec_loop(exec, 1), mproducer, &f, NULL,
	    &pid) == XTC_OK, "mt producer spawn");
	mrequire(&f, xtc_proc_spawn(xtc_exec_loop(exec, 0), mcontroller, &f,
	    NULL, &pid) == XTC_OK, "mt controller spawn");
	mrequire(&f, xtc_sim_exec_run(exec, seed, STEPS) == XTC_OK,
	    "mt step budget/determinism guard");
	mrequire(&f, f.terminals == MNREQ, "mt terminal conservation");
	mrequire(&f, f.completed + f.cancelled == MNREQ, "mt terminal split");
	/* req[0] was cancelled by another producer; it must end ABORTED. */
	mrequire(&f, f.req[0].terminal && f.req[0].aborted,
	    "mt cross-producer cancel lost");
	for (i = 0; i < MNREQ; ++i)
		mrequire(&f, f.req[i].terminal, "mt member finalized");
	mrequire(&f, xtc_sim_check(exec) == XTC_OK, "mt sim invariants");
	mrequire(&f, xtc_batch_destroy(f.batch) == XTC_OK, "mt destroy");
	mrequire(&f, xtc_exec_fini(exec) == XTC_OK, "mt exec fini");
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
	static struct trace afirst, areplay;
	static struct trace mfirst, mreplay;
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
		arun(base + i, &afirst);
		arun(base + i, &areplay);
		if (afirst.used != areplay.used || memcmp(afirst.words,
		    areplay.words,
		    afirst.used * sizeof(afirst.words[0])) != 0) {
			fprintf(stderr, "FAIL: abatch seed=%" PRIu64
			    " event replay differs\n", base + i);
			return 1;
		}
		mrun(base + i, &mfirst);
		mrun(base + i, &mreplay);
		if (mfirst.used != mreplay.used || memcmp(mfirst.words,
		    mreplay.words,
		    mfirst.used * sizeof(mfirst.words[0])) != 0) {
			fprintf(stderr, "FAIL: mbatch seed=%" PRIu64
			    " event replay differs\n", base + i);
			return 1;
		}
	}
	printf("OK: batch seeds=%" PRIu64 "..%" PRIu64
	    " replayed twice; real driver + async offload (FUTURE) + "
	    "cross-loop admission (mt), two-loop, bounded progress\n",
	    base, base + count - 1);
	return 0;
}
#endif
