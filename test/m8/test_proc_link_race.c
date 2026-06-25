/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m8/test_proc_link_race.c -- cross-loop link/monitor lifetime.
 *
 *	Regression guard for the cross-loop link/monitor race (1.x B3):
 *	xtc_link / xtc_monitor push an entry onto the PEER's list, and the
 *	peer may be exiting concurrently on another loop, where
 *	__notify_links_and_monitors walks/frees those lists and then frees
 *	the proc struct.  Before the fix the peer push was unlocked, so a
 *	link racing a peer exit could tear the list or use the freed
 *	struct.  This test runs many linker procs on several loops, each
 *	linking + monitoring short-lived children that exit immediately,
 *	so the push lands across the child's exit window.  Run under
 *	AddressSanitizer (the CI sanitizer job) it would fault on the
 *	use-after-free if the locking regressed; correctness here is "no
 *	crash, no sanitizer report, every loop drains."
 */

#define _GNU_SOURCE

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"

#define N_LOOPS   4
#define N_LINKERS 64        /* linker procs spread across the loops */
#define N_ROUNDS  40        /* children each linker races per run */

static atomic_int g_done;   /* linkers that finished */
static xtc_exec_t *g_exec;

/* A child that exits the instant it runs -- so a cross-loop link/monitor
 * targeting it lands right around its exit + free. */
static void
child_proc(void *arg)
{
	(void)arg;
	/* exit immediately (normal) */
}

/* A linker: repeatedly spawn a child on the NEXT loop (forcing the
 * cross-loop path) and immediately link + monitor it.  Exit signals
 * arrive as ordinary messages (this runtime does not force-kill on a
 * linked peer's exit), so we simply drain them. */
static void
linker_proc(void *arg)
{
	int r;
	int my = (int)(intptr_t)arg;
	for (r = 0; r < N_ROUNDS; r++) {
		xtc_loop_t *target;
		xtc_pid_t   child;
		uint64_t    ref;
		int next = (my + 1) % N_LOOPS;
		target = xtc_exec_loop(g_exec, next);
		if (target == NULL) continue;
		if (xtc_proc_spawn(target, child_proc, NULL, NULL, &child) != XTC_OK)
			continue;
		/* Race the child's exit + free with the peer-side push. */
		(void)xtc_link(child);
		(void)xtc_monitor(child, &ref);
		/* Yield so the child (possibly already done) is reaped, then
		 * drain any EXIT/DOWN we were sent. */
		xtc_yield();
		for (;;) {
			void *m = NULL; size_t n = 0;
			if (xtc_recv(&m, &n, 0) != XTC_OK) break;
			free(m);
		}
	}
	atomic_fetch_add(&g_done, 1);
}

static MunitResult
test_link_race(const MunitParameter p[], void *d)
{
	int i;
	(void)p; (void)d;

	atomic_store(&g_done, 0);
	munit_assert_int(xtc_exec_init(&g_exec, N_LOOPS), ==, XTC_OK);
	for (i = 0; i < N_LINKERS; i++) {
		xtc_loop_t *l = xtc_exec_loop(g_exec, i % N_LOOPS);
		munit_assert_ptr_not_null(l);
		munit_assert_int(
		    xtc_proc_spawn(l, linker_proc, (void *)(intptr_t)(i % N_LOOPS),
		                   NULL, NULL), ==, XTC_OK);
	}
	munit_assert_int(xtc_exec_run(g_exec), ==, XTC_OK);
	munit_assert_int(xtc_exec_fini(g_exec), ==, XTC_OK);

	/* Every linker ran to completion (the run drained without a crash
	 * or a sanitizer abort on the freed peer struct). */
	munit_assert_int(atomic_load(&g_done), ==, N_LINKERS);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/cross_loop", test_link_race, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = {
	"/proc_link_race", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};
int main(int argc, char **argv) { return munit_suite_main(&suite, NULL, argc, argv); }
