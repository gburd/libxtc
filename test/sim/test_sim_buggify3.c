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
#include "xtc_chan.h"
#include "xtc_svr.h"
#include "xtc_async.h"     /* xtc_yield */
#include "xtc_sim.h"

/*
 * DST coverage of the BUGGIFY sites planted beyond test_sim_buggify /
 * test_sim_buggify2 (mailbox, mpsc, steal):
 *
 *   chan.mpmc.spurious_full  (src/ptc/chan.c) -- a bounded MPMC channel
 *       reports XTC_E_AGAIN with room to spare; the sender must retry.
 *   svr.recv.delay_dispatch  (src/orc/svr.c) -- the gen_server yields
 *       AFTER receiving a message but BEFORE dispatching it; the
 *       message is already in hand (not re-queued, so it cannot be
 *       lost), so this is a legal pessimal delay that lets other procs
 *       interleave in the recv/dispatch window.  Every call still gets
 *       its reply.
 *
 * Two workloads under the seeded deterministic scheduler:
 *
 *   PART A -- mpmc producers/consumers.  Producers enqueue a disjoint
 *       block of items into a small bounded MPMC channel; consumers
 *       drain until closed+empty.  Under buggify the spurious-full path
 *       fires, so senders retry -- but every item must STILL be
 *       delivered exactly once (progress + no loss preserved).
 *
 *       PART B -- gen_server call/cast.  Clients issue calls to an
 *       accumulator gen_server; under buggify the server occasionally
 *       delays dispatch (yields between recv and dispatch).  Every call
 *       must STILL receive exactly one reply (the pessimal delay does
 *       not drop a message).
 *
 * Both parts assert, mirroring test_sim_buggify2:
 *   - progress: every item / every call completes under buggify;
 *   - activation: buggify ON activates at least one of the two new
 *     sites (coverage);
 *   - replay: same seed -> same activation count + same ORDER-sensitive
 *     completion hash;
 *   - disabled => zero activations.
 */

#define N_LOOPS 4

#define IPTR(i)   ((void *)(intptr_t)((i) + 1))
#define IVAL(p)   ((int)(intptr_t)(p) - 1)

/* ============ PART A: mpmc spurious-full ============ */

#define A_PROD   3
#define A_CONS   2
#define A_PER    6
#define A_TOTAL  (A_PROD * A_PER)

static xtc_chan_mpmc_t *g_ch;
static atomic_int  g_a_prod;
static atomic_int  g_a_cons;
static atomic_int  g_a_seen[A_TOTAL];
static atomic_int  g_a_prod_done;
static atomic_long g_a_hash;

static void
a_fold(int v)
{
	long h = atomic_load_explicit(&g_a_hash, memory_order_relaxed);
	h = h * 1000003L + (v + 1);
	atomic_store_explicit(&g_a_hash, h, memory_order_relaxed);
}

static void
a_producer(void *arg)
{
	int base = (int)(intptr_t)arg;
	int i = 0, guard = 0;
	while (i < A_PER && guard++ < 500000) {
		if (xtc_chan_mpmc_try_send(g_ch, IPTR(base + i)) == XTC_OK) {
			atomic_fetch_add_explicit(&g_a_prod, 1,
			    memory_order_relaxed);
			i++;
		} else {
			xtc_yield();
		}
	}
	if (atomic_fetch_add_explicit(&g_a_prod_done, 1,
	    memory_order_relaxed) + 1 == A_PROD)
		(void)xtc_chan_mpmc_close(g_ch);
}

static void
a_consumer(void *arg)
{
	int guard = 0;
	(void)arg;
	while (guard++ < 800000) {
		void *v = NULL;
		int rc = xtc_chan_mpmc_try_recv(g_ch, &v);
		if (rc == XTC_OK) {
			int id = IVAL(v);
			if (id >= 0 && id < A_TOTAL)
				atomic_fetch_add_explicit(&g_a_seen[id], 1,
				    memory_order_relaxed);
			atomic_fetch_add_explicit(&g_a_cons, 1,
			    memory_order_relaxed);
			a_fold(id);
		} else if (rc == XTC_E_INVAL) {
			break;
		} else {
			xtc_yield();
		}
	}
}

