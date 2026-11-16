/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/22_dst_lease.c -- the shape of a consumer's DST
 * harness, from examples/20_jobq, cut down to one lease.
 *
 * A lease server on loop 0 grants a lease with a fencing token that grows
 * with every grant.  Two workers on loops 1 and 2 take turns: each asks
 * for the lease, may stall past it at a buggify point, and writes to a
 * store that refuses a token below the highest it has accepted.  The
 * invariant: the store never takes a write under an older lease after one
 * under a newer lease.  Each seed runs twice, and the trace hashes must
 * match.  Built with -DXTC_DST_INJECT_BUG=1 the store skips the fence and
 * the sweep must fail.
 *
 * Off a sim build, xtc_sim_exec_run returns XTC_E_NOSYS and the program
 * says so and exits 0, so it builds and runs against any libxtc.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

#define MS (1000LL * 1000)
#define LEASE_NS (5 * MS)
#define ROUNDS 6

struct msg {
	xtc_pid_t from;
	uint64_t token;
	int64_t until;
};

static xtc_pid_t server_pid;
static uint64_t fence, last, trace;
static int writes, stale, alive_workers;

/* Fold one event into the trace: what happened, and when. */
static void
note(uint64_t what)
{
	trace = (trace ^ what) * 1099511628211ULL;
	trace = (trace ^ (uint64_t)xtc_clock_mono()) * 1099511628211ULL;
}

/* !region store */
/* The store: accept a write iff its token is not below the fence.  The
 * check the harness makes is on `last`, what actually landed. */
static void
store_write(uint64_t token)
{
#if !XTC_DST_BUG(1)
	if (token < fence) {
		note(1);
		return; /* fenced: a stale holder's write */
	}
#endif
	fence = token;
	if (token < last)
		stale++; /* the invariant broke */
	last = token;
	writes++;
	note(token);
}
/* !endregion store */

/* The lease server: a new token for each request, valid LEASE_NS. */
static void
server(void *arg)
{
	struct msg m;
	void *buf;
	size_t n;
	uint64_t next = 0;

	(void)arg;
	while (alive_workers > 0) {
		if (xtc_recv(&buf, &n, 20 * MS) != XTC_OK)
			continue;
		memcpy(&m, buf, sizeof m);
		xtc_free(buf);
		m.token = ++next;
		m.until = xtc_clock_mono() + LEASE_NS;
		(void)xtc_send(m.from, &m, sizeof m);
	}
}

/* !region worker */
static void
worker(void *arg)
{
	struct msg m;
	void *buf;
	size_t n;
	int i;

	(void)arg;
	for (i = 0; i < ROUNDS; i++) {
		m.from = xtc_self();
		if (xtc_send(server_pid, &m, sizeof m) != XTC_OK ||
		    xtc_recv(&buf, &n, 20 * MS) != XTC_OK)
			continue;
		memcpy(&m, buf, sizeof m);
		xtc_free(buf);
		(void)xtc_proc_sleep((int64_t)xtc_sim_rand(2 * MS));
		if (xtc_clock_mono() >= m.until)
			continue; /* the lease ran out: do not write */
		/* Valid a moment ago.  A pause here can outlive the lease. */
		if (XTC_SIM_BUGGIFY("worker.pause") &&
		    xtc_sim_buggify_fault(500))
			(void)xtc_proc_sleep(2 * LEASE_NS);
		store_write(m.token);
	}
	alive_workers--;
}
/* !endregion worker */

/* !region harness */
/* One run of one seed: the invariant's verdict and the run's trace. */
static int
run(uint64_t seed, uint64_t *out_trace)
{
	xtc_exec_t *e = NULL;
	int rc;

	fence = last = trace = 0;
	writes = stale = 0;
	alive_workers = 2;
	if (xtc_exec_create(&(xtc_exec_opts_t){
	    .size = sizeof(xtc_exec_opts_t), .n_loops = 3 }, &e) != XTC_OK)
		return XTC_E_NOMEM;
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), server, NULL, NULL,
	    &server_pid);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 1), worker, NULL, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 2), worker, NULL, NULL, NULL);
	xtc_sim_net_latency(MS / 10, 3 * MS);
	xtc_sim_buggify_enable(500);
	rc = xtc_sim_exec_run(e, seed, 1000000);
	xtc_sim_buggify_disable();
	xtc_sim_partition_clear(); /* also clears the latency window */
	xtc_exec_destroy(e);
	*out_trace = trace;
	if (rc == XTC_OK && stale > 0)
		rc = XTC_E_INTERNAL;
	return rc;
}

int
main(void)
{
	uint64_t seed, t1, t2;
	int rc;

	for (seed = 1; seed <= 200; seed++) {
		rc = run(seed, &t1);
		if (rc == XTC_E_NOSYS) {
			printf("not a sim build: nothing to simulate\n");
			return 0;
		}
		if (rc != XTC_OK) {
			printf("seed %llu: FAIL (%s): a stale write landed\n",
			    (unsigned long long)seed, xtc_strerror(rc));
			return 1;
		}
		if (run(seed, &t2) != XTC_OK || t2 != t1) {
			printf("seed %llu: FAIL: the replay differs\n",
			    (unsigned long long)seed);
			return 1;
		}
	}
	printf("200 seeds, each replayed; no stale write landed\n");
	return 0;
}
/* !endregion harness */
