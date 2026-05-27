/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * bench/conformance/w3_pingpong/xtc/main.c
 *   W3: mailbox ping-pong benchmark -- xtc runtime.
 *
 *   Two xtc processes (ping, pong) exchange a small message N times.
 *   ping drives the loop, measuring round-trip latency via
 *   CLOCK_MONOTONIC and recording each sample in a log-linear
 *   histogram.  At the end, ping writes the M17 results line and
 *   exits; pong exits on the stop sentinel.
 *
 *   Modelled on examples/02_proc_pingpong.c.
 *
 * Usage:
 *   ./bench [--N=<int>] [--params=N=<int>]
 *   Default N = 1 000 000
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

/* hist.h implementation is compiled via hist.c; include declarations only. */
#include "hist.h"

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

/* ------------------------------------------------------------------------- */
/* Default workload parameters                                               */
/* ------------------------------------------------------------------------- */

#define DEFAULT_N  1000000L

/* ------------------------------------------------------------------------- */
/* Message shape                                                              */
/*   n >= 0 : live round trip (send index)                                   */
/*   n <  0 : stop sentinel (pong exits without reply)                       */
/* ------------------------------------------------------------------------- */

struct rpc_msg {
	xtc_pid_t from;
	int32_t   n;
	int32_t   _pad; /* keep sizeof struct a multiple of 4 bytes */
};

/* ------------------------------------------------------------------------- */
/* Per-process argument structs                                               */
/* ------------------------------------------------------------------------- */

/* ping_args holds everything ping needs; it also carries the output stats  */
/* back to main() after xtc_loop_run() returns.                              */
struct ping_args {
	xtc_pid_t  pong;       /* pid of the pong process              */
	long       N;          /* total round trips to perform         */
	hist_t    *hist;       /* latency histogram (allocated by main)*/
	/* output stats written by ping before returning */
	uint64_t   elapsed_ns; /* wall time covering the N round trips */
	uint64_t   cpu_us;     /* user + sys CPU time at end of loop   */
	uint64_t   rss_kb;     /* peak RSS at end of loop              */
};

/* ------------------------------------------------------------------------- */
/* now_ns -- monotonic nanoseconds                                            */
/* ------------------------------------------------------------------------- */

static uint64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000)
	     + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------------- */
/* pong process: echo each message back; exit on stop sentinel               */
/* ------------------------------------------------------------------------- */

static void
pong_fn(void *arg)
{
	(void)arg;

	for (;;) {
		void           *m;
		size_t          sz;
		struct rpc_msg  req;
		struct rpc_msg  reply;

		if (xtc_recv(&m, &sz, -1) != XTC_OK)
			return;

		if (sz != sizeof req) {
			free(m);
			continue;
		}

		memcpy(&req, m, sizeof req);
		free(m);

		if (req.n < 0)
			return; /* stop sentinel -- exit without reply */

		reply.from = xtc_self();
		reply.n    = req.n + 1;
		reply._pad = 0;
		(void)xtc_send(req.from, &reply, sizeof reply);
	}
}

/* ------------------------------------------------------------------------- */
/* ping process: drive N round trips, record per-RTT latency                 */
/* ------------------------------------------------------------------------- */

static void
ping_fn(void *arg)
{
	struct ping_args *st = arg;
	struct rusage     ru;
	long              n;
	uint64_t          t_start, t_end;

	t_start = now_ns();

	for (n = 0; n < st->N; n++) {
		struct rpc_msg req, reply;
		void          *m;
		size_t         sz;
		uint64_t       t0, t1;

		req.from = xtc_self();
		req.n    = (int32_t)n;
		req._pad = 0;

		t0 = now_ns();
		if (xtc_send(st->pong, &req, sizeof req) != XTC_OK)
			break;
		if (xtc_recv(&m, &sz, -1) != XTC_OK)
			break;
		t1 = now_ns();

		if (sz == sizeof reply) {
			memcpy(&reply, m, sizeof reply);
			free(m);
		} else {
			free(m);
			break;
		}

		hist_record(st->hist, t1 - t0);
	}

	t_end = now_ns();
	st->elapsed_ns = t_end - t_start;

	/* Send stop sentinel so pong can exit cleanly */
	{
		struct rpc_msg done;
		done.from = xtc_self();
		done.n    = -1;
		done._pad = 0;
		(void)xtc_send(st->pong, &done, sizeof done);
	}

	/* Capture resource usage after the workload */
	getrusage(RUSAGE_SELF, &ru);
	st->cpu_us =
	    (uint64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec)
	    * UINT64_C(1000000)
	    + (uint64_t)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec);
	st->rss_kb = (uint64_t)ru.ru_maxrss; /* Linux: already in KiB */
}

