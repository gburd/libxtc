/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * bench/bench_uring_disk.c
 *	Drive disk I/O at the host's max rate via liburing directly,
 *	to establish a baseline against which xtc_io's M11 async-
 *	stream surface can be measured.
 *
 *	What this measures:
 *	  - Single-thread read throughput against a regular file
 *	    opened with O_DIRECT (or buffered fallback).
 *	  - p50 / p95 / p99 / p999 / max per-read latency.
 *	  - Variable block size (default 64 KiB) and pipe depth
 *	    (default 256).
 *
 *	Usage:
 *	  bench_uring_disk <file> <size_mib> <block_kib> <depth>
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <liburing.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

static int64_t
__now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

struct slot {
	uint8_t  *buf;
	int64_t   submit_ns;
	uint64_t  offset;
	int       in_flight;
};

static int
__cmp_i64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
	return x < y ? -1 : x > y ? 1 : 0;
}

int
main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/tmp/xtc_bench_uring.dat";
	uint64_t size_mib = argc > 2 ? strtoull(argv[2], NULL, 10) : 1024;
	int      block_kib = argc > 3 ? atoi(argv[3]) : 64;
	int      depth    = argc > 4 ? atoi(argv[4]) : 256;
	uint64_t bytes = size_mib * 1024 * 1024;
	int      block_size = block_kib * 1024;
	uint64_t n_blocks = bytes / (uint64_t)block_size;
	int      use_direct = 1;
	int      fd_w, fd_r;
	uint8_t *zeros;
	struct slot *slots;
	int64_t *samples;
	uint64_t n_samples = 0;
	uint64_t next_submit = 0;
	uint64_t completed  = 0;
	int      in_flight  = 0;
	struct io_uring ring;
	int      rc;
	int64_t  t0, t1;
	uint64_t i;

	(void)argc; (void)argv;
	fprintf(stderr, "bench_uring_disk: %s, %llu MiB, %d KiB blocks, depth %d\n",
	    path, (unsigned long long)size_mib, block_kib, depth);

	if (depth <= 0 || depth > 4096) depth = 256;

	/* Pre-allocate / write the file. */
	fd_w = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd_w < 0) { perror("open w"); return 1; }
	if (posix_fallocate(fd_w, 0, (off_t)bytes) != 0) {
		fprintf(stderr, "fallocate failed; falling back to write\n");
	}
	zeros = aligned_alloc(4096, (size_t)block_size);
	if (!zeros) { perror("aligned_alloc"); return 1; }
	memset(zeros, 0xa5, (size_t)block_size);
	for (i = 0; i < n_blocks; i++) {
		ssize_t w = pwrite(fd_w, zeros, (size_t)block_size,
		    (off_t)(i * (uint64_t)block_size));
		if (w != block_size) { perror("pwrite"); return 1; }
	}
	(void)fsync(fd_w);
	(void)posix_fadvise(fd_w, 0, (off_t)bytes, POSIX_FADV_DONTNEED);
	(void)close(fd_w);
	free(zeros);

	/* Open for read. */
	fd_r = open(path, O_RDONLY | O_DIRECT);
	if (fd_r < 0) {
		fprintf(stderr, "O_DIRECT unavailable: %s; using buffered\n",
		    strerror(errno));
		fd_r = open(path, O_RDONLY);
		if (fd_r < 0) { perror("open r"); return 1; }
		use_direct = 0;
	}

	/* Set up io_uring. */
	rc = io_uring_queue_init((unsigned)depth * 2, &ring, 0);
	if (rc != 0) { fprintf(stderr, "io_uring_queue_init: %d\n", rc); return 1; }

	slots = calloc((size_t)depth, sizeof *slots);
	for (i = 0; i < (uint64_t)depth; i++) {
		slots[i].buf = aligned_alloc(4096, (size_t)block_size);
		if (!slots[i].buf) { perror("aligned_alloc"); return 1; }
	}

	samples = malloc(n_blocks * sizeof *samples);

	t0 = __now_ns();

	/* Prime: submit `depth` reads. */
	for (i = 0; i < (uint64_t)depth && next_submit < n_blocks; i++) {
		struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
		slots[i].submit_ns = __now_ns();
		slots[i].offset = next_submit * (uint64_t)block_size;
		slots[i].in_flight = 1;
		io_uring_prep_read(sqe, fd_r, slots[i].buf,
		    (unsigned)block_size, slots[i].offset);
		io_uring_sqe_set_data(sqe, &slots[i]);
		next_submit++;
		in_flight++;
	}
	(void)io_uring_submit(&ring);

	while (completed < n_blocks) {
		struct io_uring_cqe *cqe;
		int64_t now;
		struct slot *s;
		rc = io_uring_wait_cqe(&ring, &cqe);
		if (rc < 0) {
			fprintf(stderr, "io_uring_wait_cqe: %d (%s)\n",
			    rc, strerror(-rc));
			return 1;
		}
		s = io_uring_cqe_get_data(cqe);
		now = __now_ns();
		if (cqe->res < 0) {
			fprintf(stderr, "read error: %s\n", strerror(-cqe->res));
			return 1;
		}
		if (cqe->res != block_size) {
			fprintf(stderr, "short read: %d / %d\n",
			    cqe->res, block_size);
		}
		samples[n_samples++] = now - s->submit_ns;
		s->in_flight = 0;
		io_uring_cqe_seen(&ring, cqe);
		in_flight--;
		completed++;

		if (next_submit < n_blocks) {
			struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
			s->submit_ns = __now_ns();
			s->offset = next_submit * (uint64_t)block_size;
			s->in_flight = 1;
			io_uring_prep_read(sqe, fd_r, s->buf,
			    (unsigned)block_size, s->offset);
			io_uring_sqe_set_data(sqe, s);
			next_submit++;
			in_flight++;
			(void)io_uring_submit(&ring);
		}
	}

	t1 = __now_ns();

	{
		double seconds = (double)(t1 - t0) / 1e9;
		double mib = (double)bytes / (1024.0 * 1024.0);
		int64_t p50, p95, p99, p999, mx;
		qsort(samples, n_samples, sizeof *samples, __cmp_i64);
		p50  = samples[n_samples * 50  / 100];
		p95  = samples[n_samples * 95  / 100];
		p99  = samples[n_samples * 99  / 100];
		p999 = samples[n_samples * 999 / 1000];
		mx   = samples[n_samples - 1];
		printf("backend     = liburing %s\n",
		    use_direct ? "O_DIRECT" : "buffered");
		printf("throughput  = %.1f MiB/s in %.3f s (%llu reads)\n",
		    mib / seconds, seconds, (unsigned long long)n_samples);
		printf("p50 latency = %lld ns\n", (long long)p50);
		printf("p95 latency = %lld ns\n", (long long)p95);
		printf("p99 latency = %lld ns\n", (long long)p99);
		printf("p999        = %lld ns\n", (long long)p999);
		printf("max         = %lld ns\n", (long long)mx);
	}

	for (i = 0; i < (uint64_t)depth; i++) free(slots[i].buf);
	free(slots);
	free(samples);
	io_uring_queue_exit(&ring);
	(void)close(fd_r);
	(void)unlink(path);
	return 0;
}