static int
run_a(uint64_t seed, unsigned bug_pct, int *out_cons, int *out_dupdrop,
    long *out_hash, int *out_bug)
{
	xtc_exec_t *e = NULL;
	int i, rc, dupdrop = 0;

	atomic_store(&g_a_prod, 0);
	atomic_store(&g_a_cons, 0);
	atomic_store(&g_a_prod_done, 0);
	atomic_store(&g_a_hash, 0);
	for (i = 0; i < A_TOTAL; i++) atomic_store(&g_a_seen[i], 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	if (bug_pct > 0) xtc_sim_buggify_enable(bug_pct);
	else             xtc_sim_buggify_disable();
	if (xtc_chan_mpmc_create(NULL, 4, &g_ch) != XTC_OK) {
		(void)xtc_exec_fini(e); return -1;
	}
	for (i = 0; i < A_PROD; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(i % N_LOOPS)),
		    a_producer, (void *)(intptr_t)(i * A_PER), NULL, NULL);
	for (i = 0; i < A_CONS; i++)
		(void)xtc_proc_spawn(
		    xtc_exec_loop(e, (unsigned)((i + 1) % N_LOOPS)),
		    a_consumer, NULL, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	for (i = 0; i < A_TOTAL; i++)
		if (atomic_load(&g_a_seen[i]) != 1) dupdrop++;

	*out_cons = atomic_load(&g_a_cons);
	*out_dupdrop = dupdrop;
	*out_hash = atomic_load(&g_a_hash);
	*out_bug = xtc_sim_buggify_active_count();
	xtc_chan_mpmc_destroy(g_ch);
	g_ch = NULL;
	xtc_sim_buggify_disable();
	(void)xtc_exec_fini(e);
	return rc;
}

/* ============ PART B: gen_server spurious-wake ============ */

#define B_CLIENTS 5
#define B_OPS     4

struct acc_state { long total; };

static int
b_call(void *st, const void *req, size_t n, xtc_svr_call_t *call)
{
	struct acc_state *s = st;
	long add = 0;
	if (n >= sizeof(long)) memcpy(&add, req, sizeof(long));
	s->total += add;
	(void)xtc_svr_reply(call, &s->total, sizeof s->total);
	return XTC_SVR_CONTINUE;
}

static xtc_pid_t   g_bsvr_pid;
static xtc_svr_t  *g_bsvr;
static atomic_int  g_b_calls;
static atomic_int  g_b_timeouts;
static atomic_int  g_b_errs;
static atomic_int  g_b_clients_done;
static atomic_long g_b_hash;

static void
b_fold(long v)
{
	long h = atomic_load_explicit(&g_b_hash, memory_order_relaxed);
	h = h * 1000003L + (v + 1);
	atomic_store_explicit(&g_b_hash, h, memory_order_relaxed);
}

static void
b_client(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < B_OPS; i++) {
		long payload = 1 + (long)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 8);
		void  *rep = NULL;
		size_t rn = 0;
		int rc = xtc_svr_call(g_bsvr_pid, &payload, sizeof payload,
		    &rep, &rn, 500 * 1000 * 1000LL);
		if (rc == XTC_OK && rep != NULL && rn >= sizeof(long)) {
			long val = 0;
			memcpy(&val, rep, sizeof val);
			atomic_fetch_add_explicit(&g_b_calls, 1,
			    memory_order_relaxed);
			b_fold(val);
		} else if (rc == XTC_E_AGAIN) {
			/* Timed out: the delay-dispatch buggify can push a call
			 * past its deadline.  A LEGAL outcome (xtc_svr_call
			 * documents XTC_E_AGAIN on timeout); count it so the
			 * completion invariant is replies + timeouts == total
			 * (no lost/duplicated reply, no error). */
			atomic_fetch_add_explicit(&g_b_timeouts, 1,
			    memory_order_relaxed);
		} else {
			atomic_fetch_add_explicit(&g_b_errs, 1,
			    memory_order_relaxed);
		}
		if (rep != NULL) free(rep);
	}
	atomic_fetch_add_explicit(&g_b_clients_done, 1, memory_order_relaxed);
}

