/*
 * test_sim_external -- DST of external completions
 * (xtc_sim_external_begin / xtc_sim_external_complete): a completion a
 * plain OS thread produces is delivered at a seeded virtual time.
 *
 * Written against the public xtc_sim.h only.  The device worker is a real
 * pthread, as in an inference server whose model compute must not run on
 * a fiber; its code is the production code (it calls
 * xtc_sim_external_complete, which outside a run delivers at once).  It
 * sleeps a few real milliseconds per job, so it is slower than the
 * simulation and runs on the kernel's schedule.
 *
 * Per seed, on a 3-loop executor, an engine proc submits N_JOBS jobs:
 * for each it calls xtc_sim_external_begin (lane 0 for even jobs, a FIFO
 * device queue; -1 for odd ones) and hands the token to the worker
 * through a mutex-protected queue.  The worker completes each with a
 * delivery function that xtc_sends the job id to the engine, from the
 * simulator's thread, outside any proc, and records the virtual time
 * of the delivery.  The engine receives every completion.
 *
 * Properties: every job is delivered exactly once, each no later than
 * its begin time + the maximum delay, lane-0 jobs in submission order,
 * and two runs of one seed record identical arrival times and orders
 * (the worker's real timing does not leak into the run).
 *
 * Full mailbox (2.3.0): a delivery fn that meets a full mailbox.  The
 * engine's mailbox holds one message and it sleeps through four
 * deliveries; each delivery fn calls xtc_send_wait, once with a timeout
 * and once with -1.  The receiver cannot run while the sim thread waits,
 * so the wait must return at once, XTC_E_STATE, without reading the wall
 * clock (the determinism guard is in count mode for this case and must
 * count nothing) and without blocking the run (a watchdog thread reports
 * a run still going after 10 s of real time).  The first delivery also
 * tries a timed xtc_notify_wait and xtc_future_wait (they expire at
 * once), xtc_reg_whereis_wait and an off-proc xtc_svr_call (XTC_E_STATE).
 *
 * Planted bugs (src/inc/xtc_dst_inject.h) this test must catch:
 *   52 EXT_NOWAIT  the due event does not wait for complete, so a job
 *                  the worker has not finished is never delivered.
 *   53 EXT_LANE    the lane is ignored, so lane 0 delivers out of order.
 *   68 SIM_OFFLOOP_WAIT  the delivery fn's xtc_send_wait reads the wall
 *                  clock or blocks the run.
 *
 * Usage: test_sim_external [base [count]]
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_atomic.h"
#include "xtc_exec.h"
#include "xtc_io.h"
#include "xtc_future.h"
#include "xtc_proc.h"
#include "xtc_reg.h"
#include "xtc_sim.h"
#include "xtc_svr.h"
#include "xtc_sync.h"

#define N_LOOPS 3
#define N_JOBS 8
#define MS (1000LL * 1000)
#define MAX_DLY (5 * MS)

/* The worker's queue (production shape: a locked FIFO of tokens). */
struct wq {
	pthread_mutex_t lock;
	pthread_cond_t cv;
	xtc_sim_ext_t *tok[N_JOBS];
	int id[N_JOBS];
	int head, tail, stop;
};

struct run {
	xtc_exec_t *e;
	xtc_pid_t engine;
	int64_t begun[N_JOBS];
	int64_t arrived[N_JOBS]; /* virtual time of the delivery */
	int seen[N_JOBS];        /* receipts by the engine */
	int order[N_JOBS];       /* delivery order */
	int n_delivered, n_seen;
	int bad_begin;
};

static struct wq g_q = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER,
	{ 0 }, { 0 }, 0, 0, 0 };
static struct run g_r;
static int g_deliver_id[N_JOBS];

/* The delivery: runs on the simulator's thread at the due time (or on
 * the worker, outside a run).  A send from outside any proc. */
static void
deliver(void *arg)
{
	int id = *(int *)arg;
	g_r.arrived[id] = xtc_clock_mono();
	g_r.order[g_r.n_delivered++] = id;
	(void)xtc_send(g_r.engine, &id, sizeof id);
}

/* The device worker: production code.  Real time passes per job. */
static void *
worker(void *arg)
{
	struct timespec ts = { 0, 3 * 1000 * 1000 };
	(void)arg;
	for (;;) {
		xtc_sim_ext_t *t;
		int id;
		(void)pthread_mutex_lock(&g_q.lock);
		while (g_q.head == g_q.tail && !g_q.stop)
			(void)pthread_cond_wait(&g_q.cv, &g_q.lock);
		if (g_q.head == g_q.tail) {
			(void)pthread_mutex_unlock(&g_q.lock);
			return NULL;
		}
		t = g_q.tok[g_q.head % N_JOBS];
		id = g_q.id[g_q.head % N_JOBS];
		g_q.head++;
		(void)pthread_mutex_unlock(&g_q.lock);
		(void)nanosleep(&ts, NULL); /* the compute */
		g_deliver_id[id] = id;
		(void)xtc_sim_external_complete(t, deliver, &g_deliver_id[id]);
	}
}

