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
 *	the scenario from inside a shard, then calls xtc_tnt_stop.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "xtc.h"        /* XTC_E_NOSYS */
#include "xtc_tnt.h"

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
	atomic_int echo_recv;         /* ECHO: RECV_COMPLETE seen */
	atomic_int echo_sent;         /* ECHO: SEND_COMPLETE seen */
	atomic_int echo_closed;       /* ECHO: CLOSE_COMPLETE seen */
	atomic_int echo_bytes;        /* ECHO: bytes echoed back */
	atomic_int echo_scratch_ok;   /* ECHO: scratch arena served a request */
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
#define T_ECHO    4   /* staged-io echo over a socketpair */

/* Set by main() BEFORE xtc_tnt_start; -1 means "not staged".  Read by the
 * driver on its first tick, which is inside the runtime -- xtc_tnt_spawn_on
 * needs the runtime to exist, so the spawn cannot happen before start. */
static int g_echo_fd = -1;


/* ---- User tags ---- */
#define TAG_ADD       (XTC_TNT_USER_TAG_BASE + 0)  /* COUNTER: add payload */
#define TAG_PING      (XTC_TNT_USER_TAG_BASE + 1)  /* SINK: count + maybe done */
#define TAG_DONE      (XTC_TNT_USER_TAG_BASE + 2)  /* SINK: tear down (DONE) */
#define TAG_ARMTIMER  (XTC_TNT_USER_TAG_BASE + 3)  /* TIMER: arm a timer */
#define TAG_KICK      (XTC_TNT_USER_TAG_BASE + 4)  /* DRIVER: run the scenario */

/* ====================================================================
 * COUNTER isolate -- accumulates an int payload across messages.
 * ==================================================================== */
typedef struct counter_iso {
	int total;
} counter_iso_t;

