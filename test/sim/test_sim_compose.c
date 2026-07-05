/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_compose.c
 *	Deterministic Simulation Testing of MULTIPLE concurrency primitives
 *	composed in ONE seeded run -- where FoundationDB-class bugs hide.
 *	Every other sim test drives one primitive in isolation; this one
 *	runs the lock manager, the sqlxtc storage engine (xstore + WAL),
 *	an mpsc channel, and a supervisor together under the seeded
 *	scheduler, and checks a GLOBAL invariant that only holds if they
 *	interoperate correctly.
 *
 *	Scenario (all under xtc_sim_exec_run, across N loops):
 *	  - N worker fibers, each supervised, repeatedly:
 *	      1. acquire an EXCLUSIVE lockmgr lock on a shared logical key
 *	         (serialises the critical section across loops);
 *	      2. under the lock, INSERT a uniquely-keyed row into the shared
 *	         table and COMMIT it (durable via the WAL) -- so the lock
 *	         orders the committed writes;
 *	      3. release the lock and report the row it wrote on an mpsc
 *	         channel to a collector.
 *	  - a COLLECTOR fiber drains the channel, counting reported rows.
 *	  - a COORDINATOR stops the run once every worker has done its
 *	    fixed quota.
 *
 *	Global invariant (per seed): the number of rows the workers
 *	COMMITTED equals the number the collector received on the channel
 *	equals N_WORKERS * QUOTA -- no lost commit, no lost channel
 *	message, no double-count under the lock -- and the run quiesces
 *	cleanly.  A repeated seed reproduces the exact committed-row set and
 *	channel order (result fingerprint), the FoundationDB replay
 *	property, now across the full primitive stack at once.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_res.h"
#include "xtc_chan.h"
#include "xtc_lockmgr.h"
#include "xtc_sim.h"

#include "bufmgr.h"
#include "btree.h"
#include "wal.h"
#include "engine.h"
#include "xstore.h"

#define N_LOOPS   3
#define N_WORKERS 4
#define QUOTA     6            /* rows each worker commits */
#define PAGE_SZ   4096

static xtc_lockmgr_t   *g_mgr;
static bt_t            *g_bt;
static wal_t           *g_wal;
static xtc_chan_mpsc_t *g_chan;
static atomic_int       g_committed;   /* rows a worker COMMITted */
static atomic_int       g_received;    /* rows the collector drained */
static atomic_int       g_done;        /* workers that hit their quota */
static atomic_int       g_lock_held;   /* must never exceed 1 (mutual excl) */
static atomic_int       g_lock_viol;   /* set if two holders overlap */

/* A shared logical key that all workers contend on. */
static const char g_key[] = "shared-row";

struct worker_arg { int id; };

/* Worker: for QUOTA iterations, take the lock, commit a row, report it. */
static void
worker(void *arg)
{
	struct worker_arg *w = arg;
	xtc_locker_t id;
	sx_db *db = NULL;
	int i;

	if (sx_open_bt(g_bt, &db) != SX_OK)
		goto done;

	if (xtc_lockmgr_id(g_mgr, &id) != XTC_OK)
		goto done;

	for (i = 0; i < QUOTA; i++) {
		char sql[128];
		long long rowid;
		int rc;

		/* 1. EXCLUSIVE lock the shared key (serialise the section). */
		rc = xtc_lock_get(g_mgr, id, g_key, sizeof g_key,
		    XTC_LOCK_X, 1000000000LL);
		if (rc != XTC_OK)
			continue;

		/* Mutual-exclusion witness: while held, the count must be 1. */
		if (atomic_fetch_add(&g_lock_held, 1) != 0)
			atomic_store(&g_lock_viol, 1);

		/* 2. Commit a uniquely-keyed row under the lock. */
		rowid = (long long)(w->id * 1000 + i);
		if (sx_exec(db, "BEGIN", NULL) == SX_OK) {
			snprintf(sql, sizeof sql,
			    "INSERT INTO t(k,v) VALUES(%lld,'%d-%d');",
			    rowid, w->id, i);
			if (sx_exec(db, sql, NULL) == SX_OK &&
			    sx_exec(db, "COMMIT", NULL) == SX_OK)
				atomic_fetch_add(&g_committed, 1);
			else
				(void)sx_exec(db, "ROLLBACK", NULL);
		}

		atomic_fetch_sub(&g_lock_held, 1);

		/* 3. Release the lock, THEN report on the channel. */
		(void)xtc_lock_put(g_mgr, id, g_key, sizeof g_key);

		{
			long long *msg = malloc(sizeof *msg);
			if (msg != NULL) {
				*msg = rowid;
				/* Retry the bounded channel until accepted so no
				 * report is lost (deterministic under sim). */
				while (xtc_chan_mpsc_try_send(g_chan, msg) !=
				    XTC_OK)
					xtc_yield();
			}
		}

		if ((i & 1) == 0)
			xtc_yield();
	}

	(void)xtc_lockmgr_id_free(g_mgr, id);
done:
	if (db != NULL)
		sx_close(db);
	atomic_fetch_add(&g_done, 1);
}