static void
engine(void *arg)
{
	struct run *r = arg;
	int k;
	for (k = 0; k < N_JOBS; k++) {
		xtc_sim_ext_t *t = NULL;
		if (xtc_sim_external_begin(k % 2 == 0 ? 0 : -1, MAX_DLY, &t) !=
		        XTC_OK ||
		    t == NULL)
			r->bad_begin++;
		r->begun[k] = xtc_clock_mono();
		(void)pthread_mutex_lock(&g_q.lock);
		g_q.tok[g_q.tail % N_JOBS] = t;
		g_q.id[g_q.tail % N_JOBS] = k;
		g_q.tail++;
		(void)pthread_cond_signal(&g_q.cv);
		(void)pthread_mutex_unlock(&g_q.lock);
		(void)xtc_proc_sleep((int64_t)xtc_sim_rand(2 * MS));
	}
	while (r->n_seen < N_JOBS) {
		void *m = NULL;
		size_t n = 0;
		if (xtc_recv(&m, &n, 100 * MS) != XTC_OK)
			break;
		if (n == sizeof(int)) {
			int id = *(int *)m;
			if (id >= 0 && id < N_JOBS)
				r->seen[id]++;
		}
		r->n_seen++;
		xtc_free(m);
	}
}

static const char *
check(const struct run *r)
{
	int k, last = -1, a;
	if (r->bad_begin != 0)
		return "xtc_sim_external_begin failed under the run";
	for (k = 0; k < N_JOBS; k++)
		if (r->seen[k] != 1)
			return "an external completion was not delivered";
	for (k = 0; k < N_JOBS; k++)
		if (r->arrived[k] < r->begun[k] ||
		    r->arrived[k] > r->begun[k] + MAX_DLY)
			return "a completion arrived outside its delay window";
	for (a = 0; a < N_JOBS; a++) {
		int id = r->order[a];
		if (id % 2 != 0)
			continue;
		if (id < last)
			return "a lane delivered out of order";
		last = id;
	}
	return NULL;
}

static uint64_t
digest(const struct run *r, uint64_t state)
{
	uint64_t h = 0xcbf29ce484222325ull ^ state;
	int k;
	for (k = 0; k < N_JOBS; k++) {
		h = (h ^ (uint64_t)r->arrived[k]) * 0x100000001B3ull;
		h = (h ^ (uint64_t)r->order[k]) * 0x100000001B3ull;
	}
	return h;
}

static int
run_one(uint64_t seed, uint64_t *out)
{
	struct run *r = &g_r;
	pthread_t th;
	const char *why;
	int rc;

	memset(r, 0, sizeof *r);
	g_q.head = g_q.tail = g_q.stop = 0;
	if (pthread_create(&th, NULL, worker, NULL) != 0)
		return -1;
	if (xtc_exec_create(&(xtc_exec_opts_t){
	    .size = sizeof(xtc_exec_opts_t), .n_loops = N_LOOPS }, &r->e) != XTC_OK)
		return -1;
	(void)xtc_proc_spawn(xtc_exec_loop(r->e,
	                         (int)(seed % (uint64_t)N_LOOPS)),
	    engine, r, NULL, &r->engine);
	rc = xtc_sim_exec_run(r->e, seed, 2000000);
	*out = digest(r, xtc_sim_state_hash(r->e));
	(void)pthread_mutex_lock(&g_q.lock);
	g_q.stop = 1;
	(void)pthread_cond_broadcast(&g_q.cv);
	(void)pthread_mutex_unlock(&g_q.lock);
	(void)pthread_join(th, NULL);
	xtc_exec_destroy(r->e);
	why = check(r);
	if (why == NULL && rc != XTC_OK)
		why = "the run did not quiesce";
	if (why == NULL)
		return 0;
	printf("  seed 0x%016llx: FAIL %s (rc=%d seen=%d)\n",
	    (unsigned long long)seed, why, rc, r->n_seen);
	return 1;
}

/* ----- a delivery fn that meets a full mailbox ----------------------- */

#define N_FULL 4

