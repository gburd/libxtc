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
#include "xtc_sim.h"

/*
 * DST phase 3 -- parking + cross-loop messaging under the deterministic
 * scheduler.  A ping proc sends to a pong proc on ANOTHER loop and
 * waits (parks) for the reply; pong recvs (parks until the ping
 * arrives) and replies.  This exercises the mailbox park/wake path
 * across loops on the single sim thread.  N independent ping/pong pairs
 * run concurrently; the run must reach quiescence and replay from seed.
 */

#define N_LOOPS 4
#define N_PAIRS 8

static atomic_int g_replies;
static atomic_long g_hash;

static void
pong(void *arg)
{
	(void)arg;
	void *m = NULL; size_t n = 0;
	/* Park until the ping arrives, then reply to the sender. */
	if (xtc_recv(&m, &n, -1) == XTC_OK && m != NULL) {
		xtc_pid_t from;
		memcpy(&from, m, sizeof from);
		free(m);
		int r = 1;
		(void)xtc_send(from, &r, sizeof r);
	}
}

struct ping_arg { xtc_pid_t peer; long id; };

static void
ping(void *arg)
{
	struct ping_arg *pa = arg;
	xtc_pid_t self = xtc_self();
	void *m = NULL; size_t n = 0;
	long h;
	/* Send our pid to pong, then PARK awaiting the reply. */
	(void)xtc_send(pa->peer, &self, sizeof self);
	if (xtc_recv(&m, &n, -1) == XTC_OK && m != NULL) {
		free(m);
		atomic_fetch_add_explicit(&g_replies, 1, memory_order_relaxed);
		/* Order-sensitive fold: the order replies are observed is a
		 * function of the seed. */
		h = atomic_load_explicit(&g_hash, memory_order_relaxed);
		h = h * 1000003L + (pa->id + 1);
		atomic_store_explicit(&g_hash, h, memory_order_relaxed);
	}
}

static struct ping_arg g_args[N_PAIRS];

static long
run_once(uint64_t seed, int *out_replies)
{
	xtc_exec_t *e = NULL;
	int i;
	atomic_store(&g_replies, 0);
	atomic_store(&g_hash, 0);
	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) { *out_replies = -1; return -1; }

	for (i = 0; i < N_PAIRS; i++) {
		/* pong on loop i%N, ping on a DIFFERENT loop (i+1)%N -- forces
		 * the cross-loop park/wake path. */
		xtc_loop_t *lp = xtc_exec_loop(e, (unsigned)(i % N_LOOPS));
		xtc_loop_t *li = xtc_exec_loop(e, (unsigned)((i + 1) % N_LOOPS));
		xtc_pid_t pong_pid;
		(void)xtc_proc_spawn(lp, pong, NULL, NULL, &pong_pid);
		g_args[i].peer = pong_pid;
		g_args[i].id = i;
		(void)xtc_proc_spawn(li, ping, &g_args[i], NULL, NULL);
	}
	(void)xtc_sim_exec_run(e, seed, 2000000);
	*out_replies = atomic_load(&g_replies);
	(void)xtc_exec_fini(e);
	return atomic_load(&g_hash);
}

int
main(void)
{
	int r1 = 0, r2 = 0;
	long h1 = run_once(0x1234, &r1);
	long h2 = run_once(0x1234, &r2);

	printf("run1: replies=%d hash=%ld\n", r1, h1);
	printf("run2: replies=%d hash=%ld\n", r2, h2);

	if (r1 != N_PAIRS || r2 != N_PAIRS) {
		printf("FAIL: not all pings got replies (expected %d)\n", N_PAIRS);
		return 1;
	}
	if (h1 != h2) {
		printf("FAIL: cross-loop park/wake did not replay (%ld != %ld)\n", h1, h2);
		return 1;
	}
	printf("OK: cross-loop ping/pong with parking replays under the "
	       "deterministic scheduler (replies=%d, hash=%ld)\n", r1, h1);
	return 0;
}
