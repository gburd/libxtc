/*
 * test_sim_wake_park -- DST guard for the cross-loop wake-of-a-parked-
 * loop class (the io_uring / event-port idle-loop lost-wakeup the
 * carrier team reported, 2026-07-06).
 *
 * The real-backend bug: a fiber spawned (or a mailbox message sent) onto
 * a loop that is parked idle in the io_uring / event-port wait was
 * sometimes never scheduled, because the wakeup-fd nudge raced the
 * loop's park/re-arm.  The fix re-arms the wakeup poll BEFORE draining
 * it on those two one-shot backends; the level-triggered backends were
 * already safe.
 *
 * DST cannot reproduce the fd-level race (the sim backend uses no real
 * wakeup fd), but it is the STRUCTURAL guard for the class: the sim
 * decides a loop is runnable by directly inspecting its inbox
 * (__sim_loop_runnable), so a cross-loop enqueue onto a parked loop is
 * ALWAYS scheduled or the run fails quiescence.  This test makes that
 * explicit: every peer parks in xtc_recv(-1) (its loop goes idle), then
 * a leader -- from a DIFFERENT loop -- both SENDS to each parked peer
 * and SPAWNS a fresh proc onto each idle loop.  Every send must be
 * received and every spawned proc must run, deterministically and with
 * byte-identical replay.  A regression that lets a parked loop be
 * falsely quiesced would drop a message or leave a proc unrun -> fail.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

#define N_LOOPS   4
#define N_PEERS   (N_LOOPS - 1)   /* one parked peer per non-leader loop */

static atomic_int g_recv;      /* parked peers that received the wake message */
static atomic_int g_spawned;   /* fresh procs (spawned onto idle loops) that ran */
static atomic_int g_parked;    /* peers that reached their recv park */
static xtc_pid_t  g_peer[N_PEERS];

/* A peer: park in an INFINITE recv (its loop goes idle with n_alive > 0,
 * the permanent-park path), then, when woken by the leader's cross-loop
 * send, record it and exit. */
static void
peer(void *arg)
{
	long id = (long)(intptr_t)arg;
	void *m = NULL;
	size_t n = 0;
	atomic_fetch_add(&g_parked, 1);
	(void)id;
	if (xtc_recv(&m, &n, -1) == XTC_OK) {   /* infinite park */
		atomic_fetch_add(&g_recv, 1);
		if (m != NULL) xtc_free(m);
	}
}

/* A freshly-spawned proc placed onto an already-idle loop: just record
 * that its body ran (proving the spawn-enqueue woke the parked loop). */
static void
fresh(void *arg)
{
	(void)arg;
	atomic_fetch_add(&g_spawned, 1);
	xtc_exit_self(0);
}

/* The leader (on loop 0): wait until every peer has parked (so their
 * loops are genuinely idle), then cross-loop SEND to each parked peer
 * AND SPAWN a fresh proc onto each idle loop.  Both the wake and the
 * spawn must reach the parked loop. */
static void
leader(void *arg)
{
	xtc_exec_t *e = arg;
	int tries, i;
	/* Wait for all peers to reach their park. */
	for (tries = 0; tries < 100000 &&
	    atomic_load(&g_parked) < N_PEERS; tries++)
		xtc_proc_sleep(1000 * 1000LL);   /* 1ms virtual */

	for (i = 0; i < N_PEERS; i++) {
		int msg = i;
		/* Cross-loop mailbox send to the parked peer (WAKE path). */
		(void)xtc_send(g_peer[i], &msg, sizeof msg);
		/* Cross-loop spawn onto the (idle) peer loop (PUBLISH path). */
		(void)xtc_proc_spawn(xtc_exec_loop(e, 1 + i), fresh, NULL,
		    NULL, NULL);
	}
}

static int
run_one(uint64_t seed, int *out_recv, int *out_spawned, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	atomic_store(&g_recv, 0);
	atomic_store(&g_spawned, 0);
	atomic_store(&g_parked, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK)
		return -1;

	/* One parked peer on each non-leader loop. */
	for (i = 0; i < N_PEERS; i++) {
		if (xtc_proc_spawn(xtc_exec_loop(e, 1 + i), peer,
		    (void *)(intptr_t)i, NULL, &g_peer[i]) != XTC_OK) {
			(void)xtc_exec_fini(e);
			return -1;
		}
	}
	/* Leader on loop 0. */
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), leader, e, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 20000000);

	if (out_recv)    *out_recv = atomic_load(&g_recv);
	if (out_spawned) *out_spawned = atomic_load(&g_spawned);
	if (out_state)   *out_state = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(void)
{
	uint64_t base = 0x77414b;   /* "WAK" */
	int n = 40, i, fails = 0;

	printf("== cross-loop wake-of-parked-loop DST (lost-wakeup class): "
	    "%d seeds ==\n", n);

	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		int r1 = 0, s1 = 0, r2 = 0, s2 = 0, rc1, rc2, pass = 1;
		uint64_t st1 = 0, st2 = 0;

		rc1 = run_one(seed, &r1, &s1, &st1);
		if (rc1 != XTC_OK) pass = 0;
		else if (r1 != N_PEERS) pass = 0;    /* every parked peer woke */
		else if (s1 != N_PEERS) pass = 0;    /* every fresh proc ran */
		if (pass) {
			rc2 = run_one(seed, &r2, &s2, &st2);
			if (rc2 != rc1 || r2 != r1 || s2 != s1 || st2 != st1)
				pass = 0;
		}
		if (!pass) {
			printf("  seed 0x%016llx: FAIL (recv=%d spawned=%d "
			    "want=%d rc=%d) -- a cross-loop enqueue onto a "
			    "parked loop was lost\n",
			    (unsigned long long)seed, r1, s1, N_PEERS, rc1);
			fails++;
		}
	}

	if (fails == 0) {
		printf("OK: cross-loop wake-of-parked-loop DST -- %d seeds, "
		    "every mailbox send AND spawn onto an idle parked loop was "
		    "scheduled (no lost wakeup), all replay\n", n);
		return 0;
	}
	printf("FAIL: %d/%d wake-park seeds failed\n", fails, n);
	return 1;
}
