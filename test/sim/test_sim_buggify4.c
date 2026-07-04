#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_aio.h"
#include "xtc_async.h"     /* xtc_yield */
#include "xtc_sim.h"

/*
 * DST coverage of the SCHEDULER-PATH + AIO buggify sites planted in the
 * real runtime beyond the primitive-level ones (mailbox / channel /
 * steal / svr / sem / reg):
 *
 *   timer.fire.late            (src/evt/loop.c) -- a due timer is fired
 *       one scheduler turn LATE (re-armed a bounded step later, at most
 *       once).  A timer firing late is always tolerated; the sleeper
 *       simply wakes a hair later in virtual time.
 *   sched.inbox.drain_one_fewer (src/evt/loop.c) -- the cross-loop inbox
 *       drain processes one FEWER message this turn, holding the tail
 *       message back for the next drain.  The held WAKE/PUBLISH is not
 *       lost (the loop stays runnable and drains it next step): a legal
 *       one-turn delay.
 *   sched.runq.defer_ready     (src/evt/loop.c) -- a ready task is
 *       DEFERRED one turn (re-enqueued, a different ready task runs
 *       instead) -- the local-run-queue twin of sched.steal.skip_near.
 *       Exactly XTC_TASK_RESCHED, which the loop already tolerates.
 *   io.aio.slow_completion     (src/io/io_sim.c) -- a deferred file-AIO
 *       completion is pushed an EXTRA tick later.  The fiber is parked
 *       awaiting it and simply wakes later: a legal slow-disk delay.
 *
 * PART A (scheduler paths): cross-loop ping/pong pairs (exercise the
 * inbox drain + the run queue) plus timer sleepers (exercise
 * timer.fire.late).  PART B (aio): fibers doing a write+read on a shared
 * temp file with faults enabled so completions defer (io.aio.slow_...
 * fires).  Both assert, mirroring test_sim_buggify2:
 *   - PROGRESS: every unit completes under buggify (the pessimal paths
 *     are all legal and lose nothing);
 *   - ACTIVATION: buggify ON activates at least one new site;
 *   - REPLAY: same seed -> same activation count + same ORDER-sensitive
 *     completion hash;
 *   - DISABLED => zero activations.
 */

#define N_LOOPS   4
#define N_PAIRS   4
#define N_HOPS    5
#define N_SLEEP   4

struct pair { xtc_pid_t peer; int id; };
static struct pair g_pairs[N_PAIRS];
static xtc_pid_t   g_pongs[N_PAIRS];

static atomic_int  g_replies;
static atomic_int  g_sleeps;
static atomic_long g_hash;

static void
fold(long v)
{
	long h = atomic_load_explicit(&g_hash, memory_order_relaxed);
	h = h * 1000003L + (v + 1);
	atomic_store_explicit(&g_hash, h, memory_order_relaxed);
}

/* Echo server: reply to every message with the sender's pid. */
static void
pong(void *arg)
{
	int guard = 0;
	(void)arg;
	while (guard++ < 200000) {
		void  *m = NULL;
		size_t n = 0;
		xtc_pid_t from;
		if (xtc_recv(&m, &n, 20 * 1000 * 1000LL) != XTC_OK || m == NULL)
			break;                 /* no more work -> exit */
		if (n >= sizeof from) {
			memcpy(&from, m, sizeof from);
			(void)xtc_send(from, &from, sizeof from);
		}
		free(m);
	}
}

static void
ping(void *arg)
{
	struct pair *pa = arg;
	int hops = N_HOPS, guard = 0;
	xtc_pid_t self = xtc_self();
	while (hops > 0 && guard++ < 200000) {
		void  *m = NULL;
		size_t n = 0;
		int rc = xtc_send(pa->peer, &self, sizeof self);
		if (rc == XTC_E_AGAIN) {       /* soft-full: back off + retry */
			(void)xtc_proc_sleep(1 * 1000 * 1000LL);
			continue;
		}
		if (rc != XTC_OK)
			return;
		if (xtc_recv(&m, &n, 10 * 1000 * 1000LL) != XTC_OK || m == NULL)
			continue;
		free(m);
		atomic_fetch_add_explicit(&g_replies, 1, memory_order_relaxed);
		fold(pa->id + 1);
		hops--;
	}
	/* Tell the peer to stop by sending nothing more; the pong exits on
	 * its recv timeout once no ping is sending. */
}

