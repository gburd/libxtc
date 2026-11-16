/*
 * test_sim_fsm_call -- DST of xtc_fsm_call against a machine that ends
 * while calls wait on it.
 *
 * A machine on loop 0 answers calls after a seeded number of yields.  At
 * a seeded virtual time it ends while calls are in flight, by one of
 * three seeded ways: an event() that calls xtc_exit_self without
 * replying, an xtc_exit_pid kill from a peer, or an xtc_fsm_stop.
 * Callers on every loop of a multi-loop executor issue calls in a loop
 * with a long timeout (CALL_TIMEOUT_NS, virtual time), under seeded net
 * latency.
 *
 * Assertions per seed: every call returns XTC_OK (a reply), or ends
 * within NOTICE_NS of the machine's end with XTC_E_NOTFOUND (or
 * XTC_E_INVAL, sent to a machine already gone), never at its timeout
 * (XTC_E_AGAIN); calls were in flight when the machine ended; every
 * caller finished; the run replays byte-identically (counts, the latest
 * notice latency and the scheduler state hash).
 *
 * Planted bug (src/inc/xtc_dst_inject.h) this test must catch:
 *   33 FSMCALL_NOLIVE  xtc_fsm_call does not check the machine's
 *                      liveness (2.1.2): a call made just before the
 *                      machine ends waits out its timeout.
 *
 * Usage: test_sim_fsm_call [base [count]]
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_fsm.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

#define N_LOOPS 3
#define N_CALLERS 6
#define MS (1000LL * 1000)
#define CALL_TIMEOUT_NS (5000 * MS)
/* A call may see the end one wait slice (25 ms) late, plus one more
 * slice for an in-flight reply, plus the net latency. */
#define NOTICE_NS (100 * MS)

enum { END_EXIT, END_KILL, END_STOP, N_ENDS };

static xtc_fsm_t *g_fsm;
static _Atomic int64_t g_end_at; /* virtual time the machine ended, or 0 */
static _Atomic int g_end_kind;
static _Atomic int g_ok, g_gone, g_bad, g_late, g_callers_done;
static _Atomic int64_t g_worst_ns; /* latest notice after the end */

static int64_t
now_ns(void)
{
	return xtc_clock_mono();
}

/* The machine: answers each call after a few yields; the "x" event
 * ends it (END_EXIT) without a reply. */
static xtc_fsm_result_t
m_event(void *st, int state, const void *msg, size_t len, xtc_fsm_call_t *call)
{
	xtc_fsm_result_t r = { XTC_FSM_KEEP, 0, 0 };
	int i, n;
	(void)st;
	(void)state;
	if (len == 1 && *(const char *)msg == 'x') {
		atomic_store_explicit(&g_end_at, now_ns(),
		    memory_order_seq_cst);
		(void)xtc_exit_self(3);
	}
	n = (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 4);
	for (i = 0; i < n; i++)
		xtc_yield();
	if (call != NULL)
		(void)xtc_fsm_reply(call, "r", 1);
	return r;
}

static const xtc_fsm_callbacks_t M_CB = {
	.size = sizeof(xtc_fsm_callbacks_t),
	.event = m_event,
};

/* Ends the machine at a seeded time, the seeded way. */
static void
ender(void *arg)
{
	xtc_pid_t m = xtc_fsm_pid(g_fsm);
	int kind = atomic_load_explicit(&g_end_kind, memory_order_seq_cst);
	(void)arg;
	(void)xtc_proc_sleep(
	    (int64_t)(2 + __xtc_sim_rng_range(XTC_SIM_RNG_APP, 30)) * MS);
	if (kind == END_EXIT) {
		(void)xtc_fsm_send(m, "x", 1);
		return;
	}
	atomic_store_explicit(&g_end_at, now_ns(), memory_order_seq_cst);
	if (kind == END_KILL)
		(void)xtc_exit_pid(m, 1);
	else
		(void)xtc_fsm_stop(g_fsm);
}

static void
caller(void *arg)
{
	xtc_pid_t m = xtc_fsm_pid(g_fsm);
	int i, rc;
	(void)arg;
	for (i = 0; i < 64; i++) {
		void *rep = NULL;
		size_t rn = 0;
		int64_t t0 = now_ns(), end, late;
		rc = xtc_fsm_call(m, "?", 1, &rep, &rn, CALL_TIMEOUT_NS);
		xtc_free(rep);
		if (rc == XTC_OK) {
			atomic_fetch_add_explicit(&g_ok, 1,
			    memory_order_seq_cst);
			continue;
		}
		/* INVAL: the machine was already gone at the send.  AGAIN:
		 * the call waited out its timeout, which is far longer than
		 * the machine lives, so it outlived the machine too. */
		if (rc == XTC_E_NOTFOUND || rc == XTC_E_INVAL ||
		    rc == XTC_E_AGAIN) {
			end = atomic_load_explicit(&g_end_at,
			    memory_order_seq_cst);
			late = now_ns() - (end > t0 ? end : t0);
			atomic_fetch_add_explicit(&g_gone, 1,
			    memory_order_seq_cst);
			if (end == 0 || late > NOTICE_NS)
				atomic_fetch_add_explicit(&g_late, 1,
				    memory_order_seq_cst);
			if (late > atomic_load_explicit(&g_worst_ns,
			               memory_order_seq_cst))
				atomic_store_explicit(&g_worst_ns, late,
				    memory_order_seq_cst);
			break;
		}
		atomic_fetch_add_explicit(&g_bad, 1, memory_order_seq_cst);
		printf("  call returned %d (want OK or NOTFOUND)\n", rc);
		break;
	}
	(void)atomic_fetch_add_explicit(&g_callers_done, 1,
	    memory_order_seq_cst);
}