static struct {
	xtc_pid_t rx;
	xtc_sim_ext_t *tok[N_FULL];
	int id[N_FULL];
	int rc[N_FULL];
	int64_t timeout_ns;
	int got;
	int other_bad;
	int running; /* the run has not returned (the watchdog reads it) */
} g_f;

/* The other waits a delivery fn could try: none can be ended by the
 * run, so each must return at once.  A timed notify and future wait
 * expire (XTC_E_AGAIN); a name wait and an off-proc server call are
 * XTC_E_STATE.  Run in the first delivery; any other result is bad. */
static int
full_other_waits(void)
{
	xtc_notify_t *n = NULL;
	xtc_promise_t *pr = NULL;
	xtc_future_t *fu = NULL;
	xtc_reg_t *reg = NULL;
	xtc_pid_t pid;
	void *reply = NULL;
	size_t rsize = 0;
	intptr_t v = 0;
	int bad = 0;

	if (xtc_notify_create(&n) != XTC_OK ||
	    xtc_future_create(&pr, &fu) != XTC_OK ||
	    xtc_reg_create(&reg) != XTC_OK)
		return 1;
	bad |= xtc_notify_wait(n, 50 * MS) != XTC_E_AGAIN;
	bad |= xtc_future_wait(fu, &v, 50 * MS) != XTC_E_AGAIN;
	bad |= xtc_reg_whereis_wait(reg, "nobody", XTC_PID_NONE, -1, &pid) !=
	    XTC_E_STATE;
	bad |= xtc_svr_call(g_f.rx, "q", 1, &reply, &rsize, -1) != XTC_E_STATE;
	xtc_promise_set(pr, 0, XTC_OK);
	(void)xtc_future_wait(fu, &v, -1);
	xtc_reg_destroy(reg);
	xtc_notify_destroy(n);
	return bad;
}

static void
full_deliver(void *arg)
{
	int id = *(int *)arg;
	if (id == 0)
		g_f.other_bad = full_other_waits();
	g_f.rc[id] = xtc_send_wait(g_f.rx, &id, sizeof id, g_f.timeout_ns);
}

/* The device: completes each token once the engine has begun it. */
static void *
full_dev(void *arg)
{
	struct timespec ts = { 0, 100 * 1000 };
	int i;
	(void)arg;
	for (i = 0; i < N_FULL; i++) {
		while (xtc_atomic_load_ptr(&g_f.tok[i], xtc_mo_acquire) == NULL)
			(void)nanosleep(&ts, NULL);
		(void)xtc_sim_external_complete(g_f.tok[i], full_deliver,
		    &g_f.id[i]);
	}
	return NULL;
}

/* The engine: mailbox_cap 1, begins N_FULL completions due within 1 ms,
 * then sleeps 20 ms so they all meet a full mailbox, then drains. */
static void
full_rx(void *arg)
{
	xtc_sim_ext_t *t;
	void *m;
	size_t n;
	int i;
	(void)arg;
	g_f.rx = xtc_self();
	for (i = 0; i < N_FULL; i++) {
		t = NULL;
		(void)xtc_sim_external_begin(0, 1 * MS, &t);
		xtc_atomic_store_ptr(&g_f.tok[i], t, xtc_mo_release);
	}
	(void)xtc_proc_sleep(20 * MS);
	while (xtc_recv(&m, &n, 50 * MS) == XTC_OK) {
		g_f.got++;
		xtc_free(m);
	}
}

/* Turns a blocked run into a reported failure: 10 s of real time is far
 * past a run this size (it takes milliseconds). */
static void *
full_watchdog(void *arg)
{
	struct timespec ts = { 0, 10 * 1000 * 1000 };
	int i;
	(void)arg;
	for (i = 0; i < 1000; i++) {
		if (!xtc_atomic_load_i32(&g_f.running, xtc_mo_acquire))
			return NULL;
		(void)nanosleep(&ts, NULL);
	}
	printf("  full mailbox: FAIL a delivery fn's wait did not return at "
	       "once (the run is blocked)\n");
	fflush(stdout);
	_exit(1);
	return NULL;
}

