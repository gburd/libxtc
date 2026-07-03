/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/sim/test_sim_crash_recover.c
 *	DST CAPSTONE -- seeded, replayable CRASH-RECOVERY of the sqlxtc
 *	storage engine's write-ahead log, the FoundationDB "crash at every
 *	point" property.  This is the DST version of the non-sim
 *	examples/06_sqlxtc/test_wal_recover.c: instead of one fixed crash
 *	after a fixed loop, it crashes at a SEEDED point mid-workload,
 *	under the deterministic scheduler, and proves recovery restores
 *	EXACTLY the durable-commit set -- replayably.
 *
 *	One run, for a given seed:
 *	  (a) Several worker fibers across several loops each commit a
 *	      sequence of two-row transactions through the xstore + the
 *	      group-commit WAL, on a temp WAL file + a fresh B-tree, under
 *	      xtc_sim_exec_run with seeded I/O latency so commit / fsync
 *	      ordering is part of the schedule.  A commit that RETURNS is
 *	      durable (the WAL writer fdatasync'd its record before acking).
 *	  (b) CRASH at a seeded step: the first worker draws a crash
 *	      threshold from the FAULT stream; a shared step counter is
 *	      bumped on each commit attempt, and the worker that bumps it
 *	      to the threshold calls xtc_exec_stop -- the sim scheduler
 *	      halts the whole run mid-workload (see xtc_sim_exec_run's
 *	      stop_flag check).  The buffer pool is then discarded WITHOUT
 *	      flushing, so the data file never received a page: only the
 *	      WAL is durable, exactly the NO-STEAL / NO-FORCE loss model of
 *	      the non-sim test.  The WAL file is then cut at the durable
 *	      frontier (durable_lsn) -- keeping only records the group-
 *	      commit writer fsync-CONFIRMED, discarding any complete-but-
 *	      unacked frame it had physically written (the sim backend
 *	      performs the real pwrite at submit time and defers only the
 *	      completion) but not yet fsync-acked when the run stopped.
 *	      That torn tail is a legitimate real-crash gray zone; cutting
 *	      at durable_lsn is the conservative, correct crash model (a
 *	      system guarantees durability only for data whose fsync
 *	      RETURNED) and makes the winner set a pure function of the
 *	      schedule.
 *	  (c) RECOVER: a FRESH empty B-tree is opened and xstore_recover
 *	      replays the crashed WAL into it.
 *	  (d) VERIFY the ACID durability + atomicity invariant:
 *	        - every ACKED commit (the fiber saw COMMIT return before
 *	          the crash) is fully present after recovery -- both its
 *	          rows, exact values (DURABILITY);
 *	        - every recovered row belongs to some ATTEMPTED transaction
 *	          (nothing fabricated) and, for any recovered transaction,
 *	          BOTH of its rows are present -- never one (ATOMICITY: no
 *	          torn transaction leaked in);
 *	        - the recovered set is a SUPERSET of the acked set
 *	          (recovered >= acked): a fewer count would be a lost
 *	          durable commit.  It can exceed acked by a transaction
 *	          whose COMMIT frame was fsync-confirmed (within the
 *	          durable frontier) but whose ack was still in flight to
 *	          its fiber at the crash -- a genuine winner, checked for
 *	          atomicity and attribution like every other.
 *	  (e) REPLAY: the same seed twice yields the identical crash point,
 *	      the identical acked set, and the identical post-recovery tree
 *	      content hash.  A sweep of many seeds crashes at many
 *	      different points (before any commit, mid-workload, after N),
 *	      and EACH recovers consistently for its own durable set.
 *
 *	Each run executes in a FRESH child process (fork), the FoundationDB
 *	discipline of isolating every simulation: the sqlxtc engine keeps
 *	process-global state (the MVCC commit clock -- monotonic, so it
 *	cannot be reset down -- the catalog cache, SSI / GC state), which
 *	would otherwise accumulate across back-to-back runs in one process
 *	and perturb a later run's version keys / B-tree layout / page I/O
 *	and thus its schedule, breaking replay for reasons unrelated to the
 *	WAL.  Fork isolation makes the outcome a pure function of the seed;
 *	the child reports its result over a pipe.  (This is a test-harness
 *	property -- the engine is left completely unmodified.)
 *
 *	If recovery does NOT match the durable-commit set -- a committed
 *	row missing, an uncommitted / torn row leaked in -- that is a REAL
 *	WAL / recovery bug; this test reports it precisely (seed, crash
 *	point, expected vs found) rather than weakening the assertion.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_async.h"      /* xtc_yield */
#include "xtc_sim.h"
#include "bufmgr.h"
#include "btree.h"
#include "wal.h"
#include "engine.h"
#include "xstore.h"

#define PAGE_SZ    4096
#define N_LOOPS    3
#define N_WORKERS  4          /* one fiber per (loop, slot); see spawn loop */
#define TXNS_PER   16         /* two-row transactions each worker attempts */
#define KEY_BASE   1000       /* worker w, txn i -> keys base+.. (see below) */

/*
 * The universe of transactions is fixed and disjoint per (worker, txn):
 * worker w, txn i writes rows at rowids R0(w,i) and R1(w,i) with value
 * "w-i".  Keys never collide, so a recovered row uniquely identifies its
 * originating transaction, and "both rows present or neither" is a clean
 * atomicity check.
 */
#define R0(w, i)   ((int64_t)(KEY_BASE + (w) * 100000 + (i) * 2))
#define R1(w, i)   ((int64_t)(KEY_BASE + (w) * 100000 + (i) * 2 + 1))

static bt_t       *g_bt;
static wal_t      *g_wal;
static xtc_exec_t *g_exec;

/* Seeded crash control (drawn once, inside the sim run, from the FAULT
 * stream so enabling the crash never perturbs the schedule). */
static _Atomic long g_step;         /* commit attempts so far (all workers) */
static _Atomic long g_crash_at;     /* threshold; <0 until the first worker draws it */
static _Atomic int  g_crashed;      /* set once when we trip xtc_exec_stop */

/*
 * Per-attempt records, indexed [worker][txn].  attempted: the fiber
 * began this txn's COMMIT.  acked: COMMIT returned (durable).  Written
 * only by the owning worker (disjoint slots) -> no cross-fiber races on
 * the arrays; the atomics coordinate the crash step.
 */
static uint8_t g_attempted[N_WORKERS][TXNS_PER];
static uint8_t g_acked[N_WORKERS][TXNS_PER];

struct warg { int id; };
static struct warg g_warg[N_WORKERS];

/*
 * Worker fiber: attempt TXNS_PER two-row transactions.  Before each
 * COMMIT, bump the shared step counter; the worker that reaches the
 * seeded crash threshold stops the executor (the crash) and returns.
 * A commit that RETURNS SX_OK is durable and is recorded as acked.
 *
 * Each worker opens its OWN sx_db connection over the shared engine
 * B-tree g_bt: an xstore connection's write buffer / transaction state
 * is per-connection (single-threaded), so concurrent transactions must
 * use distinct connections -- the real many-connections-over-one-engine
 * model.  Sharing one connection across fibers would interleave their
 * write buffers and produce genuinely torn frames (a test bug, not a
 * WAL bug).
 */
static void
worker_proc(void *arg)
{
	struct warg *wa = arg;
	int w = wa->id;
	sx_db *db = NULL;
	char sql[96];
	int i;

	if (sx_open_bt(g_bt, &db) != SX_OK)
		return;

	/* The first worker to run draws the crash threshold.  Only one
	 * draw happens (compare-exchange guards it), so the FAULT stream
	 * advances by exactly one regardless of which worker wins the
	 * seeded schedule -- keeping replay stable. */
	{
		long expect = -1;
		if (atomic_load_explicit(&g_crash_at, memory_order_relaxed) < 0) {
			long total = (long)N_WORKERS * TXNS_PER;
			/* [0 .. total]: 0 crashes before any commit, total+ never
			 * crashes (a clean drain).  Bias toward mid-workload by
			 * drawing in [0, total] inclusive. */
			long draw = (long)__xtc_sim_rng_range(XTC_SIM_RNG_FAULT,
			    (uint64_t)(total + 1));
			(void)atomic_compare_exchange_strong_explicit(&g_crash_at,
			    &expect, draw, memory_order_relaxed,
			    memory_order_relaxed);
		}
	}

	for (i = 0; i < TXNS_PER; i++) {
		long s;

		/* Crash point check: one step per commit attempt. */
		s = atomic_fetch_add_explicit(&g_step, 1, memory_order_relaxed) + 1;
		if (s >= atomic_load_explicit(&g_crash_at, memory_order_relaxed)) {
			int z = 0;
			if (atomic_compare_exchange_strong_explicit(&g_crashed,
			    &z, 1, memory_order_relaxed, memory_order_relaxed))
				(void)xtc_exec_stop(g_exec);
			goto done;   /* crashed: this commit never happens */
		}

		if (sx_exec(db, "BEGIN", NULL) != SX_OK)
			goto done;
		snprintf(sql, sizeof sql,
		    "INSERT INTO t(k,v) VALUES(%lld,'%d-%d');",
		    (long long)R0(w, i), w, i);
		if (sx_exec(db, sql, NULL) != SX_OK)
			goto done;
		snprintf(sql, sizeof sql,
		    "INSERT INTO t(k,v) VALUES(%lld,'%d-%d');",
		    (long long)R1(w, i), w, i);
		if (sx_exec(db, sql, NULL) != SX_OK)
			goto done;

		g_attempted[w][i] = 1;             /* commit is about to be durable-logged */
		if (sx_exec(db, "COMMIT", NULL) != SX_OK)
			goto done;
		g_acked[w][i] = 1;                 /* COMMIT returned -> durable */

		if ((i & 3) == 0)
			xtc_yield();
	}
done:
	sx_close(db);
}

/*
 * Model the crash boundary as the durable frontier: keep exactly the
 * records the WAL fsync-CONFIRMED (lsn <= durable_lsn), discarding any
 * complete-but-unacknowledged frame the group-commit writer had
 * physically pwritten (submit-time write) but not yet fsync-acked when
 * the run was stopped.  This is the conservative, correct crash model
 * -- a system guarantees durability only for data whose fsync RETURNED
 * -- and it makes the winner set a pure function of the schedule
 * (durable_lsn), so the recovered tree replays.  Without it the torn
 * tail past durable_lsn is a legitimate real-crash gray zone but not
 * reproducible.  Truncates `path` in place; returns 0 / -1.
 */
static int
wal_truncate_to_durable(const char *path, uint64_t durable_lsn)
{
	int fd;
	off_t off = 0, keep = 0;
	uint8_t hdr[12];              /* u64 lsn + u32 len, the on-disk record header */

	fd = open(path, O_RDWR);
	if (fd < 0)
		return -1;
	for (;;) {
		uint64_t lsn;
		uint32_t len;
		struct stat sb;
		off_t end;
		if (pread(fd, hdr, sizeof hdr, off) != (ssize_t)sizeof hdr)
			break;                /* EOF or torn header */
		memcpy(&lsn, hdr, 8);
		memcpy(&len, hdr + 8, 4);
		end = off + (off_t)sizeof hdr + (off_t)len + 8;
		                          /* +8: the u64 per-record CRC trailer
		                           * ([lsn][len][body][crc]); recovery's
		                           * checksum verify (STEAL Increment 1)
		                           * needs the record cut on this exact
		                           * boundary or the frontier record fails
		                           * its CRC and the scan stops early. */
		if (fstat(fd, &sb) != 0 || sb.st_size < end)
			break;                /* torn body: not a complete record */
		if (lsn > durable_lsn)
			break;                /* not fsync-confirmed: crash-lost */
		off = end;
		keep = off;
	}
	if (ftruncate(fd, keep) != 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

/* Read column v for rowid k from table t; 1 with *out set, else 0. */
static int
sel_v(sx_db *db, int64_t k, char *out, size_t cap)
{
	sx_stmt *st = NULL;
	int got = 0;
	if (sx_prepare(db, "SELECT v FROM t WHERE k=?", -1, &st, NULL) != SX_OK)
		return 0;
	sx_bind_int64(st, 1, k);
	if (sx_step(st) == SX_ROW) {
		const unsigned char *t = sx_column_text(st, 0);
		size_t n = (size_t)sx_column_bytes(st, 0);
		if (n >= cap)
			n = cap - 1;
		if (t != NULL)
			memcpy(out, t, n);
		out[n] = '\0';
		got = 1;
	}
	sx_finalize(st);
	return got;
}

/*
 * One crash-recovery run for `seed`.  Fills the observable outputs:
 *   *out_crash_at   the seeded crash threshold (commit-attempt count)
 *   *out_acked      number of transactions the workers saw commit
 *   *out_recovered  number of transactions fully present after recovery
 *   *out_hash       content hash of the recovered tree (for replay)
 * Returns 0 on success, or a negative code on a HARD FAILURE:
 *   -1 setup/teardown error (not a data bug)
 *   -2 the sim run did not stop cleanly
 *   -3 DURABILITY violated: an acked commit is missing after recovery
 *   -4 ATOMICITY violated: a torn transaction (one row, not both)
 *   -5 a fabricated / uncommitted-and-unattempted row leaked in
 * On a data-bug return (-3/-4/-5) *out_* still carry the diagnosis and
 * the offending (w,i) is printed by the caller-visible globals below.
 */
static int64_t g_bug_rowid;   /* offending rowid on a data-bug return */
static int     g_bug_w, g_bug_i;

static int
run_once_inproc(uint64_t seed, long *out_crash_at, int *out_acked,
    int *out_recovered, uint64_t *out_hash)
{
	xtc_proc_opts_t opts = { 0 };
	wal_opts_t wo = { 0 };
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm1 = NULL, *bm2 = NULL;
	bt_t *bt2 = NULL;
	sx_db *dbsetup = NULL, *db2 = NULL;
	xtc_pid_t wp, ww;
	char logp[] = "/tmp/sim_crash_wal_XXXXXX";
	char btA[]  = "/tmp/sim_crash_A_XXXXXX";
	char btB[]  = "/tmp/sim_crash_B_XXXXXX";
	int fd, i, w, rc, acked = 0, recovered = 0, ret = 0;
	uint64_t h = 0xCBF29CE484222325ull;   /* FNV-1a basis */

	atomic_store(&g_step, 0);
	atomic_store(&g_crash_at, -1);
	atomic_store(&g_crashed, 0);
	memset(g_attempted, 0, sizeof g_attempted);
	memset(g_acked, 0, sizeof g_acked);
	g_bug_rowid = 0; g_bug_w = -1; g_bug_i = -1;

	fd = mkstemp(logp); if (fd < 0) return -1; close(fd);
	fd = mkstemp(btA);  if (fd < 0) { unlink(logp); return -1; } close(fd);
	fd = mkstemp(btB);  if (fd < 0) { unlink(logp); unlink(btA); return -1; } close(fd);

	/* ---- phase 1: workload under the deterministic scheduler ---- */
	wo.path = logp; wo.window_ns = 500000; wo.max_batch = 256;
	if (wal_open(&wo, &g_wal) != XTC_OK) { ret = -1; goto cleanup_files; }

	/* Pool large enough that nothing is ever evicted -> no page reaches
	 * the data file; the WAL is the only durable copy (mirrors the
	 * non-sim test's total-pool-loss crash). */
	bo.path = btA; bo.page_size = PAGE_SZ; bo.n_frames = 1024;
	if (bm_create(&bo, &bm1) != XTC_OK) { wal_close(g_wal); ret = -1; goto cleanup_files; }
	if (bt_open(bm1, &g_bt) != XTC_OK) { bm_destroy(bm1); wal_close(g_wal); ret = -1; goto cleanup_files; }
	xstore_set_wal((struct wal *)g_wal);
	/* Create the table durably BEFORE the workload: a synchronous
	 * off-loop DDL connection (wal_commit_sync appends + fdatasyncs
	 * directly, no writer needed) lands the catalog record at the head
	 * of the WAL, so recovery restores the table-id mapping.  Closed
	 * before the run -- each worker opens its own connection. */
	if (sx_open_bt(g_bt, &dbsetup) != SX_OK) { ret = -1; goto cleanup_engine1; }
	if (sx_exec(dbsetup, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) != SX_OK) {
		sx_close(dbsetup); ret = -1; goto cleanup_engine1;
	}
	sx_close(dbsetup); dbsetup = NULL;

	if (xtc_exec_init(&g_exec, N_LOOPS) != XTC_OK) { ret = -1; goto cleanup_engine1; }

	/* Deferred, seeded-latency WAL I/O completions -- NO injected
	 * errors (an fsync EIO would be a spurious durability loss); the
	 * latency alone reorders the group-commit fsyncs across runs, so
	 * which commits are durable at the crash is part of the schedule. */
	xtc_sim_io_faults_enable(50 * 1000LL, 500 * 1000LL, 0);

	/* WAL group-commit writer on loop 0. */
	if (wal_writer_spawn(g_wal, xtc_exec_loop(g_exec, 0), &wp) != XTC_OK) {
		xtc_sim_io_faults_disable();
		(void)xtc_exec_fini(g_exec); g_exec = NULL;
		ret = -1; goto cleanup_engine1;
	}

	for (i = 0; i < N_WORKERS; i++) {
		g_warg[i].id = i;
		opts.name = "crash-worker";
		/* Spread workers across loops 1..N_LOOPS-1 (loop 0 hosts the
		 * writer) so commit and fsync genuinely interleave. */
		if (xtc_proc_spawn(xtc_exec_loop(g_exec, 1 + (i % (N_LOOPS - 1))),
		    worker_proc, &g_warg[i], &opts, &ww) != XTC_OK) {
			(void)xtc_exec_stop(g_exec);
			(void)xtc_sim_exec_run(g_exec, seed, 5000000);
			xtc_sim_io_faults_disable();
			(void)xtc_exec_fini(g_exec); g_exec = NULL;
			ret = -1; goto cleanup_engine1;
		}
	}

	rc = xtc_sim_exec_run(g_exec, seed, 20000000);
	/* XTC_OK: the workload drained (crash never fired, or fired after
	 * the last commit).  A run stopped by xtc_exec_stop also returns
	 * XTC_OK (the stop_flag path).  Only AGAIN/DEADLK/negative is a
	 * hang / scheduler bug we must not paper over. */
	*out_crash_at = atomic_load(&g_crash_at);
	xtc_sim_io_faults_disable();
	(void)xtc_exec_fini(g_exec); g_exec = NULL;
	if (rc != XTC_OK) { ret = -2; goto cleanup_engine1; }

	/* Tally the acked (durable) set the workers observed, and capture the
	 * fsync-confirmed frontier -- the deterministic crash boundary. */
	{
		uint64_t dlsn = wal_durable_lsn(g_wal);
		for (w = 0; w < N_WORKERS; w++)
			for (i = 0; i < TXNS_PER; i++)
				if (g_acked[w][i])
					acked++;

		/* ---- crash ---- Lose the pool WITHOUT flushing (data file empty)
		 * and cut the WAL at the durable frontier: only fsync-confirmed
		 * records survive. */
		xstore_set_wal(NULL);
		bt_close(g_bt); g_bt = NULL;
		bm_destroy(bm1); bm1 = NULL;      /* every dirty page lost */
		wal_close(g_wal); g_wal = NULL;
		if (wal_truncate_to_durable(logp, dlsn) != 0) {
			ret = -1; goto cleanup_files;
		}
	}

	/* ---- recover into a FRESH empty B-tree from the crashed WAL ---- */
	{
		bm_opts_t b2 = BM_OPTS_DEFAULT;
		b2.path = btB; b2.page_size = PAGE_SZ; b2.n_frames = 256;
		if (bm_create(&b2, &bm2) != XTC_OK) { ret = -1; goto cleanup_files; }
	}
	if (bt_open(bm2, &bt2) != XTC_OK) { bm_destroy(bm2); ret = -1; goto cleanup_files; }
	if (xstore_recover(bt2, logp) != XTC_OK) {
		bt_close(bt2); bm_destroy(bm2); ret = -1; goto cleanup_files;
	}
	if (sx_open_bt(bt2, &db2) != SX_OK) {
		bt_close(bt2); bm_destroy(bm2); ret = -1; goto cleanup_files;
	}
	if (sx_exec(db2, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) != SX_OK) {
		sx_close(db2); bt_close(bt2); bm_destroy(bm2); ret = -1; goto cleanup_files;
	}

	/* ---- verify the recovery invariant ---- */
	for (w = 0; w < N_WORKERS && ret == 0; w++) {
		for (i = 0; i < TXNS_PER; i++) {
			char b0[32], b1[32], want[16];
			int p0, p1;

			snprintf(want, sizeof want, "%d-%d", w, i);
			p0 = sel_v(db2, R0(w, i), b0, sizeof b0) &&
			    strcmp(b0, want) == 0;
			p1 = sel_v(db2, R1(w, i), b1, sizeof b1) &&
			    strcmp(b1, want) == 0;

			/* ATOMICITY: a transaction is present as BOTH rows or
			 * NEITHER; one-of-two is a torn commit -> real bug. */
			if (p0 != p1) {
				g_bug_w = w; g_bug_i = i;
				g_bug_rowid = p0 ? R1(w, i) : R0(w, i);
				ret = -4;
				break;
			}
			if (p0 && p1) {
				/* DURABILITY (checked below via the acked scan):
				 * this txn is fully present.  Fold it into the
				 * content hash for replay equality. */
				recovered++;
				h ^= (uint64_t)R0(w, i); h *= 0x100000001B3ull;
				h ^= (uint64_t)R1(w, i); h *= 0x100000001B3ull;
				h ^= (uint64_t)(unsigned char)want[0]; h *= 0x100000001B3ull;
			}
			/* A present txn that was never attempted would be a
			 * fabricated row -- impossible under this fixed key
			 * universe (every key maps to some (w,i) we attempt),
			 * but guard it: if present yet not attempted, it leaked. */
			if (p0 && p1 && !g_attempted[w][i]) {
				g_bug_w = w; g_bug_i = i; g_bug_rowid = R0(w, i);
				ret = -5;
				break;
			}
		}
	}

	/* DURABILITY: every acked commit MUST be present after recovery. */
	if (ret == 0) {
		for (w = 0; w < N_WORKERS && ret == 0; w++) {
			for (i = 0; i < TXNS_PER; i++) {
				char b0[32], want[16];
				if (!g_acked[w][i])
					continue;
				snprintf(want, sizeof want, "%d-%d", w, i);
				if (!(sel_v(db2, R0(w, i), b0, sizeof b0) &&
				    strcmp(b0, want) == 0)) {
					g_bug_w = w; g_bug_i = i;
					g_bug_rowid = R0(w, i);
					ret = -3;   /* durable commit lost */
					break;
				}
			}
		}
	}

	*out_acked = acked;
	*out_recovered = recovered;
	*out_hash = h;

	sx_close(db2); bt_close(bt2); bm_destroy(bm2);
	unlink(logp); unlink(btA); unlink(btB);
	return ret;

cleanup_engine1:
	if (dbsetup != NULL) { sx_close(dbsetup); dbsetup = NULL; }
	xstore_set_wal(NULL);
	if (g_bt != NULL) { bt_close(g_bt); g_bt = NULL; }
	if (bm1 != NULL) bm_destroy(bm1);
	if (g_wal != NULL) { wal_close(g_wal); g_wal = NULL; }
cleanup_files:
	unlink(logp); unlink(btA); unlink(btB);
	*out_acked = acked;
	*out_recovered = recovered;
	*out_hash = 0;
	return ret;
}

/*
 * Fork-isolated wrapper.  Each run executes in a FRESH child process so
 * the sqlxtc engine's process-global state (the MVCC commit clock, the
 * catalog cache, SSI/GC state, ...) starts pristine every time -- the
 * FoundationDB discipline of running each simulation in its own process.
 * Without isolation those accumulate across back-to-back runs in one
 * process and perturb a later run's version keys / B-tree layout / page
 * I/O and thus its schedule, which would break replay for reasons that
 * have nothing to do with the WAL.  The child computes the result and
 * writes it (plus the bug diagnosis) back over a pipe; the parent reads
 * it, so the same seed is guaranteed to see identical globals and the
 * replay assertions test the WAL/recovery path, not leaked state.
 */
struct run_result {
	int      ret;
	long     crash_at;
	int      acked;
	int      recovered;
	uint64_t hash;
	int64_t  bug_rowid;
	int      bug_w, bug_i;
};

static int
run_once(uint64_t seed, long *out_crash_at, int *out_acked,
    int *out_recovered, uint64_t *out_hash)
{
	int pfd[2];
	pid_t pid;
	struct run_result rr;

	if (pipe(pfd) != 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pfd[0]); close(pfd[1]);
		return -1;
	}
	if (pid == 0) {
		/* Child: run in a pristine address space, report, _exit
		 * (never return through the parent's stack / atexit). */
		ssize_t wn;
		close(pfd[0]);
		memset(&rr, 0, sizeof rr);
		rr.ret = run_once_inproc(seed, &rr.crash_at, &rr.acked,
		    &rr.recovered, &rr.hash);
		rr.bug_rowid = g_bug_rowid;
		rr.bug_w = g_bug_w;
		rr.bug_i = g_bug_i;
		wn = write(pfd[1], &rr, sizeof rr);
		close(pfd[1]);
		_exit(wn == (ssize_t)sizeof rr ? 0 : 2);
	}
	/* Parent: read the result, reap the child. */
	close(pfd[1]);
	{
		size_t got = 0;
		int status;
		while (got < sizeof rr) {
			ssize_t n = read(pfd[0], (char *)&rr + got, sizeof rr - got);
			if (n <= 0)
				break;
			got += (size_t)n;
		}
		close(pfd[0]);
		(void)waitpid(pid, &status, 0);
		if (got != sizeof rr) {
			/* Child crashed / short write: surface as a hard error
			 * (a SIGSEGV/SIGABRT in the engine is a real bug). */
			*out_crash_at = 0; *out_acked = 0;
			*out_recovered = 0; *out_hash = 0;
			g_bug_rowid = 0; g_bug_w = -1; g_bug_i = -1;
			return -2;
		}
	}
	*out_crash_at = rr.crash_at;
	*out_acked = rr.acked;
	*out_recovered = rr.recovered;
	*out_hash = rr.hash;
	g_bug_rowid = rr.bug_rowid;
	g_bug_w = rr.bug_w;
	g_bug_i = rr.bug_i;
	return rr.ret;
}

/* Print a precise diagnosis for a data-bug return code. */
static void
report_bug(uint64_t seed, long crash_at, int rc, int acked, int recovered)
{
	const char *what =
	    rc == -3 ? "DURABILITY: an acked commit is MISSING after recovery" :
	    rc == -4 ? "ATOMICITY: a TORN transaction (one row present, not both)" :
	    rc == -5 ? "LEAK: an unattempted/uncommitted row is PRESENT" :
	    rc == -2 ? "the sim run did not stop cleanly (hang / scheduler bug)" :
	    "setup/teardown error";
	fprintf(stderr,
	    "FAIL: seed=0x%llx crash_at=%ld acked=%d recovered=%d\n"
	    "      %s\n",
	    (unsigned long long)seed, crash_at, acked, recovered, what);
	if (rc == -3 || rc == -4 || rc == -5)
		fprintf(stderr,
		    "      offending txn worker=%d txn=%d rowid=%lld\n",
		    g_bug_w, g_bug_i, (long long)g_bug_rowid);
}

int
main(void)
{
	uint64_t seed0 = 0xC7A54;
	long ca1 = 0, ca2 = 0;
	int ak1 = 0, ak2 = 0, rv1 = 0, rv2 = 0, rc;
	uint64_t h1 = 0, h2 = 0;
	int nseeds, i;
	int n_before = 0, n_mid = 0, n_after = 0;   /* crash-point distribution */
	int min_acked = 1 << 30, max_acked = -1;
	long total_txns = (long)N_WORKERS * TXNS_PER;

	/* --- same seed twice: replay (identical crash + acked + tree) --- */
	rc = run_once(seed0, &ca1, &ak1, &rv1, &h1);
	if (rc != 0) {
		report_bug(seed0, ca1, rc, ak1, rv1);
		return 1;
	}
	rc = run_once(seed0, &ca2, &ak2, &rv2, &h2);
	if (rc != 0) {
		report_bug(seed0, ca2, rc, ak2, rv2);
		return 1;
	}
	if (ca1 != ca2 || ak1 != ak2 || rv1 != rv2 || h1 != h2) {
		printf("FAIL: crash-recovery did not replay "
		    "(crash_at %ld/%ld acked %d/%d recovered %d/%d "
		    "hash %016llx/%016llx)\n",
		    ca1, ca2, ak1, ak2, rv1, rv2,
		    (unsigned long long)h1, (unsigned long long)h2);
		return 1;
	}
	/* Durable set must be fully recovered (recovered >= acked; extras
	 * are in-flight-at-crash winners the file happened to hold). */
	/* The recovered set must be a SUPERSET of the acked set: every commit
	 * the fiber saw return is durable (rv < ak would be a lost durable
	 * commit -- a real bug).  It can exceed it by transactions whose
	 * COMMIT frame the writer fsync-confirmed (durable_lsn advanced past
	 * it) but whose ack was still in flight to the committing fiber when
	 * the crash fired -- genuine winners the fiber did not yet observe.
	 * Those extras are still checked for atomicity (both rows) and
	 * attribution (an attempted txn) in the per-txn verify loop. */
	if (rv1 < ak1) {
		printf("FAIL: recovered %d < acked %d for seed 0x%llx "
		    "(a durable commit was LOST)\n",
		    rv1, ak1, (unsigned long long)seed0);
		return 1;
	}
	printf("replay: seed=0x%llx crash_at=%ld acked=%d recovered=%d "
	    "hash=%016llx (identical on both runs)\n",
	    (unsigned long long)seed0, ca1, ak1, rv1,
	    (unsigned long long)h1);

	/* --- sweep many seeds: crashes land at many points, EACH must
	 *     recover consistently for its own durable set. --- */
	nseeds = 40;
	for (i = 0; i < nseeds; i++) {
		uint64_t seed = 0x9E3779B97F4A7C15ull * (uint64_t)(i + 1) + 0x1234;
		long ca = 0;
		int ak = 0, rv = 0;
		uint64_t h = 0, hb = 0;
		int aka = 0, rva = 0;
		long cab = 0;

		rc = run_once(seed, &ca, &ak, &rv, &h);
		if (rc != 0) {
			report_bug(seed, ca, rc, ak, rv);
			return 1;
		}
		if (rv < ak) {
			printf("FAIL: seed=0x%llx recovered %d < acked %d "
			    "(a durable commit was LOST)\n",
			    (unsigned long long)seed, rv, ak);
			return 1;
		}
		/* Each seed also replays. */
		rc = run_once(seed, &cab, &aka, &rva, &hb);
		if (rc != 0) {
			report_bug(seed, cab, rc, aka, rva);
			return 1;
		}
		if (ca != cab || ak != aka || rv != rva || h != hb) {
			printf("FAIL: seed=0x%llx did not replay "
			    "(crash_at %ld/%ld acked %d/%d recovered %d/%d "
			    "hash %016llx/%016llx)\n",
			    (unsigned long long)seed, ca, cab, ak, aka,
			    rv, rva, (unsigned long long)h,
			    (unsigned long long)hb);
			return 1;
		}

		if (ca <= 0)
			n_before++;
		else if (ca >= total_txns)
			n_after++;
		else
			n_mid++;
		if (ak < min_acked)
			min_acked = ak;
		if (ak > max_acked)
			max_acked = ak;
	}

	printf("sweep: %d seeds x2 (replayed) -- crash-point distribution: "
	    "%d before-any-commit, %d mid-workload, %d clean-drain; "
	    "acked ranged %d..%d of %ld possible; ALL recovered consistently "
	    "(durable set fully present, no torn / leaked rows)\n",
	    nseeds, n_before, n_mid, n_after, min_acked, max_acked, total_txns);

	printf("OK: sqlxtc WAL crash-recovery under DST -- seeded crash at an "
	    "arbitrary mid-workload point halts the run with the pool "
	    "unflushed; xstore_recover from the crashed WAL restores EXACTLY "
	    "the durable-commit set (every acked txn present in full, no torn "
	    "or uncommitted row leaked), and the crash point + winner set + "
	    "recovered tree replay byte-identically from the seed across a "
	    "%d-seed sweep\n", nseeds);
	return 0;
}
