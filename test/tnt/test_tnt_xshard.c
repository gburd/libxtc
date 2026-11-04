/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/tnt/test_tnt_xshard.c
 *	Cross-shard sends and sends from outside a handler are never lost,
 *	duplicated or reordered.
 *
 *	Through 2.1.1 an xtc_tnt_send to an Isolate on ANOTHER shard wrote
 *	the target slot's mailbox (plain fields owned by that shard) from the
 *	sender's thread.  It raced the target's dispatch: with two shards
 *	each flooding a sink on the other, 1552 of 400000 messages vanished
 *	and the run never finished (TSan: 11 races in xtc_tnt_deliver vs
 *	shard_tick).  A cross-shard message now goes through a
 *	per-(source, destination) SPSC ring the destination drains on its own
 *	thread, and xtc_tnt_send_from is the thread-safe entry for senders
 *	that are not Isolates.
 *
 *	Oracle (exact): every send is either delivered or reported dropped
 *	(XTC_TNT_SEND_MAILBOX_FULL) -- delivered + dropped == sent -- and the
 *	sequence numbers a sink receives from each source are strictly
 *	increasing (per-pair FIFO).  A source that retries on a full ring
 *	loses nothing.  Phase 1: two shards, a source on each flooding the
 *	sink on the other.  Phase 2: two external threads using
 *	xtc_tnt_send_from into both sinks.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xtc.h"
#include "xtc_tnt.h"

#if defined(_WIN32)
int
main(void)
{
	printf("SKIP: tnt is POSIX-only\n");
	return 0;
}
#else

#define TAG_HIT  (XTC_TNT_USER_TAG_BASE + 1)
#define TAG_GO   (XTC_TNT_USER_TAG_BASE + 2)
#define NSHARD   2
#define NSRC     (NSHARD + 2)      /* 2 shard sources + 2 external */
#define N        100000            /* sends per source */

struct hit {
	uint32_t src;
	uint32_t seq;
};

typedef struct {
	uint32_t last[NSRC];       /* last seq seen from each source, +1 */
} sink_t;

typedef struct {
	uint32_t id;
	uint32_t sent;
} src_t;

typedef struct {
	int x;
} boot_t;

static _Atomic xtc_tnt_handle_t g_sink[NSHARD];
static _Atomic uint64_t g_delivered, g_dropped, g_disorder;
static _Atomic int g_sinks_up, g_ext_done, g_done, g_timed_out;

static void
maybe_stop(void)
{
	if (atomic_load(&g_delivered) + atomic_load(&g_dropped) ==
	    (uint64_t)NSRC * N && atomic_load(&g_ext_done) == 2)
		xtc_tnt_stop();
}

