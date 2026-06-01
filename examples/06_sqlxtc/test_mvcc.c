/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_mvcc.c
 *	Stage 4: snapshot-isolation MVCC + cross-shard 2PC (mvcc.c).
 *	Self-contained, no daemon.  Three scenarios:
 *
 *	  1. Snapshot isolation -- a read at an old snapshot does not see
 *	     a write committed after it (slices 1-2: per-shard HLC +
 *	     versioning).
 *	  2. Cross-shard atomicity -- a two-key transaction spanning
 *	     shards commits all-or-nothing through the 2PC coordinator
 *	     (slices 3-4; the coordinator uses the gen_server deferred
 *	     reply).
 *	  3. Concurrent conflict -- N clients committing the same key at
 *	     the same snapshot: exactly one commits, the rest abort
 *	     (write-write conflict detection), on a single loop and on a
 *	     4-loop executor.
 */

#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mvcc.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"

#define N_SHARDS   4

static _Atomic int g_fail;
static _Atomic int g_commits;     /* successful commits (conflict test) */
static _Atomic int g_aborts;
static _Atomic int g_clients_left;
static uint64_t    g_conflict_snap;
static uint32_t    g_conflict_key = 4242;

#define CK(cond) do { if (!(cond)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	atomic_store(&g_fail, 1); } } while (0)

/* Scenario 1 + 2: a single driver runs sequential correctness checks. */
static void
correctness_driver(void *a)
{
	uint32_t v;
	uint64_t ct1, snap_old, snap_new, snap_x;
	mvcc_write_t w;
	mvcc_write_t pair[2];
	(void)a;

	/* (1) commit K=100, take a snapshot, commit K=200. */
	w.key = 7; w.value = 100;
	CK(mvcc_commit(mvcc_begin(), &w, 1, &ct1) == XTC_OK);

	snap_old = mvcc_begin();                 /* >= ct1 */
	CK(mvcc_read(7, snap_old, &v) == XTC_OK && v == 100);

	w.key = 7; w.value = 200;
	CK(mvcc_commit(mvcc_begin(), &w, 1, NULL) == XTC_OK);

	/* Snapshot isolation: the OLD snapshot still sees 100. */
	CK(mvcc_read(7, snap_old, &v) == XTC_OK && v == 100);
	snap_new = mvcc_begin();
	CK(mvcc_read(7, snap_new, &v) == XTC_OK && v == 200);

	/* (2) cross-shard atomic commit of two keys on (likely) two shards. */
	pair[0].key = 1001; pair[0].value = 11;
	pair[1].key = 2002; pair[1].value = 22;
	CK(mvcc_shard_of(1001) != mvcc_shard_of(2002));   /* spans shards */
	CK(mvcc_commit(mvcc_begin(), pair, 2, NULL) == XTC_OK);
	snap_x = mvcc_begin();
	CK(mvcc_read(1001, snap_x, &v) == XTC_OK && v == 11);
	CK(mvcc_read(2002, snap_x, &v) == XTC_OK && v == 22);

	/* A read of a never-written key misses cleanly. */
	CK(mvcc_read(999999, snap_x, &v) == XTC_E_NOTFOUND);

	mvcc_stop();
}

/* Scenario 3: each client commits the same key at the SAME snapshot. */
static void
conflict_client(void *arg)
{
	long id = (long)arg;
	mvcc_write_t w;
	int rc;

	w.key = g_conflict_key;
	w.value = (uint32_t)(id + 1);
	rc = mvcc_commit(g_conflict_snap, &w, 1, NULL);
	if (rc == XTC_OK)
		atomic_fetch_add(&g_commits, 1);
	else if (rc == XTC_E_AGAIN)
		atomic_fetch_add(&g_aborts, 1);
	else
		atomic_store(&g_fail, 1);

	if (atomic_fetch_sub(&g_clients_left, 1) == 1)
		mvcc_stop();
}

/* A tiny proc that takes the shared snapshot before any client commits,
 * then spawns the contending clients (so all share one pre-commit
 * snapshot -> every loser must conflict). */