static xtc_tnt_transition_t
counter_init(void *self_raw, const void *args, size_t n)
{
	counter_iso_t *self = xtc_tnt_self_as(counter_iso_t, self_raw);
	(void)args; (void)n;
	self->total = 0;
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

static xtc_tnt_transition_t
counter_handler(void *self_raw, xtc_tnt_message_t *msg)
{
	counter_iso_t *self = xtc_tnt_self_as(counter_iso_t, self_raw);

	if (msg->tag == TAG_ADD) {
		int v = *xtc_tnt_payload_as(int, msg);
		self->total += v;
		atomic_fetch_add(&g_res.counter_msgs, 1);
		atomic_store(&g_res.counter_value, self->total);
	}
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

/* ====================================================================
 * SINK isolate -- counts PINGs; on TAG_DONE it returns DONE (teardown,
 * generation bump).  Used for the mailbox-full and staleness tests.
 * ==================================================================== */
typedef struct sink_iso {
	int seen;
} sink_iso_t;

static xtc_tnt_transition_t
sink_init(void *self_raw, const void *args, size_t n)
{
	sink_iso_t *self = xtc_tnt_self_as(sink_iso_t, self_raw);
	(void)args; (void)n;
	self->seen = 0;
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

static xtc_tnt_transition_t
sink_handler(void *self_raw, xtc_tnt_message_t *msg)
{
	sink_iso_t *self = xtc_tnt_self_as(sink_iso_t, self_raw);

	if (msg->tag == TAG_DONE) {
		atomic_fetch_add(&g_res.done_seen, 1);
		return XTC_TNT_TRANSITION_DONE;
	}
	if (msg->tag == TAG_PING) {
		self->seen++;
		atomic_fetch_add(&g_res.sink_handled, 1);
	}
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

/* ====================================================================
 * TIMER isolate -- arms a one-shot timer, sets timer_fired on delivery.
 * ==================================================================== */
typedef struct timer_iso {
	int armed;
} timer_iso_t;

static xtc_tnt_transition_t
timer_init(void *self_raw, const void *args, size_t n)
{
	timer_iso_t *self = xtc_tnt_self_as(timer_iso_t, self_raw);
	(void)args; (void)n;
	self->armed = 0;
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

static xtc_tnt_transition_t
timer_handler(void *self_raw, xtc_tnt_message_t *msg)
{
	timer_iso_t *self = xtc_tnt_self_as(timer_iso_t, self_raw);

	if (msg->tag == TAG_ARMTIMER) {
		self->armed = 1;
		xtc_tnt_register_timer(20LL * 1000 * 1000 /* 20ms */, XTC_TNT_TAG_TIMER);
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	}
	if (msg->tag == XTC_TNT_TAG_TIMER) {
		atomic_store(&g_res.timer_fired, 1);
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	}
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

/* ====================================================================
 * DRIVER isolate -- runs the whole scenario from inside the shard, so
 * every xtc_tnt_* call happens in a valid handler context.  It spawns the
 * other isolates, exercises sends, verifies drop-on-full + staleness,
 * arms a timer, then waits (via re-arming a short timer) for the timer
 * to fire, checks results, and stops the runtime.
 * ==================================================================== */
typedef struct driver_iso {
	int           phase;
	xtc_tnt_handle_t  counter;
	xtc_tnt_handle_t  sink;        /* the sink we tear down for staleness */
	xtc_tnt_handle_t  fullsink;    /* the sink we overflow for drop-on-full */
	xtc_tnt_handle_t  timer;
} driver_iso_t;

static xtc_tnt_transition_t
driver_init(void *self_raw, const void *args, size_t n)
{
	driver_iso_t *self = xtc_tnt_self_as(driver_iso_t, self_raw);
	(void)args; (void)n;
	memset(self, 0, sizeof(*self));
	self->phase = 0;
	/* Kick ourselves to start phase 0 on the next tick. */
	(void)xtc_tnt_send(xtc_tnt_self(), TAG_KICK, NULL, 0);
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

static xtc_tnt_transition_t
driver_handler(void *self_raw, xtc_tnt_message_t *msg)
{
	driver_iso_t *self = xtc_tnt_self_as(driver_iso_t, self_raw);

	if (msg->tag == XTC_TNT_TAG_TIMER) {
		/* phase 4: the sink's TAG_DONE has now been processed, so the
		 * sink slot is torn down and its generation bumped.  A send to
		 * the OLD handle must be STALE. */
		if (self->phase == 4) {
			xtc_tnt_send_result_t r =
			    xtc_tnt_send(self->sink, TAG_PING, NULL, 0);
			if (r == XTC_TNT_SEND_STALE_HANDLE)
				atomic_fetch_add(&g_res.drop_stale, 1);
			/* Now arm the real timer on the timer isolate. */
			(void)xtc_tnt_send(self->timer, TAG_ARMTIMER, NULL, 0);
			self->phase = 5;
			/* Re-arm a poll timer to wait for timer_fired. */
			xtc_tnt_register_timer(5LL * 1000 * 1000, XTC_TNT_TAG_TIMER);
			return XTC_TNT_TRANSITION_WAIT_MESSAGE;
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
			xtc_tnt_stop();
			return XTC_TNT_TRANSITION_DONE;
		}
		xtc_tnt_register_timer(5LL * 1000 * 1000, XTC_TNT_TAG_TIMER);
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	}

	if (msg->tag != TAG_KICK)
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;

	/* ---- Phase 0: spawn the other isolates. ---- */
	if (self->phase == 0) {
		xtc_tnt_spawn(T_COUNTER, NULL, 0, &self->counter);
		xtc_tnt_spawn(T_SINK, NULL, 0, &self->sink);
		xtc_tnt_spawn(T_SINK, NULL, 0, &self->fullsink);
		xtc_tnt_spawn(T_TIMER, NULL, 0, &self->timer);
		/* Staged-io scenario: spawn the echo Isolate on this shard with
		 * the already-written socketpair end.  Done from inside the
		 * runtime because the spawn APIs need it to exist. */
		if (g_echo_fd >= 0) {
			xtc_tnt_handle_t eh;
			(void)xtc_tnt_spawn(T_ECHO, &g_echo_fd,
			    sizeof g_echo_fd, &eh);
		}
		self->phase = 1;
		(void)xtc_tnt_send(xtc_tnt_self(), TAG_KICK, NULL, 0);
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	}

	/* ---- Phase 1: send three ADDs to the counter. ---- */
	if (self->phase == 1) {
		int a = 1, b = 2, c = 3;
		if (xtc_tnt_send(self->counter, TAG_ADD, &a, sizeof a) ==
		    XTC_TNT_SEND_OK)
			atomic_fetch_add(&g_res.send_ok, 1);
		if (xtc_tnt_send(self->counter, TAG_ADD, &b, sizeof b) ==
		    XTC_TNT_SEND_OK)
			atomic_fetch_add(&g_res.send_ok, 1);
		if (xtc_tnt_send(self->counter, TAG_ADD, &c, sizeof c) ==
		    XTC_TNT_SEND_OK)
			atomic_fetch_add(&g_res.send_ok, 1);
		self->phase = 2;
		(void)xtc_tnt_send(xtc_tnt_self(), TAG_KICK, NULL, 0);
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	}

	/* ---- Phase 2: drop-on-full.  fullsink has mailbox capacity 4;
	 * flood it with 16 PINGs in a single turn (the sink never runs
	 * between our sends, so its mailbox fills and the later sends are
	 * dropped with MAILBOX_FULL -- Tina's drop-on-full feedback). ---- */
	if (self->phase == 2) {
		int i;
		for (i = 0; i < 16; i++) {
			xtc_tnt_send_result_t r =
			    xtc_tnt_send(self->fullsink, TAG_PING, NULL, 0);
			if (r == XTC_TNT_SEND_OK)
				atomic_fetch_add(&g_res.send_ok, 1);
			else if (r == XTC_TNT_SEND_MAILBOX_FULL)
				atomic_fetch_add(&g_res.drop_full, 1);
		}
		self->phase = 3;
		(void)xtc_tnt_send(xtc_tnt_self(), TAG_KICK, NULL, 0);
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	}

	/* ---- Phase 3: staleness.  Tear down `sink` (send TAG_DONE), then
	 * defer the stale check to phase 4 via a short timer so the sink's
	 * DONE is processed first. ---- */
	if (self->phase == 3) {
		(void)xtc_tnt_send(self->sink, TAG_DONE, NULL, 0);
		self->phase = 4;
		xtc_tnt_register_timer(2LL * 1000 * 1000, XTC_TNT_TAG_TIMER);
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	}

	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}


/* ---- ECHO: the staged-io path (courier -> completion -> commit) -----
 *
 * xtc_tnt_submit_recv / xtc_tnt_io_send / xtc_tnt_submit_close are the
 * Isolate model's whole asynchronous story: the Isolate STAGES an
 * operation and returns WAIT_IO, a courier performs it off the shard
 * fiber, and the result comes back as an ordinary message.  None of
 * that was covered by an automated test -- examples/08_tnt/echo.c
 * exercises it, but that demo binds a FIXED TCP PORT and is therefore
 * deliberately excluded from make check (port/timing fragile on shared
 * runners).
 *
 * A socketpair gives the same coverage with no port, no bind, no
 * listener and no timing assumption: main() pre-writes a request into
 * one end BEFORE the runtime starts, so the data is already buffered
 * when the courier issues its recv; the Isolate echoes it back and
 * closes.  main() then reads the echo out of the other end after the
 * runtime has stopped.
 */
#define ECHO_BUFSZ 64
typedef struct echo_iso {
	int fd;
	int echoed;
	/* The bytes to send must live in ISOLATE-owned storage: the buffer
	 * handed to us on RECV_COMPLETE belongs to the reactor and may be
	 * recycled before the asynchronous send completes.  (Sending
	 * directly from it produced an 8-byte echo of NULs.)  Same
	 * discipline as examples/08_tnt/echo.c. */
	size_t bytes;
	char   buffer[ECHO_BUFSZ];
} echo_iso_t;

static xtc_tnt_transition_t
echo_init(void *self_raw, const void *args, size_t n)
{
	echo_iso_t *self = xtc_tnt_self_as(echo_iso_t, self_raw);
	memset(self, 0, sizeof(*self));
	if (args == NULL || n != sizeof(int))
		return XTC_TNT_TRANSITION_DONE;
	memcpy(&self->fd, args, sizeof(int));

	/* Per-tick scratch: freed automatically at the end of the tick, so
	 * it needs no matching release.  Exercised here because nothing
	 * else in the suite touches it. */
	if (xtc_tnt_scratch_arena(64) != NULL)
		atomic_fetch_add(&g_res.echo_scratch_ok, 1);

	if (xtc_tnt_submit_recv(self->fd) != XTC_TNT_IO_OK)
		return XTC_TNT_TRANSITION_DONE;
	return XTC_TNT_TRANSITION_WAIT_IO;
}

static xtc_tnt_transition_t
echo_handler(void *self_raw, xtc_tnt_message_t *msg)
{
	echo_iso_t *self = xtc_tnt_self_as(echo_iso_t, self_raw);

	switch (msg->tag) {
	case XTC_TNT_IO_TAG_RECV_COMPLETE:
		atomic_fetch_add(&g_res.echo_recv, 1);
		if (msg->body.io.result <= 0) {
			/* peer closed or error -- stage the close and finish */
			(void)xtc_tnt_submit_close(self->fd);
			return XTC_TNT_TRANSITION_WAIT_IO;
		}
		/* Copy into our own storage first (see the struct comment),
		 * then echo it back. */
		self->bytes = (size_t)msg->body.io.result;
		if (self->bytes > ECHO_BUFSZ)
			self->bytes = ECHO_BUFSZ;
		memcpy(self->buffer, msg->body.io.buffer, self->bytes);
		if (xtc_tnt_io_send(self->fd, self->buffer, self->bytes)
		    != XTC_TNT_IO_OK) {
			(void)xtc_tnt_submit_close(self->fd);
			return XTC_TNT_TRANSITION_WAIT_IO;
		}
		atomic_fetch_add(&g_res.echo_bytes, (int)self->bytes);
		self->echoed = 1;
		return XTC_TNT_TRANSITION_WAIT_IO;

	case XTC_TNT_IO_TAG_SEND_COMPLETE:
		atomic_fetch_add(&g_res.echo_sent, 1);
		/* One echo is all this scenario needs; close down. */
		(void)xtc_tnt_submit_close(self->fd);
		return XTC_TNT_TRANSITION_WAIT_IO;

	case XTC_TNT_IO_TAG_CLOSE_COMPLETE:
		atomic_fetch_add(&g_res.echo_closed, 1);
		return XTC_TNT_TRANSITION_DONE;

	default:
		return XTC_TNT_TRANSITION_WAIT_IO;
	}
}

/* ---- Spec --------------------------------------------------------- */
static const xtc_tnt_type_t test_types[] = {
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
	{ .id = T_ECHO, .name = "Echo", .slot_count = 8,
	  .stride = sizeof(echo_iso_t), .mailbox_capacity = 8,
	  .budget_weight = 8, .init_fn = echo_init,
	  .handler_fn = echo_handler },
};

int
main(void)
{
	xtc_tnt_spec_t spec;
	int rc;
	int sv[2] = { -1, -1 };
	int echo_fd = -1;
	int echo_staged = 0;
	char echo_back[32];
	ssize_t echo_n = -1;

	memset(&g_res, 0, sizeof(g_res));

	memset(&spec, 0, sizeof(spec));
	spec.name = "tnt-test";
	spec.types = test_types;
	spec.n_types = 5;
	spec.shard_count = 1;
	spec.scratch_size = 65536;
	spec.recv_buf_size = 256;
	spec.boot_type = T_DRIVER;   /* runtime auto-spawns the driver on
	                              * shard 0; its init kicks the scenario */

	/*
	 * Stage the staged-io scenario BEFORE starting the runtime.
	 * socketpair, not a TCP listener: no port to collide, no bind to
	 * race, no accept to time out.  The request is written into sv[0]
	 * now, so it is already buffered when the courier issues its recv
	 * on sv[1] -- the Isolate cannot observe an empty socket and the
	 * test cannot flake on scheduling.  xtc_tnt_spawn_on routes the
	 * spawn onto shard 0 from outside any shard fiber (the other
	 * untested entry point).
	 */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
		static const char req[] = "tnt-echo";
		echo_fd = sv[1];
		if (write(sv[0], req, sizeof req - 1) == (ssize_t)(sizeof req - 1)) {
			echo_staged = 1;
			g_echo_fd = echo_fd;   /* the driver spawns T_ECHO */
		}
	}

	rc = xtc_tnt_start(&spec);
	if (rc == XTC_E_NOSYS) {
		/* tnt is a POSIX feature: its I/O couriers use raw sockets,
		 * so xtc_tnt_start is a NOSYS stub on non-POSIX targets
		 * (Windows -- see the #else in src/orc/tnt.c).  The runtime
		 * never runs, so the scenario counters would all be zero;
		 * that is not a failure, it is "unsupported here".  Skip. */
		printf("SKIP: xtc_tnt is not supported on this platform "
		    "(xtc_tnt_start -> XTC_E_NOSYS)\n");
		return 77;
	}
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

	/*
	 * Staged-io (courier) scenario.  Only asserted when the socketpair
	 * could actually be staged -- a platform without socketpair (or a
	 * spawn_on refusal) is "not exercised here", not a failure.
	 */
	if (echo_staged) {
		echo_n = read(sv[0], echo_back, sizeof echo_back);
		printf("echo_recv     = %d (want 1)\n",
		    atomic_load(&g_res.echo_recv));
		printf("echo_sent     = %d (want 1)\n",
		    atomic_load(&g_res.echo_sent));
		printf("echo_closed   = %d (want 1)\n",
		    atomic_load(&g_res.echo_closed));
		printf("echo_bytes    = %d (want 8)\n",
		    atomic_load(&g_res.echo_bytes));
		printf("echo_scratch  = %d (want 1)\n",
		    atomic_load(&g_res.echo_scratch_ok));
		printf("echo_readback = %zd (want 8) [%.*s]\n", echo_n,
		    (int)(echo_n > 0 ? echo_n : 0), echo_back);

		if (atomic_load(&g_res.echo_recv) < 1 ||
		    atomic_load(&g_res.echo_sent) < 1 ||
		    atomic_load(&g_res.echo_closed) < 1 ||
		    atomic_load(&g_res.echo_scratch_ok) < 1 ||
		    echo_n != 8 ||
		    memcmp(echo_back, "tnt-echo", 8) != 0) {
			printf("\nFAIL: staged-io echo (submit_recv -> io_send -> "
			    "submit_close) did not round-trip\n");
			(void)close(sv[0]);
			return 1;
		}
	} else {
		printf("echo scenario  = not staged (socketpair/spawn_on "
		    "unavailable); staged-io path not exercised\n");
	}
	if (sv[0] >= 0) (void)close(sv[0]);

	if (atomic_load(&g_res.all_pass)) {
		printf("\nPASS: all tnt dispatch invariants hold\n");
		return 0;
	}
	printf("\nFAIL: scenario did not pass\n");
	return 1;
}
