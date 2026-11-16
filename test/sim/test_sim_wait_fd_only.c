/*
 * test_sim_wait_fd_only -- DST of the mail-blind fd wait
 * (__xtc_proc_wait_fd_only, the internal form the library's own frame,
 * TLS, osproc and accelerator-fence waits use; 2.1 has no public flag).
 *
 * Per seed, on a 3-loop executor, a waiter proc and its peers run on
 * seeded loops with seeded delays and message counts:
 *
 *   BLIND   the waiter queues a message to itself, then waits READABLE
 *           on a pipe nobody writes, mail-blind, for a seeded T, while
 *           two senders on other loops send it seeded bursts.  The wait
 *           must return exactly once, at T or later (virtual time), with
 *           XTC_E_AGAIN and XTC_WAIT_TIMEOUT only, and every message
 *           must still be in the mailbox afterwards.
 *   LATE    the waiter then drains the mailbox and parks in an infinite
 *           xtc_recv; a sender's late message must wake it (the waker
 *           armed by the receive is mail-sensitive again).
 *   KILL    a victim waits mail-blind with no timeout while a killer
 *           sends it mail, checks it is still parked on the fd, and
 *           kills it: the DOWN must read EXIT with the kill's code (2.1
 *           has no separate kind for a kill).
 *   WAKE    a third proc waits mail-blind for 1 s and a peer
 *           xtc_proc_wakes it at a seeded time: XTC_OK, no bits, long
 *           before the deadline.
 *
 * Every seed runs twice and must replay byte-identically (the counters,
 * the observed virtual times and the scheduler state hash).
 *
 * Planted bug (src/inc/xtc_dst_inject.h) this test must catch:
 *   36 FDONLY_STALE  xtc_recv arms its waker without clearing the
 *                    mail-blind bit, so LATE is never woken.
 *
 * Usage: test_sim_wait_fd_only [base [count]]
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_inspect.h"
#include "xtc_io.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

#define N_LOOPS 3
#define MS (1000LL * 1000)

/* proc.c: the library's mail-blind xtc_proc_wait_fd (not exported; this
 * test links the static library). */
int __xtc_proc_wait_fd_only(int, uint32_t, int64_t, uint32_t *);

/* One seed's plan (drawn before the run) and its observations. */
struct run {
	xtc_exec_t *e;
	int pipe_a[2], pipe_k[2], pipe_w[2];
	int64_t t_ns;      /* BLIND timeout */
	int burst[2];      /* messages per sender */
	int64_t gap_ns[2]; /* sender spacing */
	int64_t late_ns;   /* when sender 0 sends the late message */
	int64_t kill_ns, wake_ns;
	xtc_pid_t waiter, victim, sleeper;

	/* observed */
	int blind_returns, blind_rc, blind_rev, blind_depth, drained;
	int64_t blind_elapsed;
	int late_got;
	int kill_parked, kill_kind, kill_code, kill_returned;
	int wake_rc, wake_rev;
	int64_t wake_elapsed;
	int done;
};

static struct run g_r;

static unsigned
pick_loop(void)
{
	return (unsigned)__xtc_sim_rng_range(XTC_SIM_RNG_APP, N_LOOPS);
}

static void
waiter(void *arg)
{
	struct run *r = arg;
	xtc_mailbox_stats_t st = XTC_MAILBOX_STATS_INIT;
	uint32_t rev = 0;
	int64_t t0;
	void *m;
	size_t n;
	int rc;

	(void)xtc_send(xtc_self(), "pre", 3);
	t0 = xtc_clock_mono();
	do {
		rev = 0;
		rc = __xtc_proc_wait_fd_only(r->pipe_a[0], XTC_IO_READABLE,
		    r->t_ns, &rev);
		r->blind_returns++;
	} while (rc == XTC_OK && r->blind_returns < 1000);
	r->blind_elapsed = xtc_clock_mono() - t0;
	r->blind_rc = rc;
	r->blind_rev = (int)rev;
	if (xtc_proc_mailbox_stats(xtc_self(), &st) == XTC_OK)
		r->blind_depth = (int)st.depth;

	while (xtc_recv(&m, &n, 0) == XTC_OK) {
		r->drained++;
		xtc_free(m);
	}
	if (xtc_recv(&m, &n, -1) == XTC_OK) { /* LATE */
		r->late_got = (n == 4 && memcmp(m, "late", 4) == 0);
		xtc_free(m);
	}
	r->done++;
}

