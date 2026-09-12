/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/concurrency/test_aio_migrate_wake.c
 *	Real-thread liveness guard for the cross-loop async-file completion
 *	wake (the PG lost-wake hang fixed in v1.44.1).
 *
 *	This is the REAL-THREAD counterpart to test_sim_aio_migrate.  The
 *	deterministic simulator cannot reach this bug: its scheduler never
 *	blocks in poll and it decides runnability by directly inspecting the
 *	completion store, so a completion whose owning loop is asleep and
 *	un-nudged is delivered anyway (see .agent/DST_GAP_ANALYSIS_2026-09-11).
 *	The bug lives one level down -- in whether a real loop thread, asleep
 *	on its own ring or busy with its own fibers, actually POLLS the ring
 *	its migrated fiber's completion landed on.  Only real threads exhibit
 *	it, so it must be tested here, not in DST.
 *
 *	Shape (the one that stranded PG backends):
 *	  - a real multi-loop executor (one OS thread per loop);
 *	  - many MIGRATABLE fibers, all spawned on loop 0, eager rebalance
 *	    ON, so the work-stealer scatters them across loops between ops;
 *	  - each fiber does a long run of write + fdatasync pairs.  A given
 *	    fdatasync is submitted on whichever loop the fiber is running on,
 *	    so its completion lands on THAT loop's ring while the fiber may be
 *	    resumed elsewhere.  If the submitting loop then fails to poll its
 *	    own ring (busy with its homed fibers, or asleep and un-nudged),
 *	    the completion strands and the fiber parks forever.
 *
 *	The invariant is liveness: every fiber completes every commit.  A
 *	stranded completion is a HANG, caught by the SIGALRM watchdog as a
 *	FAIL.  Runs clean under ASan and TSan (where a data-race variant of
 *	the same family would also surface).
 *
 *	Standalone: exit 0 pass, 1 fail, 77 skip.  Alarm-guarded.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_aio.h"

#define LOOPS      8
#define WORKERS    24     /* > LOOPS, all on loop 0 => heavy migration */
#define COMMITS    40     /* write+fdatasync pairs per worker */

static atomic_int g_done;
static atomic_long g_commits;
static char g_dir[512];

static void
committer(void *arg)
{
	char buf[4096], path[600];
	int fd, i;
	(void)arg;

	memset(buf, 'w', sizeof buf);
	snprintf(path, sizeof path, "%s/awm_%p.dat", g_dir, (void *)&fd);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) { atomic_fetch_add(&g_done, 1); return; }
	(void)unlink(path);
	for (i = 0; i < COMMITS; i++) {
		if (xtc_aio_pwrite(fd, buf, sizeof buf,
		    (int64_t)(i % 32) * 4096) < 0)
			break;
		if (xtc_aio_fdatasync(fd) != XTC_OK)
			break;
		atomic_fetch_add(&g_commits, 1);
		/* A park point so the stealer can migrate us between commits;
		 * the next commit then tends to run, and complete, on a
		 * different loop than this one. */
		(void)xtc_proc_sleep(1000);
	}
	close(fd);
	atomic_fetch_add(&g_done, 1);
}

static void
on_alarm(int s)
{
	(void)s;
	/* A stranded completion = a fiber that never finished = this fires.
	 * Report the shortfall so a failure log names how far it got. */
	{
		char m[128];
		int n = snprintf(m, sizeof m,
		    "FAIL: hang -- %d/%d committers finished, %ld commits "
		    "(a cross-loop aio completion was stranded)\n",
		    atomic_load(&g_done), WORKERS, atomic_load(&g_commits));
		(void)!write(2, m, (size_t)n);
	}
	_exit(1);
}

int
main(void)
{
	xtc_exec_t *e = NULL;
	xtc_proc_opts_t o;
	int i;
	const char *tmp = getenv("TMPDIR");

	snprintf(g_dir, sizeof g_dir, "%s", tmp != NULL ? tmp : "/tmp");

	signal(SIGALRM, on_alarm);
	alarm(60);   /* generous: the bug hangs FOREVER, healthy finishes in ~1s */

	if (xtc_exec_init(&e, LOOPS) != XTC_OK) {
		printf("SKIP: xtc_exec_init failed\n");
		return 77;
	}
	xtc_exec_set_eager_rebalance(e, 1);

	memset(&o, 0, sizeof o);
	o.migratable = 1;
	for (i = 0; i < WORKERS; i++) {
		xtc_pid_t p;
		if (xtc_proc_spawn(xtc_exec_loop(e, 0), committer, NULL,
		    &o, &p) != XTC_OK) {
			printf("SKIP: spawn %d failed\n", i);
			(void)xtc_exec_fini(e);
			return 77;
		}
	}

	if (xtc_exec_run(e) != XTC_OK) {
		printf("FAIL: xtc_exec_run returned nonzero\n");
		(void)xtc_exec_fini(e);
		return 1;
	}
	(void)xtc_exec_fini(e);
	alarm(0);

	if (atomic_load(&g_done) != WORKERS) {
		printf("FAIL: %d/%d committers finished\n",
		    atomic_load(&g_done), WORKERS);
		return 1;
	}
	if (atomic_load(&g_commits) != (long)WORKERS * COMMITS) {
		printf("FAIL: %ld/%d commits completed\n",
		    atomic_load(&g_commits), WORKERS * COMMITS);
		return 1;
	}
	printf("OK: cross-loop aio wake -- %d migratable committers x %d "
	    "commits, all completions reaped and all fibers resumed under "
	    "real threads with eager rebalance\n", WORKERS, COMMITS);
	return 0;
}
