/*
 * test_sim_net_inflight -- DST of the simulator's cross-loop net latency
 * against mailbox admission: a send told XTC_OK arrives.
 *
 * Under xtc_sim_net_latency a cross-loop xtc_send is put in flight and
 * lands after a seeded virtual-time latency.  Per seed:
 *
 *   BURST   a cap-2 receiver on loop 0 sleeps while a sender on loop 1
 *           sends a burst of 10 large messages, sleeps 20 ms, and sends
 *           a second burst.  Only what the mailbox can hold may return
 *           XTC_OK in the first burst, everything that returned XTC_OK
 *           must arrive, and the second burst must be admitted again
 *           (the room the first held came back when it landed).
 *   SHORT   a message sent to a proc that exits inside the latency
 *           window is dropped, and freed.
 *   CUT     the same run stopped at its third step, with the burst still
 *           in flight when the executor is finalized: nothing leaks.
 *   BUGGIFY 3 senders x 60 messages to 3 receivers with no cap, under
 *           buggify (the spurious-full site among them): every send
 *           that returned XTC_OK arrives.
 *
 * The allocation auditor counts live allocations across BURST, SHORT
 * and CUT (after a warm-up run, since process-wide caches are kept).
 * Every case replays: two runs of a seed give the same counts and
 * scheduler state hash.
 *
 * Planted bug (src/inc/xtc_dst_inject.h) this test must catch:
 *   34 SIM_DROP_LATE  a latency-deferred send is admitted without a look
 *                     at the mailbox and dropped at delivery when it
 *                     filled meanwhile (the 2.1.2 shape).
 *
 * Usage: test_sim_net_inflight [base [count]] (seeds derived from
 * base+1 .. base+count, default 0 16).
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
#include "xtc_alloc_audit.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

#define MS (1000LL * 1000)
#define IF_BURST 10
#define IF_CAP 2
#define BG_PAIRS 3
#define BG_MSGS 60

static xtc_pid_t g_if_rx;
static int g_if_ok[2], g_if_got, g_if_late;

static void
if_receiver(void *arg)
{
	void *m;
	size_t sz;
	(void)arg;
	(void)xtc_proc_sleep(5 * MS);
	while (xtc_recv(&m, &sz, 30 * MS) == XTC_OK) {
		xtc_free(m); /* through the hook the auditor counts */
		g_if_got++;
	}
}

static void
if_sender(void *arg)
{
	int i, round;
	char big[1024]; /* above the inline size: its own block */
	(void)arg;
	memset(big, 'x', sizeof big);
	for (round = 0; round < 2; round++) {
		for (i = 0; i < IF_BURST; i++)
			if (xtc_send(g_if_rx, big, sizeof big) == XTC_OK)
				g_if_ok[round]++;
		(void)xtc_proc_sleep(20 * MS);
	}
}

/* Exits after 5 us, before the latency floor (10 us): a message sent
 * to it at time 0 lands after it is gone. */
static void
if_short(void *arg)
{
	(void)arg;
	(void)xtc_proc_sleep(5 * 1000LL);
}

static void
if_late_sender(void *arg)
{
	char big[1024];
	xtc_pid_t *to = arg;
	memset(big, 'y', sizeof big);
	if (xtc_send(*to, big, sizeof big) == XTC_OK)
		g_if_late++;
}

/* One run of BURST + SHORT; *grew = live allocations the run left
 * behind (the auditor's count after the exec is finalized, minus
 * before). */
static int
if_run(uint64_t seed, long max_steps, int *grew, uint64_t *state)
{
	xtc_exec_t *e;
	xtc_proc_opts_t o = XTC_PROC_OPTS_INIT;
	xtc_pid_t short_pid;
	size_t c0 = 0, c1 = 0;
	int rc;

	g_if_ok[0] = g_if_ok[1] = g_if_got = g_if_late = 0;
	xtc_alloc_audit_stats(&c0, NULL);
	if (xtc_exec_init(&e, 2) != XTC_OK)
		return -1;
	o.mailbox_cap = IF_CAP;
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), if_receiver, NULL, &o,
	    &g_if_rx);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 1), if_sender, NULL, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), if_short, NULL, NULL,
	    &short_pid);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 1), if_late_sender, &short_pid,
	    NULL, NULL);
	xtc_sim_net_latency(10 * 1000LL, 200 * 1000LL);
	rc = xtc_sim_exec_run(e, seed, max_steps);
	xtc_sim_net_latency(0, 0);
	if (state != NULL)
		*state = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);
	xtc_alloc_audit_stats(&c1, NULL);
	*grew = c1 > c0 ? (int)(c1 - c0) : 0;
	return rc;
}

/* BUGGIFY: senders retry nothing, they count what returned XTC_OK. */
static xtc_pid_t g_bg_rx[BG_PAIRS];
static _Atomic int g_bg_sent, g_bg_got, g_bg_done;

static void
bg_receiver(void *arg)
{
	void *m;
	size_t sz;
	(void)arg;
	/* Until every sender is done and a 2 ms wait finds nothing (each
	 * sender sleeps 5 ms after its last send, longer than the
	 * latency). */
	for (;;) {
		if (xtc_recv(&m, &sz, 2 * MS) == XTC_OK) {
			xtc_free(m);
			atomic_fetch_add_explicit(&g_bg_got, 1,
			    memory_order_seq_cst);
		} else if (atomic_load_explicit(&g_bg_done,
		               memory_order_seq_cst) == BG_PAIRS) {
			break;
		}
	}
}