static void
sleeper(void *arg)
{
	long id = (long)(intptr_t)arg;
	int i;
	for (i = 0; i < 3; i++)
		(void)xtc_proc_sleep((int64_t)(id % 3 + 1) * 1000000LL);
	atomic_fetch_add_explicit(&g_sleeps, 1, memory_order_relaxed);
	fold(1000 + id);
}

static int
run_once(uint64_t seed, unsigned bug_pct, int *out_replies, int *out_sleeps,
    long *out_hash, int *out_bug)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	atomic_store(&g_replies, 0);
	atomic_store(&g_sleeps, 0);
	atomic_store(&g_hash, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	if (bug_pct > 0) xtc_sim_buggify_enable(bug_pct);
	else             xtc_sim_buggify_disable();

	for (i = 0; i < N_PAIRS; i++) {
		xtc_loop_t *lp = xtc_exec_loop(e, (unsigned)(i % N_LOOPS));
		xtc_loop_t *li = xtc_exec_loop(e, (unsigned)((i + 1) % N_LOOPS));
		(void)xtc_proc_spawn(lp, pong, NULL, NULL, &g_pongs[i]);
		g_pairs[i].peer = g_pongs[i];
		g_pairs[i].id = i;
		(void)xtc_proc_spawn(li, ping, &g_pairs[i], NULL, NULL);
	}
	for (i = 0; i < N_SLEEP; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(i % N_LOOPS)),
		    sleeper, (void *)(intptr_t)i, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_replies = atomic_load(&g_replies);
	*out_sleeps  = atomic_load(&g_sleeps);
	*out_hash    = atomic_load(&g_hash);
	*out_bug     = xtc_sim_buggify_active_count();

	xtc_sim_buggify_disable();
	(void)xtc_exec_fini(e);
	return rc;
}

/* ============ PART B: io.aio.slow_completion ============ */

#define B_WORKERS 8
#define B_REGION  1024

static atomic_int  g_b_done;
static atomic_long g_b_hash;
static int         g_b_fd = -1;

static void
b_worker(void *arg)
{
	long id = (long)(intptr_t)arg;
	int64_t off = id * B_REGION;
	uint8_t buf[B_REGION];
	int w, r;
	long h;

	memset(buf, (int)(id & 0xff), sizeof buf);
	w = xtc_aio_pwrite(g_b_fd, buf, B_REGION, off);
	(void)xtc_aio_fsync(g_b_fd);
	memset(buf, 0, sizeof buf);
	r = xtc_aio_pread(g_b_fd, buf, B_REGION, off);
	h = atomic_load_explicit(&g_b_hash, memory_order_relaxed);
	h = h * 1000003L + (w + 1);
	h = h * 1000003L + (r + 1);
	atomic_store_explicit(&g_b_hash, h, memory_order_relaxed);
	atomic_fetch_add_explicit(&g_b_done, 1, memory_order_relaxed);
}

static int
run_b(uint64_t seed, unsigned bug_pct, int *out_done, long *out_hash,
    int *out_bug)
{
	xtc_exec_t *e = NULL;
	char path[] = "/scratch/xtc-test/sim_bug4_XXXXXX";
	int i, rc;

	atomic_store(&g_b_done, 0);
	atomic_store(&g_b_hash, 0);

	g_b_fd = mkstemp(path);
	if (g_b_fd < 0) {
		char p2[] = "sim_bug4_XXXXXX";
		g_b_fd = mkstemp(p2);
		if (g_b_fd < 0) return -1;
		(void)unlink(p2);
	} else {
		(void)unlink(path);
	}
	(void)ftruncate(g_b_fd, (off_t)B_WORKERS * B_REGION);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) { close(g_b_fd); return -1; }
	if (bug_pct > 0) xtc_sim_buggify_enable(bug_pct);
	else             xtc_sim_buggify_disable();

	/* Faults enabled for LATENCY only (0% fault pct) so completions
	 * defer + park; io.aio.slow_completion then adds its extra tick. */
	xtc_sim_io_faults_enable(50 * 1000LL, 500 * 1000LL, 0);

	for (i = 0; i < B_WORKERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(i % N_LOOPS)),
		    b_worker, (void *)(intptr_t)i, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_done = atomic_load(&g_b_done);
	*out_hash = atomic_load(&g_b_hash);
	*out_bug  = xtc_sim_buggify_active_count();

	xtc_sim_io_faults_disable();
	xtc_sim_buggify_disable();
	(void)xtc_exec_fini(e);
	close(g_b_fd);
	g_b_fd = -1;
	return rc;
}