/* Collector: drain the mpsc channel until every reported row is in.
 * Backs off with a sim-clock sleep (advances virtual time) rather than
 * a bare yield, so it does not burn its idle budget before the workers
 * have produced anything. */
static void
collector(void *arg)
{
	(void)arg;
	int idle = 0;
	while (atomic_load(&g_received) < N_WORKERS * QUOTA && idle < 4000) {
		void *m = NULL;
		if (xtc_chan_mpsc_try_recv(g_chan, &m) == XTC_OK && m != NULL) {
			free(m);
			atomic_fetch_add(&g_received, 1);
			idle = 0;
		} else {
			idle++;
			(void)xtc_proc_sleep(500 * 1000LL);
		}
	}
}

/* Coordinator: stop the run once all workers finished and all reports
 * were collected (or a bounded ceiling to guarantee quiescence). */
static void
coordinator(void *arg)
{
	xtc_exec_t *e = arg;
	int tries;
	for (tries = 0; tries < 4000; tries++) {
		if (atomic_load(&g_done) >= N_WORKERS &&
		    atomic_load(&g_received) >= N_WORKERS * QUOTA)
			break;
		(void)xtc_proc_sleep(1000 * 1000LL);
	}
	(void)xtc_proc_sleep(2 * 1000 * 1000LL);
	(void)xtc_exec_stop(e);
}

