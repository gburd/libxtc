/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/cbmc/fdclaim_harness.c
 *
 *	(src/evt/task.c __xtc_loop_dispatch_event, the ev->fd >= 0 claim;
 *	src/ptc/proc.c xtc_proc_wait_fd's park and post-park cleanup;
 *	src/inc/loop_int.h __park_tok.)
 *
 *	Who owns the unregister of a parked fd.  A migratable fiber parks
 *	on fd F on loop A's io; A reaps F's readiness.  Before A dispatches
 *	it, the fiber is woken another way, cleans up, and parks on F again
 *	-- on loop B's io, or on A's own io again.  A then dispatches.
 *
 *	INVARIANT, over every interleaving: when everything has run, no io
 *	still holds a registration for F (none orphaned), and no register
 *	ever saw a duplicate (the XTC_E_INVAL the 2026-11-02 report hit).
 *
 *	WHAT IS MODELLED: the claim word and the order of every access.
 *	An io's fd registry is one flag per io.  The dispatch's del of its
 *	own io and the cleanup's del of the claimed io are immediate (the
 *	defer to the owner thread delays a del, it never drops one).  The
 *	dispatch's claim is the real statement block (check_drift.sh); the
 *	io is an index (CBMC 5.x rejects pointer atomics across threads).
 *	CBMC is sequentially consistent here; the orders are not checked.
 *
 *	Planted bugs, each must FAIL:
 *	  -DPLANT_FD_ONLY   2.1.1: claim when the fd matches;
 *	  -DPLANT_IO_GUARD  first 2.1.2 attempt: fd matches AND the park is
 *	                    on this io, but the word is a bare fd -- the
 *	                    re-park between the dispatch's loads and its CAS
 *	                    stores the same value, so the CAS succeeds
 *	                    against the newer park.
 */
#include <stdatomic.h>
#include <stdint.h>

#define F	7
#define IO_A	1
#define IO_B	2
#define __PARK_TOK_NONE ((int64_t)-1)

static _Atomic int reg[3];		/* reg[io]: io holds a reg for F */
static _Atomic int64_t park_fd;		/* the claim word */
static _Atomic int park_io;		/* 0 = none */
static uint32_t park_gen;
static _Atomic int disp_done;
static int dup_seen;
int nondet_int(void);

#if defined(PLANT_FD_ONLY) || defined(PLANT_IO_GUARD)
/* The bare-fd word of 2.1.1 and the first fix. */
static int64_t
__park_tok(int fd)
{
	return fd;
}
#else
/* src/inc/loop_int.h __park_tok, on the one modelled task. */
static int64_t
__park_tok(int fd)
{
	uint32_t g = ++park_gen;

	if (g == UINT32_MAX)
		g = park_gen = 0;
	return (int64_t)(((uint64_t)g << 32) | (uint32_t)fd);
}
#endif

static int
__park_tok_fd(int64_t tok)
{
	return tok == __PARK_TOK_NONE ? -1 : (int)(uint32_t)tok;
}

/* xtc_proc_wait_fd: register, record the io, publish the token. */
static void
park(int io)
{
	if (atomic_load(&reg[io]) != 0)
		dup_seen = 1;		/* __reg_fd's duplicate: XTC_E_INVAL */
	atomic_store(&reg[io], 1);
	atomic_store_explicit(&park_io, io, memory_order_relaxed);
	atomic_store_explicit(&park_fd, __park_tok(F), memory_order_release);
}

/* xtc_proc_wait_fd's post-park cleanup: claim, then del on that io. */
static void
cleanup(void)
{
	int64_t tok = atomic_exchange_explicit(&park_fd, __PARK_TOK_NONE,
	    memory_order_acq_rel);
	int io = atomic_load_explicit(&park_io, memory_order_relaxed);

	atomic_store_explicit(&park_io, 0, memory_order_relaxed);
	if (tok != __PARK_TOK_NONE)
		atomic_store(&reg[io], 0);
}

/* Loop A dispatches the readiness it reaped for the first park. */
struct ev { int fd; };
struct lp { int io; };
static struct ev ev0 = { F };
static struct lp loop_a = { IO_A };

static void
dispatch_on_a(void)
{
	const struct ev *ev = &ev0;
	const struct lp *loop = &loop_a;
	int64_t claimed = atomic_load_explicit(&park_fd,
	    memory_order_acquire);
	int pio = atomic_load_explicit(&park_io, memory_order_relaxed);

#ifdef PLANT_FD_ONLY
	(void)pio;
	(void)loop;
	if (__park_tok_fd(claimed) == ev->fd)
#else
	/* BEGIN SOURCE src/evt/task.c __xtc_loop_dispatch_event claim */
		if (__park_tok_fd(claimed) == ev->fd && pio == loop->io)
	/* END SOURCE */
#endif
		(void)atomic_compare_exchange_strong_explicit(
		    &park_fd, &claimed, __PARK_TOK_NONE,
		    memory_order_acq_rel, memory_order_relaxed);
	atomic_store(&reg[IO_A], 0);	/* xtc_io_del_fd(loop->io, ev->fd) */
	atomic_store(&disp_done, 1);
}

/*
 * The fiber, woken another way.  It re-parks either on loop B (it
 * migrated: runs on B's thread, concurrently with A's dispatch) or on A
 * again.  A's io is single-owner: a fiber on A runs on A's thread, so
 * its cleanup + re-park on A is SEQUENCED with A's dispatch -- before it
 * (the fiber ran first in A's turn) or after it -- never inside it.
 */
static void
fiber_on_b(void)
{
	cleanup();
	park(IO_B);
	cleanup();
}

int
main(void)
{
	/* Parked on F on A; A has reaped F's readiness. */
	park(IO_A);

	if (nondet_int()) {
		/* Migrated: the fiber on B races A's dispatch. */
		__CPROVER_ASYNC_1: dispatch_on_a();
		fiber_on_b();
	} else if (nondet_int()) {
		/* Stayed on A, ran before the dispatch in A's turn: the
		 * dispatch then meets the SECOND park on the same io. */
		cleanup();
		park(IO_A);
		dispatch_on_a();
		cleanup();
	} else {
		/* Stayed on A, ran after the dispatch. */
		dispatch_on_a();
		cleanup();
		park(IO_A);
		cleanup();
	}

	__CPROVER_assume(atomic_load(&disp_done) == 1);
	__CPROVER_assert(!dup_seen, "no register ever sees a duplicate");
	__CPROVER_assert(atomic_load(&reg[IO_A]) == 0 &&
	    atomic_load(&reg[IO_B]) == 0,
	    "no fd registration is orphaned");
	return 0;
}