int
main(void)
{
	int r1 = 0, s1 = 0, b1 = 0, r2 = 0, s2 = 0, b2 = 0;
	int roff = 0, soff = 0, boff = 0;
	long h1 = 0, h2 = 0, hoff = 0;
	int want_r = N_PAIRS * N_HOPS, want_s = N_SLEEP;
	int rc;

	rc = run_once(0x717A3E14, 1000, &r1, &s1, &h1, &b1);
	if (rc != XTC_OK) { printf("FAIL: buggify run rc=%d\n", rc); return 1; }
	(void)run_once(0x717A3E14, 1000, &r2, &s2, &h2, &b2);
	rc = run_once(0x717A3E14, 0, &roff, &soff, &hoff, &boff);
	if (rc != XTC_OK) { printf("FAIL: buggify-off run rc=%d\n", rc); return 1; }

	printf("bug ON  run1: replies=%d/%d sleeps=%d/%d active=%d\n",
	    r1, want_r, s1, want_s, b1);
	printf("bug ON  run2: replies=%d/%d sleeps=%d/%d active=%d\n",
	    r2, want_r, s2, want_s, b2);
	printf("bug OFF run : replies=%d/%d sleeps=%d/%d active=%d\n",
	    roff, want_r, soff, want_s, boff);

	if (r1 != want_r || roff != want_r || s1 != want_s || soff != want_s) {
		printf("FAIL: progress lost under buggify (replies %d/%d "
		    "sleeps %d/%d, want %d/%d) -- a scheduler pessimal path "
		    "dropped a message or a timer\n", r1, roff, s1, soff,
		    want_r, want_s);
		return 1;
	}
	if (r1 != r2 || s1 != s2 || h1 != h2 || b1 != b2) {
		printf("FAIL: buggify run did not replay (replies %d/%d "
		    "sleeps %d/%d hash %ld/%ld active %d/%d)\n",
		    r1, r2, s1, s2, h1, h2, b1, b2);
		return 1;
	}
	if (boff != 0) {
		printf("FAIL: buggify DISABLED but %d points activated\n", boff);
		return 1;
	}
	if (b1 == 0) {
		printf("FAIL: buggify enabled but no scheduler-path site "
		    "activated -- timer.fire.late / drain_one_fewer / "
		    "defer_ready never fired\n");
		return 1;
	}

	printf("OK: scheduler-path buggify sites under DST -- "
	    "timer.fire.late + sched.inbox.drain_one_fewer + "
	    "sched.runq.defer_ready (%d activation(s)); every ping replied "
	    "(%d) and every sleeper woke (%d) despite the pessimal delays, "
	    "replays from seed; disabled => zero activations\n",
	    b1, r1, s1);

	/* ---- PART B: io.aio.slow_completion ---- */
	int bd1 = 0, bb1 = 0, bd2 = 0, bb2 = 0, bdoff = 0, bboff = 0;
	long bh1 = 0, bh2 = 0, bhoff = 0;

	rc = run_b(0x5100D15C, 1000, &bd1, &bh1, &bb1);
	if (rc != XTC_OK) { printf("FAIL: aio buggify run rc=%d\n", rc); return 1; }
	(void)run_b(0x5100D15C, 1000, &bd2, &bh2, &bb2);
	rc = run_b(0x5100D15C, 0, &bdoff, &bhoff, &bboff);
	if (rc != XTC_OK) { printf("FAIL: aio buggify-off run rc=%d\n", rc); return 1; }

	printf("aio ON  run1: done=%d/%d active=%d\n", bd1, B_WORKERS, bb1);
	printf("aio ON  run2: done=%d/%d active=%d\n", bd2, B_WORKERS, bb2);
	printf("aio OFF run : done=%d/%d active=%d\n", bdoff, B_WORKERS, bboff);

	if (bd1 != B_WORKERS || bdoff != B_WORKERS) {
		printf("FAIL: aio workers lost progress (done %d/%d, want %d) -- "
		    "a slow completion lost its wakeup\n", bd1, bdoff, B_WORKERS);
		return 1;
	}
	if (bd1 != bd2 || bh1 != bh2 || bb1 != bb2) {
		printf("FAIL: aio buggify did not replay (done %d/%d hash %ld/%ld "
		    "active %d/%d)\n", bd1, bd2, bh1, bh2, bb1, bb2);
		return 1;
	}
	if (bboff != 0) {
		printf("FAIL: aio buggify DISABLED but %d activated\n", bboff);
		return 1;
	}

	printf("OK: io.aio.slow_completion under DST -- %d AIO workers, "
	    "%d activation(s), every op still completed, replays from seed; "
	    "disabled => zero activations\n", B_WORKERS, bb1);
	return 0;
}
