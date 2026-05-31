/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_wal.c
 *	Group-commit WAL writer (wal.c).  Many committer processes drive
 *	concurrent commits at one writer; the test asserts:
 *
 *	  - every commit is acknowledged with a unique, monotonic LSN
 *	    (durability + total order from the single writer);
 *	  - fsyncs (batches) are far fewer than commits -- the writer
 *	    actually coalesces (the whole point of group commit);
 *	  - after the run, the log file replays to exactly the records
 *	    committed, in LSN order (recovery reads back what was made
 *	    durable);
 *	  - the loop never wedges though the writer parks on every fsync
 *	    (offloaded) and every committer parks on its ack.
 *
 *	Self-contained, no daemon.  Runs on a single loop and again on a
 *	multi-loop executor (committers spread across OS threads).
 */

#include <assert.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wal.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"

#define N_COMMITTERS  16
#define PER_COMMITTER 64
#define N_COMMITS     (N_COMMITTERS * PER_COMMITTER)

static wal_t       *g_wal;
static _Atomic int  g_left;             /* committers still running */
static _Atomic long g_done;             /* commits acknowledged */
static _Atomic uint64_t g_lsn_xor;      /* xor of all LSNs (uniqueness check) */
static _Atomic uint64_t g_lsn_sum;      /* sum of all LSNs (1..N triangular) */

static void
committer_proc(void *arg)
{
	long id = (long)arg;
	int i;
	char rec[48];

	for (i = 0; i < PER_COMMITTER; i++) {
		uint64_t lsn = 0;
		int n = snprintf(rec, sizeof rec, "txn-%03ld-%03d-payload", id, i);
		if (wal_commit(g_wal, rec, (uint16_t)n, &lsn) == XTC_OK) {
			atomic_fetch_add(&g_done, 1);
			atomic_fetch_xor(&g_lsn_xor, lsn);
			atomic_fetch_add(&g_lsn_sum, lsn);
		}
	}
	/* The last committer to finish tells the writer to drain + exit so
	 * the loop/executor can return (mirrors bm_provider_stop). */
	if (atomic_fetch_sub(&g_left, 1) == 1)
		(void)wal_writer_stop(g_wal);
}

/* Replay the log file: count records, verify LSNs are 1..N in order. */
static int
replay_check(const char *path)
{
	int fd = open(path, O_RDONLY);
	uint64_t expect = 1;
	int count = 0;
	uint8_t hdr[10];

	if (fd < 0)
		return -1;
	for (;;) {
		ssize_t r = read(fd, hdr, sizeof hdr);
		uint64_t lsn;
		uint16_t len;
		uint8_t body[256];

		if (r == 0)
			break;                  /* EOF */
		if (r != (ssize_t)sizeof hdr) { close(fd); return -1; }
		memcpy(&lsn, hdr, 8);
		memcpy(&len, hdr + 8, 2);
		if (lsn != expect) { close(fd); return -2; }   /* out of order */
		if (len > sizeof body) { close(fd); return -3; }
		if (read(fd, body, len) != (ssize_t)len) { close(fd); return -1; }
		expect++;
		count++;
	}
	close(fd);
	return count;
}

static int
run_one(int n_loops, const char *tag)
{
	char path[] = "/tmp/sqlxtc-wal-XXXXXX";
	wal_opts_t wo = { 0 };
	wal_stats_t st;
	int fd, replayed;
	uint64_t expect_sum = (uint64_t)N_COMMITS * (N_COMMITS + 1) / 2;

	fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); return 1; }
	close(fd);

	wo.path = path;
	wo.window_ns = 500000;          /* 0.5ms gather window */
	wo.max_batch = 256;
	if (wal_open(&wo, &g_wal) != XTC_OK) { fprintf(stderr, "wal_open\n"); return 1; }

	atomic_store(&g_left, N_COMMITTERS);
	atomic_store(&g_done, 0);
	atomic_store(&g_lsn_xor, 0);
	atomic_store(&g_lsn_sum, 0);

	if (n_loops <= 1) {
		xtc_loop_t *loop = NULL;
		long c;
		assert(xtc_loop_init(&loop) == XTC_OK);
		assert(wal_writer_spawn(g_wal, loop, NULL) == XTC_OK);
		for (c = 0; c < N_COMMITTERS; c++) {
			xtc_proc_opts_t po = { .name = "cmt" };
			xtc_pid_t pid;
			assert(xtc_proc_spawn(loop, committer_proc, (void *)c, &po, &pid) == XTC_OK);
		}
		assert(xtc_loop_run(loop) == XTC_OK);
		assert(xtc_loop_fini(loop) == XTC_OK);
	} else {
		xtc_exec_t *exec = NULL;
		long c;
		assert(xtc_exec_init(&exec, n_loops) == XTC_OK);
		assert(wal_writer_spawn(g_wal, xtc_exec_loop(exec, 0), NULL) == XTC_OK);
		for (c = 0; c < N_COMMITTERS; c++) {
			xtc_loop_t *lp = xtc_exec_loop(exec, (int)(c % n_loops));
			xtc_proc_opts_t po = { .name = "cmt" };
			xtc_pid_t pid;
			assert(xtc_proc_spawn(lp, committer_proc, (void *)c, &po, &pid) == XTC_OK);
		}
		assert(xtc_exec_run(exec) == XTC_OK);
		(void)xtc_exec_fini(exec);
	}

	wal_get_stats(g_wal, &st);

	if (atomic_load(&g_done) != N_COMMITS) {
		fprintf(stderr, "FAIL[%s]: %ld/%d commits acked\n", tag,
		    atomic_load(&g_done), N_COMMITS);
		return 1;
	}
	/* LSNs must be exactly 1..N: the sum is the triangular number and
	 * (since each is unique) replay sees them in order. */
	if (atomic_load(&g_lsn_sum) != expect_sum) {
		fprintf(stderr, "FAIL[%s]: LSN sum %llu != %llu (dup/missing LSN)\n",
		    tag, (unsigned long long)atomic_load(&g_lsn_sum),
		    (unsigned long long)expect_sum);
		return 1;
	}
	if (st.commits != N_COMMITS || st.durable_lsn != (uint64_t)N_COMMITS) {
		fprintf(stderr, "FAIL[%s]: stats commits=%llu durable_lsn=%llu\n",
		    tag, (unsigned long long)st.commits,
		    (unsigned long long)st.durable_lsn);
		return 1;
	}
	replayed = replay_check(path);
	if (replayed != N_COMMITS) {
		fprintf(stderr, "FAIL[%s]: replay returned %d (want %d)\n",
		    tag, replayed, N_COMMITS);
		return 1;
	}
	if (st.batches >= (uint64_t)N_COMMITS) {
		fprintf(stderr, "FAIL[%s]: no coalescing (%llu batches for %d commits)\n",
		    tag, (unsigned long long)st.batches, N_COMMITS);
		return 1;
	}

	printf("  ok   [%s] %d commits across %d committers durable in %llu fsync "
	    "batches (group factor %.1fx, max batch %llu); log replays to %d "
	    "records in LSN order\n",
	    tag, N_COMMITS, N_COMMITTERS, (unsigned long long)st.batches,
	    (double)N_COMMITS / (double)st.batches,
	    (unsigned long long)st.max_batch_seen, replayed);

	wal_close(g_wal);
	unlink(path);
	return 0;
}

int
main(void)
{
	if (run_one(1, "single-loop") != 0)
		return 1;
	if (run_one(4, "4-loop executor") != 0)
		return 1;
	printf("All sqlxtc WAL group-commit tests passed.\n");
	return 0;
}
