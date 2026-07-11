/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_proc_teardown.c
 *	DST coverage of the proc-teardown refcount (src/ptc/proc.c): the
 *	resolve-then-deliver window where a proc could be freed out from
 *	under an in-flight cross-thread send / DOWN / wake.
 *
 *	Scenario: across N loops, "target" procs are spawned, monitored by
 *	a watcher, sent a message, and told to exit -- so a DOWN delivery,
 *	a message send, and the target's own teardown all race under the
 *	seeded scheduler.  With the refcount, every resolved pointer is
 *	pinned until the caller is done, so no send/DOWN ever touches freed
 *	memory.  The determinism guard (xtc_sim_exec_run refusing XTC_OK on
 *	any nondeterministic primitive, plus the state hash) proves the run
 *	is byte-identical on replay; a use-after-free would corrupt the run
 *	or trip ASan when this test is also run under the sanitizer tier.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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

#define N_LOOPS   4
#define N_TARGETS 12

static atomic_int g_downs;        /* DOWNs the watcher received */
static atomic_int g_spawned;
static atomic_int g_bad;          /* any observed inconsistency */

/* Target: receive one message (or time out), then exit with a fixed
 * reason -- its exit races the watcher's monitor DOWN and the sender. */
static void
target_proc(void *arg)
{
	int want = (int)(intptr_t)arg;
	void *m = NULL; size_t n = 0;
	(void)xtc_recv(&m, &n, 0);        /* try-once; usually empty */
	if (m) xtc_free(m);
	xtc_exit_self(want & 0x7f);
}

/* Watcher: spawn_monitor each target, send it a message, and collect the
 * DOWNs.  Each spawn_monitor + send + the target's exit exercises the
 * resolve/deliver/teardown race for that target. */
static void
watcher_proc(void *arg)
{
	xtc_loop_t **loops = arg;
	int i;
	xtc_pid_t targets[N_TARGETS];

	for (i = 0; i < N_TARGETS; i++) {
		xtc_loop_t *tl = loops[(i + 1) % N_LOOPS];   /* cross-loop */
		uint64_t ref = 0;
		if (xtc_proc_spawn(tl, target_proc,
		    (void *)(intptr_t)(i + 1), NULL, &targets[i]) != XTC_OK) {
			atomic_fetch_add_explicit(&g_bad, 1, memory_order_relaxed);
			continue;
		}
		atomic_fetch_add_explicit(&g_spawned, 1, memory_order_relaxed);
		/* Monitor it (may race an immediate exit -> NOPROC DOWN, which
		 * is fine), then send it a message (races its teardown). */
		(void)xtc_monitor(targets[i], &ref);
		{ int v = i; (void)xtc_send(targets[i], &v, sizeof v); }
		/* Also poke it (another resolve/deliver path). */
		(void)xtc_proc_wake(targets[i]);
	}

	/* Drain the DOWNs (one per monitored target, NOPROC or real). */
	for (i = 0; i < N_TARGETS; i++) {
		void *m = NULL; size_t n = 0;
		if (xtc_recv(&m, &n, 2000LL * 1000 * 1000) == XTC_OK) {
			xtc_down_info_t di;
			if (xtc_down_decode_ex(m, n, &di) == XTC_OK)
				atomic_fetch_add_explicit(&g_downs, 1,
				    memory_order_relaxed);
		}
		if (m) xtc_free(m);
	}
}

static int
run_teardown(uint64_t seed, int *out_spawned, int *out_downs, int *out_bad,
             uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	xtc_loop_t *loops[N_LOOPS];
	int i, rc;

	atomic_store(&g_downs, 0);
	atomic_store(&g_spawned, 0);
	atomic_store(&g_bad, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	for (i = 0; i < N_LOOPS; i++) loops[i] = xtc_exec_loop(e, (unsigned)i);
	(void)xtc_proc_spawn(loops[0], watcher_proc, loops, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_spawned = atomic_load(&g_spawned);
	*out_downs = atomic_load(&g_downs);
	*out_bad = atomic_load(&g_bad);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(void)
{
	int sp1 = 0, d1 = 0, b1 = 0, sp2 = 0, d2 = 0, b2 = 0, sp3 = 0, d3 = 0, b3 = 0;
	uint64_t s1 = 0, s2 = 0, s3 = 0;
	int rc1, rc2, rc3;

	rc1 = run_teardown(0x7EA5D0, &sp1, &d1, &b1, &s1);
	if (rc1 != XTC_OK) { printf("FAIL: teardown run rc=%d (hang?)\n", rc1); return 1; }
	rc2 = run_teardown(0x7EA5D0, &sp2, &d2, &b2, &s2);
	rc3 = run_teardown(0x3D0FF5, &sp3, &d3, &b3, &s3);
	if (rc2 != XTC_OK || rc3 != XTC_OK) {
		printf("FAIL: teardown replay/diff rc=%d/%d\n", rc2, rc3); return 1;
	}

	printf("run1: spawned=%d downs=%d bad=%d state=%016llx\n",
	    sp1, d1, b1, (unsigned long long)s1);
	printf("run3 (diff seed): spawned=%d downs=%d bad=%d\n", sp3, d3, b3);

	if (sp1 != N_TARGETS) {
		printf("FAIL: spawned=%d want %d\n", sp1, N_TARGETS); return 1;
	}
	/* Every monitored target yields exactly one DOWN (real or NOPROC). */
	if (d1 != N_TARGETS) {
		printf("FAIL: downs=%d want %d (lost/dup DOWN)\n", d1, N_TARGETS);
		return 1;
	}
	if (b1 != 0 || b3 != 0) {
		printf("FAIL: observed inconsistency (bad=%d/%d)\n", b1, b3);
		return 1;
	}
	/* Byte-identical replay proves the race is scheduled deterministically
	 * and never corrupts state. */
	if (sp1 != sp2 || d1 != d2 || b1 != b2 || s1 != s2) {
		printf("FAIL: teardown run did not replay byte-identically\n");
		return 1;
	}

	printf("OK: monitored targets exit while DOWN/send/wake are in "
	    "flight; every target yields exactly one DOWN, no use-after-free "
	    "or lost delivery, and the run replays byte-identically under a "
	    "different seed\n");
	return 0;
}
