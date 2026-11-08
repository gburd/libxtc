/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/cbmc/tntring_harness.c
 *
 *	(src/orc/tnt.c: xring_push, xring_drain, xring_pending and the park
 *	handshake in xtc_tnt_shard_main -- the cross-shard message ring.)
 *
 *	A message for an Isolate on another shard goes into a per-(source,
 *	destination) SPSC ring; the destination drains it on its own
 *	thread.  A producer writes the wake byte only when the destination
 *	announced it is parking (`sleeping`), so a busy shard costs its
 *	senders no system call.
 *
 *	INVARIANTS, two pushes from the source shard racing the destination
 *	shard's "drain, then park" decision, over every interleaving:
 *	  - no lost wake: the destination never parks with no wake byte
 *	    pending while a message sits in the ring;
 *	  - FIFO, nothing lost or duplicated: once drained, the messages
 *	    arrive exactly once, in push order;
 *	  - capacity: a push to a full ring is refused (never overwrites).
 *
 *	WHAT IS MODELLED: the statements between BEGIN/END SOURCE are the
 *	real ones (check_drift.sh); a message is an int, the ring capacity
 *	is 2 so the full case is reached, and the wake pipe is a counter.
 *	CBMC is sequentially consistent: the seq_cst pairing the handshake
 *	relies on is what the source uses, but a weaker order would NOT be
 *	caught here.  Planted bugs, each must FAIL:
 *	  -DPLANT_NO_RECHECK  the shard parks without re-checking the rings;
 *	  -DPLANT_NO_WAKE     the producer never wakes a sleeping shard;
 *	  -DPLANT_NO_FULL     the producer ignores the capacity check.
 */
#include "xtc_atomic.h"
#include <stddef.h>
#include <stdint.h>

#define XTC_TNT_XSHARD_CAP	2u
#define XTC_TNT_SEND_OK		0
#define XTC_TNT_SEND_MAILBOX_FULL 1
typedef int xtc_tnt_send_result_t;
typedef int xtc_tnt_handle_t;
typedef int xtc_tnt_message_t;

typedef struct xtc_tnt_xring {
	uint32_t head;
	uint32_t tail;
	xtc_tnt_handle_t to[XTC_TNT_XSHARD_CAP];
	xtc_tnt_message_t msg[XTC_TNT_XSHARD_CAP];
} xtc_tnt_xring_t;

typedef struct xtc_tnt_shard {
	int sleeping;
} xtc_tnt_shard_t;

static xtc_tnt_xring_t R;
static xtc_tnt_shard_t D;
static int wakes, pushes_done;
static int got[4], ngot, refused;

static int __xtc_sim_active(void) { return 0; }
/* No pointer may cross threads in a CBMC 5.x concurrent model, so the
 * callees the copied code uses are macros over the globals. */
#define shard_wake(s)                                                          \
	((void)xtc_atomic_fetch_add_i32(&wakes, 1, xtc_mo_seq_cst))
#define xtc_tnt_deliver(s, to, m) \
	((void)(ngot < 4 ? (got[ngot] = *(m)) : 0), ngot++)

/*
 * CBMC 5.x refuses pointers into shared state across threads, so the
 * copied functions take no pointer to the ring or the shard: `r`, `dst`
 * and `sh` are the globals R and D, spelled the way the source spells
 * them so the statements below stay byte-for-byte checkable.
 */
#define r	(&R)
#define dst	(&D)
#define sh	(&D)

