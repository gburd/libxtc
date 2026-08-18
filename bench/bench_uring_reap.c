/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * bench/bench_uring_reap.c
 *	L4 measure-first: is a SINGLE io_uring ring's CQE reap / SQE fill
 *	the wall that would justify Glommio's 3-ring QoS split or
 *	registered DMA buffers?
 *
 *	Glommio (ScyllaDB) splits into three rings (latency / poll-IOPOLL
 *	/ main) and pre-registers DMA buffers by id.  Before adopting any
 *	of that in libxtc (single ring per loop), MEASURE whether the
 *	single-ring submit+reap path is actually the bottleneck.
 *
 *	Method: an IORING_OP_NOP storm at a fixed queue depth.  NOP does
 *	NO I/O -- it exercises ONLY the SQE fill + submit + CQE reap loop,
 *	so the rate it sustains IS the single-ring reap/fill ceiling in
 *	ops/s and ns/op.  Compare that ceiling to what real mixed net+disk
 *	I/O could ever demand: if the ring can push tens of millions of
 *	ops/s, the reap is not the wall for a runtime whose real I/O is
 *	syscall/DMA-bound at far lower rates.
 *
 *	Also times a per-SQE user_data pointer set (the per-SQE buffer
 *	pointer cost the L4 note asks about) as part of the loop.
 *
 *	Usage: bench_uring_reap [depth] [total_ops]
 */

#define _GNU_SOURCE
#include <liburing.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int
main(int argc, char **argv)
{
	int depth = argc > 1 ? atoi(argv[1]) : 256;
	uint64_t total = argc > 2 ? strtoull(argv[2], NULL, 10)
	                          : 50ULL * 1000 * 1000;
	struct io_uring ring;
	uint64_t submitted = 0, completed = 0;
	int64_t t0, t1;
	int rc, i;
	uintptr_t token = 0;

	if (depth <= 0 || depth > 4096) depth = 256;
	rc = io_uring_queue_init((unsigned)depth * 2, &ring, 0);
	if (rc != 0) {
		fprintf(stderr, "io_uring_queue_init: %d\n", rc);
		return 1;
	}
	fprintf(stderr, "bench_uring_reap: depth=%d total_ops=%llu (NOP storm)\n",
	    depth, (unsigned long long)total);

	t0 = now_ns();

	/* Prime the ring with `depth` NOPs. */
	for (i = 0; i < depth && submitted < total; i++) {
		struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
		io_uring_prep_nop(sqe);
		io_uring_sqe_set_data(sqe, (void *)(++token));  /* per-SQE ptr */
		submitted++;
	}
	(void)io_uring_submit(&ring);

	/* Steady state: reap a CQE, refill an SQE, keep the ring full. */
	while (completed < total) {
		struct io_uring_cqe *cqe;
		unsigned head, seen = 0;
		rc = io_uring_wait_cqe(&ring, &cqe);
		if (rc < 0) {
			fprintf(stderr, "wait_cqe: %d\n", rc);
			return 1;
		}
		/* Drain everything currently ready (the single-ring reap). */
		io_uring_for_each_cqe(&ring, head, cqe) {
			(void)io_uring_cqe_get_data(cqe);
			seen++;
		}
		io_uring_cq_advance(&ring, seen);
		completed += seen;

		/* Refill to keep the pipe at `depth`. */
		for (i = 0; i < (int)seen && submitted < total; i++) {
			struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
			if (sqe == NULL)
				break;
			io_uring_prep_nop(sqe);
			io_uring_sqe_set_data(sqe, (void *)(++token));
			submitted++;
		}
		(void)io_uring_submit(&ring);
	}

	t1 = now_ns();
	{
		double sec = (double)(t1 - t0) / 1e9;
		double ops_s = (double)completed / sec;
		printf("single-ring NOP reap: %.2f Mops/s  (%.2f ns/op)  "
		    "%.3f s for %llu ops at depth %d\n",
		    ops_s / 1e6, (double)(t1 - t0) / (double)completed, sec,
		    (unsigned long long)completed, depth);
	}
	io_uring_queue_exit(&ring);
	return 0;
}
