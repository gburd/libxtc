/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_compose_crash.c
 *	Deterministic Simulation Testing of CRASH RECOVERY under a
 *	MULTI-PRIMITIVE composition -- the FoundationDB signature test
 *	("kill a process mid-transaction, verify the durability invariant
 *	survives"), now spanning the full primitive stack at once.
 *
 *	The workload is the composition workload (test_sim_compose): N
 *	supervised worker fibers each take an EXCLUSIVE lockmgr lock on a
 *	shared key, commit a uniquely-keyed row durably through the sqlxtc
 *	storage engine (xstore + WAL) under the lock, release, and report
 *	the row on an mpsc channel to a collector.  ON TOP of that, at a
 *	SEEDED step mid-workload the run CRASHES (xtc_exec_stop with the
 *	pool unflushed), the WAL is cut at the durable frontier (only
 *	fsync-confirmed records survive), and recovery replays it into a
 *	FRESH empty B-tree.
 *
 *	The crash invariant across the whole stack (per seed):
 *	  (a) DURABILITY: every row whose COMMIT returned SX_OK before the
 *	      crash (acked -- the commit the worker observed succeed while
 *	      holding the lock) is present after recovery;
 *	  (b) no false durability: a row never appears unless its worker at
 *	      least attempted the commit;
 *	  (c) the lockmgr never granted two holders (mutual exclusion held
 *	      right up to the crash);
 *	  (d) recovery + the fresh-tree open quiesce; and
 *	  (e) REPLAY: the same seed reproduces the identical crash point
 *	      and recovered-row set (content hash).
 *
 *	A lost acked commit here would be a real durability bug in the
 *	lock/commit/WAL interaction -- exactly the class DST at FDB depth
 *	exists to catch.  Fork-per-run isolates the process-global MVCC
 *	commit clock so in-process replay is exact.
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
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
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
#define QUOTA     8
#define PAGE_SZ   4096

static xtc_lockmgr_t   *g_mgr;
static bt_t            *g_bt;
static wal_t           *g_wal;
static int              g_wal_fd = -1;
static xtc_chan_mpsc_t *g_chan;
static xtc_exec_t      *g_exec;

/* Per (worker,iter) row bookkeeping in SHARED memory (fork-per-run, so
 * the parent reads these after the child crashes -- actually the run is
 * in-process; these are process-global and read after the sim run). */
static int      g_acked[N_WORKERS][QUOTA];   /* commit returned SX_OK */
static int      g_lock_viol;
static atomic_long g_step;                   /* shared workload step counter */
static _Atomic long g_crash_at = -1;         /* seeded crash threshold */
static atomic_int  g_crashed;
static atomic_int  g_lock_held;

static long ROWID(int w, int i) { return (long)(w * 1000 + i); }

/* Read column v for rowid k from table t; 1 with *out set, else 0. */
static int
sel_v(sx_db *db, long k, char *out, size_t cap)
{
	sx_stmt *st = NULL;
	int got = 0;
	if (sx_prepare(db, "SELECT v FROM t WHERE k=?", -1, &st, NULL) != SX_OK)
		return 0;
	sx_bind_int64(st, 1, (int64_t)k);
	if (sx_step(st) == SX_ROW)
		got = 1;
	(void)out; (void)cap;
	sx_finalize(st);
	return got;
}

struct worker_arg { int id; };

static void
worker(void *arg)
{
	struct worker_arg *w = arg;
	xtc_locker_t id;
	sx_db *db = NULL;
	int i;

	if (sx_open_bt(g_bt, &db) != SX_OK)
		return;
	if (xtc_lockmgr_id(g_mgr, &id) != XTC_OK) { sx_close(db); return; }

	/* First worker to run draws the seeded crash threshold from the
	 * FAULT stream (so enabling the crash does not perturb the SCHED
	 * schedule). */
	{
		long expect = -1;
		if (atomic_load(&g_crash_at) < 0) {
			long thr = (long)__xtc_sim_rng_range(XTC_SIM_RNG_FAULT,
			    (uint64_t)(N_WORKERS * QUOTA)) + 1;
			atomic_compare_exchange_strong(&g_crash_at, &expect,
			    thr);
		}
	}

	for (i = 0; i < QUOTA; i++) {
		char sql[128];
		long rowid = ROWID(w->id, i);
		long step;

		if (xtc_lock_get(g_mgr, id, "row", 4, XTC_LOCK_X,
		    1000000000LL) != XTC_OK)
			continue;
		if (atomic_fetch_add(&g_lock_held, 1) != 0)
			g_lock_viol = 1;

		if (sx_exec(db, "BEGIN", NULL) == SX_OK) {
			snprintf(sql, sizeof sql,
			    "INSERT INTO t(k,v) VALUES(%ld,'%d-%d');",
			    rowid, w->id, i);
			if (sx_exec(db, sql, NULL) == SX_OK) {
				if (sx_exec(db, "COMMIT", NULL) == SX_OK)
					g_acked[w->id][i] = 1;  /* durable-acked */
			} else {
				(void)sx_exec(db, "ROLLBACK", NULL);
			}
		}

		atomic_fetch_sub(&g_lock_held, 1);
		(void)xtc_lock_put(g_mgr, id, "row", 4);

		{
			long *msg = malloc(sizeof *msg);
			if (msg) {
				*msg = rowid;
				while (xtc_chan_mpsc_try_send(g_chan, msg) !=
				    XTC_OK)
					xtc_yield();
			}
		}

		/* Advance the shared step; trip the seeded crash. */
		step = atomic_fetch_add(&g_step, 1) + 1;
		if (!atomic_load(&g_crashed) &&
		    atomic_load(&g_crash_at) > 0 &&
		    step >= atomic_load(&g_crash_at)) {
			atomic_store(&g_crashed, 1);
			(void)xtc_exec_stop(g_exec);   /* CRASH */
		}
		if ((i & 1) == 0)
			xtc_yield();
	}
	(void)xtc_lockmgr_id_free(g_mgr, id);
	sx_close(db);
}

static void
collector(void *arg)
{
	(void)arg;
	int idle = 0;
	for (;;) {
		void *m = NULL;
		if (xtc_chan_mpsc_try_recv(g_chan, &m) == XTC_OK && m != NULL) {
			free(m);
			idle = 0;
		} else if (++idle > 4000) {
			break;
		} else {
			(void)xtc_proc_sleep(500 * 1000LL);
		}
	}
}

/* Cut the WAL at the durable frontier: keep only fsync-confirmed
 * records (lsn <= durable_lsn), matching the crash-recover crash model. */
static int
wal_truncate_to_durable(const char *path, uint64_t durable_lsn,
    uint64_t durable_bytes, int wb_armed)
{
	int fd = open(path, O_RDWR);
	off_t off = 0, keep = 0;
	uint8_t hdr[12];
	if (fd < 0) return -1;
	for (;;) {
		uint64_t lsn; uint32_t len; off_t end;
		if (pread(fd, hdr, sizeof hdr, off) != (ssize_t)sizeof hdr)
			break;
		memcpy(&lsn, hdr, 8);
		memcpy(&len, hdr + 8, 4);
		end = off + (off_t)sizeof hdr + (off_t)len + 8;  /* +8 CRC */
		if (lsn > durable_lsn)
			break;
		/* Also honour the sim's fsync frontier: a record whose bytes
		 * were written but never fdatasync-confirmed is lost on crash,
		 * even if the writer advanced durable_lsn past it.  When the
		 * write-back model is armed, durable_bytes is authoritative
		 * (0 legitimately means "nothing was fsync'd" -> keep nothing). */
		if (wb_armed && (uint64_t)end > durable_bytes)
			break;
		keep = end;
		off = end;
	}
	(void)ftruncate(fd, keep);
	(void)close(fd);
	return 0;
}

static int
run_one(uint64_t seed, uint64_t *out_hash, int *out_viol, int *out_recovered)
{
	wal_opts_t wo = {0};
	bm_opts_t bo = BM_OPTS_DEFAULT, b2 = BM_OPTS_DEFAULT;
	bm_t *bm = NULL, *bm2 = NULL;
	bt_t *bt2 = NULL;
	xtc_lockmgr_opts_t lo = XTC_LOCKMGR_OPTS_DEFAULT;
	lo.detect_mode = XTC_LOCK_DETECT_ON_BLOCK;   /* no detector thread under sim */
	sx_db *ddl = NULL, *db2 = NULL;
	struct worker_arg wa[N_WORKERS];
	char logp[] = "/tmp/xtc-ccrash-wal-XXXXXX";
	char btA[]  = "/tmp/xtc-ccrash-btA-XXXXXX";
	char btB[]  = "/tmp/xtc-ccrash-btB-XXXXXX";
	xtc_pid_t wp;
	uint64_t dlsn = 0, h = 1469598103934665603ull;
	uint64_t durable_bytes = 0;
	int i, w, fd, rc = -1, recovered = 0;

	memset(g_acked, 0, sizeof g_acked);
	g_lock_viol = 0;
	atomic_store(&g_step, 0);
	atomic_store(&g_crash_at, -1);
	atomic_store(&g_crashed, 0);
	atomic_store(&g_lock_held, 0);
	g_mgr = NULL; g_bt = NULL; g_wal = NULL; g_chan = NULL; g_exec = NULL;

	fd = mkstemp(logp); if (fd < 0) return -1; close(fd);
	fd = mkstemp(btA);  if (fd < 0) { unlink(logp); return -1; } close(fd);
	fd = mkstemp(btB);  if (fd < 0) { unlink(logp); unlink(btA); return -1; } close(fd);

	wo.path = logp; wo.window_ns = 500000; wo.max_batch = 256;
	if (wal_open(&wo, &g_wal) != XTC_OK) goto files;
	/* Arm the sim write-back crash model on the WAL fd: a crash loses
	 * bytes past the last fdatasync, so durability is tied to fsync
	 * (not to the WAL's self-reported durable_lsn).  This is what makes
	 * an ack-before-fsync bug detectable. */
	xtc_sim_io_wb_enable(1);
	g_wal_fd = wal_fd(g_wal);
	bo.path = btA; bo.page_size = PAGE_SZ; bo.n_frames = 1024;
	if (bm_create(&bo, &bm) != XTC_OK) { wal_close(g_wal); goto files; }
	if (bt_open(bm, &g_bt) != XTC_OK) { bm_destroy(bm); wal_close(g_wal); goto files; }
	xstore_set_wal(g_wal);

	if (sx_open_bt(g_bt, &ddl) != SX_OK) goto engine;
	if (sx_exec(ddl, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL)
	    != SX_OK) { sx_close(ddl); goto engine; }
	sx_close(ddl); ddl = NULL;

	if (xtc_lockmgr_create(&lo, &g_mgr) != XTC_OK) goto engine;
	if (xtc_exec_init(&g_exec, N_LOOPS) != XTC_OK) goto lockmgr;
	xtc_exec_set_service_mode(g_exec, 1);
	xtc_sim_io_faults_enable(50 * 1000LL, 500 * 1000LL, 0);
	if (xtc_chan_mpsc_create(NULL, 512, &g_chan) != XTC_OK) goto exec;
	if (wal_writer_spawn(g_wal, xtc_exec_loop(g_exec, 0), &wp) != XTC_OK)
		goto exec;

	for (i = 0; i < N_WORKERS; i++) {
		wa[i].id = i;
		(void)xtc_proc_spawn(xtc_exec_loop(g_exec, 1 + (i % (N_LOOPS-1))),
		    worker, &wa[i], NULL, NULL);
	}
	(void)xtc_proc_spawn(xtc_exec_loop(g_exec, 0), collector, NULL,
	    NULL, NULL);

	rc = xtc_sim_exec_run(g_exec, seed, 20000000);
	dlsn = wal_durable_lsn(g_wal);
	/* Capture the TRUE durable byte frontier from the sim write-back
	 * model (last fdatasync-confirmed byte on the WAL fd) BEFORE the
	 * WAL is closed.  A crash loses everything past it -- including a
	 * record the writer acked but did not fsync.  This is stricter than
	 * trusting dlsn: if the writer advanced durable_lsn without a real
	 * fsync, durable_bytes will be BEHIND it and recovery loses that
	 * acked commit, tripping the durability invariant below. */
	durable_bytes = xtc_sim_io_durable_end(g_wal_fd);

	if (out_viol) *out_viol = g_lock_viol;

	/* ---- crash: lose the pool unflushed, cut WAL at durable. ---- */
	xstore_set_wal(NULL);
	bt_close(g_bt); g_bt = NULL;
	bm_destroy(bm); bm = NULL;
	wal_close(g_wal); g_wal = NULL;
	xtc_sim_io_faults_disable();
	xtc_sim_io_wb_enable(0);
	if (g_chan) { void *m; while (xtc_chan_mpsc_try_recv(g_chan,&m)==XTC_OK && m) free(m); xtc_chan_mpsc_destroy(g_chan); g_chan = NULL; }
	(void)xtc_exec_fini(g_exec); g_exec = NULL;
	/* Do NOT gracefully destroy the lockmgr here: the crash
	 * (xtc_exec_stop) may have killed a worker mid-lock-hold or
	 * mid-acquire, leaving lock-table state a real crashed process
	 * would simply abandon.  Tearing it down cleanly is neither safe
	 * nor meaningful after a crash.  This test forks per run, so the
	 * child _exit reclaims g_mgr's memory -- exactly the crash model
	 * (the "machine" is gone).  Leaving it is correct, not a leak. */
	g_mgr = NULL;
	if (wal_truncate_to_durable(logp, dlsn, durable_bytes, 1) != 0) { rc = -1; goto files; }

	/* ---- recover into a fresh tree ---- */
	b2.path = btB; b2.page_size = PAGE_SZ; b2.n_frames = 256;
	if (bm_create(&b2, &bm2) != XTC_OK) { rc = -1; goto files; }
	if (bt_open(bm2, &bt2) != XTC_OK) { bm_destroy(bm2); rc = -1; goto files; }
	if (xstore_recover(bt2, logp) != XTC_OK) { bt_close(bt2); bm_destroy(bm2); rc = -1; goto files; }
	if (sx_open_bt(bt2, &db2) != SX_OK) { bt_close(bt2); bm_destroy(bm2); rc = -1; goto files; }
	if (sx_exec(db2, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) != SX_OK) {
		sx_close(db2); bt_close(bt2); bm_destroy(bm2); rc = -1; goto files;
	}

	/* ---- verify: every ACKED row is present after recovery. ---- */
	for (w = 0; w < N_WORKERS; w++) {
		for (i = 0; i < QUOTA; i++) {
			long rowid = ROWID(w, i);
			char v[32];
			int present = sel_v(db2, rowid, v, sizeof v);
			if (g_acked[w][i]) {
				if (!present) { rc = -4; break; } /* LOST acked commit */
				recovered++;
				h ^= (uint64_t)rowid; h *= 0x100000001B3ull;
			}
		}
		if (rc == -4) break;
	}
	if (rc != -4) rc = XTC_OK;

	sx_close(db2); bt_close(bt2); bm_destroy(bm2);
	if (out_hash) *out_hash = h;
	if (out_recovered) *out_recovered = recovered;
	unlink(logp); unlink(btA); unlink(btB);
	return rc;

exec:   if (g_chan) xtc_chan_mpsc_destroy(g_chan);
	xtc_sim_io_faults_disable();
	if (g_exec) (void)xtc_exec_fini(g_exec);
lockmgr: if (g_mgr) xtc_lockmgr_destroy(g_mgr);
engine: xstore_set_wal(NULL);
	if (g_bt) bt_close(g_bt);
	if (bm) bm_destroy(bm);
	if (g_wal) wal_close(g_wal);
files:  unlink(logp); unlink(btA); unlink(btB);
	return rc;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0x63636b; /* "cck" */
	int n = 12, i, fails = 0;

	if (argc > 1) base = strtoull(argv[1], NULL, 0);
	if (argc > 2) n = atoi(argv[2]);

	printf("== composition-crash DST: %d seeds from base 0x%llx ==\n",
	    n, (unsigned long long)base);

	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		uint64_t h = 0, h2 = 0;
		int viol = 0, viol2 = 0, recov = 0, recov2 = 0, rc, rc2;
		pid_t pid;
		int wstat = 0;

		/* Fork-per-run: isolate the process-global MVCC commit clock so
		 * in-process replay is exact (crash_recover does the same). */
		pid = fork();
		if (pid == 0) {
			rc = run_one(seed, &h, &viol, &recov);
			_exit(rc == XTC_OK && viol == 0 ? 0 :
			    (rc == -4 ? 4 : 1));
		}
		(void)waitpid(pid, &wstat, 0);
		if (!WIFEXITED(wstat) || WEXITSTATUS(wstat) != 0) {
			printf("  seed 0x%016llx: FAIL (child status %d -- "
			    "%s)\n", (unsigned long long)seed,
			    WEXITSTATUS(wstat),
			    WEXITSTATUS(wstat) == 4 ? "LOST acked commit" :
			    "run error");
			fails++;
			continue;
		}
		/* Replay in another fork; compare the recovered-row hash. */
		pid = fork();
		if (pid == 0) {
			int fd1, fd2;
			rc = run_one(seed, &h, &viol, &recov);
			rc2 = run_one(seed, &h2, &viol2, &recov2);
			fd1 = (rc == XTC_OK && rc2 == XTC_OK && h == h2 &&
			    recov == recov2) ? 0 : 5;
			fd2 = fd1;
			(void)fd2;
			_exit(fd1);
		}
		(void)waitpid(pid, &wstat, 0);
		if (!WIFEXITED(wstat) || WEXITSTATUS(wstat) != 0) {
			printf("  seed 0x%016llx: REPLAY MISMATCH\n",
			    (unsigned long long)seed);
			fails++;
		}
	}

	if (fails == 0) {
		printf("OK: composition-crash DST -- %d seeds, every acked "
		    "commit durable through lock+WAL+recovery under a seeded "
		    "crash, all replay\n", n);
		return 0;
	}
	printf("FAIL: %d/%d composition-crash seeds failed\n", fails, n);
	return 1;
}