/* ------------------------------------------------------------------------- */
/* Argument parsing                                                           */
/* ------------------------------------------------------------------------- */

/*
 * Accept both --N=<int> (shorthand) and --params=N=<int>[:key=val...].
 * Returns DEFAULT_N if neither form is present.
 */
static long
parse_N(int argc, char **argv)
{
	long       n = DEFAULT_N;
	int        i;
	const char *p;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (strncmp(a, "--N=", 4) == 0) {
			n = strtol(a + 4, NULL, 10);
		} else if (strncmp(a, "--params=", 9) == 0) {
			p = a + 9;
			do {
				if (strncmp(p, "N=", 2) == 0)
					n = strtol(p + 2, NULL, 10);
				p = strchr(p, ':');
				if (p != NULL)
					p++;
			} while (p != NULL);
		}
	}

	if (n <= 0)
		n = DEFAULT_N;

	return n;
}

/* ------------------------------------------------------------------------- */
/* main                                                                       */
/* ------------------------------------------------------------------------- */

int
main(int argc, char **argv)
{
	xtc_loop_t       *loop;
	hist_t            hist;
	struct ping_args  ping_st;
	xtc_pid_t         pong_pid;
	long              N;

	N = parse_N(argc, argv);

	if (hist_init(&hist, HIST_SUB_BITS_DEFAULT) != 0) {
		fprintf(stderr, "w3/xtc: hist_init failed\n");
		return 1;
	}

	memset(&ping_st, 0, sizeof ping_st);
	ping_st.N    = N;
	ping_st.hist = &hist;

	if (xtc_loop_init(&loop) != XTC_OK) {
		fprintf(stderr, "w3/xtc: xtc_loop_init failed\n");
		hist_fini(&hist);
		return 1;
	}

	/* Spawn pong first so we have its pid before spawning ping */
	if (xtc_proc_spawn(loop, pong_fn, NULL, NULL, &pong_pid) != XTC_OK) {
		fprintf(stderr, "w3/xtc: spawn pong failed\n");
		(void)xtc_loop_fini(loop);
		hist_fini(&hist);
		return 1;
	}
	ping_st.pong = pong_pid;

	if (xtc_proc_spawn(loop, ping_fn, &ping_st, NULL, NULL) != XTC_OK) {
		fprintf(stderr, "w3/xtc: spawn ping failed\n");
		(void)xtc_loop_fini(loop);
		hist_fini(&hist);
		return 1;
	}

	if (xtc_loop_run(loop) != XTC_OK) {
		fprintf(stderr, "w3/xtc: xtc_loop_run failed\n");
		(void)xtc_loop_fini(loop);
		hist_fini(&hist);
		return 1;
	}

	(void)xtc_loop_fini(loop);

	/* M17 results line -- exactly one line on stdout */
	printf("workload=W3 runtime=xtc params=N=%ld"
	       " elapsed_ns=%llu"
	       " cpu_us=%llu"
	       " rss_kb=%llu"
	       " p50_ns=%llu"
	       " p95_ns=%llu"
	       " p99_ns=%llu"
	       " p999_ns=%llu\n",
	    N,
	    (unsigned long long)ping_st.elapsed_ns,
	    (unsigned long long)ping_st.cpu_us,
	    (unsigned long long)ping_st.rss_kb,
	    (unsigned long long)hist_percentile(&hist, 50.0),
	    (unsigned long long)hist_percentile(&hist, 95.0),
	    (unsigned long long)hist_percentile(&hist, 99.0),
	    (unsigned long long)hist_percentile(&hist, 99.9));

	hist_fini(&hist);
	return 0;
}
