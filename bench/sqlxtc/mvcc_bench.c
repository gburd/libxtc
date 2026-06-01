/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * bench/sqlxtc/mvcc_bench.c
 *	A YCSB-shaped load generator for the sqlxtc MVCC engine
 *	(examples/06_sqlxtc/mvcc.c), measuring throughput and latency
 *	percentiles for the libxtc concurrency model directly.
 *
 *	N shard servers (one per loop = one core under an executor) plus
 *	the 2PC coordinator; C client procs spread across the loops each
 *	issue OPS operations against a KEYSPACE-wide key range with a
 *	configurable read/write mix.  A write is a single-key MVCC
 *	transaction (begin + commit); a read is a snapshot read.  Each
 *	op is timed; the run reports ops/sec and p50/p95/p99/p999/max
 *	latency as one JSON line for the harness to aggregate.
 *
 *	This measures the ENGINE and the runtime model, not a SQL TPC-C
 *	(the SQL layer is not yet on this engine -- see README.md).  It
 *	is an honest proxy for "predictable, fast, low p99 variability"
 *	on an OLTP-ish workload.
 *
 * usage: mvcc_bench [--cores N] [--clients C] [--ops O] [--keyspace K]
 *                   [--read-pct P] [--label TAG]
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mvcc.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"

static int      g_cores    = 4;
static int      g_clients  = 16;
static int      g_ops      = 20000;     /* per client */
static uint32_t g_keyspace = 1000000;
static int      g_read_pct = 80;
static const char *g_label = "mvcc";

static _Atomic int g_left;
static uint32_t   *g_lat_ns;            /* C*OPS latency samples (ns) */
static xtc_loop_t **g_cloops;

static uint64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void
client_proc(void *arg)
{
	long id = (long)arg;
	uint64_t rng = (uint64_t)id * 0x9E3779B97F4A7C15ull + 1;
	uint32_t *lat = &g_lat_ns[(size_t)id * g_ops];
	int i;

	for (i = 0; i < g_ops; i++) {
		uint64_t t0, t1;
		uint32_t key;
		int is_read;

		rng = rng * 6364136223846793005ull + 1442695040888963407ull;
		key = (uint32_t)((rng >> 33) % g_keyspace);
		is_read = ((int)((rng >> 17) % 100) < g_read_pct);

		t0 = now_ns();
		if (is_read) {
			uint32_t v;
			uint64_t snap = mvcc_begin();
			(void)mvcc_read(key, snap, &v);
			mvcc_snapshot_release(snap);
		} else {
			mvcc_write_t w;
			uint64_t snap = mvcc_begin();
			w.key = key; w.value = (uint32_t)(rng & 0xFFFFFFFF);
			(void)mvcc_commit(snap, &w, 1, NULL);
			mvcc_snapshot_release(snap);
		}
		t1 = now_ns();
		lat[i] = (t1 - t0) > 0xFFFFFFFFull ? 0xFFFFFFFFu
		                                   : (uint32_t)(t1 - t0);
	}
	if (atomic_fetch_sub(&g_left, 1) == 1)
		mvcc_stop();
}

static int
cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return (x > y) - (x < y);
}

int
main(int argc, char **argv)
{
	xtc_exec_t *exec = NULL;
	xtc_loop_t *sl[MVCC_MAX_SHARDS];
	size_t n_samples, i;
	uint64_t wall0, wall1;
	double secs, kops;
	int c;

	for (c = 1; c < argc; c++) {
		if (!strcmp(argv[c], "--cores") && c + 1 < argc) g_cores = atoi(argv[++c]);
		else if (!strcmp(argv[c], "--clients") && c + 1 < argc) g_clients = atoi(argv[++c]);
		else if (!strcmp(argv[c], "--ops") && c + 1 < argc) g_ops = atoi(argv[++c]);
		else if (!strcmp(argv[c], "--keyspace") && c + 1 < argc) g_keyspace = (uint32_t)strtoul(argv[++c], NULL, 10);
		else if (!strcmp(argv[c], "--read-pct") && c + 1 < argc) g_read_pct = atoi(argv[++c]);
		else if (!strcmp(argv[c], "--label") && c + 1 < argc) g_label = argv[++c];
	}
	if (g_cores < 1) g_cores = 1;
	if (g_cores > MVCC_MAX_SHARDS) g_cores = MVCC_MAX_SHARDS;
	if (g_keyspace < 1) g_keyspace = 1;

	n_samples = (size_t)g_clients * g_ops;
	g_lat_ns = calloc(n_samples, sizeof *g_lat_ns);
	if (g_lat_ns == NULL) { fprintf(stderr, "oom\n"); return 1; }

	if (xtc_exec_init(&exec, g_cores) != XTC_OK) { fprintf(stderr, "exec\n"); return 1; }
	for (c = 0; c < g_cores; c++) sl[c] = xtc_exec_loop(exec, c);
	if (mvcc_start(sl, g_cores, xtc_exec_loop(exec, 0)) != XTC_OK) { fprintf(stderr, "mvcc\n"); return 1; }

	g_cloops = sl;
	atomic_store(&g_left, g_clients);
	wall0 = now_ns();
	for (c = 0; c < g_clients; c++) {
		xtc_proc_opts_t po = { .name = "bench" };
		xtc_pid_t pid;
		if (xtc_proc_spawn(xtc_exec_loop(exec, c % g_cores), client_proc,
		    (void *)(long)c, &po, &pid) != XTC_OK) { fprintf(stderr, "spawn\n"); return 1; }
	}
	if (xtc_exec_run(exec) != XTC_OK) { fprintf(stderr, "run\n"); return 1; }
	wall1 = now_ns();
	mvcc_fini();
	(void)xtc_exec_fini(exec);

	secs = (double)(wall1 - wall0) / 1e9;
	kops = (double)n_samples / secs / 1000.0;

	qsort(g_lat_ns, n_samples, sizeof *g_lat_ns, cmp_u32);
#define PCT(p) ((double)g_lat_ns[(size_t)((double)(p) / 100.0 * (double)(n_samples - 1))] / 1000.0)
	printf("{\"label\":\"%s\",\"cores\":%d,\"clients\":%d,\"ops_per_client\":%d,"
	    "\"keyspace\":%u,\"read_pct\":%d,\"total_ops\":%zu,\"secs\":%.4f,"
	    "\"kops_per_sec\":%.1f,\"p50_us\":%.2f,\"p95_us\":%.2f,\"p99_us\":%.2f,"
	    "\"p999_us\":%.2f,\"max_us\":%.2f}\n",
	    g_label, g_cores, g_clients, g_ops, g_keyspace, g_read_pct,
	    n_samples, secs, kops, PCT(50), PCT(95), PCT(99), PCT(99.9),
	    (double)g_lat_ns[n_samples - 1] / 1000.0);
#undef PCT
	free(g_lat_ns);
	return 0;
}
