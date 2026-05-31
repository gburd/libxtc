/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_btree_loop.c
 *	The B-tree engine driven ON a cooperative loop with the live
 *	page-provider running concurrently and page I/O offloaded
 *	(xtc_blocking), so faults and eviction writebacks PARK the worker
 *	rather than blocking the loop.  This exercises the async-I/O path
 *	the off-loop test_btree does not.  A single worker process drives
 *	the tree; the only other process is the page-provider, which
 *	takes no content latch, so the worker may hold a node latch
 *	across a parked write without wedging the loop.  No daemon.
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
#include "xtc_proc.h"

#define N_KEYS    2000
#define N_FRAMES  20
#define PAGE_SZ   4096

static bm_t       *g_bm;
static bt_t       *g_bt;
static _Atomic int g_result;

static void
worker_proc(void *arg)
{
	int k, ok = 1;
	char key[32], val[48], buf[64];
	uint16_t vlen;
	(void)arg;

	/* Build a tree far larger than the pool: every fault/eviction
	 * here offloads its page I/O and parks this worker. */
	for (k = 0; k < N_KEYS; k++) {
		int j = (int)(((uint64_t)k * 2654435761ull) % N_KEYS); /* 64-bit: a real permutation */
		snprintf(key, sizeof key, "key-%06d", j);
		snprintf(val, sizeof val, "val-%06d-payload", j);
		if (bt_insert(g_bt, key, (uint16_t)strlen(key),
		    val, (uint16_t)strlen(val)) != XTC_OK) { ok = 0; break; }
	}

	/* Look every key back up (most pages have been evicted -> reload,
	 * parking on each miss). */
	for (k = 0; ok && k < N_KEYS; k++) {
		snprintf(key, sizeof key, "key-%06d", k);
		snprintf(val, sizeof val, "val-%06d-payload", k);
		if (bt_lookup(g_bt, key, (uint16_t)strlen(key), buf, sizeof buf,
		    &vlen) != XTC_OK) { ok = 0; break; }
		if (vlen != strlen(val) || memcmp(buf, val, vlen) != 0) { ok = 0; break; }
	}

	/* Full ascending scan: count + order check. */
	if (ok) {
		bt_cursor_t *c = NULL;
		const void *ck, *cv; uint16_t cklen, cvlen;
		char prev[32]; int n = 0, have_prev = 0;
		if (bt_cursor_open(g_bt, NULL, 0, &c) == XTC_OK) {
			while (bt_cursor_next(c, &ck, &cklen, &cv, &cvlen) == XTC_OK) {
				if (have_prev && memcmp(prev, ck,
				    cklen < sizeof prev ? cklen : sizeof prev) >= 0) {
					ok = 0; break;
				}
				memcpy(prev, ck, cklen < sizeof prev ? cklen : sizeof prev);
				have_prev = 1;
				n++;
			}
			bt_cursor_close(c);
		}
		if (n != N_KEYS) ok = 0;
	}

	atomic_store(&g_result, ok ? 1 : -1);
	bm_provider_stop(g_bm);
}

int
main(void)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	bm_opts_t bo = BM_OPTS_DEFAULT;
	xtc_pid_t w, pp;
	bm_stats_t bs;
	bt_stats_t ts;
	char path[] = "/tmp/sqlxtc-btloop-XXXXXX";
	int fd;

	fd = mkstemp(path); if (fd < 0) { perror("mkstemp"); return 1; }
	close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = N_FRAMES; bo.cool_pct = 25;
	if (bm_create(&bo, &g_bm) != XTC_OK) { fprintf(stderr, "bm_create\n"); return 1; }
	if (bt_open(g_bm, &g_bt) != XTC_OK) { fprintf(stderr, "bt_open\n"); return 1; }

	atomic_store(&g_result, 0);
	if (xtc_loop_init(&loop) != XTC_OK) return 1;
	if (bm_provider_spawn(g_bm, loop, 1LL * 1000 * 1000, &pp) != XTC_OK) return 1;
	opts.name = "bt-worker";
	if (xtc_proc_spawn(loop, worker_proc, NULL, &opts, &w) != XTC_OK) return 1;
	if (xtc_loop_run(loop) != XTC_OK) { fprintf(stderr, "loop_run\n"); return 1; }
	(void)xtc_loop_fini(loop);

	bm_get_stats(g_bm, &bs);
	bt_get_stats(g_bt, &ts);
	bt_close(g_bt);
	bm_destroy(g_bm);
	unlink(path);
	{ char wal[80]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }

	if (atomic_load(&g_result) != 1) {
		fprintf(stderr, "FAIL: workload result=%d\n", atomic_load(&g_result));
		return 1;
	}
	if (bs.loads == 0 || bs.evicted == 0) {
		fprintf(stderr, "FAIL: tree did not page through eviction "
		    "(loads=%llu evicted=%llu)\n",
		    (unsigned long long)bs.loads, (unsigned long long)bs.evicted);
		return 1;
	}

	printf("  ok   %d-key B-tree built + scanned ON a loop with offloaded "
	    "I/O + live page-provider (height=%llu splits=%llu)\n",
	    N_KEYS, (unsigned long long)ts.height, (unsigned long long)ts.splits);
	printf("  ok   paged through the cooling buffer pool while parking on "
	    "async I/O (loads=%llu evicted=%llu flushed=%llu resident=%llu)\n",
	    (unsigned long long)bs.loads, (unsigned long long)bs.evicted,
	    (unsigned long long)bs.flushed, (unsigned long long)bs.resident);
	printf("All sqlxtc B-tree on-loop tests passed.\n");
	return 0;
}
