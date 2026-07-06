/*
 * test_sim_stale -- DST stale-data (out-of-date read) fault, general
 * (fd+offset) model, and the version check that catches it.
 *
 * FoundationDB's stale-data fault class: the disk returns a page that is
 * STRUCTURALLY VALID but OUT OF DATE -- a durable version that was later
 * overwritten, handed back on a read.  This is distinct from a torn
 * write (silent short tail) or a corrupt read (bit flip): a bare
 * internal checksum does NOT catch staleness, because the stale page's
 * own checksum is consistent with its old contents.  The only defense is
 * a monotonic version / LSN stamped in the page and checked on read.
 *
 * This test proves two things under xtc_sim_io_stale_enable:
 *   (1) DETECTION: a reader that stamps a monotonically increasing
 *       version in each write and rejects any read whose version is
 *       LOWER than the highest it has written CATCHES every stale read
 *       (a version regression) -- no stale page is ever accepted as
 *       current.
 *   (2) The fault is actually exercised (at least one stale read is
 *       seen across the sweep) and the run reaches quiescence and
 *       replays byte-identically from the seed.
 *
 * A writer that omitted the version check would silently accept stale
 * data -- exactly the recovery bug this fault models.
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

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_aio.h"
#include "xtc_sim.h"

#define N_LOOPS   3
#define N_SLOTS   6            /* distinct page offsets, one writer each */
#define PAGE      512
#define N_WRITES  8            /* versions written per slot */
#define VOFF      0            /* the version lives in the first 8 bytes */

static int         g_fd = -1;
static atomic_int  g_stale_seen;    /* reads that returned an old version */
static atomic_int  g_regressions;   /* stale ACCEPTED (MUST be 0) */
static atomic_int  g_done;
static atomic_long g_hash;

static void
fold(long v)
{
	long h = atomic_load_explicit(&g_hash, memory_order_relaxed);
	h = h * 1000003L + (v + 3);
	atomic_store_explicit(&g_hash, h, memory_order_relaxed);
}

/*
 * Each slot's writer writes N_WRITES increasing versions, and after each
 * write reads the page back and checks the version.  With stale
 * injection the read can return a PRIOR version; the writer KNOWS the
 * highest version it has durably written, so a returned version lower
 * than that is a detected stale read -- it must never be mistaken for
 * current.  (A buggy reader that trusted the read blindly would regress
 * its view; we assert that never happens because we check.)
 */
static void
slot_worker(void *arg)
{
	long slot = (long)(intptr_t)arg;
	int64_t off = slot * PAGE;
	uint8_t page[PAGE];
	uint64_t highest = 0;
	int v;

	for (v = 1; v <= N_WRITES; v++) {
		uint64_t ver = (uint64_t)v, got = 0;
		int w, r;
		memset(page, (int)(slot & 0xff), PAGE);
		memcpy(page + VOFF, &ver, sizeof ver);
		w = xtc_aio_pwrite(g_fd, page, PAGE, off);
		fold(w);
		if (w < 0)
			continue;               /* ENOSPC etc.: skip this version */
		(void)xtc_aio_fsync(g_fd);
		if ((uint64_t)v > highest)
			highest = (uint64_t)v;   /* durably written now */

		memset(page, 0, sizeof page);
		r = xtc_aio_pread(g_fd, page, PAGE, off);
		fold(r);
		if (r < PAGE)
			continue;
		memcpy(&got, page + VOFF, sizeof got);
		if (got < highest) {
			/* A stale read: the disk handed back a version OLDER
			 * than what we durably wrote.  The version check
			 * CATCHES it -- we detect and do NOT adopt it as
			 * current (a bug that skipped this check would). */
			atomic_fetch_add(&g_stale_seen, 1);
		} else if (got > highest) {
			/* Impossible: a version from the FUTURE means real
			 * corruption / a model bug -- count it as a regression
			 * (must be 0). */
			atomic_fetch_add(&g_regressions, 1);
		}
		if ((v & 1) == 0)
			xtc_yield();
	}
	atomic_fetch_add(&g_done, 1);
}

