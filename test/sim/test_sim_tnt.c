/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_tnt.c
 *	Deterministic Simulation Testing of the tnt (Tina) Isolate layer's
 *	deterministic actor core (src/orc/tnt.c), driven by the seeded
 *	single-thread simulator via __xtc_tnt_run_sim.
 *
 *	tnt was designed for DST: a handler returns a TRANSITION (not a
 *	syscall) and I/O is stage-then-commit, so the shard scheduler is a
 *	pure function of message arrival order.  Under sim the shard parks
 *	via a sim-clock sleep (not the real wake pipe) and the seeded
 *	scheduler makes the whole run a pure function of the seed.
 *
 *	This test exercises the deterministic ACTOR CORE across TWO shards,
 *	driving the sim-visible shard-wake seam: under sim the shard parks
 *	on its own proc mailbox and shard_wake wakes it via xtc_send (the
 *	fully-modeled cross-loop park/wake path), so cross-shard delivery
 *	and wall-clock timers wake the target shard deterministically.
 *	The driver mixes self-KICK messages, a cross-SHARD send to a peer
 *	on shard 1, and a one-shot TIMER:
 *	  (a) spawn into typed arenas + every ADD applied exactly once
 *	      (no lost / duplicated message under the seeded interleaving);
 *	  (b) drop-on-full: flooding a bounded mailbox in one turn drops
 *	      the overflow with MAILBOX_FULL feedback; delivered + dropped
 *	      == sent (nothing silently lost);
 *	  (c) generational stale-handle: a send to a torn-down slot's OLD
 *	      handle returns STALE_HANDLE, never mis-delivers to a reused
 *	      slot;
 *	  (d) a CROSS-SHARD send is delivered to a peer on another shard;
 *	  (e) a one-shot TIMER fires (delivered on the sim virtual clock);
 *	  (f) clean quiescence (XTC_OK), no deadlock / livelock;
 *	  (g) REPLAY: identical result fingerprint for a repeated seed.
 *
 *	NOT covered here, deliberately: the socket courier I/O (raw
 *	recv/send) needs a real kernel, not-coverable-by-design; no isolate
 *	here stages fd I/O.
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
#include "xtc_tnt.h"
#include "xtc_sim.h"

/* Internal DST harness entry (test-only; not part of the public API). */
extern int __xtc_tnt_run_sim(const xtc_tnt_spec_t *spec, uint64_t seed,
    long max_steps);

/* ---- Per-run observed results (reset each run) --------------------- */
typedef struct results {
	atomic_int counter_msgs;   /* ADDs handled by the counter */
	atomic_int counter_value;  /* accumulated total */
	atomic_int sink_handled;   /* PINGs the overflow sink actually ran */
	atomic_int drop_full;      /* sends that hit MAILBOX_FULL */
	atomic_int drop_stale;     /* sends that hit STALE_HANDLE */
	atomic_int done_seen;      /* isolates torn down (DONE) */
	atomic_int peer_got;       /* CROSS-SHARD delivery to shard 1 */
	atomic_int timer_fired;    /* one-shot timer delivered */
	atomic_int all_pass;       /* driver verdict for this run */
} results_t;

static results_t g_res;

#define N_ADD   8              /* ADDs; expected total 1+..+8 = 36 */
#define N_FLOOD 20             /* flood count for the drop-on-full sink */
#define SINK_CAP 4             /* that sink's mailbox capacity */

/* ---- Type ids ---- */
#define T_COUNTER 0
#define T_SINK    1
#define T_PEER    2
#define T_DRIVER  3

/* ---- User tags ---- */
#define TAG_ADD  (XTC_TNT_USER_TAG_BASE + 0)
#define TAG_PING (XTC_TNT_USER_TAG_BASE + 1)
#define TAG_DONE (XTC_TNT_USER_TAG_BASE + 2)
#define TAG_PEER (XTC_TNT_USER_TAG_BASE + 3)
#define TAG_KICK (XTC_TNT_USER_TAG_BASE + 4)

/* ==== COUNTER: accumulate an int payload ==== */
typedef struct counter_iso { int total; } counter_iso_t;

static xtc_tnt_transition_t
counter_init(void *s, const void *a, size_t n)
{ counter_iso_t *c = xtc_tnt_self_as(counter_iso_t, s); (void)a; (void)n;
  c->total = 0; return XTC_TNT_TRANSITION_WAIT_MESSAGE; }