static xtc_tnt_transition_t
sink_init(void *self, const void *a, size_t n)
{
	(void)a;
	(void)n;
	memset(self, 0, sizeof(sink_t));
	atomic_store(&g_sink[xtc_tnt_shard_id()], xtc_tnt_self());
	atomic_fetch_add(&g_sinks_up, 1);
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

static xtc_tnt_transition_t
sink_handler(void *self, xtc_tnt_message_t *msg)
{
	sink_t *s = self;
	struct hit h;

	if (msg->tag != TAG_HIT)
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	memcpy(&h, msg->body.user.payload, sizeof h);
	if (h.src >= NSRC || h.seq + 1u <= s->last[h.src])
		atomic_fetch_add(&g_disorder, 1);
	else
		s->last[h.src] = h.seq + 1u;
	atomic_fetch_add(&g_delivered, 1);
	maybe_stop();
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

static xtc_tnt_transition_t
src_init(void *self, const void *a, size_t n)
{
	src_t *s = self;

	(void)a;
	(void)n;
	s->id = xtc_tnt_shard_id();
	s->sent = 0;
	xtc_tnt_register_timer(10LL * 1000 * 1000, TAG_GO);
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

/* Shard 0's source never retries (it exercises drop accounting); shard
 * 1's retries a full ring on its next turn (it must lose nothing). */
static xtc_tnt_transition_t
src_handler(void *self, xtc_tnt_message_t *msg)
{
	src_t *s = self;
	xtc_tnt_handle_t peer;
	struct hit h;
	int i;

	if (msg->tag != TAG_GO)
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	if (atomic_load(&g_sinks_up) < NSHARD) {
		xtc_tnt_register_timer(1000000, TAG_GO);
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	}
	peer = atomic_load(&g_sink[(s->id + 1) % NSHARD]);
	for (i = 0; i < 256 && s->sent < N; i++) {
		h.src = s->id;
		h.seq = s->sent;
		if (xtc_tnt_send(peer, TAG_HIT, &h, sizeof h) !=
		    XTC_TNT_SEND_OK) {
			if (s->id == 1)
				break;          /* retry next turn */
			atomic_fetch_add(&g_dropped, 1);
		}
		s->sent++;
	}
	maybe_stop();
	if (s->sent < N)
		xtc_tnt_register_timer(1000, TAG_GO);
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

/* A lost message means the accounting never completes and the system
 * never stops: bound the run so that fails instead of hanging. */
static void *
watchdog(void *arg)
{
	int i;

	(void)arg;
	for (i = 0; i < 600 && !atomic_load(&g_done); i++)
		xtc_sleep_ns(100LL * 1000 * 1000);
	if (!atomic_load(&g_done)) {
		atomic_store(&g_timed_out, 1);
		xtc_tnt_stop();
	}
	return NULL;
}

static void *
ext_thread(void *arg)
{
	uint32_t id = (uint32_t)(uintptr_t)arg, seq;
	struct hit h;

	while (atomic_load(&g_sinks_up) < NSHARD &&
	    !atomic_load(&g_timed_out))
		xtc_sleep_ns(1000000);
	for (seq = 0; seq < N && !atomic_load(&g_timed_out); ) {
		h.src = id;
		h.seq = seq;
		/* Alternate sinks so both shards' external rings carry load;
		 * retry on a full ring (no loss). */
		if (xtc_tnt_send_from(XTC_TNT_HANDLE_NONE,
		    atomic_load(&g_sink[seq & 1]), TAG_HIT, &h, sizeof h) ==
		    XTC_TNT_SEND_OK)
			seq++;
		else
			xtc_sleep_ns(10000);
	}
	atomic_fetch_add(&g_ext_done, 1);
	return NULL;
}

static xtc_tnt_transition_t
boot_init(void *self, const void *a, size_t n)
{
	int i;

	(void)self;
	(void)a;
	(void)n;
	for (i = 0; i < NSHARD; i++) {
		(void)xtc_tnt_spawn_on((uint8_t)i, 0, NULL, 0);
		(void)xtc_tnt_spawn_on((uint8_t)i, 1, NULL, 0);
	}
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

static xtc_tnt_transition_t
boot_handler(void *self, xtc_tnt_message_t *msg)
{
	(void)self;
	(void)msg;
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

static const xtc_tnt_type_t types[] = {
	{ 0, "sink", 2, sizeof(sink_t), 0, 1u << 20, 64, sink_init,
	    sink_handler },
	{ 1, "src", 2, sizeof(src_t), 0, 64, 64, src_init, src_handler },
	{ 2, "boot", 1, sizeof(boot_t), 0, 4, 1, boot_init, boot_handler },
};

int
main(void)
{
	xtc_tnt_spec_t spec;
	pthread_t th[2], wd;
	uint64_t d, x, o;
	int rc, i;

	memset(&spec, 0, sizeof spec);
	spec.name = "xshard";
	spec.types = types;
	spec.n_types = 3;
	spec.shard_count = NSHARD;
	spec.boot_type = 2;
	for (i = 0; i < 2; i++)
		if (pthread_create(&th[i], NULL, ext_thread,
		    (void *)(uintptr_t)(NSHARD + i)) != 0) {
			printf("FAIL: pthread_create\n");
			return 1;
		}
	if (pthread_create(&wd, NULL, watchdog, NULL) != 0) {
		printf("FAIL: pthread_create\n");
		return 1;
	}
	rc = xtc_tnt_start(&spec);
	atomic_store(&g_done, 1);
	for (i = 0; i < 2; i++)
		(void)pthread_join(th[i], NULL);
	(void)pthread_join(wd, NULL);
	d = atomic_load(&g_delivered);
	x = atomic_load(&g_dropped);
	o = atomic_load(&g_disorder);
	printf("delivered=%llu dropped=%llu disorder=%llu sent=%d rc=%d\n",
	    (unsigned long long)d, (unsigned long long)x,
	    (unsigned long long)o, NSRC * N, rc);
	if (atomic_load(&g_timed_out))
		printf("FAIL: timed out after 60 s: messages were lost\n");
	if (rc != 0 || d + x != (uint64_t)NSRC * N || o != 0 ||
	    atomic_load(&g_timed_out)) {
		printf("FAIL: a cross-shard message was lost, duplicated or "
		    "reordered\n");
		return 1;
	}
	printf("PASS: cross-shard and external sends: every message "
	    "delivered or reported dropped, per-pair FIFO\n");
	return 0;
}
#endif /* !_WIN32 */
