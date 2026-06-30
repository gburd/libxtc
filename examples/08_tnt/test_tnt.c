/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * examples/08_tnt/test_tnt.c
 *	In-process dispatch test for the tnt Isolate layer.  No network.
 *	Proves, end to end on the real shard scheduler:
 *	  1. spawn isolates into a typed arena,
 *	  2. send messages and see handlers run (the dispatch loop +
 *	     WAIT_MESSAGE / YIELD / DONE transitions),
 *	  3. drop-on-full bounded mailbox with sender feedback,
 *	  4. generational-handle staleness after teardown,
 *	  5. one-shot timers delivering back to the Isolate.
 *
 *	The test isolates record into a process-global results struct
 *	(atomics), and a "driver" isolate spawned at boot orchestrates
 *	the scenario from inside a shard, then calls tnt_stop.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "tnt.h"
#include "os_time.h"

/* ---- Shared results -------------------------------------------------
 * Written by handlers (on the shard thread) and the driver; read by
 * main after the runtime stops.  Single shard in this test, so the
 * shard fiber is the only writer of most fields. */
typedef struct results {
	atomic_int counter_msgs;      /* COUNTER isolate: messages handled */
	atomic_int counter_value;     /* COUNTER isolate: accumulated value */
	atomic_int timer_fired;       /* TIMER isolate: timer delivered */
	atomic_int sink_handled;      /* SINK isolate: messages handled */
	atomic_int drop_full;         /* sends that hit MAILBOX_FULL */
	atomic_int drop_stale;        /* sends that hit STALE_HANDLE */
	atomic_int send_ok;           /* sends that succeeded */
	atomic_int done_seen;         /* SINK isolates torn down (DONE) */
	atomic_int all_pass;          /* set by driver: 1 = scenario passed */
} results_t;

static results_t g_res;

/* ---- Type ids ---- */
#define T_COUNTER 0
#define T_SINK    1
#define T_TIMER   2
#define T_DRIVER  3

/* ---- User tags ---- */
#define TAG_ADD       (TNT_USER_TAG_BASE + 0)  /* COUNTER: add payload */
#define TAG_PING      (TNT_USER_TAG_BASE + 1)  /* SINK: count + maybe done */
#define TAG_DONE      (TNT_USER_TAG_BASE + 2)  /* SINK: tear down (DONE) */
#define TAG_ARMTIMER  (TNT_USER_TAG_BASE + 3)  /* TIMER: arm a timer */
#define TAG_KICK      (TNT_USER_TAG_BASE + 4)  /* DRIVER: run the scenario */

/* ====================================================================
 * COUNTER isolate -- accumulates an int payload across messages.
 * ==================================================================== */
typedef struct counter_iso {
	int total;
} counter_iso_t;

static tnt_transition_t
counter_init(void *self_raw, const void *args, size_t n)
{
	counter_iso_t *self = tnt_self_as(counter_iso_t, self_raw);
	(void)args; (void)n;
	self->total = 0;
	return TNT_TRANSITION_WAIT_MESSAGE;
}

static tnt_transition_t
counter_handler(void *self_raw, tnt_message_t *msg)
{
	counter_iso_t *self = tnt_self_as(counter_iso_t, self_raw);

	if (msg->tag == TAG_ADD) {
		int v = *tnt_payload_as(int, msg);
		self->total += v;
		atomic_fetch_add(&g_res.counter_msgs, 1);
		atomic_store(&g_res.counter_value, self->total);
	}
	return TNT_TRANSITION_WAIT_MESSAGE;
}

/* ====================================================================
 * SINK isolate -- counts PINGs; on TAG_DONE it returns DONE (teardown,
 * generation bump).  Used for the mailbox-full and staleness tests.
 * ==================================================================== */
typedef struct sink_iso {
	int seen;
} sink_iso_t;

static tnt_transition_t
sink_init(void *self_raw, const void *args, size_t n)
{
	sink_iso_t *self = tnt_self_as(sink_iso_t, self_raw);
	(void)args; (void)n;
	self->seen = 0;
	return TNT_TRANSITION_WAIT_MESSAGE;
}

static tnt_transition_t
sink_handler(void *self_raw, tnt_message_t *msg)
{
	sink_iso_t *self = tnt_self_as(sink_iso_t, self_raw);

	if (msg->tag == TAG_DONE) {
		atomic_fetch_add(&g_res.done_seen, 1);
		return TNT_TRANSITION_DONE;
	}
	if (msg->tag == TAG_PING) {
		self->seen++;
		atomic_fetch_add(&g_res.sink_handled, 1);
	}
	return TNT_TRANSITION_WAIT_MESSAGE;
}

/* ====================================================================
 * TIMER isolate -- arms a one-shot timer, sets timer_fired on delivery.
 * ==================================================================== */
typedef struct timer_iso {
	int armed;
} timer_iso_t;

