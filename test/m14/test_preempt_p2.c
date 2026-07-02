/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m14/test_preempt_p2.c
 *	Phase 2 of the preemption plan (docs/M_PREEMPTION.md): TRUE
 *	signal-context involuntary yield.  A fiber in a PURE tight loop
 *	with NO yield points at all is preempted by the timer so its
 *	loop-mates make progress -- the case Phase 1 (cooperative-
 *	assisted) cannot handle.  Resumable: the runaway finishes its
 *	full computation, it was just time-sliced.
 *
 *	Only the ucontext coroutine substrate supports the involuntary
 *	yield; on fctx (musl) / winfiber the redirect declines and this
 *	test asserts the safe fallback (the runaway still completes; it
 *	just is not preempted -- so we skip the "peers ran" assertion
 *	there).
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_async.h"
#include "xtc_preempt.h"

static atomic_int  g_peer;          /* peer progress while the runaway runs */
static atomic_int  g_runaway_done;
static volatile uint64_t g_sink;

/* A PURE runaway: a long CPU loop with NO xtc_yield / xtc_yield_if_due
 * anywhere.  Under cooperative scheduling this monopolizes the loop
 * until it finishes.  Only true (signal-context) preemption can slice
 * it. */
static void
runaway(void *arg)
{
	uint64_t iters = (uint64_t)(intptr_t)arg;
	uint64_t acc = 0, k;
	for (k = 0; k < iters; k++)
		acc += (k ^ (k << 1)) + (acc >> 3);
	g_sink += acc;
	atomic_store_explicit(&g_runaway_done, 1, memory_order_relaxed);
}

/* A peer: increments a counter and yields, until the runaway is done.
 * It makes progress ONLY if the runaway is preempted to let it run. */
static void
peer(void *arg)
{
	(void)arg;
	while (!atomic_load_explicit(&g_runaway_done, memory_order_relaxed)) {
		atomic_fetch_add_explicit(&g_peer, 1, memory_order_relaxed);
		xtc_yield();
	}
}

static MunitResult
test_p2_preempt_tight_loop(const MunitParameter p[], void *d)
{
	xtc_exec_t *e = NULL;
	xtc_loop_t *l;
	int i;
	(void)p; (void)d;

	if (!xtc_preempt_supported())
		return MUNIT_OK;   /* no per-thread CPU-time timers; skip */

	atomic_store(&g_peer, 0);
	atomic_store(&g_runaway_done, 0);

	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	l = xtc_exec_loop(e, 0);

	/* Enable the timer (1 ms slices) AND the signal-context
	 * involuntary yield. */
	munit_assert_int(xtc_exec_set_preempt(e, 1 * 1000 * 1000LL), ==, XTC_OK);
	xtc_preempt_set_involuntary(1);

	/* One pure-tight-loop runaway (no yield points) + peers. */
	(void)xtc_proc_spawn(l, runaway,
	    (void *)(intptr_t)(400ULL * 1000 * 1000), NULL, NULL);
	for (i = 0; i < 4; i++)
		(void)xtc_proc_spawn(l, peer, NULL, NULL, NULL);

	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);

	/* The runaway completed (resumable preemption: it was sliced, not
	 * killed). */
	munit_assert_int(atomic_load(&g_runaway_done), ==, 1);

	/*
	 * If the involuntary yield is active (ucontext substrate), the
	 * peers made progress WHILE the pure-tight-loop runaway ran --
	 * impossible under cooperation alone, since the runaway never
	 * yields.  On the fctx/winfiber substrate the redirect declines
	 * (the runaway is not preemptible with no yield points), so the
	 * peers may not have run until the runaway finished; we only
	 * assert the runaway completed there.
	 */
	xtc_preempt_set_involuntary(0);
	(void)xtc_exec_fini(e);
	return MUNIT_OK;
}

/* A stronger, substrate-aware variant: on a ucontext build, assert the
 * peers actually ran mid-runaway.  Detected by whether a prior
 * involuntary preemption sliced anything -- we infer the substrate by
 * re-running with a compute fiber that never yields and checking peer
 * progress is nonzero. */
static MunitResult
test_p2_peers_progress_on_ucontext(const MunitParameter p[], void *d)
{
	xtc_exec_t *e = NULL;
	xtc_loop_t *l;
	int i, peers;
	(void)p; (void)d;

	if (!xtc_preempt_supported())
		return MUNIT_OK;

	atomic_store(&g_peer, 0);
	atomic_store(&g_runaway_done, 0);

	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	l = xtc_exec_loop(e, 0);
	munit_assert_int(xtc_exec_set_preempt(e, 1 * 1000 * 1000LL), ==, XTC_OK);
	xtc_preempt_set_involuntary(1);

	(void)xtc_proc_spawn(l, runaway,
	    (void *)(intptr_t)(400ULL * 1000 * 1000), NULL, NULL);
	for (i = 0; i < 4; i++)
		(void)xtc_proc_spawn(l, peer, NULL, NULL, NULL);

	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	peers = atomic_load(&g_peer);
	munit_assert_int(atomic_load(&g_runaway_done), ==, 1);

	/*
	 * peers > 0 proves true preemption occurred (a pure tight loop was
	 * sliced).  On a substrate that declines the involuntary yield the
	 * peers cannot have run before the runaway finished, so peers would
	 * be 0 -- which is the documented fallback, not a failure.  We
	 * therefore assert the invariant that HOLDS on every substrate (the
	 * runaway completed) and additionally REPORT whether preemption
	 * fired.
	 */
	munit_logf(MUNIT_LOG_INFO,
	    "peers advanced %d times during a pure-tight-loop runaway "
	    "(>0 == true preemption fired; 0 == substrate fallback)", peers);

	/*
	 * On a substrate that implements Phase 2b-arch (x86-64 ucontext,
	 * the default glibc build), a pure tight loop with no yield points
	 * MUST have been sliced, so peers MUST have advanced.  This is the
	 * assertion that proves involuntary preemption actually works --
	 * Phase 1 cooperative scheduling could never make peers run while a
	 * non-yielding runaway holds the loop.  On substrates that decline
	 * the redirect (fctx/musl, winfiber, the amalgamation) peers may be
	 * 0 (documented fallback); __xtc_coro_preempt_effective() reports
	 * which, at runtime, so this needs no build-macro visibility.
	 */
	{
		extern int __xtc_coro_preempt_effective(void);
		if (__xtc_coro_preempt_effective())
			munit_assert_int(peers, >, 0);
	}

	xtc_preempt_set_involuntary(0);
	(void)xtc_exec_fini(e);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/preempt_tight_loop", test_p2_preempt_tight_loop, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/peers_progress", test_p2_peers_progress_on_ucontext, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m14/preempt_p2", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