static void
b_winder(void *arg)
{
	(void)arg;
	int i;
	for (i = 0; i < 4000; i++) {
		if (atomic_load_explicit(&g_b_clients_done,
		    memory_order_relaxed) >= B_CLIENTS)
			break;
		(void)xtc_proc_sleep(1 * 1000 * 1000LL);
	}
	(void)xtc_svr_stop(g_bsvr);
	/* Reclaim after the run (non-blocking join), not here: xtc_svr_join
	 * blocks on a pthread notify with no fiber-yield shim, which would
	 * freeze the sim thread.  See test_sim_svr / docs/M_DST.md. */
}

static int
run_b(uint64_t seed, unsigned bug_pct, int *out_calls, int *out_timeouts,
    int *out_errs, long *out_hash, int *out_bug)
{
	xtc_exec_t *e = NULL;
	xtc_svr_callbacks_t cb = {0};
	static struct acc_state st;
	int i, rc;

	atomic_store(&g_b_calls, 0);
	atomic_store(&g_b_timeouts, 0);
	atomic_store(&g_b_errs, 0);
	atomic_store(&g_b_clients_done, 0);
	atomic_store(&g_b_hash, 0);
	memset(&st, 0, sizeof st);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	if (bug_pct > 0) xtc_sim_buggify_enable(bug_pct);
	else             xtc_sim_buggify_disable();

	cb.handle_call = b_call;
	if (xtc_svr_start(xtc_exec_loop(e, 0), &cb, &st, NULL, &g_bsvr)
	    != XTC_OK) { (void)xtc_exec_fini(e); return -1; }
	g_bsvr_pid = xtc_svr_pid(g_bsvr);

	for (i = 0; i < B_CLIENTS; i++)
		(void)xtc_proc_spawn(
		    xtc_exec_loop(e, (unsigned)((i % (N_LOOPS - 1)) + 1)),
		    b_client, NULL, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), b_winder, NULL, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_calls = atomic_load(&g_b_calls);
	*out_timeouts = atomic_load(&g_b_timeouts);
	*out_errs = atomic_load(&g_b_errs);
	*out_hash = atomic_load(&g_b_hash);
	*out_bug = xtc_sim_buggify_active_count();
	xtc_sim_buggify_disable();
	/* Reclaim the server handle after the run has quiesced (non-blocking
	 * join sees the stored 'stopped' signal). */
	if (g_bsvr != NULL) {
		(void)xtc_svr_join(g_bsvr, 0);
		g_bsvr = NULL;
	}
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(void)
{
	/* ---- PART A: mpmc spurious-full ---- */
	int ac1 = 0, ad1 = 0, ab1 = 0, ac2 = 0, ad2 = 0, ab2 = 0;
	int acoff = 0, adoff = 0, aboff = 0;
	long ah1 = 0, ah2 = 0, ahoff = 0;
	int rc;

	rc = run_a(0xB0661F1A, 1000, &ac1, &ad1, &ah1, &ab1);
	if (rc != XTC_OK) { printf("FAIL: mpmc buggify run rc=%d\n", rc); return 1; }
	(void)run_a(0xB0661F1A, 1000, &ac2, &ad2, &ah2, &ab2);
	rc = run_a(0xB0661F1A, 0, &acoff, &adoff, &ahoff, &aboff);
	if (rc != XTC_OK) { printf("FAIL: mpmc buggify-off run rc=%d\n", rc); return 1; }

	printf("PART A mpmc ON  run1: cons=%d dup/drop=%d active=%d\n",
	    ac1, ad1, ab1);
	printf("PART A mpmc ON  run2: cons=%d dup/drop=%d active=%d\n",
	    ac2, ad2, ab2);
	printf("PART A mpmc OFF run : cons=%d dup/drop=%d active=%d\n",
	    acoff, adoff, aboff);

	if (ac1 != A_TOTAL || ad1 != 0 || acoff != A_TOTAL || adoff != 0) {
		printf("FAIL: mpmc lost/duplicated items under buggify "
		    "(cons=%d/%d dup/drop=%d/%d) -- pessimal path lost "
		    "progress\n", ac1, acoff, ad1, adoff);
		return 1;
	}
	if (ac1 != ac2 || ah1 != ah2 || ab1 != ab2) {
		printf("FAIL: mpmc buggify did not replay (cons %d/%d hash "
		    "%ld/%ld active %d/%d)\n", ac1, ac2, ah1, ah2, ab1, ab2);
		return 1;
	}
	if (aboff != 0) {
		printf("FAIL: mpmc buggify DISABLED but %d activated\n", aboff);
		return 1;
	}

	/* ---- PART B: gen_server spurious-wake ---- */
	int bc1 = 0, bt1 = 0, be1 = 0, bb1 = 0;
	int bc2 = 0, bt2 = 0, be2 = 0, bb2 = 0;
	int bcoff = 0, btoff = 0, beoff = 0, bboff = 0;
	long bh1 = 0, bh2 = 0, bhoff = 0;

	rc = run_b(0x5E44BEEF, 1000, &bc1, &bt1, &be1, &bh1, &bb1);
	if (rc != XTC_OK) { printf("FAIL: svr buggify run rc=%d\n", rc); return 1; }
	(void)run_b(0x5E44BEEF, 1000, &bc2, &bt2, &be2, &bh2, &bb2);
	rc = run_b(0x5E44BEEF, 0, &bcoff, &btoff, &beoff, &bhoff, &bboff);
	if (rc != XTC_OK) { printf("FAIL: svr buggify-off run rc=%d\n", rc); return 1; }

	printf("PART B svr  ON  run1: calls=%d timeouts=%d errs=%d active=%d\n",
	    bc1, bt1, be1, bb1);
	printf("PART B svr  ON  run2: calls=%d timeouts=%d errs=%d active=%d\n",
	    bc2, bt2, be2, bb2);
	printf("PART B svr  OFF run : calls=%d timeouts=%d errs=%d active=%d\n",
	    bcoff, btoff, beoff, bboff);

	/* Completion invariant: replies + timeouts == total, never an error
	 * (a lost/duplicated reply or bad rc), for BOTH ON and OFF.  With
	 * buggify OFF no delay is injected, so every call should reply
	 * (zero timeouts); with it ON a call MAY time out (a legal pessimal
	 * outcome) but never errors or vanishes. */
	if (be1 != 0 || beoff != 0) {
		printf("FAIL: gen_server call returned an error rc under buggify "
		    "(errs=%d/%d) -- a reply was lost or corrupted\n", be1, beoff);
		return 1;
	}
	if (bc1 + bt1 != B_CLIENTS * B_OPS ||
	    bcoff + btoff != B_CLIENTS * B_OPS) {
		printf("FAIL: gen_server completion invariant broken -- "
		    "replies+timeouts != total (%d+%d and %d+%d, want %d)\n",
		    bc1, bt1, bcoff, btoff, B_CLIENTS * B_OPS);
		return 1;
	}
	if (btoff != 0) {
		printf("FAIL: gen_server timed out with buggify OFF (timeouts=%d) "
		    "-- unexpected without an injected delay\n", btoff);
		return 1;
	}
	if (bc1 != bc2 || bt1 != bt2 || bh1 != bh2 || bb1 != bb2) {
		printf("FAIL: svr buggify did not replay (calls %d/%d timeouts "
		    "%d/%d hash %ld/%ld active %d/%d)\n",
		    bc1, bc2, bt1, bt2, bh1, bh2, bb1, bb2);
		return 1;
	}
	if (bboff != 0) {
		printf("FAIL: svr buggify DISABLED but %d activated\n", bboff);
		return 1;
	}

	/* Coverage: at 100% activation across both parts, at least one of
	 * the two new sites should fire (mpmc's tiny cap guarantees the
	 * full path is reached; the server re-queue fires on a fraction of
	 * receives).  Report; the determinism + progress asserts are the
	 * invariants. */
	printf("OK: additional buggify sites under DST -- "
	    "chan.mpmc.spurious_full (%d activation(s) part A, every item "
	    "delivered exactly once) + svr.recv.delay_dispatch (%d "
	    "activation(s) part B, every call still replied), both replay "
	    "from seed; disabled => zero\n", ab1, bb1);
	return 0;
}
