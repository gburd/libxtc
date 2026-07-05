/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_aiov.c
 *	Deterministic Simulation Testing of the vectored (scatter/gather)
 *	async file I/O -- xtc_aio_preadv / xtc_aio_pwritev -- under the
 *	seeded single-thread simulator with the sim I/O backend.
 *
 *	The sim I/O backend performs the real preadv/pwritev against a real
 *	file at submit time and (under seeded I/O faults) defers the
 *	completion by a seeded latency, so concurrent workers genuinely
 *	park on their vectored ops and the completion ORDER is part of the
 *	replayable schedule.  Each worker gather-writes its own file region
 *	from several iovecs, fdatasyncs, then scatter-reads it back into
 *	differently sized iovecs and verifies the whole region round-trips
 *	byte-for-byte.
 *
 *	Invariants (per seed): (a) every vectored write/read moved all its
 *	bytes and each region round-tripped exactly; (b) the run reaches
 *	clean quiescence (no deferred vectored completion lost its wakeup);
 *	(c) REPLAY: the same seed reproduces the result hash and the
 *	scheduler state hash.
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
#include <sys/uio.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_aio.h"
#include "xtc_sim.h"

#define N_LOOPS   3
#define N_WORKERS 5
#define SEG0 1000
#define SEG1 2000
#define SEG2 1096
#define REGION (SEG0 + SEG1 + SEG2)   /* 4096 per worker */

static int         g_fd;
static atomic_int  g_ok;         /* workers that round-tripped their region */
static atomic_int  g_done;
static atomic_long g_hash;       /* fold of transfer counts (replay check) */

static void
fold(long v)
{
	long h;
	do {
		h = atomic_load_explicit(&g_hash, memory_order_relaxed);
	} while (!atomic_compare_exchange_weak_explicit(&g_hash, &h,
	    h * 1000003L + (v + 11), memory_order_relaxed,
	    memory_order_relaxed));
}

static void
vec_worker(void *arg)
{
	long id = (long)(intptr_t)arg;
	int64_t off = id * REGION;
	unsigned char w[REGION], r[REGION];
	struct iovec wv[3], rv[3];
	int wn, rn, i;

	for (i = 0; i < REGION; i++)
		w[i] = (unsigned char)((id * 31 + i * 7 + 3) & 0xff);

	/* Gather-write from 3 segments. */
	wv[0].iov_base = w;             wv[0].iov_len = SEG0;
	wv[1].iov_base = w + SEG0;      wv[1].iov_len = SEG1;
	wv[2].iov_base = w + SEG0 + SEG1; wv[2].iov_len = SEG2;
	wn = xtc_aio_pwritev(g_fd, wv, 3, off);
	fold(wn);
	(void)xtc_aio_fdatasync(g_fd);

	/* Scatter-read back into DIFFERENTLY sized segments. */
	memset(r, 0, sizeof r);
	rv[0].iov_base = r;             rv[0].iov_len = 2048;
	rv[1].iov_base = r + 2048;      rv[1].iov_len = 1024;
	rv[2].iov_base = r + 3072;      rv[2].iov_len = REGION - 3072;
	rn = xtc_aio_preadv(g_fd, rv, 3, off);
	fold(rn);

	if (wn == REGION && rn == REGION && memcmp(w, r, REGION) == 0)
		atomic_fetch_add(&g_ok, 1);
	atomic_fetch_add(&g_done, 1);
}

static void
coordinator(void *arg)
{
	xtc_exec_t *e = arg;
	int tries;
	for (tries = 0; tries < 8000; tries++) {
		if (atomic_load(&g_done) >= N_WORKERS)
			break;
		(void)xtc_proc_sleep(1000 * 1000LL);
	}
	(void)xtc_proc_sleep(2 * 1000 * 1000LL);
	(void)xtc_exec_stop(e);
}

static int
run_one(uint64_t seed, int *out_ok, long *out_hash, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	char path[] = "/scratch/xtc-test/sim_aiov_XXXXXX";
	int i, rc;

	atomic_store(&g_ok, 0);
	atomic_store(&g_done, 0);
	atomic_store(&g_hash, 0);

	g_fd = mkstemp(path);
	if (g_fd < 0) {
		char p2[] = "sim_aiov_XXXXXX";
		g_fd = mkstemp(p2);
		if (g_fd < 0)
			return -1;
		(void)unlink(p2);
	} else {
		(void)unlink(path);
	}

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) { close(g_fd); return -1; }
	xtc_exec_set_service_mode(e, 1);

	/* Seeded vectored-completion latency so concurrent ops interleave
	 * and the completion order joins the replayable schedule (no
	 * injected errors -- a clean round-trip workload). */
	xtc_sim_io_faults_enable(50 * 1000LL, 500 * 1000LL, 0);

	for (i = 0; i < N_WORKERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, i % N_LOOPS), vec_worker,
		    (void *)(intptr_t)i, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), coordinator, e, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 20000000);

	if (out_ok)    *out_ok = atomic_load(&g_ok);
	if (out_hash)  *out_hash = atomic_load(&g_hash);
	if (out_state) *out_state = xtc_sim_state_hash(e);

	xtc_sim_io_faults_disable();
	(void)xtc_exec_fini(e);
	close(g_fd);
	return rc;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0x61696f76; /* "aiov" */
	int n = 16, i, fails = 0;

	if (argc > 1) base = strtoull(argv[1], NULL, 0);
	if (argc > 2) n = atoi(argv[2]);

	printf("== vectored-AIO DST: %d seeds from base 0x%llx ==\n", n,
	    (unsigned long long)base);

	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		int ok = 0, ok2 = 0, rc, rc2, pass = 1;
		long h = 0, h2 = 0;
		uint64_t st = 0, st2 = 0;

		rc = run_one(seed, &ok, &h, &st);
		if (rc != XTC_OK) pass = 0;
		else if (ok != N_WORKERS) pass = 0;   /* every region round-tripped */

		if (pass) {
			rc2 = run_one(seed, &ok2, &h2, &st2);
			if (rc2 != rc || ok2 != ok || h2 != h || st2 != st)
				pass = 0;
		}

		if (!pass) {
			printf("  seed 0x%016llx: FAIL (ok=%d/%d rc=%d)\n",
			    (unsigned long long)seed, ok, N_WORKERS, rc);
			fails++;
		}
	}

	if (fails == 0) {
		printf("OK: vectored-AIO DST -- %d seeds, scatter/gather "
		    "round-trip under seeded completion order, all replay\n", n);
		return 0;
	}
	printf("FAIL: %d/%d vectored-AIO seeds failed\n", fails, n);
	return 1;
}