static void
sender(void *arg)
{
	int i, k = (int)(intptr_t)arg;
	for (i = 0; i < g_r.burst[k]; i++) {
		(void)xtc_proc_sleep(g_r.gap_ns[k]);
		(void)xtc_send(g_r.waiter, "m", 1);
	}
	if (k == 0) {
		(void)xtc_proc_sleep(g_r.late_ns);
		(void)xtc_send(g_r.waiter, "late", 4);
	}
	g_r.done++;
}

static void
victim(void *arg)
{
	struct run *r = arg;
	uint32_t rev = 0;
	(void)xtc_send(xtc_self(), "pre", 3);
	(void)__xtc_proc_wait_fd_only(r->pipe_k[0], XTC_IO_READABLE, -1, &rev);
	r->kill_returned = 1; /* only a kill may end this wait */
}

static void
killer(void *arg)
{
	struct run *r = arg;
	xtc_proc_info_t info = XTC_PROC_INFO_INIT;
	xtc_down_info_t di = XTC_DOWN_INFO_INIT;
	uint64_t ref = 0;
	void *m;
	size_t n;
	int i;

	if (xtc_proc_spawn_monitor(xtc_exec_loop(r->e, pick_loop()), victim, r,
	        NULL, &r->victim, &ref) != XTC_OK)
		return;
	for (i = 0; i < 3; i++) {
		(void)xtc_proc_sleep(r->kill_ns / 3);
		(void)xtc_send(r->victim, "m", 1);
	}
	if (xtc_proc_info(r->victim, &info) == XTC_OK &&
	    info.park_reason == XTC_PARK_FD)
		r->kill_parked = 1;
	(void)xtc_exit_pid(r->victim, 7);
	if (xtc_recv(&m, &n, 1000 * MS) == XTC_OK) {
		if (xtc_down_decode_ex(m, n, &di) == XTC_OK && di.ref == ref) {
			r->kill_kind = (int)di.kind;
			r->kill_code = di.exit_code;
		}
		xtc_free(m);
	}
	r->done++;
}

static void
sleeper(void *arg)
{
	struct run *r = arg;
	uint32_t rev = 0;
	int64_t t0 = xtc_clock_mono();
	(void)xtc_send(xtc_self(), "pre", 3);
	r->wake_rc = __xtc_proc_wait_fd_only(r->pipe_w[0], XTC_IO_READABLE,
	    1000 * MS, &rev);
	r->wake_rev = (int)rev;
	r->wake_elapsed = xtc_clock_mono() - t0;
	r->done++;
}

static void
waker(void *arg)
{
	struct run *r = arg;
	(void)xtc_proc_sleep(r->wake_ns);
	(void)xtc_proc_wake(r->sleeper);
	r->done++;
}

/* The plan.  Drawn from the APP stream inside the run, so the scheduler
 * streams never see these draws. */
static void
plan(void *arg)
{
	struct run *r = arg;
	int k;

	r->t_ns = (5 + (int64_t)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 16)) * MS;
	for (k = 0; k < 2; k++) {
		r->burst[k] = 1 + (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 6);
		r->gap_ns[k] = r->t_ns / (2 * r->burst[k] + 1);
	}
	r->late_ns = r->t_ns +
	    (1 + (int64_t)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 8)) * MS;
	r->kill_ns =
	    (3 + (int64_t)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 10)) * MS;
	r->wake_ns =
	    (1 + (int64_t)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 20)) * MS;
	(void)xtc_proc_spawn(xtc_exec_loop(r->e, pick_loop()), waiter, r, NULL,
	    &r->waiter);
	(void)xtc_proc_spawn(xtc_exec_loop(r->e, pick_loop()), sender,
	    (void *)(intptr_t)0, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(r->e, pick_loop()), sender,
	    (void *)(intptr_t)1, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(r->e, pick_loop()), killer, r, NULL,
	    NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(r->e, pick_loop()), sleeper, r, NULL,
	    &r->sleeper);
	(void)xtc_proc_spawn(xtc_exec_loop(r->e, pick_loop()), waker, r, NULL,
	    NULL);
}

/* Fold every observation into one value for the replay comparison. */
static uint64_t
digest(const struct run *r, uint64_t state)
{
	int64_t v[16];
	uint64_t h = 0xcbf29ce484222325ull;
	size_t i;
	v[0] = r->blind_returns;
	v[1] = r->blind_rc;
	v[2] = r->blind_rev;
	v[3] = r->blind_depth;
	v[4] = r->blind_elapsed;
	v[5] = r->drained;
	v[6] = r->late_got;
	v[7] = r->kill_parked;
	v[8] = r->kill_kind;
	v[9] = r->kill_code;
	v[10] = r->wake_rc;
	v[11] = r->wake_elapsed;
	v[12] = r->t_ns;
	v[13] = r->kill_returned;
	v[14] = r->done;
	v[15] = (int64_t)state;
	for (i = 0; i < sizeof v / sizeof v[0]; i++) {
		h ^= (uint64_t)v[i];
		h *= 0x100000001B3ull;
	}
	return h;
}