/* Stops the run once every caller is done. */
static void
winder(void *arg)
{
	xtc_exec_t *e = arg;
	while (atomic_load_explicit(&g_callers_done, memory_order_seq_cst) <
	    N_CALLERS)
		(void)xtc_proc_sleep(1 * MS);
	(void)xtc_exec_stop(e);
}

struct result {
	int rc, ok, gone, bad, late, done;
	int64_t worst;
	uint64_t state;
};

static void
run_one(uint64_t seed, struct result *res)
{
	xtc_exec_t *e = NULL;
	int i;

	atomic_store_explicit(&g_end_at, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_ok, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_gone, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_bad, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_late, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_callers_done, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_worst_ns, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_end_kind, (int)(seed % N_ENDS),
	    memory_order_seq_cst);
	memset(res, 0, sizeof *res);
	res->rc = -1;
	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK)
		return;
	if (xtc_fsm_start(xtc_exec_loop(e, 0), &M_CB, NULL, 0, NULL, &g_fsm) !=
	    XTC_OK) {
		(void)xtc_exec_fini(e);
		return;
	}
	for (i = 0; i < N_CALLERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(i % N_LOOPS)),
		    caller, NULL, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 1), ender, NULL, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 2), winder, e, NULL, NULL);
	xtc_sim_net_latency(10 * 1000, 2 * MS);
	res->rc = xtc_sim_exec_run(e, seed, 20000000);
	xtc_sim_net_latency(0, 0);
	res->ok = atomic_load_explicit(&g_ok, memory_order_seq_cst);
	res->gone = atomic_load_explicit(&g_gone, memory_order_seq_cst);
	res->bad = atomic_load_explicit(&g_bad, memory_order_seq_cst);
	res->late = atomic_load_explicit(&g_late, memory_order_seq_cst);
	res->done = atomic_load_explicit(&g_callers_done, memory_order_seq_cst);
	res->worst = atomic_load_explicit(&g_worst_ns, memory_order_seq_cst);
	res->state = xtc_sim_state_hash(e);
	(void)xtc_fsm_join(g_fsm, 0);
	g_fsm = NULL;
	(void)xtc_exec_fini(e);
}

int
main(int argc, char **argv)
{
	uint64_t base = 0x6663; /* "fc" */
	int n = 24, i, fails = 0;

	if (argc > 1)
		base = strtoull(argv[1], NULL, 0);
	if (argc > 2)
		n = atoi(argv[2]);
	printf("== fsm call vs machine end DST: %d seeds from base 0x%llx ==\n",
	    n, (unsigned long long)base);
	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		struct result a, b;

		run_one(seed, &a);
		if (a.rc != XTC_OK || a.bad != 0 || a.done != N_CALLERS) {
			printf("  seed 0x%016llx: FAIL a call did not end "
			       "(rc=%d bad=%d done=%d/%d)\n",
			    (unsigned long long)seed, a.rc, a.bad, a.done,
			    N_CALLERS);
			fails++;
			continue;
		}
		if (a.late != 0 || a.gone == 0) {
			printf("  seed 0x%016llx: FAIL a call outlived the "
			       "machine by %lld ms (ok=%d gone=%d late=%d)\n",
			    (unsigned long long)seed, (long long)(a.worst / MS),
			    a.ok, a.gone, a.late);
			fails++;
			continue;
		}
		run_one(seed, &b);
		if (b.rc != a.rc || b.ok != a.ok || b.gone != a.gone ||
		    b.worst != a.worst || b.state != a.state) {
			printf("  seed 0x%016llx: FAIL replay diverged\n",
			    (unsigned long long)seed);
			fails++;
		}
	}
	if (fails == 0) {
		printf("OK: fsm call DST -- %d seeds x %d callers, the machine "
		       "exits, is killed or stops mid-call: every call got its "
		       "reply or XTC_E_NOTFOUND within %lld ms, all replay\n",
		    n, N_CALLERS, (long long)(NOTICE_NS / MS));
		return 0;
	}
	printf("FAIL: %d/%d fsm-call seeds failed\n", fails, n);
	return 1;
}