static xtc_loop_t **g_conflict_loops;
static int          g_conflict_nloops;
static int          g_conflict_nclients;

static void
conflict_starter(void *a)
{
	long c;
	(void)a;
	g_conflict_snap = mvcc_begin();
	atomic_store(&g_clients_left, g_conflict_nclients);
	for (c = 0; c < g_conflict_nclients; c++) {
		xtc_loop_t *lp = g_conflict_loops[c % g_conflict_nloops];
		xtc_proc_opts_t po = { .name = "cli" };
		xtc_pid_t pid;
		if (xtc_proc_spawn(lp, conflict_client, (void *)c, &po, &pid)
		    != XTC_OK)
			atomic_store(&g_fail, 1);
	}
}

static int
run_correctness(void)
{
	xtc_loop_t *loop = NULL;
	xtc_loop_t *sl[N_SHARDS];
	xtc_proc_opts_t o = { .name = "drv" };
	xtc_pid_t pid;
	int i;

	atomic_store(&g_fail, 0);
	assert(xtc_loop_init(&loop) == XTC_OK);
	for (i = 0; i < N_SHARDS; i++) sl[i] = loop;
	if (mvcc_start(sl, N_SHARDS, loop) != XTC_OK) return 1;
	assert(xtc_proc_spawn(loop, correctness_driver, NULL, &o, &pid) == XTC_OK);
	assert(xtc_loop_run(loop) == XTC_OK);
	mvcc_fini();
	assert(xtc_loop_fini(loop) == XTC_OK);
	if (atomic_load(&g_fail)) return 1;
	printf("  ok   snapshot isolation + cross-shard atomic commit "
	    "(2PC via deferred-reply coordinator, per-shard HLC)\n");
	return 0;
}

static int
run_conflict(int n_loops, int n_clients, const char *tag)
{
	xtc_loop_t *sl[N_SHARDS];
	int i;

	atomic_store(&g_fail, 0);
	atomic_store(&g_commits, 0);
	atomic_store(&g_aborts, 0);
	g_conflict_nclients = n_clients;

	if (n_loops <= 1) {
		xtc_loop_t *loop = NULL;
		xtc_loop_t *cloops[1];
		xtc_proc_opts_t o = { .name = "start" };
		xtc_pid_t pid;
		assert(xtc_loop_init(&loop) == XTC_OK);
		for (i = 0; i < N_SHARDS; i++) sl[i] = loop;
		if (mvcc_start(sl, N_SHARDS, loop) != XTC_OK) return 1;
		cloops[0] = loop; g_conflict_loops = cloops; g_conflict_nloops = 1;
		assert(xtc_proc_spawn(loop, conflict_starter, NULL, &o, &pid) == XTC_OK);
		assert(xtc_loop_run(loop) == XTC_OK);
		mvcc_fini();
		assert(xtc_loop_fini(loop) == XTC_OK);
	} else {
		xtc_exec_t *exec = NULL;
		xtc_loop_t *cloops[N_SHARDS];
		xtc_proc_opts_t o = { .name = "start" };
		xtc_pid_t pid;
		assert(xtc_exec_init(&exec, n_loops) == XTC_OK);
		for (i = 0; i < N_SHARDS; i++) sl[i] = xtc_exec_loop(exec, i % n_loops);
		if (mvcc_start(sl, N_SHARDS, xtc_exec_loop(exec, 0)) != XTC_OK) return 1;
		for (i = 0; i < n_loops; i++) cloops[i] = xtc_exec_loop(exec, i);
		g_conflict_loops = cloops; g_conflict_nloops = n_loops;
		assert(xtc_proc_spawn(xtc_exec_loop(exec, 0), conflict_starter,
		    NULL, &o, &pid) == XTC_OK);
		assert(xtc_exec_run(exec) == XTC_OK);
		mvcc_fini();
		(void)xtc_exec_fini(exec);
	}

	if (atomic_load(&g_fail)) {
		fprintf(stderr, "FAIL[%s]: unexpected commit error\n", tag);
		return 1;
	}
	if (atomic_load(&g_commits) != 1) {
		fprintf(stderr, "FAIL[%s]: %d commits (want exactly 1), %d aborts\n",
		    tag, atomic_load(&g_commits), atomic_load(&g_aborts));
		return 1;
	}
	printf("  ok   [%s] %d clients raced one key at one snapshot: exactly 1 "
	    "committed, %d aborted (write-write conflict detected)\n",
	    tag, n_clients, atomic_load(&g_aborts));
	return 0;
}