static xtc_tnt_send_result_t
xring_push(xtc_tnt_handle_t to, xtc_tnt_message_t mv)
{
#define m (&mv)
	/* BEGIN SOURCE src/orc/tnt.c xring_push */
	uint32_t t = xtc_atomic_load_u32(&r->tail, xtc_mo_relaxed);
	uint32_t h = xtc_atomic_load_u32(&r->head, xtc_mo_acquire);
	uint32_t i;

#ifndef PLANT_NO_FULL
	if (t - h >= XTC_TNT_XSHARD_CAP)
		return XTC_TNT_SEND_MAILBOX_FULL;   /* drop, with feedback */
#endif
	i = t & (XTC_TNT_XSHARD_CAP - 1u);
	r->to[i] = to;
	r->msg[i] = *m;
	xtc_atomic_store_u32(&r->tail, t + 1u, xtc_mo_seq_cst);
#ifndef PLANT_NO_WAKE
	if (__xtc_sim_active() ||
	    xtc_atomic_load_i32(&dst->sleeping, xtc_mo_seq_cst))
		shard_wake(dst);
#endif
	return XTC_TNT_SEND_OK;
	/* END SOURCE */
#undef m
}

static uint32_t
xring_drain(void)
{
	/* BEGIN SOURCE src/orc/tnt.c xring_drain */
	uint32_t h = xtc_atomic_load_u32(&r->head, xtc_mo_relaxed);
	uint32_t t = xtc_atomic_load_u32(&r->tail, xtc_mo_acquire);
	uint32_t n = t - h;

	for (; h != t; h++) {
		uint32_t i = h & (XTC_TNT_XSHARD_CAP - 1u);
		(void)xtc_tnt_deliver(sh, r->to[i], &r->msg[i]);
	}
	xtc_atomic_store_u32(&r->head, h, xtc_mo_release);
	return n;
	/* END SOURCE */
}

static int
xring_pending(void)
{
	/* BEGIN SOURCE src/orc/tnt.c xring_pending */
	return r != NULL &&
	    xtc_atomic_load_u32(&((xtc_tnt_xring_t *)r)->tail,
	        xtc_mo_seq_cst) !=
	    xtc_atomic_load_u32(&((xtc_tnt_xring_t *)r)->head, xtc_mo_relaxed);
	/* END SOURCE */
}
#undef r
#undef dst
#undef sh

/* The source shard: two pushes (and a third, which may find the ring
 * full while the destination has not drained). */
static void
producer(void)
{
	int m;
	for (m = 1; m <= 3; m++)
		if (xring_push(0, m) != XTC_TNT_SEND_OK)
			refused++;
	xtc_atomic_store_i32(&pushes_done, 1, xtc_mo_seq_cst);
}

int
main(void)
{
	int parked = 0, i;

	xtc_atomic_store_u32(&R.head, 0, xtc_mo_seq_cst);
	xtc_atomic_store_u32(&R.tail, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&D.sleeping, 0, xtc_mo_seq_cst);

	__CPROVER_ASYNC_1: producer();

	/* The destination's tick then park (xtc_tnt_shard_main). */
	(void)xring_drain();
	xtc_atomic_store_i32(&D.sleeping, 1, xtc_mo_seq_cst);
#ifdef PLANT_NO_RECHECK
	if (1) {
#else
	if (!xring_pending()) {
#endif
		/* Parked: wakes only on a wake byte.  Let the producer finish,
		 * then ask whether it is asleep with work queued. */
		__CPROVER_assume(
		    xtc_atomic_load_i32(&pushes_done, xtc_mo_seq_cst) == 1);
		if (xtc_atomic_load_i32(&wakes, xtc_mo_seq_cst) == 0 &&
		    xring_pending())
			parked = 1;
	}
	xtc_atomic_store_i32(&D.sleeping, 0, xtc_mo_seq_cst);
	__CPROVER_assert(!parked,
	    "no lost wake: never parked with a message queued and no wake");

	/* Eventually everything is drained, in order. */
	__CPROVER_assume(
	    xtc_atomic_load_i32(&pushes_done, xtc_mo_seq_cst) == 1);
	(void)xring_drain();
	__CPROVER_assert(ngot + refused == 3,
	    "every push delivered once or refused (none lost, none doubled)");
	for (i = 1; i < ngot && i < 4; i++)
		__CPROVER_assert(got[i] > got[i - 1],
		    "delivered in push order (FIFO)");
	__CPROVER_assert(ngot <= 3, "nothing fabricated");
	return 0;
}
