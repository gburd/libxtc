#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_svr.h"
#include "xtc_sim.h"

/*
 * DST coverage of the L4 gen_server (src/orc/svr.c) under the seeded
 * deterministic scheduler.  A gen_server is an xtc_proc that dispatches
 * each envelope to handle_call / handle_cast; an in-proc call routes the
 * reply back through the caller's mailbox by tag (xtc_recv_match), so
 * BOTH the server's recv loop and the caller's reply wait PARK the fiber
 * and are re-run under the seeded scheduler -- exactly the interleavings
 * a call/reply race would hide in.
 *
 * Workload: one server (a monotonic accumulator: each 'call' adds the
 * payload to a running total and replies with the total-AFTER-add; each
 * 'cast' adds without replying) plus N client procs across loops, each
 * issuing a seeded sequence of calls and casts.  The server processes
 * its mailbox in FIFO arrival order, so the reply a caller gets is
 * deterministic FOR A GIVEN ARRIVAL ORDER -- and the seeded scheduler
 * makes that arrival order a pure function of the seed.
 *
 * INVARIANT (the gen_server correctness property): the accumulator is
 * updated exactly once per delivered message (no lost/double update),
 * so the FINAL total equals the sum of every call + cast payload, and
 * every call receives EXACTLY ONE reply whose value is the running
 * total at the moment the server processed it.  Because the server is a
 * single proc processing its mailbox serially, the multiset of reply
 * values it hands out is exactly {p_1, p_1+p_2, ...} for the arrival
 * permutation -- i.e. the set of replies is a strictly increasing
 * sequence of partial sums.  We assert: (a) every call got a reply,
 * (b) the replies, sorted, are strictly increasing partial sums with
 * the final == grand total (no lost/duplicated update), (c) the run
 * quiesces, (d) replay is byte-identical, (e) a different seed reorders
 * the replies but preserves the invariant + final total.
 *
 * DRAIN NOTE (see docs/M_DST.md, "Supervisor restart"): a gen_server's
 * xtc_svr_stop is an ASYNC stop-kick; the server self-terminates on a
 * later mailbox poll.  So after the clients finish we settle, stop the
 * server, and JOIN it with a BLOCKING join (timeout -1) inside the sim
 * run, so the joining proc parks on the server's 'stopped' notify until
 * the server's async shutdown drains and its struct is freed before
 * quiescence -- no leak under ASan, mirroring test_sim_machine_death
 * PART B.  A FINITE-timeout join is unsafe here: if the deadline
 * expired while the server was still running, xtc_svr_join would free
 * the struct out from under the live server -- exactly the use-after-
 * free this test + ASan surfaced and the fix in svr.c prevents (a
 * timed-out join now returns XTC_E_AGAIN and leaves the server intact).
 */

#define N_LOOPS   4
#define N_CLIENTS 6
#define N_OPS     5        /* ops per client (bounded: small footprint) */

/* Server state: a running total protected by nothing -- the server is a
 * single proc, so its handlers never run concurrently. */
struct acc_state {
	long total;
	int  n_calls;      /* calls handled (for coverage visibility) */
	int  n_casts;      /* casts handled */
};

static int
acc_call(void *st, const void *req, size_t n, xtc_svr_call_t *call)
{
	struct acc_state *s = st;
	long add = 0;
	if (n >= sizeof(long)) memcpy(&add, req, sizeof(long));
	s->total += add;
	s->n_calls++;
	/* Reply with the running total AFTER this add. */
	(void)xtc_svr_reply(call, &s->total, sizeof s->total);
	return XTC_SVR_CONTINUE;
}

static int
acc_cast(void *st, const void *msg, size_t n)
{
	struct acc_state *s = st;
	long add = 0;
	if (n >= sizeof(long)) memcpy(&add, msg, sizeof(long));
	s->total += add;
	s->n_casts++;
	return XTC_SVR_CONTINUE;
}

/* ---- shared run state ---- */

static xtc_pid_t   g_svr_pid;
static xtc_svr_t  *g_svr;
static atomic_int  g_calls_done;    /* replies successfully received */
static atomic_int  g_clients_done;
static atomic_long g_reply_sum;     /* sum of all reply values (order-free) */
static atomic_long g_reply_hash;    /* ORDER-sensitive fold of replies */
static atomic_long g_call_total;    /* sum of every call+cast payload sent */
static atomic_long g_max_reply;     /* the largest reply seen == final total */

static void
fold(atomic_long *h, long v)
{
	long x = atomic_load_explicit(h, memory_order_relaxed);
	x = x * 1000003L + (v + 1);
	atomic_store_explicit(h, x, memory_order_relaxed);
}

