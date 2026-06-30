#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sync.h"
#include "xtc_sim.h"

/*
 * DST coverage of the fiber-yielding latches (xtc_amutex / xtc_arwlock)
 * -- the data-plane primitives the threaded-PostgreSQL goal rests on.
 * Under contention these PARK the fiber (xtc_yield to the loop) and a
 * waker re-grants the latch when the holder unlocks, so the seeded
 * scheduler exercises exactly the lost-wakeup / double-grant / unfair-
 * grant interleavings these bugs hide in.
 *
 * Two workloads, both across N loops under the deterministic scheduler:
 *
 *   amutex: N procs each take a shared amutex, increment a plain
 *           (non-atomic) counter inside the critical section, unlock.
 *           Mutual exclusion is the invariant: the final count must be
 *           exactly N.  A double-grant (two holders at once) or a lost
 *           update would show as count < N.
 *
 *   arwlock: a mix of readers (which may overlap) and writers (which
 *            must be exclusive); each writer bumps a version under the
 *            write lock and each reader checks the value it reads is
 *            stable across its read section.  Exclusivity + no torn
 *            read is the invariant.
 *
 * Both must reach quiescence (no deadlock/lost-wakeup hang) and replay
 * identically from a seed.
 */

#define N_LOOPS   4
#define N_MUWORK  24      /* amutex contenders */
#define N_RDWORK  16      /* arwlock readers */
#define N_WRWORK  6       /* arwlock writers */

/* ---- amutex mutual-exclusion workload ---- */
static xtc_amutex_t *g_mu;
static long          g_mu_count;   /* deliberately non-atomic: the lock
				    * is what protects it.  A double-grant
				    * corrupts it. */
static atomic_int    g_mu_done;

static void
mu_worker(void *arg)
{
	(void)arg;
	if (xtc_amutex_lock(g_mu, -1) == XTC_OK) {
		/* Critical section: read-modify-write a plain long.  A yield
		 * inside the section would let a second holder interleave if
		 * exclusion were broken -- but we must NOT yield here (the
		 * test of exclusion is that no two fibers are ever both
		 * past the lock); the scheduler already interleaves at the
		 * lock/unlock boundaries. */
		long v = g_mu_count;
		g_mu_count = v + 1;
		(void)xtc_amutex_unlock(g_mu);
	}
	atomic_fetch_add_explicit(&g_mu_done, 1, memory_order_relaxed);
}

static int
run_amutex(uint64_t seed, long *out_count, int *out_done, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc;
	g_mu_count = 0;
	atomic_store(&g_mu_done, 0);
	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	if (xtc_amutex_create(&g_mu) != XTC_OK) { xtc_exec_fini(e); return -1; }
	for (i = 0; i < N_MUWORK; i++) {
		xtc_loop_t *l = xtc_exec_loop(e, (unsigned)(i % N_LOOPS));
		(void)xtc_proc_spawn(l, mu_worker, NULL, NULL, NULL);
	}
	rc = xtc_sim_exec_run(e, seed, 5000000);
	*out_count = g_mu_count;
	*out_done = atomic_load(&g_mu_done);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	xtc_amutex_destroy(g_mu);
	g_mu = NULL;
	(void)xtc_exec_fini(e);
	return rc;
}

/* ---- arwlock exclusivity workload ---- */
static xtc_arwlock_t *g_rw;
static long           g_rw_version;     /* protected by the write lock */
static atomic_int     g_rw_done;
static atomic_int     g_rw_torn;        /* set if a reader saw an unstable value */

static void
rw_reader(void *arg)
{
	(void)arg;
	if (xtc_arwlock_rdlock(g_rw, -1) == XTC_OK) {
		long a = g_rw_version;
		/* Under a correct rwlock no writer can change the version
		 * while we hold the read lock.  We do not yield inside the
		 * read section, so even a single observation suffices to
		 * assert: the value is whatever the last committed writer
		 * set, never a half-written state. */
		long b = g_rw_version;
		if (a != b)
			atomic_store_explicit(&g_rw_torn, 1,
			    memory_order_relaxed);
		(void)xtc_arwlock_unlock(g_rw);
	}
	atomic_fetch_add_explicit(&g_rw_done, 1, memory_order_relaxed);
}

