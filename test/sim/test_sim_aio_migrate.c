/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_aio_migrate.c
 *	DST proof that a MIGRATABLE fiber's async-file completion is always
 *	reaped and the fiber always resumed, even when the fiber is
 *	work-stolen to a different loop between submitting the op and its
 *	completion arriving.
 *
 *	This is the deterministic model of the consumer's lost-wake hang
 *	(2026-09): a fiber homed on one loop submits an fdatasync while
 *	running (possibly transiently) on another loop, so the completion
 *	lands on the RUNNING loop's ring -- and that loop must reap it even
 *	though the fiber is not homed there and may have migrated away
 *	again.  Real io_uring ties the completion to the submitting ring;
 *	the sim io backend models that faithfully (io->sim event store,
 *	drained by that io's own poll), and the SAME __xtc_loop_step_once /
 *	__xtc_loop_step scheduling decision that runs in production runs here
 *	under one deterministic thread.
 *
 *	The invariant is strong and simple: every worker completes every
 *	commit and the run terminates within budget.  A lost completion is a
 *	HANG, which the bounded xtc_sim_exec_run turns into a deterministic
 *	failure (done < N_WORKERS) with a replayable seed -- the
 *	bug-detection-latency yardstick.  The pinned (migratable=0) control
 *	must also pass, so a migratable-only failure localizes the defect to
 *	the migration path rather than to aio.
 *
 *	Sim tests are plain main()/return-0-or-1, not munit (they link the
 *	sim library, which does not pull in the munit runner).
 */

#include <stdlib.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_aio.h"
#include "xtc_sim.h"

#define N_LOOPS    8
#define N_WORKERS  24     /* > N_LOOPS, all spawned on loop 0 => heavy steal */
#define N_COMMITS  16     /* write+fdatasync pairs per worker */

static _Atomic int  g_done;
static _Atomic long g_commits;
static int          g_fd;

static void
commit_worker(void *arg)
{
	int i;
	int64_t off_base = (int64_t)(intptr_t)arg * 512 * (N_COMMITS + 1);
	char buf[512];

	memset(buf, 'c', sizeof buf);
	for (i = 0; i < N_COMMITS; i++) {
		if (xtc_aio_pwrite(g_fd, buf, sizeof buf,
		    off_base + (int64_t)i * 512) < 0)
			return;
		if (xtc_aio_fdatasync(g_fd) != XTC_OK)
			return;
		atomic_fetch_add_explicit(&g_commits, 1, memory_order_relaxed);
		/* A park point between commits so the stealer can migrate the
		 * fiber -- successive commits then tend to run, and complete,
		 * on different loops. */
		(void)xtc_proc_sleep(1000);
	}
	atomic_fetch_add_explicit(&g_done, 1, memory_order_relaxed);
}

static int
run_once(uint64_t seed, int migratable, long *out_commits, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	xtc_proc_opts_t o;
	char path[] = "/tmp/sim_aio_migrate_XXXXXX";
	int i, done;

	atomic_store(&g_done, 0);
	atomic_store(&g_commits, 0);

	g_fd = mkstemp(path);
	if (g_fd < 0)
		return -1;
	(void)unlink(path);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) { close(g_fd); return -1; }
	xtc_exec_set_eager_rebalance(e, 1);
	xtc_sim_io_faults_enable(50 * 1000LL, 500 * 1000LL, 0);

	memset(&o, 0, sizeof o);
	o.migratable = migratable;
	for (i = 0; i < N_WORKERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, 0), commit_worker,
		    (void *)(intptr_t)i, &o, NULL);

	(void)xtc_sim_exec_run(e, seed, 200000000);

	done = atomic_load(&g_done);
	if (out_commits) *out_commits = atomic_load(&g_commits);
	if (out_state)   *out_state = xtc_sim_state_hash(e);

	xtc_sim_io_faults_disable();
	(void)xtc_exec_fini(e);
	close(g_fd);
	return done;
}

int
main(void)
{
	uint64_t seed, h1 = 0, h2 = 0;
	int fails = 0;

	/* Migratable arm: every worker must finish, every commit reaped. */
	for (seed = 1; seed <= 24; seed++) {
		long commits = 0;
		int done = run_once(seed, 1, &commits, NULL);
		if (done != N_WORKERS ||
		    commits != (long)N_WORKERS * (long)N_COMMITS) {
			printf("  seed 0x%016llx: FAIL migratable "
			    "(done=%d/%d commits=%ld/%d)\n",
			    (unsigned long long)seed, done, N_WORKERS,
			    commits, N_WORKERS * N_COMMITS);
			fails++;
		}
	}

	/* Pinned control: must also pass. */
	for (seed = 1; seed <= 8; seed++) {
		int done = run_once(seed, 0, NULL, NULL);
		if (done != N_WORKERS) {
			printf("  seed 0x%016llx: FAIL pinned (done=%d/%d)\n",
			    (unsigned long long)seed, done, N_WORKERS);
			fails++;
		}
	}

	/* Replay: same seed, byte-identical state hash. */
	if (run_once(7, 1, NULL, &h1) != N_WORKERS ||
	    run_once(7, 1, NULL, &h2) != N_WORKERS || h1 != h2) {
		printf("  replay FAIL (h1=0x%016llx h2=0x%016llx)\n",
		    (unsigned long long)h1, (unsigned long long)h2);
		fails++;
	}

	if (fails == 0) {
		printf("OK: migratable-fiber AIO completion DST -- 24 seeds, "
		    "%d workers x %d commits each, all reaped and resumed even "
		    "when work-stolen across loops mid-flight; pinned control "
		    "and replay clean\n", N_WORKERS, N_COMMITS);
		return 0;
	}
	printf("FAIL: %d migratable-AIO case(s) failed\n", fails);
	return 1;
}