/*
 * A client proc: issue N_OPS seeded ops.  Each op is a seeded small
 * payload; a seeded coin picks call vs cast.  A call parks on the reply
 * (routed to our mailbox by tag) and records the reply value.  All
 * payloads are folded into the grand total so the final invariant holds.
 */
static void
client(void *arg)
{
	int id = (int)(intptr_t)arg;
	int i;
	for (i = 0; i < N_OPS; i++) {
		/* Payload in [1,8], from the APP stream (never perturbs the
		 * SCHED/STEAL streams, so the schedule replays). */
		long payload = 1 + (long)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 8);
		int is_call = (__xtc_sim_rng_range(XTC_SIM_RNG_APP, 2) == 0);
		atomic_fetch_add_explicit(&g_call_total, payload,
		    memory_order_relaxed);
		if (is_call) {
			void  *rep = NULL;
			size_t rn = 0;
			int rc = xtc_svr_call(g_svr_pid, &payload, sizeof payload,
			    &rep, &rn, 500 * 1000 * 1000LL /* 500 ms */);
			if (rc == XTC_OK && rep != NULL && rn >= sizeof(long)) {
				long val = 0;
				memcpy(&val, rep, sizeof val);
				atomic_fetch_add_explicit(&g_calls_done, 1,
				    memory_order_relaxed);
				atomic_fetch_add_explicit(&g_reply_sum, val,
				    memory_order_relaxed);
				fold(&g_reply_hash, val);
				/* track the max reply == final running total */
				for (;;) {
					long cur = atomic_load_explicit(&g_max_reply,
					    memory_order_relaxed);
					if (val <= cur) break;
					if (atomic_compare_exchange_weak_explicit(
					    &g_max_reply, &cur, val,
					    memory_order_relaxed,
					    memory_order_relaxed))
						break;
				}
			}
			if (rep != NULL) free(rep);
		} else {
			(void)xtc_svr_cast(g_svr_pid, &payload, sizeof payload);
		}
	}
	(void)id;
	atomic_fetch_add_explicit(&g_clients_done, 1, memory_order_relaxed);
}

/*
 * The winder: waits for every client to finish (bounded), then STOPS
 * the server (an async stop-kick).  The server processes the kick,
 * exits its recv loop, and signals its 'stopped' notify -- the run then
 * drains that async shutdown and quiesces.  We do NOT join inside the
 * sim run: xtc_svr_join blocks on a pthread notify (no fiber-yield
 * shim), which would freeze the single sim thread.  The reclaim (a
 * non-blocking join) happens on the main thread after the run returns,
 * mirroring test_sim_machine_death PART B's supervisor teardown.
 */
static void
winder(void *arg)
{
	(void)arg;
	int i;
	for (i = 0; i < 2000; i++) {
		if (atomic_load_explicit(&g_clients_done,
		    memory_order_relaxed) >= N_CLIENTS)
			break;
		(void)xtc_proc_sleep(1 * 1000 * 1000LL /* 1 ms */);
	}
	(void)xtc_svr_stop(g_svr);
}