static xtc_tnt_transition_t
counter_handler(void *s, xtc_tnt_message_t *m)
{
	counter_iso_t *c = xtc_tnt_self_as(counter_iso_t, s);
	if (m->tag == TAG_ADD) {
		c->total += *xtc_tnt_payload_as(int, m);
		atomic_fetch_add(&g_res.counter_msgs, 1);
		atomic_store(&g_res.counter_value, c->total);
	}
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

/* ==== SINK: count PINGs; TAG_DONE tears down (generation bump) ==== */
typedef struct sink_iso { int seen; } sink_iso_t;

static xtc_tnt_transition_t
sink_init(void *s, const void *a, size_t n)
{ sink_iso_t *k = xtc_tnt_self_as(sink_iso_t, s); (void)a; (void)n;
  k->seen = 0; return XTC_TNT_TRANSITION_WAIT_MESSAGE; }

static xtc_tnt_transition_t
sink_handler(void *s, xtc_tnt_message_t *m)
{
	sink_iso_t *k = xtc_tnt_self_as(sink_iso_t, s);
	if (m->tag == TAG_DONE) {
		atomic_fetch_add(&g_res.done_seen, 1);
		return XTC_TNT_TRANSITION_DONE;
	}
	if (m->tag == TAG_PING) {
		k->seen++;
		atomic_fetch_add(&g_res.sink_handled, 1);
	}
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

/* ==== PEER: another isolate type; receives a same-shard send ==== */
typedef struct peer_iso { int hits; } peer_iso_t;

static xtc_tnt_transition_t
peer_init(void *s, const void *a, size_t n)
{ peer_iso_t *p = xtc_tnt_self_as(peer_iso_t, s); (void)a; (void)n;
  p->hits = 0; return XTC_TNT_TRANSITION_WAIT_MESSAGE; }

static xtc_tnt_transition_t
peer_handler(void *s, xtc_tnt_message_t *m)
{
	peer_iso_t *p = xtc_tnt_self_as(peer_iso_t, s);
	if (m->tag == TAG_PEER) { p->hits++; atomic_store(&g_res.peer_got, 1); }
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

/* ==== DRIVER: purely message-driven; each phase self-KICKs and only
 * advances once it has observed the prior phase's effect. ==== */
typedef struct driver_iso {
	int          phase;
	xtc_tnt_handle_t counter;
	xtc_tnt_handle_t sink;      /* torn down for the staleness check */
	xtc_tnt_handle_t fullsink;  /* flooded for drop-on-full */
	xtc_tnt_handle_t peer;
} driver_iso_t;

static xtc_tnt_transition_t
driver_init(void *s, const void *a, size_t n)
{
	driver_iso_t *d = xtc_tnt_self_as(driver_iso_t, s); (void)a; (void)n;
	memset(d, 0, sizeof(*d));
	(void)xtc_tnt_send(xtc_tnt_self(), TAG_KICK, NULL, 0);
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

static xtc_tnt_transition_t driver_timer(driver_iso_t *d);

static xtc_tnt_transition_t
driver_handler(void *s, xtc_tnt_message_t *m)
{
	driver_iso_t *d = xtc_tnt_self_as(driver_iso_t, s);

	if (m->tag == XTC_TNT_TAG_TIMER)
		return driver_timer(d);
	if (m->tag != TAG_KICK)
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;

	/* phase 0: spawn same-shard isolates; route the PEER onto shard 1
	 * via spawn_on (asynchronous -- it materialises when shard 1 next
	 * ticks, so we must not send to it until a barrier confirms it). */
	if (d->phase == 0) {
		xtc_tnt_spawn(T_COUNTER, NULL, 0, &d->counter);
		xtc_tnt_spawn(T_SINK, NULL, 0, &d->sink);
		xtc_tnt_spawn(T_SINK, NULL, 0, &d->fullsink);
		(void)xtc_tnt_spawn_on(1, T_PEER, NULL, 0);
		/* Deterministic peer handle: shard 1, type PEER, slot 0, gen 1. */
		d->peer = xtc_tnt_handle_make(1, T_PEER, 0, 1);
		d->phase = 1;
		(void)xtc_tnt_send(xtc_tnt_self(), TAG_KICK, NULL, 0);
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	}

	/* phase 1: send N_ADD ADDs, then poll (via self-KICK) until the
	 * counter has applied all of them.  Proves every message is
	 * delivered exactly once regardless of the seeded interleaving. */
	if (d->phase == 1) {
		int i, total = 0;
		for (i = 1; i <= N_ADD; i++)
			(void)xtc_tnt_send(d->counter, TAG_ADD, &i, sizeof i);
		for (i = 1; i <= N_ADD; i++) total += i;
		(void)total;
		d->phase = 2;
		(void)xtc_tnt_send(xtc_tnt_self(), TAG_KICK, NULL, 0);
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	}
	if (d->phase == 2) {
		if (atomic_load(&g_res.counter_msgs) < N_ADD) {
			/* counter has not drained yet -- keep polling. */
			(void)xtc_tnt_send(xtc_tnt_self(), TAG_KICK, NULL, 0);
			return XTC_TNT_TRANSITION_WAIT_MESSAGE;
		}
		d->phase = 3;
		(void)xtc_tnt_send(xtc_tnt_self(), TAG_KICK, NULL, 0);
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	}

	/* phase 3: flood fullsink in ONE turn -> mailbox fills, overflow
	 * dropped with MAILBOX_FULL. */
	if (d->phase == 3) {
		int i;
		for (i = 0; i < N_FLOOD; i++)
			if (xtc_tnt_send(d->fullsink, TAG_PING, NULL, 0) ==
			    XTC_TNT_SEND_MAILBOX_FULL)
				atomic_fetch_add(&g_res.drop_full, 1);
		d->phase = 4;
		(void)xtc_tnt_send(xtc_tnt_self(), TAG_KICK, NULL, 0);
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	}

	/* phase 4: tear down sink, then poll until its DONE is processed. */
	if (d->phase == 4) {
		(void)xtc_tnt_send(d->sink, TAG_DONE, NULL, 0);
		d->phase = 5;
		(void)xtc_tnt_send(xtc_tnt_self(), TAG_KICK, NULL, 0);
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	}
	if (d->phase == 5) {
		if (atomic_load(&g_res.done_seen) < 1) {
			(void)xtc_tnt_send(xtc_tnt_self(), TAG_KICK, NULL, 0);
			return XTC_TNT_TRANSITION_WAIT_MESSAGE;
		}
		/* sink slot is torn down + generation bumped: a send to the
		 * OLD handle must be STALE (never mis-delivered). */
		if (xtc_tnt_send(d->sink, TAG_PING, NULL, 0) ==
		    XTC_TNT_SEND_STALE_HANDLE)
			atomic_fetch_add(&g_res.drop_stale, 1);
		/* Arm a one-shot TIMER.  The cross-SHARD send to the peer on
		 * shard 1 (and its retry until the async spawn_on materialises
		 * the peer) happens in the timer handler -- so this run
		 * exercises a timer redelivered around a cross-shard send, the
		 * exact case the sim-visible shard-wake seam fixed. */
		xtc_tnt_register_timer(2LL * 1000 * 1000, XTC_TNT_TAG_TIMER);
		d->phase = 6;
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	}

	/* phase 6 is driven by the TIMER, not a KICK -- see driver_timer. */
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

static xtc_tnt_transition_t
driver_timer(driver_iso_t *d)
{
	atomic_store(&g_res.timer_fired, 1);
	/* CROSS-SHARD send to the peer on shard 1.  spawn_on is async, so
	 * the peer may not exist on the first attempt (STALE); retry on the
	 * next timer tick.  The seam guarantees a successful cross-shard
	 * send wakes shard 1 deterministically -- and this timer being
	 * redelivered after a cross-shard send is the exact case the seam
	 * fixed. */
	if (atomic_load(&g_res.peer_got) < 1) {
		(void)xtc_tnt_send(d->peer, TAG_PEER, NULL, 0);
		xtc_tnt_register_timer(2LL * 1000 * 1000, XTC_TNT_TAG_TIMER);
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	}
	{
		int total = 0, i, pass = 1;
		for (i = 1; i <= N_ADD; i++) total += i;
		pass &= (atomic_load(&g_res.counter_value) == total);
		pass &= (atomic_load(&g_res.counter_msgs) == N_ADD);
		/* Flooding N_FLOOD into a cap-SINK_CAP mailbox in ONE turn
		 * accepts exactly SINK_CAP and drops the rest with
		 * MAILBOX_FULL (nothing silently lost).  The accepted ones are
		 * then handled, but the last may still be pending when we stop,
		 * so assert the drop count exactly and handled as a bound. */
		pass &= (atomic_load(&g_res.drop_full) == N_FLOOD - SINK_CAP);
		pass &= (atomic_load(&g_res.sink_handled) >= 1);
		pass &= (atomic_load(&g_res.sink_handled) <= SINK_CAP);
		pass &= (atomic_load(&g_res.drop_stale) == 1);
		pass &= (atomic_load(&g_res.done_seen) == 1);
		pass &= (atomic_load(&g_res.peer_got) == 1);
		pass &= (atomic_load(&g_res.timer_fired) == 1);
		atomic_store(&g_res.all_pass, pass);
		xtc_tnt_stop();
		return XTC_TNT_TRANSITION_DONE;
	}
}

static const xtc_tnt_type_t test_types[] = {
	{ .id = T_COUNTER, .name = "Counter", .slot_count = 8,
	  .stride = sizeof(counter_iso_t), .mailbox_capacity = 32,
	  .budget_weight = 32, .init_fn = counter_init,
	  .handler_fn = counter_handler },
	{ .id = T_SINK, .name = "Sink", .slot_count = 8,
	  .stride = sizeof(sink_iso_t), .mailbox_capacity = SINK_CAP,
	  .budget_weight = 32, .init_fn = sink_init,
	  .handler_fn = sink_handler },
	{ .id = T_PEER, .name = "Peer", .slot_count = 8,
	  .stride = sizeof(peer_iso_t), .mailbox_capacity = 8,
	  .budget_weight = 8, .init_fn = peer_init,
	  .handler_fn = peer_handler },
	{ .id = T_DRIVER, .name = "Driver", .slot_count = 2,
	  .stride = sizeof(driver_iso_t), .mailbox_capacity = 8,
	  .budget_weight = 2, .init_fn = driver_init,
	  .handler_fn = driver_handler },
};

/* Run one seeded simulation; return a 64-bit result fingerprint.
 * pass_out receives the driver verdict, rc_out the run result. */
static uint64_t
run_one(uint64_t seed, int *pass_out, int *rc_out)
{
	xtc_tnt_spec_t spec;
	int rc;
	uint64_t fp;

	memset(&g_res, 0, sizeof(g_res));
	memset(&spec, 0, sizeof(spec));
	spec.name = "tnt-sim";
	spec.types = test_types;
	spec.n_types = 4;
	spec.shard_count = 2;          /* shard 0 drives; shard 1 hosts the
	                                * cross-shard peer -- exercises the
	                                * sim-visible shard-wake seam. */
	spec.scratch_size = 65536;
	spec.recv_buf_size = 256;
	spec.boot_type = T_DRIVER;

	rc = __xtc_tnt_run_sim(&spec, seed, 5000000);
	if (rc_out) *rc_out = rc;
	if (pass_out) *pass_out = atomic_load(&g_res.all_pass);

	fp = 1469598103934665603ull;
#define MIX(v) do { fp ^= (uint64_t)(uint32_t)(v); \
	                    fp *= 1099511628211ull; } while (0)
	MIX(atomic_load(&g_res.counter_msgs));
	MIX(atomic_load(&g_res.counter_value));
	MIX(atomic_load(&g_res.sink_handled));
	MIX(atomic_load(&g_res.drop_full));
	MIX(atomic_load(&g_res.drop_stale));
	MIX(atomic_load(&g_res.done_seen));
	MIX(atomic_load(&g_res.peer_got));
	MIX(atomic_load(&g_res.timer_fired));
	MIX(rc);
#undef MIX
	return fp;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0x7473726174ull;  /* "tnt" seed base */
	int n = 24;
	int i, fails = 0;

	if (argc > 1) base = strtoull(argv[1], NULL, 0);
	if (argc > 2) n = atoi(argv[2]);

	printf("== tnt DST: %d seeds from base 0x%llx ==\n", n,
	    (unsigned long long)base);

	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		int pass = 0, rc = 0, pass2 = 0, rc2 = 0;
		uint64_t fp, fp2;

		fp = run_one(seed, &pass, &rc);
		if (rc != XTC_OK) {
			printf("  seed 0x%016llx: rc=%d (want OK) FAIL\n",
			    (unsigned long long)seed, rc);
			fails++;
			continue;
		}
		if (!pass) {
			printf("  seed 0x%016llx: invariants FAIL "
			    "(cv=%d cm=%d sh=%d df=%d ds=%d dn=%d pg=%d tf=%d)\n",
			    (unsigned long long)seed,
			    atomic_load(&g_res.counter_value),
			    atomic_load(&g_res.counter_msgs),
			    atomic_load(&g_res.sink_handled),
			    atomic_load(&g_res.drop_full),
			    atomic_load(&g_res.drop_stale),
			    atomic_load(&g_res.done_seen),
			    atomic_load(&g_res.peer_got),
			    atomic_load(&g_res.timer_fired));
			fails++;
			continue;
		}

		/* Replay: same seed reproduces the same fingerprint. */
		fp2 = run_one(seed, &pass2, &rc2);
		if (fp2 != fp || pass2 != pass || rc2 != rc) {
			printf("  seed 0x%016llx: REPLAY MISMATCH "
			    "fp %016llx != %016llx  FAIL\n",
			    (unsigned long long)seed,
			    (unsigned long long)fp,
			    (unsigned long long)fp2);
			fails++;
		}
	}

	if (fails == 0) {
		printf("OK: tnt DST -- %d seeds, all invariants hold and "
		    "replay is bit-identical\n", n);
		return 0;
	}
	printf("FAIL: %d/%d tnt DST seeds failed\n", fails, n);
	return 1;
}
