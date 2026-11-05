/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m14/test_aio_kill_stress.c
 *	The aio reaper must not touch an xtc_aio_t after its done store.
 *
 *	The xtc_aio_t lives on the parked fiber's stack (src/ptc/aio.c).  A
 *	fiber woken by xtc_exit_pid and stolen by another loop checks done on
 *	ITS thread; once the reaper's done store is visible it leaves the
 *	wait, takes the kill and dies, and its stack is reused.  Until 2.1.2
 *	every native reaper read a->tag after that store, so that read could
 *	land on a dead (or reused) frame.  ASan cannot see it (fiber stacks
 *	are pooled mmaps); TSan can, as a race between the reaper's read and
 *	the next write to that stack slot.  .github/tsan.supp suppressed it as
 *	`race:aio_do` until the fix: run this binary without that stanza.
 *
 *	Shape: 8 loops, migratable procs doing back-to-back xtc_aio_pwrite +
 *	xtc_aio_pread on their own 512-byte slot of one file, in masked
 *	bursts of BURST ops, and a boss proc that xtc_exit_pid()s random
 *	workers and respawns each one it sees go DOWN, until
 *	XTC_AIO_STRESS_OPS ops (default 100000).  Inside a burst a kill is
 *	latched but still wakes the parked op: the fiber, possibly stolen by
 *	another loop, sees done without the reaper's wake and its next op
 *	reuses the same stack slot at once -- the window the late read hit.
 *	The kill lands at xtc_mask_leave.
 *	Checks: every op returns the full length and reads back its own
 *	bytes, every DOWN carries reason 0 or the kill reason, kills landed,
 *	and the run ends (SIGALRM guard).
 */

#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_aio.h"
#include "xtc_exec.h"
#include "xtc_fs.h"
#include "xtc_proc.h"

#define LOOPS 8
#define WORKERS 32
#define SLOT 512
#define KILL_REASON 77
#define BURST 16 /* ops per masked burst */

static xtc_exec_t *g_exec;
static int g_fd = -1;
static long g_target;
static atomic_long g_ops;
static atomic_int g_bad;
static xtc_pid_t g_pid[WORKERS];
static int g_kills_sent, g_killed, g_exited, g_bad_reason;

static void
worker(void *arg)
{
	int slot = (int)(intptr_t)arg;
	int64_t off = (int64_t)slot * SLOT;
	char buf[SLOT], rd[SLOT];

	memset(buf, 'a' + slot % 26, sizeof buf);
	while (atomic_load_explicit(&g_ops, memory_order_relaxed) < g_target) {
		int j, ok = 1;
		if (xtc_mask_enter() != XTC_OK) {
			atomic_fetch_add(&g_bad, 1);
			return;
		}
		for (j = 0; ok && j < BURST / 2; j++) {
			memset(rd, 0, sizeof rd);
			ok = xtc_aio_pwrite(g_fd, buf, sizeof buf, off) ==
			        SLOT &&
			    xtc_aio_pread(g_fd, rd, sizeof rd, off) == SLOT &&
			    memcmp(rd, buf, sizeof rd) == 0;
		}
		if (!ok)
			atomic_fetch_add(&g_bad, 1);
		atomic_fetch_add_explicit(&g_ops, 2 * j, memory_order_relaxed);
		(void)xtc_mask_leave(); /* a latched kill lands here */
		if (!ok)
			return;
	}
}

static int
spawn_worker(int slot)
{
	xtc_proc_opts_t o;
	uint64_t ref;

	memset(&o, 0, sizeof o);
	o.size = sizeof o;
	o.migratable = 1;
	return xtc_proc_spawn_monitor(xtc_exec_loop(g_exec,
	                                  (unsigned)(slot % LOOPS)),
	    worker, (void *)(intptr_t)slot, &o, &g_pid[slot], &ref);
}

/* Spawns the workers, kills one at random every idle 20-100 us, and
 * respawns every worker it sees go DOWN until the op target is met. */
