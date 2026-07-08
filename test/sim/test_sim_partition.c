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
#include "xtc_sim.h"

/*
 * DST coverage of a SIMULATED NETWORK PARTITION -- the FoundationDB-style
 * seam where cross-loop message flow is cut by a seeded partition matrix
 * and each surviving delivery is deferred by a seeded latency (so the
 * delivery ORDER is part of the replayable schedule).  This models a
 * network partition at the cross-LOOP granularity xtc's sim actually
 * models: xtc_send between procs on different loops routes through
 * __mbox_deliver (proc.c), the single cross-loop delivery seam that the
 * partition/latency knobs hook.  (The real cross-machine raw-socket
 * transport is NOT modelled -- it cannot run under the single-thread
 * sim;.)
 *
 * Topology: 4 loops split into two groups, A = {loop 0, loop 1} and
 * B = {loop 2, loop 3}.  One receiver proc per loop.  A sender proc for
 * every ordered pair of distinct loops (i -> j) sends one message to the
 * receiver on loop j and then exits; a send that comes back XTC_E_AGAIN
 * (mailbox soft-full OR a partition drop, indistinguishable to the
 * sender by design) is retried a bounded number of times with a short
 * sleep, then given up.  Each receiver recvs with a finite timeout and
 * exits when it has collected its expected count or times out, so a
 * partitioned peer NEVER hangs.
 *
 * With the A|B cut installed (all cross-group edges blocked, both ways):
 * only the within-group cross-loop pairs deliver -- 0<->1 and 2<->3 --
 * so exactly 4 of the 12 sends land.  The run must:
 *   (a) deliver every message inside a connected group (4 arrivals);
 *   (b) reach QUIESCENCE (rc == XTC_OK -- no hang, no XTC_E_DEADLK: a
 *       partitioned peer that can never receive its cross-group messages
 *       must not deadlock the sim);
 *   (c) REPLAY: the same seed yields the identical arrival count, an
 *       order-sensitive arrival hash, and the sim state hash (delivery
 *       under a seeded latency is order-deterministic);
 *   (d) with the partition CLEARED, all 12 sends deliver.
 */

#define N_LOOPS 4
#define GROUP_OF(loop) ((loop) < 2 ? 0 : 1)   /* A = {0,1}, B = {2,3} */

static atomic_int  g_arrived;      /* messages a receiver actually got */
static atomic_long g_arr_hash;     /* order-sensitive fold of arrivals */

static void
fold(long v)
{
	long h = atomic_load_explicit(&g_arr_hash, memory_order_relaxed);
	h = h * 1000003L + (v + 7);
	atomic_store_explicit(&g_arr_hash, h, memory_order_relaxed);
}

/* Receiver arg: how many messages to expect on this loop this run. */
struct recv_arg { int expect; };

static void
receiver(void *arg)
{
	struct recv_arg *ra = arg;
	int got = 0;
	while (got < ra->expect) {
		void *m = NULL;
		size_t n = 0;
		/* Finite timeout: if fewer than `expect` arrive (should not
		 * happen for a correctly-connected receiver, but a robust
		 * receiver must not park forever), we still terminate. */
		int rc = xtc_recv(&m, &n, 50 * 1000 * 1000LL /* 50 ms */);
		if (rc != XTC_OK) {
			if (m != NULL) free(m);
			break;                   /* timed out: stop waiting */
		}
		if (m != NULL) {
			long v;
			memcpy(&v, m, sizeof v < n ? sizeof v : n);
			free(m);
			atomic_fetch_add_explicit(&g_arrived, 1,
			    memory_order_relaxed);
			fold(v);
			got++;
		}
	}
}

/* Sender arg: the receiver pid to hit and a per-send payload tag. */
struct send_arg { xtc_pid_t peer; long tag; };

static void
sender(void *arg)
{
	struct send_arg *sa = arg;
	int tries;
	for (tries = 0; tries < 16; tries++) {
		int rc = xtc_send(sa->peer, &sa->tag, sizeof sa->tag);
		if (rc == XTC_OK)
			return;
		if (rc != XTC_E_AGAIN)
			return;              /* target gone: give up */
		/* AGAIN: soft-full or a partition drop.  Back off a little
		 * (advances virtual time, lets peers run) and retry.  A
		 * partitioned target AGAINs forever, so the bounded retry
		 * count is what keeps this sender from spinning the sim. */
		(void)xtc_proc_sleep(1 * 1000 * 1000LL /* 1 ms */);
	}
}

