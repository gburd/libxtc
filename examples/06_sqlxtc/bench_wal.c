/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/bench_wal.c
 *	Group-commit microbenchmark for wal.c (not part of make test).
 *
 *	C committers append LEN-byte records for SECS seconds, each waiting
 *	for its commit to be durable, as plain threads (the in-process
 *	binding's client threads) or as procs on an L-loop executor (the
 *	server).  Prints one CSV line:
 *	  mode,C,len,commits_per_s,fsyncs_per_s,group,p50_us,p99_us
 *
 *	usage: bench_wal thread|fiber C LEN SECS [LOOPS] [replay]
 *	  replay: then time wal_scan of the log it wrote (s, MB/s)
 *	       bench_wal scan PATH     time wal_scan of an existing log
 *	  (scan,records,bytes,seconds,MB/s,cpu_seconds)
 *	       bench_wal copy SRC DST  re-frame SRC's records into a new log
 *	  DST with this build's checksum (wal_commit_sync, one at a time)
 */

#include <pthread.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wal.h"
#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "t_tmp.h"

#define MAXC   256
#define NLAT   (1u << 20)

static wal_t           *g_wal;
static int              g_len;
static int64_t          g_end;
static _Atomic int      g_bfail;
static _Atomic int      g_live;
static uint32_t        *g_lat[MAXC];
static long             g_n[MAXC];

static void
commit_loop(int id)
{
	uint8_t *rec = xtc_malloc((size_t)g_len > 0 ? (size_t)g_len : 1);
	long n = 0;
	if (rec == NULL) {
		atomic_store(&g_bfail, 1);
		return;
	}
	memset(rec, id & 0xff, (size_t)g_len);
	while (xtc_clock_mono() < g_end) {
		uint64_t lsn;
		int64_t t0 = xtc_clock_mono();
		if (wal_commit(g_wal, rec, (uint32_t)g_len, &lsn) != XTC_OK) {
			atomic_store(&g_bfail, 1);
			break;
		}
		if (n < (long)NLAT)
			g_lat[id][n] = (uint32_t)((xtc_clock_mono() - t0) / 1000);
		n++;
	}
	g_n[id] = n;
	xtc_free(rec);
}

static void *
thread_main(void *arg)
{
	commit_loop((int)(intptr_t)arg);
	return NULL;
}

static void
proc_main(void *arg)
{
	commit_loop((int)(intptr_t)arg);
	atomic_fetch_sub(&g_live, 1);
}

static int
cmpu(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : x > y;
}

static int
count_cb(uint64_t lsn, const void *rec, uint32_t len, void *user)
{
	(void)lsn; (void)rec; (void)len;
	(*(long *)user)++;
	return 0;
}

static int
copy_cb(uint64_t lsn, const void *rec, uint32_t len, void *user)
{
	uint64_t l;
	(void)lsn;
	return wal_commit_sync(user, rec, len, &l) != XTC_OK;
}