/* Check one run's observations; print the first violation. */
static int
verdict(const struct run *r, uint64_t seed)
{
	const char *why = NULL;
	int sent = r->burst[0] + r->burst[1];

	if (r->late_got != 1)
		why = "a receive after a mail-blind wait was never woken";
	else if (r->blind_returns != 1)
		why = "the mail-blind wait returned more than once";
	else if (r->blind_rc != XTC_E_AGAIN ||
	    r->blind_rev != (int)XTC_WAIT_TIMEOUT)
		why = "the mail-blind wait did not end by its timeout";
	else if (r->blind_elapsed < r->t_ns)
		why = "the mail-blind wait ended before its timeout";
	else if (r->blind_depth != 1 + sent || r->drained != 1 + sent)
		why = "mail sent during the wait was not kept";
	else if (!r->kill_parked || r->kill_returned ||
	    r->kill_kind != XTC_DOWN_KIND_EXIT || r->kill_code != 7)
		why = "a kill did not end a mail-blind wait";
	else if (r->wake_rc != XTC_OK || r->wake_rev != 0 ||
	    r->wake_elapsed >= 1000 * MS)
		why = "xtc_proc_wake did not end a mail-blind wait";
	else if (r->done != 6)
		why = "a proc did not finish";
	if (why == NULL)
		return 0;
	printf("  seed 0x%016llx: FAIL %s (returns=%d rc=%d rev=0x%x "
	       "depth=%d/%d late=%d kill=%d/%d/%d wake=%d/%d done=%d)\n",
	    (unsigned long long)seed, why, r->blind_returns, r->blind_rc,
	    r->blind_rev, r->blind_depth, 1 + sent, r->late_got, r->kill_parked,
	    r->kill_kind, r->kill_code, r->wake_rc, r->wake_rev, r->done);
	return 1;
}

static int
run_one(uint64_t seed, uint64_t *out_digest)
{
	struct run *r = &g_r;
	int rc;

	memset(r, 0, sizeof *r);
	if (pipe(r->pipe_a) != 0 || pipe(r->pipe_k) != 0 ||
	    pipe(r->pipe_w) != 0 || xtc_exec_init(&r->e, N_LOOPS) != XTC_OK)
		return -1;
	(void)xtc_proc_spawn(xtc_exec_loop(r->e, 0), plan, r, NULL, NULL);
	rc = xtc_sim_exec_run(r->e, seed, 2000000);
	*out_digest = digest(r, xtc_sim_state_hash(r->e));
	(void)xtc_exec_fini(r->e);
	(void)close(r->pipe_a[0]);
	(void)close(r->pipe_a[1]);
	(void)close(r->pipe_k[0]);
	(void)close(r->pipe_k[1]);
	(void)close(r->pipe_w[0]);
	(void)close(r->pipe_w[1]);
	if (verdict(r, seed) != 0)
		return 1;
	if (rc != XTC_OK) {
		printf("  seed 0x%016llx: FAIL run rc=%d\n",
		    (unsigned long long)seed, rc);
		return 1;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0x66646f; /* "fdo" */
	int n = 32, i, fails = 0;

	if (argc > 1)
		base = strtoull(argv[1], NULL, 0);
	if (argc > 2)
		n = atoi(argv[2]);
	if (strcmp(xtc_io_backend_name(), "sim") != 0) {
		printf("SKIP: needs --with-io-backend=sim\n");
		return 0;
	}
	printf("== mail-blind wait_fd DST: %d seeds from base 0x%llx ==\n", n,
	    (unsigned long long)base);
	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		uint64_t d1 = 0, d2 = 0;
		if (run_one(seed, &d1) != 0) {
			fails++;
			continue;
		}
		if (run_one(seed, &d2) != 0 || d1 != d2) {
			printf("  seed 0x%016llx: FAIL replay diverged\n",
			    (unsigned long long)seed);
			fails++;
		}
	}
	if (fails == 0) {
		printf("OK: mail-blind wait_fd DST -- %d seeds: one return at "
		       "the timeout while mail flows, mail kept, a later recv "
		       "woken, kill and xtc_proc_wake still end it, all "
		       "replay\n",
		    n);
		return 0;
	}
	printf("FAIL: %d/%d mail-blind wait_fd seeds failed\n", fails, n);
	return 1;
}