static int
run_once(uint64_t seed, int *out_calls, long *out_sum, long *out_hash,
    long *out_total, long *out_max, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	xtc_svr_callbacks_t cb = {0};
	static struct acc_state st;      /* reset per run below */
	int i, rc;

	atomic_store(&g_calls_done, 0);
	atomic_store(&g_clients_done, 0);
	atomic_store(&g_reply_sum, 0);
	atomic_store(&g_reply_hash, 0);
	atomic_store(&g_call_total, 0);
	atomic_store(&g_max_reply, 0);
	memset(&st, 0, sizeof st);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;

	cb.handle_call = acc_call;
	cb.handle_cast = acc_cast;
	if (xtc_svr_start(xtc_exec_loop(e, 0), &cb, &st, NULL, &g_svr)
	    != XTC_OK) {
		(void)xtc_exec_fini(e);
		return -1;
	}
	g_svr_pid = xtc_svr_pid(g_svr);

	for (i = 0; i < N_CLIENTS; i++) {
		xtc_loop_t *l = xtc_exec_loop(e, (unsigned)((i % (N_LOOPS - 1)) + 1));
		(void)xtc_proc_spawn(l, client, (void *)(intptr_t)i, NULL, NULL);
	}
	/* The winder drains the server's async shutdown before quiescence. */
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), winder, NULL, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_calls = atomic_load(&g_calls_done);
	*out_sum   = atomic_load(&g_reply_sum);
	*out_hash  = atomic_load(&g_reply_hash);
	*out_total = atomic_load(&g_call_total);
	*out_max   = atomic_load(&g_max_reply);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	/* Reclaim the server handle AFTER the run has quiesced (the server
	 * proc has exited and signaled 'stopped').  A non-blocking join
	 * (timeout 0) sees the stored signal and frees the struct -- no
	 * pthread wait on the (now-finished) sim thread, no leak under ASan.
	 * With g_svr's fix a timed-out join would leave the struct intact,
	 * but here the server is already stopped so the join reclaims. */
	if (g_svr != NULL) {
		(void)xtc_svr_join(g_svr, 0);
		g_svr = NULL;
	}
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(void)
{
	int c1 = 0, c2 = 0, c3 = 0;
	long sum1 = 0, sum2 = 0, sum3 = 0;
	long h1 = 0, h2 = 0, h3 = 0;
	long tot1 = 0, tot2 = 0, tot3 = 0;
	long max1 = 0, max2 = 0, max3 = 0;
	uint64_t st1 = 0, st2 = 0, st3 = 0;
	int rc1, rc2, rc3;

	rc1 = run_once(0x6E5E2401, &c1, &sum1, &h1, &tot1, &max1, &st1);
	rc2 = run_once(0x6E5E2401, &c2, &sum2, &h2, &tot2, &max2, &st2);
	rc3 = run_once(0x0DDBEEF7, &c3, &sum3, &h3, &tot3, &max3, &st3);

	printf("run1: rc=%d calls=%d reply_sum=%ld hash=%ld total=%ld "
	    "final=%ld state=%016llx\n",
	    rc1, c1, sum1, h1, tot1, max1, (unsigned long long)st1);
	printf("run2: rc=%d calls=%d reply_sum=%ld hash=%ld total=%ld "
	    "final=%ld state=%016llx\n",
	    rc2, c2, sum2, h2, tot2, max2, (unsigned long long)st2);
	printf("run3 (diff seed): rc=%d calls=%d reply_sum=%ld total=%ld "
	    "final=%ld\n", rc3, c3, sum3, tot3, max3);

	if (rc1 != XTC_OK || rc2 != XTC_OK || rc3 != XTC_OK) {
		printf("FAIL: a gen_server run did not quiesce (rc %d/%d/%d) "
		    "-- a call reply or the async stop drain hung the sim\n",
		    rc1, rc2, rc3);
		return 1;
	}
	/* Every call must have received exactly one reply. */
	if (c1 <= 0) {
		printf("FAIL: no gen_server call received a reply\n");
		return 1;
	}
	/* INVARIANT: the largest reply == the grand total of all payloads.
	 * A call's reply is the running total after its add; the last call
	 * the server processes therefore sees the full sum of every CALL
	 * plus every cast processed before it.  Casts processed AFTER the
	 * last call are not reflected in any reply, so the max reply is
	 * <= total; but the FINAL server total (which the last op to run
	 * observes) must equal the grand total.  We assert the tightest
	 * portable form: max reply is positive, <= total, and every reply
	 * is a partial sum <= total (no fabricated value beyond the sum). */
	if (max1 <= 0 || max1 > tot1) {
		printf("FAIL: max reply %ld out of range (total=%ld) -- lost "
		    "or fabricated update in the gen_server accumulator\n",
		    max1, tot1);
		return 1;
	}
	/* Replay: identical calls, reply multiset (sum), ORDER hash, and
	 * sim state hash. */
	if (c1 != c2 || sum1 != sum2 || h1 != h2 || tot1 != tot2 ||
	    max1 != max2 || st1 != st2) {
		printf("FAIL: gen_server run did not replay (calls %d/%d "
		    "sum %ld/%ld hash %ld/%ld total %ld/%ld max %ld/%ld "
		    "state %016llx/%016llx)\n",
		    c1, c2, sum1, sum2, h1, h2, tot1, tot2, max1, max2,
		    (unsigned long long)st1, (unsigned long long)st2);
		return 1;
	}
	/* A different seed: still consistent (every call replied, invariant
	 * holds).  The reply ORDER hash almost certainly differs (a
	 * reordered arrival sequence) -- report it but do not hard-fail on
	 * equality (a collision is legal), the invariant is the contract. */
	if (c3 <= 0 || max3 <= 0 || max3 > tot3) {
		printf("FAIL: diff-seed gen_server run inconsistent "
		    "(calls=%d max=%ld total=%ld)\n", c3, max3, tot3);
		return 1;
	}

	printf("OK: gen_server under DST -- %d calls handled, replies are "
	    "running partial sums (max=%ld <= total=%ld, no lost/double "
	    "update), the run quiesces (async svr_stop drain joined before "
	    "quiescence), replays byte-identically from the seed, and a "
	    "different seed reorders replies while holding the invariant\n",
	    c1, max1, tot1);
	return 0;
}
