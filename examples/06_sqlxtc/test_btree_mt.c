/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_btree_mt.c
 *	Concurrent B-tree: multiple writer processes and reader processes
 *	driving one tree on a multi-loop executor (one OS thread per
 *	loop), with the page-provider live and page I/O offloaded.
 *
 *	Writers own disjoint key ranges and insert them concurrently;
 *	they serialize on the tree's fiber-yielding writer lock (so a
 *	writer parked on I/O never wedges the loop and contending writers
 *	park instead of blocking).  Readers run lock-free lookups
 *	concurrently with the writers: a hit must carry the correct
 *	value, and a miss is confirmed under the writer lock.  After the
 *	writers finish, every key must be findable -- the gate against
 *	lost writes and the reader-vs-split false-miss.  No daemon.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bufmgr.h"
#include "btree.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"

#define N_LOOPS     4
#define N_WRITERS   4
#define N_READERS   4
#define KEYS_PER    600
#define N_KEYS      (N_WRITERS * KEYS_PER)
#define N_FRAMES    24
#define PAGE_SZ     4096

static bm_t       *g_bm;
static bt_t       *g_bt;
static _Atomic int g_writers_left;
static _Atomic long g_read_hits;
static _Atomic long g_read_mismatch;   /* found a key with the wrong value */

static void mkkv(int idx, char *k, char *v)
{
	snprintf(k, 24, "key-%07d", idx);
	snprintf(v, 32, "val-%07d-data", idx);
}

static void
writer_proc(void *arg)
{
	long w = (long)arg;
	int base = (int)w * KEYS_PER;
	int i;
	char k[24], v[32];

	/* Insert this writer's disjoint range, shuffled. */
	for (i = 0; i < KEYS_PER; i++) {
		int j = base + (int)(((uint64_t)i * 2654435761ull) % KEYS_PER);
		mkkv(j, k, v);
		(void)bt_insert(g_bt, k, (uint16_t)strlen(k), v, (uint16_t)strlen(v));
	}
	atomic_fetch_sub(&g_writers_left, 1);
	/* The writer that drops the count to zero stops the provider so the
	 * executor can observe a globally idle state and return. */
	if (atomic_load(&g_writers_left) == 0)
		bm_provider_stop(g_bm);
}

static void
reader_proc(void *arg)
{
	uint64_t rng = (uint64_t)(uintptr_t)arg * 0x9E3779B97F4A7C15ull + 1;
	char k[24], v[32], buf[40];
	uint16_t vl;
	int spins = 0;

	/* Probe random keys while writers run.  A found key must be
	 * correct; a not-yet-inserted key simply misses (no assertion). */
	while (atomic_load(&g_writers_left) > 0 && spins < 200000) {
		int idx;
		rng = rng * 6364136223846793005ull + 1442695040888963407ull;
		idx = (int)((rng >> 33) % N_KEYS);
		mkkv(idx, k, v);
		if (bt_lookup(g_bt, k, (uint16_t)strlen(k), buf, sizeof buf, &vl)
		    == XTC_OK) {
			atomic_fetch_add(&g_read_hits, 1);
			if (vl != strlen(v) || memcmp(buf, v, vl) != 0)
				atomic_fetch_add(&g_read_mismatch, 1);
		}
		spins++;
		if ((spins & 63) == 0) {
			void *m = NULL; size_t n = 0;
			(void)xtc_recv(&m, &n, 1LL * 1000 * 1000);   /* yield 1ms */
			if (m) m = NULL;
		}
	}
}

