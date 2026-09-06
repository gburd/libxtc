/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * bench/bench_aio_fibers.c
 *	Fiber-driven file I/O through xtc_aio -- the ONLY libxtc path that
 *	reaches io_uring's kernel io-wq worker pool.
 *
 *	WHY THIS EXISTS.  Two proposals from the Seastar asymmetric-io_uring
 *	review are gated on the same precondition, and neither could be
 *	evaluated because nothing in this tree measured the affected path:
 *
 *	  P2  io_uring_register_iowq_aff -- confine the bounded io-wq pool
 *	      to designated cores (Seastar's mechanism for keeping kernel
 *	      I/O work off application cores).
 *	  P5  registered files (IOSQE_FIXED_FILE) -- skip the per-op
 *	      fget/fput on fds used repeatedly by xtc_io_aio_submit.
 *
 *	The existing bench_uring_disk.c drives liburing DIRECTLY and never
 *	calls xtc_aio, so it cannot show whether libxtc's own AIO path is
 *	worker-bound or lookup-bound.  Without that, both proposals would
 *	have been adopted or rejected on intuition.  This is the instrument
 *	that makes them decidable.
 *
 *	WHY xtc_aio IS THE ONLY RELEVANT PATH.  Of the ops libxtc submits --
 *	poll_add / poll_multishot / poll_remove, a preempt timeout, and file
 *	read/write/readv/writev/fsync/fdatasync -- only the file ops can be
 *	handed to io-wq.  Polls and timeouts never are, and sockets are
 *	polled for readiness then read/written INLINE on the fiber's own
 *	thread.  So io-wq affinity and registered files can only ever affect
 *	FILE I/O in libxtc, which means a network benchmark cannot justify
 *	either one.  That is why this bench is separate from
 *	bench_net_compute.c rather than a mode of it.
 *
 *	WHAT IT REPORTS.  IOPS, p50/p99 completion latency, CPU-us per op,
 *	and the io-wq thread count observed during the run (read from
 *	/proc/<pid>/task, counting "iou-wrk" kernel threads).  That last
 *	number is the one P2 turns on: if the bounded pool never grows and
 *	never saturates, pinning it is a knob for a problem this workload
 *	does not have.
 *
 *	Usage:
 *	  bench_aio_fibers <file> [fibers] [ops_each] [bytes] [op] [iowq_cap]
 *	    op: read | write | fsync | fdatasync   (default read)
 *	    iowq_cap: 0 = leave the shipped default, N = xtc_io_set_iowq_max_workers(N,0)
 *
 *	MUST run against a real block device (AGENTS.md: durable benchmarks
 *	on real NVMe/ext4, never tmpfs) or the numbers measure page cache.
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_aio.h"
#include "xtc_exec.h"
#include "xtc_io.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

#define MAX_FIBERS 512

enum { OP_READ, OP_WRITE, OP_FSYNC, OP_FDATASYNC };

static int      g_fd;
static int      g_op;
static int      g_ops_each;
static int      g_bytes;
static off_t    g_file_size;
static int      g_nfibers;

static _Atomic uint64_t g_done;
static _Atomic uint64_t g_errs;
static int64_t         *g_lat;          /* nfibers * ops_each */
static _Atomic int      g_lat_idx;