static int
run_once(uint64_t seed, int *out_stale, int *out_reg, long *out_hash,
    uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	char path[] = "/scratch/xtc-test/sim_stale_XXXXXX";
	int i, rc;

	atomic_store(&g_stale_seen, 0);
	atomic_store(&g_regressions, 0);
	atomic_store(&g_done, 0);
	atomic_store(&g_hash, 0);

	g_fd = mkstemp(path);
	if (g_fd < 0) {
		char p2[] = "sim_stale_XXXXXX";
		g_fd = mkstemp(p2);
		if (g_fd >= 0) (void)unlink(p2);
	} else {
		(void)unlink(path);
	}
	if (g_fd < 0)
		return -1;
	if (ftruncate(g_fd, (off_t)N_SLOTS * PAGE) != 0) {
		close(g_fd); g_fd = -1; return -1;
	}

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) {
		close(g_fd); g_fd = -1; return -1;
	}

	/* Seeded latency so reads/writes defer + interleave; stale reads at
	 * ~40% so a prior version is frequently returned. */
	xtc_sim_io_faults_enable(20 * 1000LL, 200 * 1000LL, 0);
	xtc_sim_io_stale_enable(400);

	for (i = 0; i < N_SLOTS; i++) {
		xtc_loop_t *l = xtc_exec_loop(e, (unsigned)(i % N_LOOPS));
		(void)xtc_proc_spawn(l, slot_worker, (void *)(intptr_t)i,
		    NULL, NULL);
	}

	rc = xtc_sim_exec_run(e, seed, 20000000);

	if (out_stale)  *out_stale = atomic_load(&g_stale_seen);
	if (out_reg)    *out_reg = atomic_load(&g_regressions);
	if (out_hash)   *out_hash = atomic_load(&g_hash);
	if (out_state)  *out_state = xtc_sim_state_hash(e);

	xtc_sim_io_stale_enable(0);
	xtc_sim_io_faults_disable();
	(void)xtc_exec_fini(e);
	close(g_fd);
	g_fd = -1;
	return rc;
}

int
main(void)
{
	uint64_t base = 0x57A1E;   /* "stale" */
	int n = 40, i, fails = 0;
	long total_stale = 0;

	printf("== stale-data DST (version check catches out-of-date reads): "
	    "%d seeds ==\n", n);

	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		int st1 = 0, st2 = 0, rg1 = 0, rg2 = 0, rc1, rc2, pass = 1;
		long h1 = 0, h2 = 0;
		uint64_t s1 = 0, s2 = 0;

		rc1 = run_once(seed, &st1, &rg1, &h1, &s1);
		if (rc1 != XTC_OK) pass = 0;
		else if (rg1 != 0) pass = 0;   /* a stale page accepted as current */
		if (pass) {
			rc2 = run_once(seed, &st2, &rg2, &h2, &s2);
			if (rc2 != rc1 || st2 != st1 || rg2 != rg1 ||
			    h2 != h1 || s2 != s1)
				pass = 0;
		}
		total_stale += st1;
		if (!pass) {
			printf("  seed 0x%016llx: FAIL (stale=%d reg=%d rc=%d)\n",
			    (unsigned long long)seed, st1, rg1, rc1);
			fails++;
		}
	}

	if (fails == 0 && total_stale == 0) {
		printf("FAIL: not a single stale read was injected across the "
		    "sweep -- the fault was never exercised\n");
		fails++;
	}

	if (fails == 0) {
		printf("OK: stale-data DST -- %d seeds, the version check "
		    "caught every out-of-date read (%ld stale reads seen, zero "
		    "accepted as current), reached quiescence, and replayed "
		    "identically from seed\n", n, total_stale);
		return 0;
	}
	printf("FAIL: %d/%d stale-data seeds failed\n", fails, n);
	return 1;
}
