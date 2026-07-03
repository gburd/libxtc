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
#include "xtc_async.h"     /* xtc_yield */
#include "xtc_sim.h"

/*
 * DST coverage of the L3 channels (src/ptc/chan.c) BEYOND the mpsc
 * buggify site already covered by test_sim_buggify2.  Three variants,
 * each driven under the seeded deterministic scheduler with seeded
 * producer/consumer interleavings + replay:
 *
 *   mpmc  -- a bounded multi-producer / multi-consumer queue.  P
 *            producers each enqueue a disjoint contiguous block of
 *            integers; C consumers drain until the channel is closed
 *            and empty.  INVARIANT: every produced item is consumed
 *            EXACTLY ONCE (the received multiset == the produced
 *            multiset; no drop, no duplicate).  The delivery ORDER
 *            across producers/consumers is part of the replayable
 *            schedule.
 *
 *   watch -- a single-slot latest-value channel.  A writer publishes a
 *            monotonic sequence of values; a reader samples.  INVARIANT:
 *            every value a reader observes is one the writer actually
 *            published, and the reader never sees a value regress below
 *            the last it saw (watch keeps the LATEST, monotonic here).
 *
 *   broadcast -- a lossy multi-receiver ring: one sender, R subscribers.
 *            Each subscriber that keeps up sees a monotonically
 *            increasing sequence; a lagging one is told how many it
 *            lost (the "lagged" count) and resumes at the oldest
 *            readable slot.  INVARIANT: what a subscriber DOES see is a
 *            contiguous suffix of the published sequence (never a value
 *            the sender did not publish, never out of order).
 *
 * Channels have no fiber-park primitive (try_send / try_recv are
 * non-blocking, recv is a snapshot), so producers/consumers POLL and
 * xtc_yield() back to the scheduler between attempts -- the seeded
 * scheduler then owns the interleaving.  Each proc is bounded (a step
 * cap) so a stuck channel surfaces as a missing item, never a hang.
 *
 * Each variant asserts: (a) quiescence (rc == XTC_OK), (b) its delivery
 * invariant, (c) byte-identical replay from the seed (a set/order hash
 * plus the sim state hash), (d) a different seed reorders but stays
 * consistent.  Footprint is small (few producers/consumers, tiny item
 * counts, per-run free) so the suite stays memory-bounded.
 */

#define N_LOOPS 4

/* Encode a small positive int as a non-NULL void* (channels reject
 * NULL as the empty sentinel). */
#define IPTR(i)   ((void *)(intptr_t)((i) + 1))
#define IVAL(p)   ((int)(intptr_t)(p) - 1)

/* ================= mpmc ================= */

#define MPMC_PROD   3
#define MPMC_CONS   2
#define MPMC_PER    6        /* items per producer */
#define MPMC_TOTAL  (MPMC_PROD * MPMC_PER)

static xtc_chan_mpmc_t *g_mpmc;
static atomic_int  g_mpmc_produced;
static atomic_int  g_mpmc_consumed;
static atomic_int  g_mpmc_seen[MPMC_TOTAL];   /* per-item consume count */
static atomic_int  g_mpmc_prod_done;
static atomic_long g_mpmc_hash;               /* ORDER-sensitive fold */

static void
mpmc_fold(int v)
{
	long h = atomic_load_explicit(&g_mpmc_hash, memory_order_relaxed);
	h = h * 1000003L + (v + 1);
	atomic_store_explicit(&g_mpmc_hash, h, memory_order_relaxed);
}

static void
mpmc_producer(void *arg)
{
	int base = (int)(intptr_t)arg;     /* first item id */
	int i = 0, guard = 0;
	while (i < MPMC_PER && guard++ < 200000) {
		if (xtc_chan_mpmc_try_send(g_mpmc, IPTR(base + i)) == XTC_OK) {
			atomic_fetch_add_explicit(&g_mpmc_produced, 1,
			    memory_order_relaxed);
			i++;
		} else {
			xtc_yield();       /* full -- let a consumer drain */
		}
	}
	if (atomic_fetch_add_explicit(&g_mpmc_prod_done, 1,
	    memory_order_relaxed) + 1 == MPMC_PROD)
		(void)xtc_chan_mpmc_close(g_mpmc);   /* last producer closes */
}