/* Scenario 4 (slice 5): GC against the oldest live snapshot. */
static void
gc_driver(void *a)
{
	uint32_t v, K = 5000, K2 = 6000;
	uint64_t s, pin;
	mvcc_write_t w;
	int i;
	(void)a;

	/* Hammer one key 40 times, releasing each snapshot.  The low-water
	 * mark tracks the latest, so the version chain is GC'd down to a
	 * couple of versions rather than growing without bound. */
	for (i = 1; i <= 40; i++) {
		s = mvcc_begin();
		w.key = K; w.value = (uint32_t)i;
		CK(mvcc_commit(s, &w, 1, NULL) == XTC_OK);
		mvcc_snapshot_release(s);
	}
	CK(mvcc_total_versions() < 8);            /* GC reclaimed the old ones */
	s = mvcc_begin();
	CK(mvcc_read(K, s, &v) == XTC_OK && v == 40);
	mvcc_snapshot_release(s);

	/* Long-lived reader: a snapshot that has fallen behind the GC
	 * horizon gets XTC_E_ABORTED on read (its version was reclaimed),
	 * distinct from XTC_E_NOTFOUND for a key that never existed. */
	CK(mvcc_read(K, 1 /* ancient snapshot */, &v) == XTC_E_ABORTED);
	s = mvcc_begin();
	CK(mvcc_read(888888 /* never written */, s, &v) == XTC_E_NOTFOUND);
	mvcc_snapshot_release(s);

	/* A LIVE old snapshot pins the version it can see: even after many
	 * newer commits, reading at the pinned snapshot still sees it. */
	s = mvcc_begin();
	w.key = K2; w.value = 100;
	CK(mvcc_commit(s, &w, 1, NULL) == XTC_OK);
	mvcc_snapshot_release(s);
	pin = mvcc_begin();                       /* pins K2 == 100 */
	CK(mvcc_read(K2, pin, &v) == XTC_OK && v == 100);
	for (i = 0; i < 12; i++) {
		uint64_t s2 = mvcc_begin();
		w.key = K2; w.value = (uint32_t)(200 + i);
		CK(mvcc_commit(s2, &w, 1, NULL) == XTC_OK);
		mvcc_snapshot_release(s2);
	}
	CK(mvcc_read(K2, pin, &v) == XTC_OK && v == 100);   /* still retained */
	mvcc_snapshot_release(pin);

	mvcc_stop();
}

static int
run_gc(void)
{
	xtc_loop_t *loop = NULL;
	xtc_loop_t *sl[N_SHARDS];
	xtc_proc_opts_t o = { .name = "gc" };
	xtc_pid_t pid;
	int i;

	atomic_store(&g_fail, 0);
	assert(xtc_loop_init(&loop) == XTC_OK);
	for (i = 0; i < N_SHARDS; i++) sl[i] = loop;
	if (mvcc_start(sl, N_SHARDS, loop) != XTC_OK) return 1;
	assert(xtc_proc_spawn(loop, gc_driver, NULL, &o, &pid) == XTC_OK);
	assert(xtc_loop_run(loop) == XTC_OK);
	mvcc_fini();
	assert(xtc_loop_fini(loop) == XTC_OK);
	if (atomic_load(&g_fail)) return 1;
	printf("  ok   version GC against the oldest live snapshot: a hot key's "
	    "chain stays short; a live snapshot pins its version\n");
	return 0;
}

int
main(void)
{
	if (run_correctness() != 0) return 1;
	if (run_gc() != 0) return 1;
	if (run_conflict(1, 6, "single-loop") != 0) return 1;
	if (run_conflict(4, 8, "4-loop executor") != 0) return 1;
	printf("All sqlxtc MVCC + 2PC tests passed.\n");
	return 0;
}