static int64_t
now_ns(void)
{
	struct timespec ts;
	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/*
 * Count this process's io-wq kernel threads.  They appear in
 * /proc/self/task as comm "iou-wrk-<pid>" (and "iou-sqp-" for a SQPOLL
 * thread, which libxtc never creates).  This is the direct measurement of
 * whether the bounded pool is under pressure -- the question P2's
 * affinity knob would address.
 */
static int
count_iowq_threads(void)
{
	DIR *d = opendir("/proc/self/task");
	struct dirent *e;
	int n = 0;

	if (d == NULL)
		return -1;
	while ((e = readdir(d)) != NULL) {
		char path[512], comm[128];
		FILE *f;

		if (e->d_name[0] == '.')
			continue;
		(void)snprintf(path, sizeof path, "/proc/self/task/%s/comm",
		    e->d_name);
		f = fopen(path, "r");
		if (f == NULL)
			continue;
		if (fgets(comm, sizeof comm, f) != NULL &&
		    strncmp(comm, "iou-", 4) == 0)
			n++;
		(void)fclose(f);
	}
	(void)closedir(d);
	return n;
}

static _Atomic int g_peak_iowq;

static void
aio_fiber(void *arg)
{
	int id = (int)(intptr_t)arg;
	void *buf = NULL;
	int i;

	if (g_op == OP_READ || g_op == OP_WRITE) {
		/* Aligned for a possible O_DIRECT fd; harmless otherwise. */
		if (posix_memalign(&buf, 4096, (size_t)g_bytes) != 0) {
			atomic_fetch_add(&g_errs, 1);
			return;
		}
		memset(buf, 'a' + (id % 26), (size_t)g_bytes);
	}

	for (i = 0; i < g_ops_each; i++) {
		int64_t s, e;
		int rc = 0;
		off_t off = 0;

		if (g_op == OP_READ || g_op == OP_WRITE) {
			/* Spread offsets so fibers do not all hit one block;
			 * aligned to the transfer size for O_DIRECT. */
			off_t span = g_file_size > g_bytes
			    ? (g_file_size - g_bytes) : 0;
			if (span > 0) {
				off = (off_t)(((uint64_t)id * 2654435761u +
				    (uint64_t)i * 40503u) % (uint64_t)span);
				off -= off % g_bytes;
			}
		}
		s = now_ns();
		switch (g_op) {
		case OP_READ:
			rc = xtc_aio_pread(g_fd, buf, (uint32_t)g_bytes, off);
			break;
		case OP_WRITE:
			rc = xtc_aio_pwrite(g_fd, buf, (uint32_t)g_bytes, off);
			break;
		case OP_FSYNC:
			rc = xtc_aio_fsync(g_fd);
			break;
		case OP_FDATASYNC:
			rc = xtc_aio_fdatasync(g_fd);
			break;
		}
		e = now_ns();
		if (rc < 0)
			atomic_fetch_add_explicit(&g_errs, 1,
			    memory_order_relaxed);
		{
			int idx = atomic_fetch_add_explicit(&g_lat_idx, 1,
			    memory_order_relaxed);
			if (idx < g_nfibers * g_ops_each)
				g_lat[idx] = e - s;
		}
		atomic_fetch_add_explicit(&g_done, 1, memory_order_relaxed);

		/* Sample the io-wq pool periodically: it grows on demand, so
		 * the peak during the run is the informative figure. */
		if ((i & 63) == 0) {
			int n = count_iowq_threads();
			int prev = atomic_load_explicit(&g_peak_iowq,
			    memory_order_relaxed);
			while (n > prev &&
			    !atomic_compare_exchange_weak_explicit(
			        &g_peak_iowq, &prev, n,
			        memory_order_relaxed, memory_order_relaxed))
				;
		}
	}
	free(buf);
}

static int
cmp_i64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
	return x < y ? -1 : (x > y ? 1 : 0);
}