static void
mpmc_consumer(void *arg)
{
	int guard = 0;
	(void)arg;
	while (guard++ < 400000) {
		void *v = NULL;
		int rc = xtc_chan_mpmc_try_recv(g_mpmc, &v);
		if (rc == XTC_OK) {
			int id = IVAL(v);
			if (id >= 0 && id < MPMC_TOTAL)
				atomic_fetch_add_explicit(&g_mpmc_seen[id], 1,
				    memory_order_relaxed);
			atomic_fetch_add_explicit(&g_mpmc_consumed, 1,
			    memory_order_relaxed);
			mpmc_fold(id);
		} else if (rc == XTC_E_INVAL) {
			break;             /* closed and drained */
		} else {
			xtc_yield();       /* empty -- let a producer fill */
		}
	}
}

static int
run_mpmc(uint64_t seed, int *out_prod, int *out_cons, int *out_dupdrop,
    long *out_hash, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc, dupdrop = 0;

	atomic_store(&g_mpmc_produced, 0);
	atomic_store(&g_mpmc_consumed, 0);
	atomic_store(&g_mpmc_prod_done, 0);
	atomic_store(&g_mpmc_hash, 0);
	for (i = 0; i < MPMC_TOTAL; i++)
		atomic_store(&g_mpmc_seen[i], 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	/* Small cap so producers actually block on a full channel and the
	 * backpressure/retry interleaving is exercised. */
	if (xtc_chan_mpmc_create(NULL, 4, &g_mpmc) != XTC_OK) {
		(void)xtc_exec_fini(e); return -1;
	}
	for (i = 0; i < MPMC_PROD; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(i % N_LOOPS)),
		    mpmc_producer, (void *)(intptr_t)(i * MPMC_PER), NULL, NULL);
	for (i = 0; i < MPMC_CONS; i++)
		(void)xtc_proc_spawn(
		    xtc_exec_loop(e, (unsigned)((i + 1) % N_LOOPS)),
		    mpmc_consumer, NULL, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	for (i = 0; i < MPMC_TOTAL; i++)
		if (atomic_load(&g_mpmc_seen[i]) != 1) dupdrop++;

	*out_prod = atomic_load(&g_mpmc_produced);
	*out_cons = atomic_load(&g_mpmc_consumed);
	*out_dupdrop = dupdrop;
	*out_hash = atomic_load(&g_mpmc_hash);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	xtc_chan_mpmc_destroy(g_mpmc);
	g_mpmc = NULL;
	(void)xtc_exec_fini(e);
	return rc;
}

/* ================= watch ================= */

#define WATCH_N 12

static xtc_chan_watch_t *g_watch;
static atomic_int  g_watch_writes;
static atomic_int  g_watch_reads;
static atomic_int  g_watch_regress;      /* set if a reader saw a value < last */
static atomic_int  g_watch_bogus;        /* set if a reader saw an unpublished value */

static void
watch_writer(void *arg)
{
	int i;
	(void)arg;
	for (i = 1; i <= WATCH_N; i++) {
		(void)xtc_chan_watch_send(g_watch, IPTR(i)); /* value i */
		atomic_fetch_add_explicit(&g_watch_writes, 1,
		    memory_order_relaxed);
		xtc_yield();
	}
}

static void
watch_reader(void *arg)
{
	int guard = 0, last = 0;
	(void)arg;
	while (guard++ < 4000) {
		void *v = NULL;
		if (xtc_chan_watch_recv(g_watch, &v) == XTC_OK) {
			int val = IVAL(v);
			if (val < 1 || val > WATCH_N)
				atomic_store_explicit(&g_watch_bogus, 1,
				    memory_order_relaxed);
			if (val < last)
				atomic_store_explicit(&g_watch_regress, 1,
				    memory_order_relaxed);
			last = val;
			atomic_fetch_add_explicit(&g_watch_reads, 1,
			    memory_order_relaxed);
			if (val == WATCH_N) break;   /* saw the final value */
		}
		xtc_yield();
	}
}

static int
run_watch(uint64_t seed, int *out_reads, int *out_bad, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int rc;

	atomic_store(&g_watch_writes, 0);
	atomic_store(&g_watch_reads, 0);
	atomic_store(&g_watch_regress, 0);
	atomic_store(&g_watch_bogus, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	if (xtc_chan_watch_create(NULL, &g_watch) != XTC_OK) {
		(void)xtc_exec_fini(e); return -1;
	}
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), watch_writer, NULL, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 1), watch_reader, NULL, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 2), watch_reader, NULL, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_reads = atomic_load(&g_watch_reads);
	*out_bad = atomic_load(&g_watch_regress) | atomic_load(&g_watch_bogus);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	xtc_chan_watch_destroy(g_watch);
	g_watch = NULL;
	(void)xtc_exec_fini(e);
	return rc;
}