static int
run_one(uint64_t seed, int *out_committed, int *out_received,
    int *out_viol, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	wal_opts_t wo = {0};
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	xtc_lockmgr_opts_t lo = XTC_LOCKMGR_OPTS_DEFAULT;
	lo.detect_mode = XTC_LOCK_DETECT_ON_BLOCK;   /* no detector thread under sim */
	sx_db *ddl = NULL;
	struct worker_arg wa[N_WORKERS];
	char logp[] = "/tmp/xtc-compose-wal-XXXXXX";
	char btf[]  = "/tmp/xtc-compose-bt-XXXXXX";
	xtc_pid_t wp;
	int i, rc = -1, fd;

	atomic_store(&g_committed, 0);
	atomic_store(&g_received, 0);
	atomic_store(&g_done, 0);
	atomic_store(&g_lock_held, 0);
	atomic_store(&g_lock_viol, 0);
	g_mgr = NULL; g_bt = NULL; g_wal = NULL; g_chan = NULL;

	fd = mkstemp(logp); if (fd < 0) return -1; close(fd);
	fd = mkstemp(btf);  if (fd < 0) { unlink(logp); return -1; } close(fd);

	wo.path = logp; wo.window_ns = 500000; wo.max_batch = 256;
	if (wal_open(&wo, &g_wal) != XTC_OK) goto files;

	bo.path = btf; bo.page_size = PAGE_SZ; bo.n_frames = 1024;
	if (bm_create(&bo, &bm) != XTC_OK) { wal_close(g_wal); goto files; }
	if (bt_open(bm, &g_bt) != XTC_OK) { bm_destroy(bm); wal_close(g_wal); goto files; }
	xstore_set_wal(g_wal);

	/* Create the table durably before the workload (off-loop DDL). */
	if (sx_open_bt(g_bt, &ddl) != SX_OK) goto engine;
	if (sx_exec(ddl, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL)
	    != SX_OK) { sx_close(ddl); goto engine; }
	sx_close(ddl); ddl = NULL;

	if (xtc_lockmgr_create(&lo, &g_mgr) != XTC_OK) goto engine;

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) goto lockmgr;
	xtc_exec_set_service_mode(e, 1);

	/* Seeded WAL I/O latency reorders group-commit fsyncs across runs
	 * (no injected errors -- a clean-commit workload). */
	xtc_sim_io_faults_enable(50 * 1000LL, 500 * 1000LL, 0);

	if (xtc_chan_mpsc_create(NULL, 256, &g_chan) != XTC_OK) goto exec;

	/* WAL group-commit writer on loop 0. */
	if (wal_writer_spawn(g_wal, xtc_exec_loop(e, 0), &wp) != XTC_OK)
		goto exec;

	for (i = 0; i < N_WORKERS; i++) {
		wa[i].id = i;
		(void)xtc_proc_spawn(xtc_exec_loop(e, 1 + (i % (N_LOOPS - 1))),
		    worker, &wa[i], NULL, NULL);
	}
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), collector, NULL, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), coordinator, e, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 20000000);

	if (out_committed) *out_committed = atomic_load(&g_committed);
	if (out_received)  *out_received = atomic_load(&g_received);
	if (out_viol)      *out_viol = atomic_load(&g_lock_viol);
	if (out_state)     *out_state = xtc_sim_state_hash(e);

	xtc_sim_io_faults_disable();
	if (g_chan) xtc_chan_mpsc_destroy(g_chan);
exec:
	if (e) { (void)xtc_exec_fini(e); }
lockmgr:
	if (g_mgr) xtc_lockmgr_destroy(g_mgr);
engine:
	xstore_set_wal(NULL);
	if (g_bt) bt_close(g_bt);
	if (bm) bm_destroy(bm);
	if (g_wal) wal_close(g_wal);
files:
	unlink(logp);
	unlink(btf);
	return rc;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0x636d70; /* "cmp" */
	int n = 12, i, fails = 0;

	if (argc > 1) base = strtoull(argv[1], NULL, 0);
	if (argc > 2) n = atoi(argv[2]);

	printf("== composition DST (lockmgr+xstore+chan+wal): %d seeds "
	    "from base 0x%llx ==\n", n, (unsigned long long)base);

	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		int c = 0, r = 0, v = 0, c2 = 0, r2 = 0, v2 = 0, rc, rc2;
		uint64_t st = 0, st2 = 0;
		int pass = 1;

		rc = run_one(seed, &c, &r, &v, &st);
		if (rc != XTC_OK) pass = 0;
		else if (v != 0) pass = 0;                       /* mutual excl */
		else if (c != N_WORKERS * QUOTA) pass = 0;       /* all committed */
		else if (r != N_WORKERS * QUOTA) pass = 0;       /* all collected */

		if (pass) {
			rc2 = run_one(seed, &c2, &r2, &v2, &st2);
			if (rc2 != rc || c2 != c || r2 != r || v2 != v ||
			    st2 != st)
				pass = 0;
		}

		if (!pass) {
			printf("  seed 0x%016llx: FAIL (committed=%d recv=%d "
			    "viol=%d want=%d rc=%d)\n",
			    (unsigned long long)seed, c, r, v,
			    N_WORKERS * QUOTA, rc);
			fails++;
		}
	}

	if (fails == 0) {
		printf("OK: composition DST -- %d seeds, lockmgr mutual "
		    "exclusion + all commits durable + all channel reports "
		    "collected, all replay\n", n);
		return 0;
	}
	printf("FAIL: %d/%d composition seeds failed\n", fails, n);
	return 1;
}