static tnt_transition_t
timer_init(void *self_raw, const void *args, size_t n)
{
	timer_iso_t *self = tnt_self_as(timer_iso_t, self_raw);
	(void)args; (void)n;
	self->armed = 0;
	return TNT_TRANSITION_WAIT_MESSAGE;
}

static tnt_transition_t
timer_handler(void *self_raw, tnt_message_t *msg)
{
	timer_iso_t *self = tnt_self_as(timer_iso_t, self_raw);

	if (msg->tag == TAG_ARMTIMER) {
		self->armed = 1;
		tnt_register_timer(20LL * 1000 * 1000 /* 20ms */, TNT_TAG_TIMER);
		return TNT_TRANSITION_WAIT_MESSAGE;
	}
	if (msg->tag == TNT_TAG_TIMER) {
		atomic_store(&g_res.timer_fired, 1);
		return TNT_TRANSITION_WAIT_MESSAGE;
	}
	return TNT_TRANSITION_WAIT_MESSAGE;
}

/* ====================================================================
 * DRIVER isolate -- runs the whole scenario from inside the shard, so
 * every tnt_* call happens in a valid handler context.  It spawns the
 * other isolates, exercises sends, verifies drop-on-full + staleness,
 * arms a timer, then waits (via re-arming a short timer) for the timer
 * to fire, checks results, and stops the runtime.
 * ==================================================================== */
typedef struct driver_iso {
	int           phase;
	tnt_handle_t  counter;
	tnt_handle_t  sink;        /* the sink we tear down for staleness */
	tnt_handle_t  fullsink;    /* the sink we overflow for drop-on-full */
	tnt_handle_t  timer;
} driver_iso_t;

static tnt_transition_t
driver_init(void *self_raw, const void *args, size_t n)
{
	driver_iso_t *self = tnt_self_as(driver_iso_t, self_raw);
	(void)args; (void)n;
	memset(self, 0, sizeof(*self));
	self->phase = 0;
	/* Kick ourselves to start phase 0 on the next tick. */
	(void)tnt_send(tnt_self(), TAG_KICK, NULL, 0);
	return TNT_TRANSITION_WAIT_MESSAGE;
}

static tnt_transition_t
driver_handler(void *self_raw, tnt_message_t *msg)
{
	driver_iso_t *self = tnt_self_as(driver_iso_t, self_raw);

	if (msg->tag == TNT_TAG_TIMER) {
		/* phase 4: the sink's TAG_DONE has now been processed, so the
		 * sink slot is torn down and its generation bumped.  A send to
		 * the OLD handle must be STALE. */
		if (self->phase == 4) {
			tnt_send_result_t r =
			    tnt_send(self->sink, TAG_PING, NULL, 0);
			if (r == TNT_SEND_STALE_HANDLE)
				atomic_fetch_add(&g_res.drop_stale, 1);
			/* Now arm the real timer on the timer isolate. */
			(void)tnt_send(self->timer, TAG_ARMTIMER, NULL, 0);
			self->phase = 5;
			/* Re-arm a poll timer to wait for timer_fired. */
			tnt_register_timer(5LL * 1000 * 1000, TNT_TAG_TIMER);
			return TNT_TRANSITION_WAIT_MESSAGE;
		}
		/* phase 5: poll until the timer isolate's timer fired. */
		if (atomic_load(&g_res.timer_fired)) {
			int pass = 1;
			pass &= (atomic_load(&g_res.counter_value) == 6);
			pass &= (atomic_load(&g_res.counter_msgs) == 3);
			pass &= (atomic_load(&g_res.drop_full) >= 1);
			pass &= (atomic_load(&g_res.drop_stale) >= 1);
			pass &= (atomic_load(&g_res.done_seen) == 1);
			pass &= (atomic_load(&g_res.timer_fired) == 1);
			atomic_store(&g_res.all_pass, pass);
			tnt_stop();
			return TNT_TRANSITION_DONE;
		}
		tnt_register_timer(5LL * 1000 * 1000, TNT_TAG_TIMER);
		return TNT_TRANSITION_WAIT_MESSAGE;
	}

	if (msg->tag != TAG_KICK)
		return TNT_TRANSITION_WAIT_MESSAGE;

	/* ---- Phase 0: spawn the other isolates. ---- */
	if (self->phase == 0) {
		tnt_spawn(T_COUNTER, NULL, 0, &self->counter);
		tnt_spawn(T_SINK, NULL, 0, &self->sink);
		tnt_spawn(T_SINK, NULL, 0, &self->fullsink);
		tnt_spawn(T_TIMER, NULL, 0, &self->timer);
		self->phase = 1;
		(void)tnt_send(tnt_self(), TAG_KICK, NULL, 0);
		return TNT_TRANSITION_WAIT_MESSAGE;
	}

	/* ---- Phase 1: send three ADDs to the counter. ---- */
	if (self->phase == 1) {
		int a = 1, b = 2, c = 3;
		if (tnt_send(self->counter, TAG_ADD, &a, sizeof a) ==
		    TNT_SEND_OK)
			atomic_fetch_add(&g_res.send_ok, 1);
		if (tnt_send(self->counter, TAG_ADD, &b, sizeof b) ==
		    TNT_SEND_OK)
			atomic_fetch_add(&g_res.send_ok, 1);
		if (tnt_send(self->counter, TAG_ADD, &c, sizeof c) ==
		    TNT_SEND_OK)
			atomic_fetch_add(&g_res.send_ok, 1);
		self->phase = 2;
		(void)tnt_send(tnt_self(), TAG_KICK, NULL, 0);
		return TNT_TRANSITION_WAIT_MESSAGE;
	}

	/* ---- Phase 2: drop-on-full.  fullsink has mailbox capacity 4;
	 * flood it with 16 PINGs in a single turn (the sink never runs
	 * between our sends, so its mailbox fills and the later sends are
	 * dropped with MAILBOX_FULL -- Tina's drop-on-full feedback). ---- */
	if (self->phase == 2) {
		int i;
		for (i = 0; i < 16; i++) {
			tnt_send_result_t r =
			    tnt_send(self->fullsink, TAG_PING, NULL, 0);
			if (r == TNT_SEND_OK)
				atomic_fetch_add(&g_res.send_ok, 1);
			else if (r == TNT_SEND_MAILBOX_FULL)
				atomic_fetch_add(&g_res.drop_full, 1);
		}
		self->phase = 3;
		(void)tnt_send(tnt_self(), TAG_KICK, NULL, 0);
		return TNT_TRANSITION_WAIT_MESSAGE;
	}

	/* ---- Phase 3: staleness.  Tear down `sink` (send TAG_DONE), then
	 * defer the stale check to phase 4 via a short timer so the sink's
	 * DONE is processed first. ---- */
	if (self->phase == 3) {
		(void)tnt_send(self->sink, TAG_DONE, NULL, 0);
		self->phase = 4;
		tnt_register_timer(2LL * 1000 * 1000, TNT_TAG_TIMER);
		return TNT_TRANSITION_WAIT_MESSAGE;
	}

	return TNT_TRANSITION_WAIT_MESSAGE;
}