int
main(int argc, char **argv)
{
	xtc_exec_t *e = NULL;
	const char *path;
	const char *opname = "read";
	int nloops, i, iowq_cap = 0;
	int64_t t0, t1;
	struct stat st;

	if (argc < 2) {
		fprintf(stderr,
		    "usage: %s <file> [fibers] [ops_each] [bytes] "
		    "[read|write|fsync|fdatasync] [iowq_cap]\n"
		    "\n"
		    "Drives libxtc's xtc_aio path -- the only path that reaches\n"
		    "io_uring's io-wq.  Use a file on a REAL block device; on\n"
		    "tmpfs this measures the page cache, not I/O.\n", argv[0]);
		return 2;
	}
	path = argv[1];
	g_nfibers  = argc > 2 ? atoi(argv[2]) : 32;
	g_ops_each = argc > 3 ? atoi(argv[3]) : 200;
	g_bytes    = argc > 4 ? atoi(argv[4]) : 4096;
	if (argc > 5)
		opname = argv[5];
	if (argc > 6)
		iowq_cap = atoi(argv[6]);

	if (g_nfibers < 1 || g_nfibers > MAX_FIBERS) {
		fprintf(stderr, "fibers must be 1..%d\n", MAX_FIBERS);
		return 2;
	}
	if (strcmp(opname, "read") == 0)            g_op = OP_READ;
	else if (strcmp(opname, "write") == 0)      g_op = OP_WRITE;
	else if (strcmp(opname, "fsync") == 0)      g_op = OP_FSYNC;
	else if (strcmp(opname, "fdatasync") == 0)  g_op = OP_FDATASYNC;
	else {
		fprintf(stderr, "unknown op %s\n", opname);
		return 2;
	}

	/* Set the io-wq cap BEFORE the executor creates any ring: the cap is
	 * read once per ring at init, which is exactly the ordering
	 * constraint the public knob documents. */
	if (iowq_cap > 0)
		xtc_io_set_iowq_max_workers((unsigned)iowq_cap, 0);

	g_fd = open(path, (g_op == OP_READ ? O_RDONLY : O_RDWR) | O_CREAT,
	    0644);
	if (g_fd < 0) {
		perror("open");
		return 1;
	}
	if (fstat(g_fd, &st) != 0) {
		perror("fstat");
		(void)close(g_fd);
		return 1;
	}
	g_file_size = st.st_size;
	if ((g_op == OP_READ || g_op == OP_WRITE) &&
	    g_file_size < (off_t)g_bytes * 16) {
		fprintf(stderr,
		    "file is %lld bytes; too small to spread %d-byte ops.\n"
		    "Create one first, e.g.:\n"
		    "  dd if=/dev/zero of=%s bs=1M count=256\n",
		    (long long)g_file_size, g_bytes, path);
		(void)close(g_fd);
		return 2;
	}

	g_lat = calloc((size_t)g_nfibers * g_ops_each, sizeof *g_lat);
	if (g_lat == NULL) {
		fprintf(stderr, "oom\n");
		(void)close(g_fd);
		return 1;
	}

	nloops = xtc_ncpus();
	if (nloops < 1)
		nloops = 4;
	if (nloops > 16)
		nloops = 16;
	if (xtc_exec_init(&e, nloops) != XTC_OK) {
		fprintf(stderr, "xtc_exec_init failed\n");
		free(g_lat);
		(void)close(g_fd);
		return 1;
	}
	for (i = 0; i < g_nfibers; i++) {
		xtc_proc_opts_t o;
		memset(&o, 0, sizeof o);
		o.name = "aio";
		o.migratable = 1;
		if (xtc_proc_spawn(xtc_exec_loop(e, i % nloops), aio_fiber,
		    (void *)(intptr_t)i, &o, NULL) != XTC_OK) {
			fprintf(stderr, "spawn failed at %d\n", i);
			(void)xtc_exec_fini(e);
			free(g_lat);
			(void)close(g_fd);
			return 1;
		}
	}

	fprintf(stderr, "[aio] backend=%s loops=%d fibers=%d ops_each=%d "
	    "bytes=%d op=%s iowq_cap=%s\n",
	    xtc_io_backend_name(), nloops, g_nfibers, g_ops_each, g_bytes,
	    opname, iowq_cap > 0 ? argv[6] : "default");

	t0 = now_ns();
	(void)xtc_exec_run(e);
	t1 = now_ns();

	{
		uint64_t done = atomic_load(&g_done);
		uint64_t errs = atomic_load(&g_errs);
		int nlat = atomic_load(&g_lat_idx);
		double secs = (double)(t1 - t0) / 1e9;
		struct timespec cpu;
		double cpu_s = 0.0;

		if (nlat > g_nfibers * g_ops_each)
			nlat = g_nfibers * g_ops_each;
		if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu) == 0)
			cpu_s = (double)cpu.tv_sec +
			    (double)cpu.tv_nsec / 1e9;

		printf("ops=%llu errors=%llu elapsed=%.3fs iops=%.0f\n",
		    (unsigned long long)done, (unsigned long long)errs, secs,
		    secs > 0 ? (double)done / secs : 0.0);
		if (nlat > 0) {
			qsort(g_lat, (size_t)nlat, sizeof *g_lat, cmp_i64);
			printf("p50=%.1fus p99=%.1fus\n",
			    (double)g_lat[nlat / 2] / 1000.0,
			    (double)g_lat[(nlat * 99) / 100] / 1000.0);
		}
		printf("cpu=%.3fs", cpu_s);
		if (done > 0)
			printf(" cpu_us_per_op=%.3f",
			    cpu_s * 1e6 / (double)done);
		printf("\n");
		/*
		 * The P2 verdict hinges on this line.  A peak at or below the
		 * shipped cap with no latency cliff means the bounded pool is
		 * not under pressure, and confining it to specific cores would
		 * be a knob for a problem this workload does not have.
		 */
		printf("peak_iowq_threads=%d\n",
		    atomic_load(&g_peak_iowq));
	}

	(void)xtc_exec_fini(e);
	free(g_lat);
	(void)close(g_fd);
	return 0;
}
