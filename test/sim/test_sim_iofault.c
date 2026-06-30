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
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_aio.h"
#include "xtc_sim.h"

/*
 * DST coverage of SIMULATED I/O FAULTS -- the FoundationDB-style seam
 * where file-AIO completions are DEFERRED by a seeded latency (so the
 * completion ORDER across concurrent ops is part of the replayable
 * schedule) and may carry a seeded fault (a short transfer or an EIO).
 *
 * Several fibers, across loops, each do a write+fsync+read sequence on
 * a private region of a shared temp file, with I/O faults ENABLED.  The
 * fibers genuinely park on each AIO (deferred completion), so the
 * deterministic scheduler interleaves them, and the seeded latency
 * decides which completes first.  We fold each observed completion
 * result into an order-sensitive hash.  The run must:
 *   - reach quiescence (every fiber finishes its sequence; no hang from
 *     a lost AIO wakeup);
 *   - REPLAY: the same seed yields the identical completion-result hash
 *     AND state hash (the deferred-completion order + the fault pattern
 *     are both seed-determined and stable);
 *   - actually exercise faults: at least one short/failed op is seen
 *     (the fault stream fires) -- proving the injection is live.
 *
 * Because faults draw from the dedicated IO stream, this also implies
 * the scheduling streams are unperturbed (replay holds with faults on).
 */

#define N_LOOPS 4
#define N_WORKERS 12
#define REGION 4096

static atomic_int  g_done;
static atomic_long g_result_hash;   /* order-sensitive fold of AIO results */
static atomic_int  g_faults_seen;   /* short/failed ops observed */
static int         g_fd = -1;

static void
fold(long v)
{
	long h = atomic_load_explicit(&g_result_hash, memory_order_relaxed);
	h = h * 1000003L + (v + 7);
	atomic_store_explicit(&g_result_hash, h, memory_order_relaxed);
}

static void
io_worker(void *arg)
{
	long id = (long)(intptr_t)arg;
	int64_t off = id * REGION;
	uint8_t buf[REGION];
	int w, r;

	memset(buf, (int)(id & 0xff), sizeof buf);

	/* Write our region (may be reported short under fault injection). */
	w = xtc_aio_pwrite(g_fd, buf, REGION, off);
	fold(w);
	if (w >= 0 && w < REGION)
		atomic_fetch_add_explicit(&g_faults_seen, 1, memory_order_relaxed);

	/* Durability barrier (may be reported as EIO under fault). */
	{
		int fr = xtc_aio_fsync(g_fd);
		fold(fr);
		if (fr < 0)
			atomic_fetch_add_explicit(&g_faults_seen, 1,
			    memory_order_relaxed);
	}

	/* Read it back (may be reported short). */
	memset(buf, 0, sizeof buf);
	r = xtc_aio_pread(g_fd, buf, REGION, off);
	fold(r);
	if (r >= 0 && r < REGION)
		atomic_fetch_add_explicit(&g_faults_seen, 1, memory_order_relaxed);

	atomic_fetch_add_explicit(&g_done, 1, memory_order_relaxed);
}

static int
run_once(uint64_t seed, int *out_done, long *out_rhash, int *out_faults,
    uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	char path[] = "/scratch/xtc-test/sim_iofault_XXXXXX";
	int i, rc;

	atomic_store(&g_done, 0);
	atomic_store(&g_result_hash, 0);
	atomic_store(&g_faults_seen, 0);

	g_fd = mkstemp(path);
	if (g_fd < 0) {
		/* Fall back to a relative path if /scratch is absent. */
		char p2[] = "sim_iofault_XXXXXX";
		g_fd = mkstemp(p2);
		if (g_fd < 0)
			return -1;
		(void)unlink(p2);
	} else {
		(void)unlink(path);
	}
	(void)ftruncate(g_fd, (off_t)N_WORKERS * REGION);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) { close(g_fd); return -1; }

	/* Enable I/O faults: 100us-2ms seeded latency, ~12% op fault rate. */
	xtc_sim_io_faults_enable(100 * 1000LL, 2 * 1000 * 1000LL, 120);

	for (i = 0; i < N_WORKERS; i++) {
		xtc_loop_t *l = xtc_exec_loop(e, (unsigned)(i % N_LOOPS));
		(void)xtc_proc_spawn(l, io_worker, (void *)(intptr_t)i, NULL, NULL);
	}

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_done = atomic_load(&g_done);
	*out_rhash = atomic_load(&g_result_hash);
	*out_faults = atomic_load(&g_faults_seen);
	if (out_state) *out_state = xtc_sim_state_hash(e);

	xtc_sim_io_faults_disable();
	(void)xtc_exec_fini(e);
	close(g_fd);
	g_fd = -1;
	return rc;
}

int
main(void)
{
	int d1 = 0, d2 = 0, f1 = 0, f2 = 0, rc;
	long h1 = 0, h2 = 0;
	uint64_t s1 = 0, s2 = 0;

	rc = run_once(0x10FA, &d1, &h1, &f1, &s1);
	if (rc != XTC_OK) {
		printf("FAIL: io-fault run did not quiesce (rc=%d) -- "
		    "a deferred AIO completion may have lost its wakeup\n", rc);
		return 1;
	}
	(void)run_once(0x10FA, &d2, &h2, &f2, &s2);

	printf("run1: done=%d result_hash=%ld faults_seen=%d state=%016llx\n",
	    d1, h1, f1, (unsigned long long)s1);
	printf("run2: done=%d result_hash=%ld faults_seen=%d state=%016llx\n",
	    d2, h2, f2, (unsigned long long)s2);

	if (d1 != N_WORKERS || d2 != N_WORKERS) {
		printf("FAIL: not all workers finished (%d/%d, want %d)\n",
		    d1, d2, N_WORKERS);
		return 1;
	}
	if (f1 == 0) {
		printf("FAIL: no I/O faults were injected -- the IO fault "
		    "stream never fired (expected ~12%% of %d ops)\n",
		    N_WORKERS * 3);
		return 1;
	}
	if (h1 != h2 || f1 != f2 || s1 != s2) {
		printf("FAIL: io-fault run did not replay "
		    "(rhash %ld/%ld faults %d/%d state %016llx/%016llx)\n",
		    h1, h2, f1, f2,
		    (unsigned long long)s1, (unsigned long long)s2);
		return 1;
	}

	printf("OK: simulated I/O faults under DST -- %d workers x 3 deferred "
	    "AIO ops, %d seeded faults injected, completion order + fault "
	    "pattern replay identically from seed\n", N_WORKERS, f1);
	return 0;
}