static void
boss(void *arg)
{
	int i, live = 0;
	(void)arg;

	for (i = 0; i < WORKERS; i++)
		if (spawn_worker(i) == XTC_OK)
			live++;
	while (live > 0) {
		void *m = NULL;
		size_t n = 0;
		xtc_pid_t who;
		int reason = 0;
		int64_t wait = 20000 + (int64_t)(xtc_rand_u64() % 80000);

		if (xtc_recv(&m, &n, wait) != XTC_OK) {
			int k;
			for (k = 0; k < 4 && atomic_load(&g_ops) < g_target;
			     k++) {
				i = (int)(xtc_rand_u64() % WORKERS);
				if (xtc_exit_pid(g_pid[i], KILL_REASON) ==
				    XTC_OK)
					g_kills_sent++;
			}
			continue;
		}
		if (xtc_down_decode(m, n, &who, &reason) != XTC_OK) {
			xtc_free(m);
			continue;
		}
		xtc_free(m);
		live--;
		if (reason == KILL_REASON)
			g_killed++;
		else if (reason == 0)
			g_exited++;
		else
			g_bad_reason++;
		for (i = 0; i < WORKERS; i++)
			if (xtc_pid_eq(g_pid[i], who))
				break;
		if (i < WORKERS && atomic_load(&g_ops) < g_target &&
		    spawn_worker(i) == XTC_OK)
			live++;
	}
}

static void
on_alarm(int s)
{
	char m[160];
	int n;
	(void)s;
	n = snprintf(m, sizeof m,
	    "FAIL: hang -- %ld/%ld ops, %d kills "
	    "landed\n",
	    atomic_load(&g_ops), g_target, g_killed);
	(void)!write(2, m, (size_t)n);
	_exit(1);
}

static MunitResult
test_kill_stress(const MunitParameter p[], void *d)
{
	char dir[512], path[600];
	const char *env = getenv("XTC_AIO_STRESS_OPS");
	(void)p;
	(void)d;

	g_target = env != NULL ? atol(env) : 100000;
	atomic_store(&g_ops, 0);
	atomic_store(&g_bad, 0);
	g_kills_sent = g_killed = g_exited = g_bad_reason = 0;
	munit_assert_int(xtc_fs_tmpdir(dir, sizeof dir), ==, XTC_OK);
	snprintf(path, sizeof path, "%s/xtc_aio_kill_XXXXXX", dir);
	g_fd = mkstemp(path);
	munit_assert_int(g_fd, >=, 0);
	(void)unlink(path);
	munit_assert_int(ftruncate(g_fd, (off_t)WORKERS * SLOT), ==, 0);

	signal(SIGALRM, on_alarm);
	alarm(900); /* healthy: ~1 s plain, a few minutes under TSan */
	munit_assert_int(xtc_exec_init(&g_exec, LOOPS), ==, XTC_OK);
	xtc_exec_set_eager_rebalance(g_exec, 1);
	munit_assert_int(xtc_proc_spawn(xtc_exec_loop(g_exec, LOOPS - 1), boss,
	                     NULL, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_exec_run(g_exec), ==, XTC_OK);
	(void)xtc_exec_fini(g_exec);
	alarm(0);
	close(g_fd);

	fprintf(stderr,
	    "aio_kill: %ld ops, %d kills sent, %d landed, %d "
	    "workers exited normally\n",
	    atomic_load(&g_ops), g_kills_sent, g_killed, g_exited);
	munit_assert_int(atomic_load(&g_bad), ==, 0);
	munit_assert_int(g_bad_reason, ==, 0);
	munit_assert_long(atomic_load(&g_ops), >=, g_target);
	munit_assert_int(g_killed, >, 0);
	return MUNIT_OK;
}

static MunitTest tests[] = { { "/kill_stress", test_kill_stress, NULL, NULL,
	                         MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL } };

static const MunitSuite suite = { "/m14/aio_kill", tests, NULL, 1,
	MUNIT_SUITE_OPTION_NONE };

int
main(int argc, char *argv[MUNIT_ARRAY_PARAM(argc + 1)])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