/* ---- Spec --------------------------------------------------------- */
static const tnt_type_t test_types[] = {
	{ .id = T_COUNTER, .name = "Counter", .slot_count = 64,
	  .stride = sizeof(counter_iso_t), .mailbox_capacity = 64,
	  .budget_weight = 64, .init_fn = counter_init,
	  .handler_fn = counter_handler },
	{ .id = T_SINK, .name = "Sink", .slot_count = 64,
	  .stride = sizeof(sink_iso_t), .mailbox_capacity = 4,
	  .budget_weight = 64, .init_fn = sink_init,
	  .handler_fn = sink_handler },
	{ .id = T_TIMER, .name = "Timer", .slot_count = 8,
	  .stride = sizeof(timer_iso_t), .mailbox_capacity = 8,
	  .budget_weight = 8, .init_fn = timer_init,
	  .handler_fn = timer_handler },
	{ .id = T_DRIVER, .name = "Driver", .slot_count = 2,
	  .stride = sizeof(driver_iso_t), .mailbox_capacity = 8,
	  .budget_weight = 2, .init_fn = driver_init,
	  .handler_fn = driver_handler },
};

int
main(void)
{
	tnt_spec_t spec;
	int rc;

	memset(&g_res, 0, sizeof(g_res));

	memset(&spec, 0, sizeof(spec));
	spec.name = "tnt-test";
	spec.types = test_types;
	spec.n_types = 4;
	spec.shard_count = 1;
	spec.scratch_size = 65536;
	spec.recv_buf_size = 256;
	spec.boot_type = T_DRIVER;   /* runtime auto-spawns the driver on
	                              * shard 0; its init kicks the scenario */

	rc = tnt_start(&spec);
	(void)rc;

	/* Report. */
	printf("counter_msgs  = %d (want 3)\n",
	    atomic_load(&g_res.counter_msgs));
	printf("counter_value = %d (want 6)\n",
	    atomic_load(&g_res.counter_value));
	printf("send_ok       = %d\n", atomic_load(&g_res.send_ok));
	printf("drop_full     = %d (want >=1)\n",
	    atomic_load(&g_res.drop_full));
	printf("drop_stale    = %d (want >=1)\n",
	    atomic_load(&g_res.drop_stale));
	printf("done_seen     = %d (want 1)\n",
	    atomic_load(&g_res.done_seen));
	printf("timer_fired   = %d (want 1)\n",
	    atomic_load(&g_res.timer_fired));

	if (atomic_load(&g_res.all_pass)) {
		printf("\nPASS: all tnt dispatch invariants hold\n");
		return 0;
	}
	printf("\nFAIL: scenario did not pass\n");
	return 1;
}