/* ================= broadcast ================= */

#define BC_N     16          /* messages published */
#define BC_SUBS  3

static xtc_chan_broadcast_t *g_bc;
static atomic_int  g_bc_recv;            /* total messages received */
static atomic_int  g_bc_bad;             /* out-of-order / unpublished */
static atomic_long g_bc_hash;

static void
bc_fold(int v)
{
	long h = atomic_load_explicit(&g_bc_hash, memory_order_relaxed);
	h = h * 1000003L + (v + 1);
	atomic_store_explicit(&g_bc_hash, h, memory_order_relaxed);
}

/*
 * A subscriber: subscribes, then drains until it has seen the last
 * published value (BC_N) or a bounded step cap.  It checks that each
 * value it receives is a published one (1..BC_N) and strictly greater
 * than the previous it saw (broadcast delivers in publish order; a lag
 * jump skips ahead but never goes backward).  It subscribes BEFORE the
 * sender starts (spawned first, sender sleeps a tick) so cursor 0.
 */
struct bc_arg { int id; };

static void
bc_subscriber(void *arg)
{
	struct bc_arg *a = arg;
	xtc_chan_broadcast_recv_t *r = NULL;
	int guard = 0, last = 0;
	if (xtc_chan_broadcast_subscribe(g_bc, &r) != XTC_OK) return;
	while (guard++ < 20000) {
		void *v = NULL;
		int lagged = 0;
		int rc = xtc_chan_broadcast_recv(r, &v, &lagged);
		if (rc == XTC_OK) {
			int val = IVAL(v);
			if (val < 1 || val > BC_N ||
			    val <= last /* strictly increasing */)
				atomic_store_explicit(&g_bc_bad, 1,
				    memory_order_relaxed);
			last = val;
			atomic_fetch_add_explicit(&g_bc_recv, 1,
			    memory_order_relaxed);
			bc_fold(val * 8 + a->id);
			if (val == BC_N) break;
		} else {
			/* Empty: SLEEP (not yield) so the virtual clock advances
			 * and the sender's sleep timer + sends get scheduled --
			 * a busy xtc_yield would keep this proc runnable forever
			 * and starve the clock (no timer would ever fire). */
			(void)xtc_proc_sleep(200 * 1000LL /* 200 us */);
		}
	}
	xtc_chan_broadcast_unsubscribe(r);
}

static void
bc_sender(void *arg)
{
	int i;
	(void)arg;
	/* Let subscribers register first (their cursor should be 0). */
	(void)xtc_proc_sleep(1 * 1000 * 1000LL);
	for (i = 1; i <= BC_N; i++) {
		(void)xtc_chan_broadcast_send(g_bc, IPTR(i)); /* value i */
		xtc_yield();
	}
}