int
main(int argc, char **argv)
{
	char path[256];
	wal_opts_t wo;
	wal_stats_t st;
	pthread_t th[MAXC];
	xtc_exec_t *ex = NULL;
	uint32_t *all;
	long tot = 0, k = 0, nl;
	int c, i, fd, loops, fiber, secs;
	double el;
	int64_t t0;

	if (argc == 4 && strcmp(argv[1], "copy") == 0) {
		memset(&wo, 0, sizeof wo);
		wo.path = argv[3];
		if (wal_open(&wo, &g_wal) != XTC_OK)
			return 1;
		if (wal_scan(argv[2], copy_cb, g_wal) != XTC_OK)
			return 1;
		wal_get_stats(g_wal, &st);
		printf("copy,%llu,%llu\n", (unsigned long long)st.commits,
		    (unsigned long long)wal_size(g_wal));
		wal_close(g_wal);
		return 0;
	}
	if (argc == 3 && strcmp(argv[1], "scan") == 0) {
		struct rusage ru0, ru1;
		struct stat sb;
		long nrec = 0;
		int64_t r0;
		double cpu;
		if (stat(argv[2], &sb) != 0)
			return 1;
		(void)getrusage(RUSAGE_SELF, &ru0);
		r0 = xtc_clock_mono();
		if (wal_scan(argv[2], count_cb, &nrec) != XTC_OK)
			return 1;
		el = (double)(xtc_clock_mono() - r0) / 1e9;
		(void)getrusage(RUSAGE_SELF, &ru1);
		cpu = (double)(ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec +
		    ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) +
		    (double)(ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec +
		    ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec) / 1e6;
		printf("scan,%ld,%lld,%.3f,%.0f,%.3f\n", nrec, (long long)sb.st_size,
		    el, (double)sb.st_size / 1e6 / el, cpu);
		return 0;
	}
	if (argc < 5) {
		fprintf(stderr, "usage: bench_wal thread|fiber C LEN SECS [LOOPS] [replay]\n");
		return 2;
	}
	fiber = strcmp(argv[1], "fiber") == 0;
	c = atoi(argv[2]);
	g_len = atoi(argv[3]);
	secs = atoi(argv[4]);
	loops = argc > 5 ? atoi(argv[5]) : 4;
	if (c < 1 || c > MAXC || g_len < 0 || secs < 1 || loops < 1)
		return 2;
	t_tmpl(path, sizeof path, "sqlxtc-benchwal");
	if ((fd = mkstemp(path)) < 0)
		return 1;
	close(fd);
	memset(&wo, 0, sizeof wo);
	wo.path = path;
	if (wal_open(&wo, &g_wal) != XTC_OK)
		return 1;
	for (i = 0; i < c; i++)
		if ((g_lat[i] = calloc(NLAT, sizeof(uint32_t))) == NULL)
			return 1;
	t0 = xtc_clock_mono();
	g_end = t0 + (int64_t)secs * 1000000000LL;
	if (fiber) {
		if (xtc_exec_init(&ex, loops) != XTC_OK)
			return 1;
		atomic_store(&g_live, c);
		for (i = 0; i < c; i++) {
			xtc_proc_opts_t po = { .name = "committer" };
			xtc_pid_t p;
			if (xtc_proc_spawn(xtc_exec_loop(ex, i % loops), proc_main,
			    (void *)(intptr_t)i, &po, &p) != XTC_OK)
				return 1;
		}
		(void)xtc_exec_run(ex);
		(void)xtc_exec_fini(ex);
	} else {
		for (i = 0; i < c; i++)
			if (pthread_create(&th[i], NULL, thread_main, (void *)(intptr_t)i))
				return 1;
		for (i = 0; i < c; i++)
			(void)pthread_join(th[i], NULL);
	}
	el = (double)(xtc_clock_mono() - t0) / 1e9;
	wal_get_stats(g_wal, &st);
	for (i = 0; i < c; i++)
		tot += g_n[i] < (long)NLAT ? g_n[i] : (long)NLAT;
	all = malloc(sizeof(uint32_t) * (size_t)(tot ? tot : 1));
	for (i = 0; i < c; i++) {
		nl = g_n[i] < (long)NLAT ? g_n[i] : (long)NLAT;
		memcpy(all + k, g_lat[i], sizeof(uint32_t) * (size_t)nl);
		k += nl;
	}
	qsort(all, (size_t)k, sizeof(uint32_t), cmpu);
	printf("%s,%d,%d,%.0f,%.0f,%.2f,%u,%u\n", fiber ? "fiber" : "thread", c,
	    g_len, (double)st.commits / el, (double)st.batches / el,
	    st.batches ? (double)st.commits / (double)st.batches : 0.0,
	    k ? all[k / 2] : 0, k ? all[k * 99 / 100] : 0);
	wal_close(g_wal);
	if (argc > 6 && strcmp(argv[6], "replay") == 0) {
		long nrec = 0;
		int64_t r0 = xtc_clock_mono();
		(void)wal_scan(path, count_cb, &nrec);
		el = (double)(xtc_clock_mono() - r0) / 1e9;
		printf("replay,%ld,%.3f\n", nrec, el);
	}
	unlink(path);
	return atomic_load(&g_bfail) ? 1 : 0;
}
