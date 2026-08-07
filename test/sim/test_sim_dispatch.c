#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_dispatch.h"
#include "xtc_future.h"
#include "xtc_sim.h"

/*
 * DST -- xtc_dispatch (roadmap B1) under the deterministic scheduler.
 *
 * The dispatcher is the callback -> fiber bridge: submit an effect from
 * OUTSIDE a loop and get a future for its result.  The deterministic
 * property proven here: every dispatched effect runs EXACTLY ONCE and
 * its future resolves (never lost, never doubled), and the whole run
 * replays byte-for-byte from (seed, config).
 *
 * xtc_dispatch is called BEFORE xtc_sim_exec_run, from the sim driver
 * thread (not on any loop), so each submission takes the cross-thread
 * spawn path (MPSC inbox + wakeup) that the executor drains
 * deterministically.  The effect fn increments a run counter and folds
 * its id into an order-sensitive hash; a finalizer-registered future
 * cannot be awaited from the driver (that would block the sim), so the
 * effect resolves its own future via xtc_promise_set inside the runtime
 * and a monitor-style at-exit records that the future became ready.
 */

#define N_LOOPS   4
#define N_EFFECTS 12

static atomic_int  g_ran;        /* effect bodies that executed */
static atomic_int  g_resolved;   /* futures observed resolved */
static atomic_long g_hash;       /* order-sensitive fold of effect ids */

/* Each dispatched effect: run once, fold id, resolve via return. */
static int
effect_body(void *arg)
{
	long id = (long)(intptr_t)arg;
	long h;
	atomic_fetch_add_explicit(&g_ran, 1, memory_order_relaxed);
	h = atomic_load_explicit(&g_hash, memory_order_relaxed);
	h = h * 1000003L + (id + 1);
	atomic_store_explicit(&g_hash, h, memory_order_relaxed);
	return (int)id;
}

/*
 * A consumer fiber that awaits the dispatched future FROM INSIDE the
 * runtime (a fiber can park cooperatively on it), so the resolution is
 * observed on the sim thread deterministically.  We spawn one consumer
 * per dispatched effect, each handed the future.
 */
static void
consumer(void *arg)
{
	xtc_future_t *fut = arg;
	intptr_t out = -1;
	if (xtc_future_await(fut, &out) == XTC_OK)
		atomic_fetch_add_explicit(&g_resolved, 1, memory_order_relaxed);
}

static long
run_once(uint64_t seed, int *out_ran, int *out_resolved)
{
	xtc_exec_t *e = NULL;
	int i;

	atomic_store(&g_ran, 0);
	atomic_store(&g_resolved, 0);
	atomic_store(&g_hash, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) {
		*out_ran = -1; *out_resolved = -1; return -1;
	}

	for (i = 0; i < N_EFFECTS; i++) {
		xtc_loop_t *l = xtc_exec_loop(e, (unsigned)(i % N_LOOPS));
		xtc_future_t *fut = NULL;
		/* Dispatch the effect from the driver thread (off any loop):
		 * the cross-thread spawn path. */
		if (xtc_dispatch(l, effect_body, (void *)(intptr_t)i, &fut, NULL)
		    != XTC_OK)
			continue;
		/* Spawn a consumer on a DIFFERENT loop to await the future,
		 * exercising cross-loop future wake under the scheduler. */
		{
			xtc_loop_t *lc =
			    xtc_exec_loop(e, (unsigned)((i + 1) % N_LOOPS));
			(void)xtc_proc_spawn(lc, consumer, fut, NULL, NULL);
		}
	}

	(void)xtc_sim_exec_run(e, seed, 2000000);
	*out_ran = atomic_load(&g_ran);
	*out_resolved = atomic_load(&g_resolved);
	(void)xtc_exec_fini(e);
	return atomic_load(&g_hash);
}

int
main(void)
{
	int ran1 = 0, ran2 = 0, res1 = 0, res2 = 0;
	long h1 = run_once(0x51ce, &ran1, &res1);
	long h2 = run_once(0x51ce, &ran2, &res2);

	printf("run1: ran=%d resolved=%d hash=%ld\n", ran1, res1, h1);
	printf("run2: ran=%d resolved=%d hash=%ld\n", ran2, res2, h2);

	/* Every effect runs EXACTLY once. */
	if (ran1 != N_EFFECTS || ran2 != N_EFFECTS) {
		printf("FAIL: effect did not run exactly once per dispatch "
		       "(expected %d)\n", N_EFFECTS);
		return 1;
	}
	/* Every future resolves (never lost). */
	if (res1 != N_EFFECTS || res2 != N_EFFECTS) {
		printf("FAIL: a dispatched future did not resolve "
		       "(expected %d)\n", N_EFFECTS);
		return 1;
	}
	/* Byte-identical replay from the seed. */
	if (h1 != h2) {
		printf("FAIL: dispatch did not replay (%ld != %ld)\n", h1, h2);
		return 1;
	}
	printf("OK: %d dispatched effects each ran exactly once and their "
	       "futures resolved, replaying deterministically "
	       "(hash=%ld)\n", N_EFFECTS, h1);
	return 0;
}