static void
bg_sender(void *arg)
{
	int i = (int)(intptr_t)arg, k;
	for (k = 0; k < BG_MSGS; k++) {
		if (xtc_send(g_bg_rx[(i + k) % BG_PAIRS], &k, sizeof k) ==
		    XTC_OK)
			atomic_fetch_add_explicit(&g_bg_sent, 1,
			    memory_order_seq_cst);
		if (k % 8 == 0)
			xtc_yield();
	}
	/* Let the last messages land before the receivers may finish. */
	(void)xtc_proc_sleep(5 * MS);
	atomic_fetch_add_explicit(&g_bg_done, 1, memory_order_seq_cst);
}

static int
bg_run(uint64_t seed, int *sent, int *got, uint64_t *state)
{
	xtc_exec_t *e;
	int i, rc;

	atomic_store_explicit(&g_bg_sent, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_bg_got, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_bg_done, 0, memory_order_seq_cst);
	if (xtc_exec_init(&e, BG_PAIRS) != XTC_OK)
		return -1;
	for (i = 0; i < BG_PAIRS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)i), bg_receiver,
		    NULL, NULL, &g_bg_rx[i]);
	for (i = 0; i < BG_PAIRS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e,
		                         (unsigned)((i + 1) % BG_PAIRS)),
		    bg_sender, (void *)(intptr_t)i, NULL, NULL);
	xtc_sim_buggify_enable(500);
	xtc_sim_net_latency(10 * 1000LL, 300 * 1000LL);
	rc = xtc_sim_exec_run(e, seed, 50000000);
	xtc_sim_net_latency(0, 0);
	xtc_sim_buggify_disable();
	*sent = atomic_load_explicit(&g_bg_sent, memory_order_seq_cst);
	*got = atomic_load_explicit(&g_bg_got, memory_order_seq_cst);
	*state = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);
	return rc;
}

/* Every case for one seed; 1 = pass.  *sig folds what must replay. */
static int
check_seed(uint64_t k, uint64_t seed, uint64_t *sig)
{
	int grew = 0, rc, pass = 1, sent = 0, got = 0;
	uint64_t st = 0, st2 = 0;

	rc = if_run(seed, 50000000, &grew, &st);
	if (g_if_got != g_if_ok[0] + g_if_ok[1] || g_if_ok[0] > IF_CAP) {
		printf("  seed %llu: a message sent with XTC_OK was lost "
		       "(sent %d+%d, received %d, cap %d)\n",
		    (unsigned long long)k, g_if_ok[0], g_if_ok[1], g_if_got,
		    IF_CAP);
		pass = 0;
	}
	if (rc != XTC_OK || g_if_ok[0] == 0 || g_if_ok[1] == 0 ||
	    g_if_late != 1) {
		printf("  seed %llu: in-flight run rc=%d sent %d+%d late %d "
		       "(the room did not come back)\n",
		    (unsigned long long)k, rc, g_if_ok[0], g_if_ok[1],
		    g_if_late);
		pass = 0;
	}
	if (grew != 0) {
		printf("  seed %llu: a message dropped in flight leaked "
		       "(%d allocations)\n",
		    (unsigned long long)k, grew);
		pass = 0;
	}
	*sig = st * 31 + (uint64_t)(g_if_ok[0] * 100 + g_if_ok[1]);
	/* CUT: the burst is still in flight when the exec is finalized. */
	(void)if_run(seed, 3, &grew, NULL);
	if (grew != 0) {
		printf("  seed %llu: a message in flight at the end of a run "
		       "leaked (%d allocations)\n",
		    (unsigned long long)k, grew);
		pass = 0;
	}
	rc = bg_run(seed, &sent, &got, &st2);
	if (rc != XTC_OK || got != sent) {
		printf("  seed %llu: a message sent with XTC_OK was lost "
		       "under buggify (rc=%d sent %d, received %d)\n",
		    (unsigned long long)k, rc, sent, got);
		pass = 0;
	}
	*sig = *sig * 31 + st2 * 7 + (uint64_t)sent;
	return pass;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0, k, seed, sig, sig2;
	int n = 16, i, fails = 0;

	if (argc > 1)
		base = strtoull(argv[1], NULL, 0);
	if (argc > 2)
		n = atoi(argv[2]);
	printf("== net in-flight DST: %d seeds from base %llu ==\n", n,
	    (unsigned long long)base);
	if (xtc_alloc_audit_enable(1) != XTC_OK) {
		printf("FAIL: xtc_alloc_audit_enable\n");
		return 1;
	}
	(void)if_run(0x9E3779B97F4A7C15ull, 50000000, &i, NULL);
	for (k = 1; k <= (uint64_t)n; k++) {
		seed = 0x9E3779B97F4A7C15ull * (base + k);
		if (!check_seed(base + k, seed, &sig)) {
			fails++;
			continue;
		}
		if (!check_seed(base + k, seed, &sig2) || sig2 != sig) {
			printf("  seed %llu: replay diverged\n",
			    (unsigned long long)(base + k));
			fails++;
		}
	}
	(void)xtc_alloc_audit_enable(0);
	if (fails != 0) {
		printf("FAIL: %d/%d net in-flight seeds failed\n", fails, n);
		return 1;
	}
	printf("OK: net in-flight DST -- %d seeds: a cap-%d burst across "
	       "loops, a receiver gone in flight, a run cut short, and "
	       "%d x %d sends under buggify; nothing sent with XTC_OK lost "
	       "or leaked in flight, all replay\n",
	    n, IF_CAP, BG_PAIRS, BG_MSGS);
	return 0;
}