static int
run_broadcast(uint64_t seed, int *out_recv, int *out_bad, long *out_hash,
    uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc;
	static struct bc_arg args[BC_SUBS];

	atomic_store(&g_bc_recv, 0);
	atomic_store(&g_bc_bad, 0);
	atomic_store(&g_bc_hash, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	/* Ring bigger than BC_N so a keeping-up subscriber never lags,
	 * giving a clean "contiguous full sequence" invariant. */
	if (xtc_chan_broadcast_create(NULL, 32, &g_bc) != XTC_OK) {
		(void)xtc_exec_fini(e); return -1;
	}
	for (i = 0; i < BC_SUBS; i++) {
		args[i].id = i;
		(void)xtc_proc_spawn(
		    xtc_exec_loop(e, (unsigned)(i % N_LOOPS)),
		    bc_subscriber, &args[i], NULL, NULL);
	}
	(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(N_LOOPS - 1)),
	    bc_sender, NULL, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_recv = atomic_load(&g_bc_recv);
	*out_bad = atomic_load(&g_bc_bad);
	*out_hash = atomic_load(&g_bc_hash);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	xtc_chan_broadcast_destroy(g_bc);
	g_bc = NULL;
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(void)
{
	/* ---- mpmc ---- */
	int p1 = 0, c1 = 0, dd1 = 0, p2 = 0, c2 = 0, dd2 = 0;
	int p3 = 0, c3 = 0, dd3 = 0;
	long mh1 = 0, mh2 = 0, mh3 = 0;
	uint64_t ms1 = 0, ms2 = 0, ms3 = 0;
	int rc;

	rc = run_mpmc(0xC4A11101, &p1, &c1, &dd1, &mh1, &ms1);
	if (rc != XTC_OK) { printf("FAIL: mpmc run rc=%d (hang?)\n", rc); return 1; }
	(void)run_mpmc(0xC4A11101, &p2, &c2, &dd2, &mh2, &ms2);
	rc = run_mpmc(0x0FF51DE3, &p3, &c3, &dd3, &mh3, &ms3);
	if (rc != XTC_OK) { printf("FAIL: mpmc diff-seed rc=%d\n", rc); return 1; }

	printf("mpmc  run1: prod=%d cons=%d dup/drop=%d hash=%ld state=%016llx\n",
	    p1, c1, dd1, mh1, (unsigned long long)ms1);
	printf("mpmc  run2: prod=%d cons=%d dup/drop=%d hash=%ld state=%016llx\n",
	    p2, c2, dd2, mh2, (unsigned long long)ms2);
	printf("mpmc  run3 (diff seed): prod=%d cons=%d dup/drop=%d\n",
	    p3, c3, dd3);

	if (p1 != MPMC_TOTAL || c1 != MPMC_TOTAL || dd1 != 0) {
		printf("FAIL: mpmc lost/duplicated items (prod=%d cons=%d "
		    "want %d, dup/drop=%d) -- delivery invariant broken\n",
		    p1, c1, MPMC_TOTAL, dd1);
		return 1;
	}
	if (c1 != c2 || mh1 != mh2 || ms1 != ms2 || dd2 != 0) {
		printf("FAIL: mpmc did not replay (cons %d/%d hash %ld/%ld "
		    "state %016llx/%016llx)\n", c1, c2, mh1, mh2,
		    (unsigned long long)ms1, (unsigned long long)ms2);
		return 1;
	}
	if (p3 != MPMC_TOTAL || c3 != MPMC_TOTAL || dd3 != 0) {
		printf("FAIL: mpmc diff-seed lost/duplicated items "
		    "(prod=%d cons=%d dup/drop=%d)\n", p3, c3, dd3);
		return 1;
	}

	/* ---- watch ---- */
	int wr1 = 0, wb1 = 0, wr2 = 0, wb2 = 0, wr3 = 0, wb3 = 0;
	uint64_t ws1 = 0, ws2 = 0, ws3 = 0;
	rc = run_watch(0x3AC44201, &wr1, &wb1, &ws1);
	if (rc != XTC_OK) { printf("FAIL: watch run rc=%d\n", rc); return 1; }
	(void)run_watch(0x3AC44201, &wr2, &wb2, &ws2);
	rc = run_watch(0x9E110FF5, &wr3, &wb3, &ws3);
	if (rc != XTC_OK) { printf("FAIL: watch diff-seed rc=%d\n", rc); return 1; }

	printf("watch run1: reads=%d bad=%d state=%016llx\n",
	    wr1, wb1, (unsigned long long)ws1);
	printf("watch run3 (diff seed): reads=%d bad=%d\n", wr3, wb3);

	if (wb1 != 0 || wb3 != 0) {
		printf("FAIL: watch reader saw an unpublished or regressing "
		    "value (bad=%d/%d) -- latest-value invariant broken\n",
		    wb1, wb3);
		return 1;
	}
	if (wr1 <= 0) { printf("FAIL: watch reader saw nothing\n"); return 1; }
	if (wr1 != wr2 || ws1 != ws2) {
		printf("FAIL: watch did not replay (reads %d/%d state "
		    "%016llx/%016llx)\n", wr1, wr2,
		    (unsigned long long)ws1, (unsigned long long)ws2);
		return 1;
	}

	/* ---- broadcast ---- */
	int br1 = 0, bb1 = 0, br2 = 0, bb2 = 0, br3 = 0, bb3 = 0;
	long bh1 = 0, bh2 = 0, bh3 = 0;
	uint64_t bs1 = 0, bs2 = 0, bs3 = 0;
	rc = run_broadcast(0xB0AD0001, &br1, &bb1, &bh1, &bs1);
	if (rc != XTC_OK) { printf("FAIL: broadcast run rc=%d\n", rc); return 1; }
	(void)run_broadcast(0xB0AD0001, &br2, &bb2, &bh2, &bs2);
	rc = run_broadcast(0x5E11B0AD, &br3, &bb3, &bh3, &bs3);
	if (rc != XTC_OK) { printf("FAIL: broadcast diff-seed rc=%d\n", rc); return 1; }

	printf("bcast run1: recv=%d bad=%d hash=%ld state=%016llx\n",
	    br1, bb1, bh1, (unsigned long long)bs1);
	printf("bcast run3 (diff seed): recv=%d bad=%d\n", br3, bb3);

	if (bb1 != 0 || bb3 != 0) {
		printf("FAIL: broadcast subscriber saw an unpublished / "
		    "out-of-order value (bad=%d/%d)\n", bb1, bb3);
		return 1;
	}
	/* Ring (32) >= BC_N (16), sender lets subscribers register first,
	 * so every subscriber should see the full sequence: BC_SUBS*BC_N. */
	if (br1 != BC_SUBS * BC_N) {
		printf("FAIL: broadcast did not deliver the full sequence to "
		    "every subscriber (recv=%d want %d)\n",
		    br1, BC_SUBS * BC_N);
		return 1;
	}
	if (br1 != br2 || bh1 != bh2 || bs1 != bs2) {
		printf("FAIL: broadcast did not replay (recv %d/%d hash "
		    "%ld/%ld state %016llx/%016llx)\n", br1, br2, bh1, bh2,
		    (unsigned long long)bs1, (unsigned long long)bs2);
		return 1;
	}
	if (br3 != BC_SUBS * BC_N) {
		printf("FAIL: broadcast diff-seed did not deliver full "
		    "sequence (recv=%d want %d)\n", br3, BC_SUBS * BC_N);
		return 1;
	}

	printf("OK: channels under DST -- mpmc delivers every item exactly "
	    "once (%d/%d, no dup/drop), watch never regresses or fabricates "
	    "(%d reads), broadcast delivers the full ordered sequence to "
	    "every subscriber (%d), all quiesce and replay byte-identically "
	    "from the seed; a different seed reorders while holding each "
	    "invariant\n", c1, MPMC_TOTAL, wr1, br1);
	return 0;
}