static struct recv_arg g_recv_arg[N_LOOPS];
static struct send_arg g_send_arg[N_LOOPS * N_LOOPS];

/*
 * Run the workload once.  When `partition` is set, cut A|B (all cross-
 * group edges, both directions) and turn on a seeded delivery latency.
 * Returns the sim run rc; writes the arrival count / hash / state hash.
 */
static int
run_once(uint64_t seed, int partition, int *out_arrived, long *out_hash,
    uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	xtc_pid_t rpid[N_LOOPS];
	int i, j, ns = 0, rc;

	atomic_store(&g_arrived, 0);
	atomic_store(&g_arr_hash, 0);

	xtc_sim_partition_clear();       /* start from a healed network */
	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK)
		return -1;

	/*
	 * Per-loop expected arrival count.  A receiver on loop j expects a
	 * message from every OTHER loop i whose edge (i -> j) is not cut.
	 * loop_id in the partition matrix is exec_id + 1.
	 */
	for (j = 0; j < N_LOOPS; j++) {
		int expect = 0;
		for (i = 0; i < N_LOOPS; i++) {
			if (i == j)
				continue;
			if (partition == 2) {
				/* Asymmetric: only A->B cut; a send i->j is
				 * dropped iff i in group 0 and j in group 1. */
				if (GROUP_OF(i) == 0 && GROUP_OF(j) == 1)
					continue;
			} else if (partition && GROUP_OF(i) != GROUP_OF(j)) {
				continue;        /* this edge will be cut */
			}
			expect++;
		}
		g_recv_arg[j].expect = expect;
	}

	/* Spawn one receiver per loop; capture its pid for the senders. */
	for (j = 0; j < N_LOOPS; j++) {
		xtc_loop_t *l = xtc_exec_loop(e, (unsigned)j);
		if (xtc_proc_spawn(l, receiver, &g_recv_arg[j], NULL,
		    &rpid[j]) != XTC_OK) {
			(void)xtc_exec_fini(e);
			return -1;
		}
	}

	/* Install the partition + a seeded delivery latency BEFORE the
	 * senders run.  Cutting after exec_init but before exec_run means no
	 * message has moved yet -- the whole run observes the same network. */
	if (partition) {
		for (i = 0; i < N_LOOPS; i++)
			for (j = 0; j < N_LOOPS; j++)
				if (GROUP_OF(i) != GROUP_OF(j)) {
					if (partition == 2) {
						/* Asymmetric: cut only A->B
						 * (group 0 -> group 1); leave
						 * B->A open.  A one-way cut is
						 * harder: B still hears from A?
						 * no -- A cannot reach B, but B
						 * can reach A, so replies flow
						 * one way only. */
						if (GROUP_OF(i) == 0)
							xtc_sim_partition_set(
							    i + 1, j + 1, 1);
					} else {
						xtc_sim_partition_set(i + 1,
						    j + 1, 1);
					}
				}
		/* Seeded per-message latency so delivery ORDER across the
		 * surviving pairs is part of the replayable schedule. */
		xtc_sim_net_latency(10 * 1000LL, 500 * 1000LL);
	}

	/* Spawn a sender for every ordered distinct loop pair (i -> j). */
	for (i = 0; i < N_LOOPS; i++) {
		for (j = 0; j < N_LOOPS; j++) {
			xtc_loop_t *l;
			if (i == j)
				continue;
			l = xtc_exec_loop(e, (unsigned)i);
			g_send_arg[ns].peer = rpid[j];
			g_send_arg[ns].tag = (long)(i * 10 + j);
			(void)xtc_proc_spawn(l, sender, &g_send_arg[ns],
			    NULL, NULL);
			ns++;
		}
	}

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_arrived = atomic_load(&g_arrived);
	*out_hash = atomic_load(&g_arr_hash);
	if (out_state != NULL)
		*out_state = xtc_sim_state_hash(e);

	xtc_sim_partition_clear();       /* heal for the next run */
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(void)
{
	int a1 = 0, a2 = 0, an = 0, rc1, rc2, rcn;
	long h1 = 0, h2 = 0, hn = 0;
	uint64_t s1 = 0, s2 = 0, sn = 0;

	/* (a)+(b): run WITH the A|B partition; expect exactly 4 arrivals
	 * (the two within-group cross-loop pairs) and clean quiescence. */
	rc1 = run_once(0x9A27, 1, &a1, &h1, &s1);
	/* (c): replay the identical seeded partitioned run. */
	rc2 = run_once(0x9A27, 1, &a2, &h2, &s2);
	/* (d): no partition -- every one of the 12 cross-loop sends lands. */
	rcn = run_once(0x9A27, 0, &an, &hn, &sn);

	printf("partitioned run1: rc=%d arrived=%d hash=%ld state=%016llx\n",
	    rc1, a1, h1, (unsigned long long)s1);
	printf("partitioned run2: rc=%d arrived=%d hash=%ld state=%016llx\n",
	    rc2, a2, h2, (unsigned long long)s2);
	printf("no-partition run: rc=%d arrived=%d\n", rcn, an);

	/* (b) quiescence: a partitioned peer must not deadlock/hang. */
	if (rc1 != XTC_OK || rc2 != XTC_OK || rcn != XTC_OK) {
		printf("FAIL: a run did not quiesce (rc %d/%d/%d) -- a "
		    "partitioned peer must not deadlock the sim\n",
		    rc1, rc2, rcn);
		return 1;
	}
	/* (a) connected groups still deliver: 0<->1 and 2<->3 = 4 arrivals. */
	if (a1 != 4) {
		printf("FAIL: partitioned run delivered %d, expected 4 "
		    "(the two within-group cross-loop pairs)\n", a1);
		return 1;
	}
	/* (c) replay: identical arrivals + order hash + state hash. */
	if (a1 != a2 || h1 != h2 || s1 != s2) {
		printf("FAIL: partitioned run did not replay "
		    "(arrived %d/%d hash %ld/%ld state %016llx/%016llx)\n",
		    a1, a2, h1, h2,
		    (unsigned long long)s1, (unsigned long long)s2);
		return 1;
	}
	/* (d) healed network: all 12 ordered cross-loop pairs deliver. */
	if (an != 12) {
		printf("FAIL: with partition disabled, %d of 12 cross-loop "
		    "messages delivered\n", an);
		return 1;
	}

	/* (e) ASYMMETRIC one-way partition: cut only A->B, leave B->A open.
	 * The 4 A->B sends drop; the 4 B->A and 4 within-group sends land
	 * (8/12).  The run must still quiesce (an asymmetric cut must not
	 * hang a peer waiting on a reply that can never arrive) and replay. */
	{
		int aa1 = 0, aa2 = 0, rca1, rca2;
		long ha1 = 0, ha2 = 0;
		uint64_t sa1 = 0, sa2 = 0;
		rca1 = run_once(0x9A27, 2, &aa1, &ha1, &sa1);
		rca2 = run_once(0x9A27, 2, &aa2, &ha2, &sa2);
		if (rca1 != XTC_OK || rca2 != XTC_OK) {
			printf("FAIL: asymmetric-partition run did not quiesce "
			    "(rc %d/%d) -- a one-way cut must not deadlock\n",
			    rca1, rca2);
			return 1;
		}
		if (aa1 != 8) {
			printf("FAIL: asymmetric A->B cut delivered %d, expected "
			    "8 (only the 4 A->B sends drop)\n", aa1);
			return 1;
		}
		if (aa1 != aa2 || ha1 != ha2 || sa1 != sa2) {
			printf("FAIL: asymmetric-partition run did not replay\n");
			return 1;
		}
	}

	printf("OK: simulated network partition under DST -- A|B cut drops "
	    "cross-group messages (4/12 land, connected groups intact), the "
	    "run quiesces (no partitioned-peer deadlock) and replays "
	    "identically from seed; healed, all 12 deliver; an asymmetric "
	    "one-way A->B cut lands 8/12 and still quiesces + replays\n");
	return 0;
}