static void
rw_writer(void *arg)
{
	(void)arg;
	if (xtc_arwlock_wrlock(g_rw, -1) == XTC_OK) {
		g_rw_version += 1;
		(void)xtc_arwlock_unlock(g_rw);
	}
	atomic_fetch_add_explicit(&g_rw_done, 1, memory_order_relaxed);
}

static int
run_arwlock(uint64_t seed, long *out_ver, int *out_done, int *out_torn,
    uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc;
	g_rw_version = 0;
	atomic_store(&g_rw_done, 0);
	atomic_store(&g_rw_torn, 0);
	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	if (xtc_arwlock_create(&g_rw) != XTC_OK) { xtc_exec_fini(e); return -1; }
	for (i = 0; i < N_RDWORK; i++) {
		xtc_loop_t *l = xtc_exec_loop(e, (unsigned)(i % N_LOOPS));
		(void)xtc_proc_spawn(l, rw_reader, NULL, NULL, NULL);
	}
	for (i = 0; i < N_WRWORK; i++) {
		xtc_loop_t *l = xtc_exec_loop(e, (unsigned)((i + 1) % N_LOOPS));
		(void)xtc_proc_spawn(l, rw_writer, NULL, NULL, NULL);
	}
	rc = xtc_sim_exec_run(e, seed, 5000000);
	*out_ver = g_rw_version;
	*out_done = atomic_load(&g_rw_done);
	*out_torn = atomic_load(&g_rw_torn);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	xtc_arwlock_destroy(g_rw);
	g_rw = NULL;
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(void)
{
	long c1 = 0, c2 = 0, v1 = 0, v2 = 0;
	int d1 = 0, d2 = 0, t1 = 0, t2 = 0, rc;
	uint64_t s1 = 0, s2 = 0;

	/* --- amutex: mutual exclusion + replay --- */
	rc = run_amutex(0xA404, &c1, &d1, &s1);
	if (rc != XTC_OK) { printf("FAIL: amutex run rc=%d (hang/deadlock?)\n", rc); return 1; }
	(void)run_amutex(0xA404, &c2, &d2, &s2);
	printf("amutex: count=%ld (want %d) done=%d state=%016llx\n",
	    c1, N_MUWORK, d1, (unsigned long long)s1);
	if (d1 != N_MUWORK || c1 != N_MUWORK) {
		printf("FAIL: amutex lost an update -- exclusion broken "
		    "(count=%ld want %d)\n", c1, N_MUWORK);
		return 1;
	}
	if (c1 != c2 || s1 != s2) {
		printf("FAIL: amutex run did not replay (count %ld/%ld "
		    "state %016llx/%016llx)\n", c1, c2,
		    (unsigned long long)s1, (unsigned long long)s2);
		return 1;
	}

	/* --- arwlock: exclusivity + no torn read + replay --- */
	rc = run_arwlock(0x4717, &v1, &d1, &t1, &s1);
	if (rc != XTC_OK) { printf("FAIL: arwlock run rc=%d (hang/deadlock?)\n", rc); return 1; }
	(void)run_arwlock(0x4717, &v2, &d2, &t2, &s2);
	printf("arwlock: version=%ld (want %d) done=%d torn=%d state=%016llx\n",
	    v1, N_WRWORK, d1, t1, (unsigned long long)s1);
	if (d1 != N_RDWORK + N_WRWORK) {
		printf("FAIL: arwlock did not complete all workers\n");
		return 1;
	}
	if (t1 != 0) {
		printf("FAIL: a reader saw a torn value -- writer ran during "
		    "a read section (exclusivity broken)\n");
		return 1;
	}
	if (v1 != N_WRWORK) {
		printf("FAIL: arwlock lost a write (version=%ld want %d)\n",
		    v1, N_WRWORK);
		return 1;
	}
	if (v1 != v2 || t1 != t2 || s1 != s2) {
		printf("FAIL: arwlock run did not replay\n");
		return 1;
	}

	printf("OK: amutex mutual exclusion (count=%ld) + arwlock "
	    "exclusivity (version=%ld, no torn reads) hold under the "
	    "deterministic scheduler and replay from seed\n", c1, v1);
	return 0;
}