static int
full_mailbox(uint64_t seed, int64_t timeout_ns)
{
	xtc_proc_opts_t po = XTC_PROC_OPTS_INIT;
	xtc_exec_opts_t eo = XTC_EXEC_OPTS_INIT;
	xtc_exec_t *e = NULL;
	pthread_t dev, dog;
	int rc, i, bad = 0, ok = 0, nondet;

	memset(&g_f, 0, sizeof g_f);
	g_f.timeout_ns = timeout_ns;
	xtc_atomic_store_i32(&g_f.running, 1, xtc_mo_release);
	for (i = 0; i < N_FULL; i++) {
		g_f.id[i] = i;
		g_f.rc[i] = 1; /* not delivered */
	}
	eo.n_loops = 1;
	if (xtc_exec_create(&eo, &e) != XTC_OK)
		return 1;
	po.mailbox_cap = 1;
	if (xtc_proc_spawn(xtc_exec_loop(e, 0), full_rx, NULL, &po, NULL) !=
	        XTC_OK ||
	    pthread_create(&dev, NULL, full_dev, NULL) != 0 ||
	    pthread_create(&dog, NULL, full_watchdog, NULL) != 0)
		return 1;
	/* Count mode: a wall-clock read is counted, not an abort, so it is
	 * reported here by name. */
	xtc_sim_strict(0);
	rc = xtc_sim_exec_run(e, seed, 100000);
	nondet = xtc_sim_nondeterminism_count();
	xtc_sim_strict(1);
	xtc_atomic_store_i32(&g_f.running, 0, xtc_mo_release);
	(void)pthread_join(dog, NULL);
	(void)pthread_join(dev, NULL);
	xtc_exec_destroy(e);
	/* One delivery finds room; the rest a full mailbox. */
	for (i = 0; i < N_FULL; i++)
		if (g_f.rc[i] == XTC_OK)
			ok++;
		else if (g_f.rc[i] != XTC_E_STATE)
			bad = 1;
	if (nondet != 0 || rc != XTC_OK || bad || ok != 1 || g_f.got != 1 ||
	    g_f.other_bad) {
		printf(
		    "  full mailbox (timeout %lld): FAIL a delivery fn's wait "
		    "did not return at once (rc=%d nondet=%d send=%d,%d,%d,%d "
		    "got=%d other=%d)\n",
		    (long long)timeout_ns, rc, nondet, g_f.rc[0], g_f.rc[1],
		    g_f.rc[2], g_f.rc[3], g_f.got, g_f.other_bad);
		return 1;
	}
	return 0;
}

static void
count_call(void *arg)
{
	(*(int *)arg)++;
}

/* Outside a run, begin hands out NULL and complete delivers at once. */
static int
production_path(void)
{
	xtc_sim_ext_t *t = (xtc_sim_ext_t *)&g_q;
	int hits = 0;
	if (xtc_sim_external_begin(0, MAX_DLY, &t) != XTC_OK || t != NULL)
		return 1;
	if (xtc_sim_external_begin(16, MAX_DLY, &t) != XTC_E_INVAL ||
	    xtc_sim_external_begin(0, -1, &t) != XTC_E_INVAL ||
	    xtc_sim_external_begin(0, 0, NULL) != XTC_E_INVAL)
		return 1;
	if (xtc_sim_external_complete(NULL, NULL, NULL) != XTC_OK ||
	    xtc_sim_external_complete(NULL, count_call, &hits) != XTC_OK)
		return 1;
	return hits != 1;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0x657874; /* "ext" */
	int n = 16, i, fails = 0;

	if (argc > 1)
		base = strtoull(argv[1], NULL, 0);
	if (argc > 2)
		n = atoi(argv[2]);
	if (strcmp(xtc_io_backend_name(), "sim") != 0) {
		printf("SKIP: needs --with-io-backend=sim\n");
		return 0;
	}
	if (production_path() != 0) {
		printf("FAIL: the outside-a-run path of begin/complete\n");
		return 1;
	}
	printf("== external completion DST: %d seeds from base 0x%llx ==\n", n,
	    (unsigned long long)base);
	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		uint64_t d1 = 0, d2 = 0;
		if (run_one(seed, &d1) != 0) {
			fails++;
			continue;
		}
		if (run_one(seed, &d2) != 0 || d1 != d2) {
			printf("  seed 0x%016llx: FAIL replay diverged\n",
			    (unsigned long long)seed);
			fails++;
		}
	}
	/* Both shapes of the 2.2.0 defect: a timed wait (wall-clock read)
	 * and an unbounded one (a blocked run).  After the sweep, so a bug
	 * the sweep catches is reported by the sweep's own message. */
	if (fails == 0 &&
	    (full_mailbox(base, 100 * MS) != 0 || full_mailbox(base, -1) != 0))
		return 1;
	if (fails == 0) {
		printf(
		    "OK: external completion DST -- %d seeds x %d jobs from a "
		    "real pthread worker: each delivered once within its "
		    "window, lane 0 in order, all replay despite the "
		    "worker's real timing; a delivery fn's xtc_send_wait on a "
		    "full mailbox returns XTC_E_STATE at once\n",
		    n, N_JOBS);
		return 0;
	}
	printf("FAIL: %d/%d external-completion seeds failed\n", fails, n);
	return 1;
}