int
main(void)
{
	xtc_exec_t *exec = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	xtc_loop_t *l0;
	bm_stats_t bs;
	bt_stats_t ts;
	char path[] = "/tmp/sqlxtc-btmt-XXXXXX";
	int fd, i, missing = 0;
	char k[24], v[32], buf[40];
	uint16_t vl;

	fd = mkstemp(path); if (fd < 0) { perror("mkstemp"); return 1; }
	close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = N_FRAMES; bo.cool_pct = 25;
	if (bm_create(&bo, &g_bm) != XTC_OK) { fprintf(stderr, "bm_create\n"); return 1; }
	if (bt_open(g_bm, &g_bt) != XTC_OK) { fprintf(stderr, "bt_open\n"); return 1; }

	atomic_store(&g_writers_left, N_WRITERS);
	atomic_store(&g_read_hits, 0);
	atomic_store(&g_read_mismatch, 0);

	if (xtc_exec_init(&exec, N_LOOPS) != XTC_OK) { fprintf(stderr, "exec_init\n"); return 1; }
	l0 = xtc_exec_loop(exec, 0);
	if (bm_provider_spawn(g_bm, l0, 1LL * 1000 * 1000, NULL) != XTC_OK) return 1;

	/* Spread writers and readers across the loops (== OS threads). */
	{
		long w, r;
		for (w = 0; w < N_WRITERS; w++) {
			xtc_loop_t *lp = xtc_exec_loop(exec, (int)(w % N_LOOPS));
			xtc_proc_opts_t po = { .name = "wr" };
			xtc_pid_t pid;
			if (xtc_proc_spawn(lp, writer_proc, (void *)w, &po, &pid) != XTC_OK)
				return 1;
		}
		for (r = 0; r < N_READERS; r++) {
			xtc_loop_t *lp = xtc_exec_loop(exec, (int)((r + 1) % N_LOOPS));
			xtc_proc_opts_t po = { .name = "rd" };
			xtc_pid_t pid;
			if (xtc_proc_spawn(lp, reader_proc, (void *)(r + 1), &po, &pid) != XTC_OK)
				return 1;
		}
	}

	if (xtc_exec_run(exec) != XTC_OK) { fprintf(stderr, "exec_run\n"); return 1; }
	bm_provider_stop(g_bm);
	(void)xtc_exec_fini(exec);

	/* Gate: every key must be present with the correct value. */
	for (i = 0; i < N_KEYS; i++) {
		mkkv(i, k, v);
		if (bt_lookup(g_bt, k, (uint16_t)strlen(k), buf, sizeof buf, &vl)
		    != XTC_OK) { missing++; if (missing <= 5) fprintf(stderr, "MISSING %s\n", k); continue; }
		if (vl != strlen(v) || memcmp(buf, v, vl) != 0) {
			fprintf(stderr, "WRONG VALUE %s\n", k); missing++;
		}
	}

	bm_get_stats(g_bm, &bs);
	bt_get_stats(g_bt, &ts);
	bt_close(g_bt); bm_destroy(g_bm); unlink(path);
	{ char wal[80]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }

	if (atomic_load(&g_read_mismatch) != 0) {
		fprintf(stderr, "FAIL: %ld concurrent reads saw a wrong value\n",
		    atomic_load(&g_read_mismatch));
		return 1;
	}
	if (missing != 0) {
		fprintf(stderr, "FAIL: %d/%d keys missing or wrong after concurrent build\n",
		    missing, N_KEYS);
		return 1;
	}

	printf("  ok   %d writers + %d readers on %d OS threads: all %d keys "
	    "present + correct after concurrent build (height=%llu splits=%llu)\n",
	    N_WRITERS, N_READERS, N_LOOPS, N_KEYS,
	    (unsigned long long)ts.height, (unsigned long long)ts.splits);
	printf("  ok   %ld concurrent lock-free reads, 0 wrong values; tree paged "
	    "through the pool (loads=%llu evicted=%llu flushed=%llu resident=%llu)\n",
	    atomic_load(&g_read_hits), (unsigned long long)bs.loads,
	    (unsigned long long)bs.evicted, (unsigned long long)bs.flushed,
	    (unsigned long long)bs.resident);
	printf("All sqlxtc concurrent B-tree tests passed.\n");
	return 0;
}
