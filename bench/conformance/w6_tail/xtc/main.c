/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * bench/conformance/w6_tail/xtc/main.c
 *   W6: tail latency under backpressure — xtc runtime.
 *
 *   N generator pthreads pump 4-byte messages onto a bounded
 *   xtc_chan_mpsc channel; one consumer pthread drains them.
 *   An xtc_res cap on XTC_RES_CHAN_SLOTS limits the number of
 *   in-flight messages.
 *
 *   When a generator hits the cap, xtc_chan_mpsc_try_send returns
 *   XTC_E_RESOURCE (global slot cap exhausted) or XTC_E_AGAIN (ring
 *   buffer full); the generator records the rejection and continues
 *   immediately without blocking — no parking, no sleeping.  This is
 *   xtc's distinguishing property: the cap fires explicitly via
 *   xtc_res rather than implicitly queuing forever.
 *
 *   The high-water alert callback fires once per upward crossing of
 *   80% capacity and re-arms on the way back down, so oscillating
 *   load fires it multiple times.  The alert count is reported to
 *   stderr for diagnostics.
 *
 *   Per admitted request the generator timestamps the message with
 *   CLOCK_MONOTONIC before try_send; the consumer captures a second
 *   timestamp after try_recv and records the delta in a log-linear
 *   histogram.
 *
 * Build:
 *   make                  (see Makefile)
 *
 * Usage:
 *   ./bench [--gens=<int>] [--ops=<int>] [--cap=<int>]
 *   ./bench --params=gens=8:ops=1000000:cap=1000
 *   Defaults: gens=8, ops=1000000, cap=1000
 *
 * Output (one M17 line):
 *   workload=W6 runtime=xtc params=gens=8:ops=1000000:cap=1000
 *   elapsed_ns=... cpu_us=... rss_kb=...
 *   p50_ns=... p95_ns=... p99_ns=... p999_ns=... rejected=...
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <sys/resource.h>

#include "hist.h"

#include "xtc.h"
#include "xtc_res.h"
#include "xtc_chan.h"

/* -------------------------------------------------------------------------
 * Defaults
 * ------------------------------------------------------------------------- */

#define DEFAULT_GENS  8
#define DEFAULT_OPS   1000000L
#define DEFAULT_CAP   1000L

/* -------------------------------------------------------------------------
 * Message type
 *   Heap-allocated; generator sets ts_ns; consumer frees after latency
 *   recording.  payload is the logical 4-byte body.
 * ------------------------------------------------------------------------- */

struct w6_msg {
	uint64_t ts_ns;    /* CLOCK_MONOTONIC nanoseconds at generator issue  */
	uint32_t payload;  /* 4-byte workload body (the "message content")    */
	uint32_t _pad;     /* keep size a multiple of 8                       */
};

/* -------------------------------------------------------------------------
 * Shared state between main and threads
 * ------------------------------------------------------------------------- */

struct w6_shared {
	xtc_chan_mpsc_t  *chan;
	atomic_int        gens_done;   /* counts completed generators */
	int               n_gens;      /* total generator threads     */
};

/* -------------------------------------------------------------------------
 * Per-generator thread argument (rejection count written only by this
 * thread; read safely after pthread_join).
 * ------------------------------------------------------------------------- */

struct gen_arg {
	struct w6_shared *shared;
	long              ops;        /* iterations this generator performs */
	long              rejected;   /* incremented locally; summed in main */
};

/* -------------------------------------------------------------------------
 * Consumer thread argument
 * ------------------------------------------------------------------------- */

struct consumer_arg {
	xtc_chan_mpsc_t  *chan;
	hist_t           *hist;       /* latency histogram                  */
	long              admitted;   /* messages successfully processed    */
};

/* -------------------------------------------------------------------------
 * Alert state (fires once per upward 80%-capacity crossing)
 * ------------------------------------------------------------------------- */

static atomic_int g_alert_fires;

static void
hwm_alert(xtc_res_kind_t k, int64_t used, int64_t cap, void *user)
{
	(void)k; (void)used; (void)cap; (void)user;
	atomic_fetch_add_explicit(&g_alert_fires, 1, memory_order_relaxed);
}

/* -------------------------------------------------------------------------
 * now_ns — CLOCK_MONOTONIC in nanoseconds
 * ------------------------------------------------------------------------- */

static uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000)
	     + (uint64_t)ts.tv_nsec;
}

/* -------------------------------------------------------------------------
 * ru_cpu_us — user+sys CPU time in microseconds
 * ------------------------------------------------------------------------- */

static uint64_t
ru_cpu_us(const struct rusage *ru)
{
	return (uint64_t)(ru->ru_utime.tv_sec  + ru->ru_stime.tv_sec)
	     * UINT64_C(1000000)
	     + (uint64_t)(ru->ru_utime.tv_usec + ru->ru_stime.tv_usec);
}

/* -------------------------------------------------------------------------
 * Generator thread
 *   For each operation: allocate a message, timestamp it, try_send.
 *   On rejection (XTC_E_RESOURCE or XTC_E_AGAIN) increment local
 *   counter and free the message — no blocking, no waiting.
 * ------------------------------------------------------------------------- */

static void *
gen_fn(void *arg_)
{
	struct gen_arg   *a = arg_;
	struct w6_shared *sh = a->shared;
	long              i;

	a->rejected = 0;

	for (i = 0; i < a->ops; i++) {
		struct w6_msg *m = malloc(sizeof *m);
		int            rc;

		if (m == NULL) {
			/* OOM is not a backpressure rejection, but skip cleanly */
			a->rejected++;
			continue;
		}

		m->ts_ns   = now_ns();
		m->payload = (uint32_t)(unsigned long)i;
		m->_pad    = 0;

		rc = xtc_chan_mpsc_try_send(sh->chan, m);
		if (rc != XTC_OK) {
			/*
			 * XTC_E_RESOURCE: xtc_res slot cap reached.
			 * XTC_E_AGAIN:    ring buffer full.
			 * Both mean: caller-side backpressure, no blocking.
			 */
			a->rejected++;
			free(m);
		}
		/* XTC_OK: message is owned by the consumer now */
	}

	/*
	 * Signal completion.  The last generator to finish closes the
	 * channel so the consumer sees XTC_E_INVAL after draining.
	 */
	if (atomic_fetch_add_explicit(&sh->gens_done, 1,
	                              memory_order_acq_rel) + 1 == sh->n_gens)
		xtc_chan_mpsc_close(sh->chan);

	return NULL;
}

/* -------------------------------------------------------------------------
 * Consumer thread
 *   Drain the channel until it is closed and empty (XTC_E_INVAL).
 *   Yield when momentarily empty (XTC_E_AGAIN) rather than spinning hot.
 * ------------------------------------------------------------------------- */

static void *
consumer_fn(void *arg_)
{
	struct consumer_arg *a = arg_;
	void                *raw;
	int                  rc;

	a->admitted = 0;

	for (;;) {
		rc = xtc_chan_mpsc_try_recv(a->chan, &raw);

		if (rc == XTC_OK) {
			struct w6_msg *m      = (struct w6_msg *)raw;
			uint64_t       t_recv = now_ns();
			uint64_t       delta  = t_recv - m->ts_ns;

			hist_record(a->hist, delta);
			free(m);
			a->admitted++;

		} else if (rc == XTC_E_AGAIN) {
			/*
			 * Channel is open but empty: generators are still
			 * running.  Yield rather than spinning hot so
			 * generators get CPU time to produce more messages.
			 */
			sched_yield();

		} else {
			/*
			 * XTC_E_INVAL: channel closed and buffer drained.
			 * All admitted messages have been processed.
			 */
			break;
		}
	}

	return NULL;
}

/* -------------------------------------------------------------------------
 * Argument parsing
 * ------------------------------------------------------------------------- */

static void
parse_args(int argc, char **argv,
           int *gens_out, long *ops_out, long *cap_out)
{
	int        i;
	const char *p;

	*gens_out = DEFAULT_GENS;
	*ops_out  = DEFAULT_OPS;
	*cap_out  = DEFAULT_CAP;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (strncmp(a, "--gens=", 7) == 0) {
			*gens_out = (int)strtol(a + 7, NULL, 10);
		} else if (strncmp(a, "--ops=", 6) == 0) {
			*ops_out = strtol(a + 6, NULL, 10);
		} else if (strncmp(a, "--cap=", 6) == 0) {
			*cap_out = strtol(a + 6, NULL, 10);
		} else if (strncmp(a, "--params=", 9) == 0) {
			p = a + 9;
			do {
				if (strncmp(p, "gens=", 5) == 0)
					*gens_out = (int)strtol(p + 5, NULL, 10);
				else if (strncmp(p, "ops=", 4) == 0)
					*ops_out = strtol(p + 4, NULL, 10);
				else if (strncmp(p, "cap=", 4) == 0)
					*cap_out = strtol(p + 4, NULL, 10);
				p = strchr(p, ':');
				if (p != NULL)
					p++;
			} while (p != NULL);
		}
	}

	if (*gens_out < 1) *gens_out = 1;
	if (*ops_out  < 1) *ops_out  = 1;
	if (*cap_out  < 1) *cap_out  = 1;
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int
main(int argc, char **argv)
{
	int                  gens;
	long                 ops, cap;
	xtc_res_t            res;
	xtc_res_caps_t       caps;
	xtc_chan_mpsc_t     *chan        = NULL;
	hist_t               hist;
	struct w6_shared     shared;
	struct gen_arg      *gen_args   = NULL;
	struct consumer_arg  con_arg;
	pthread_t           *gtids      = NULL;
	pthread_t            ctid;
	struct rusage        ru0, ru1;
	uint64_t             t_start, t_end;
	uint64_t             elapsed_ns, cpu_us, rss_kb;
	long                 total_rejected = 0;
	long                 actual_ops     = 0;
	int                  j;
	int                  rc;

	parse_args(argc, argv, &gens, &ops, &cap);

	/* ---- xtc_res: set chan_slots cap to --cap ---- */
	caps           = (xtc_res_caps_t)XTC_RES_CAPS_DEFAULT;
	caps.chan_slots = cap;

	if (xtc_res_init(&res, &caps) != XTC_OK) {
		fprintf(stderr, "w6/xtc: xtc_res_init failed\n");
		return 1;
	}

	/* ---- high-water alert at 80% ---- */
	atomic_init(&g_alert_fires, 0);
	(void)xtc_res_set_alert(&res, XTC_RES_CHAN_SLOTS, 0.80);
	(void)xtc_res_set_alert_fn(&res, hwm_alert, NULL);

	/* ---- channel: capacity matches the xtc_res cap ---- */
	rc = xtc_chan_mpsc_create(&res, (size_t)cap, &chan);
	if (rc != XTC_OK) {
		fprintf(stderr, "w6/xtc: xtc_chan_mpsc_create failed (%d)\n", rc);
		return 1;
	}

	/* ---- histogram ---- */
	if (hist_init(&hist, HIST_SUB_BITS_DEFAULT) != 0) {
		fprintf(stderr, "w6/xtc: hist_init failed\n");
		xtc_chan_mpsc_destroy(chan);
		return 1;
	}

	/* ---- shared state ---- */
	shared.chan    = chan;
	atomic_init(&shared.gens_done, 0);
	shared.n_gens  = gens;

	/* ---- per-generator args ---- */
	gen_args = calloc((size_t)gens, sizeof *gen_args);
	gtids    = calloc((size_t)gens, sizeof *gtids);
	if (gen_args == NULL || gtids == NULL) {
		fprintf(stderr, "w6/xtc: calloc failed\n");
		free(gen_args);
		free(gtids);
		hist_fini(&hist);
		xtc_chan_mpsc_destroy(chan);
		return 1;
	}

	for (j = 0; j < gens; j++) {
		long base = ops / gens;
		long rem  = ops % gens;

		gen_args[j].shared   = &shared;
		gen_args[j].ops      = base + (j < (int)rem ? 1 : 0);
		gen_args[j].rejected = 0;
		actual_ops          += gen_args[j].ops;
	}

	/* ---- consumer arg ---- */
	con_arg.chan     = chan;
	con_arg.hist     = &hist;
	con_arg.admitted = 0;

	/* ---- launch ---- */
	getrusage(RUSAGE_SELF, &ru0);
	t_start = now_ns();

	/* Consumer starts first so it is ready when generators begin. */
	(void)pthread_create(&ctid, NULL, consumer_fn, &con_arg);

	for (j = 0; j < gens; j++)
		(void)pthread_create(&gtids[j], NULL, gen_fn, &gen_args[j]);

	/* Join generators; last one will close the channel. */
	for (j = 0; j < gens; j++)
		(void)pthread_join(gtids[j], NULL);

	/* Consumer exits after channel is closed and drained. */
	(void)pthread_join(ctid, NULL);

	t_end = now_ns();
	getrusage(RUSAGE_SELF, &ru1);

	/* ---- aggregate ---- */
	for (j = 0; j < gens; j++)
		total_rejected += gen_args[j].rejected;

	elapsed_ns = t_end  - t_start;
	cpu_us     = ru_cpu_us(&ru1) - ru_cpu_us(&ru0);
	rss_kb     = (uint64_t)ru1.ru_maxrss;   /* Linux: already KiB */

	/* ---- diagnostics to stderr (not part of M17 format) ---- */
	fprintf(stderr,
	    "# w6/xtc: admitted=%ld rejected=%ld alerts=%d hwm=%lld\n",
	    con_arg.admitted,
	    total_rejected,
	    atomic_load(&g_alert_fires),
	    (long long)xtc_res_high(&res, XTC_RES_CHAN_SLOTS));

	/* ---- M17 results line ---- */
	printf("workload=W6 runtime=xtc params=gens=%d:ops=%ld:cap=%ld"
	       " elapsed_ns=%llu"
	       " cpu_us=%llu"
	       " rss_kb=%llu"
	       " p50_ns=%llu"
	       " p95_ns=%llu"
	       " p99_ns=%llu"
	       " p999_ns=%llu"
	       " rejected=%ld\n",
	    gens,
	    actual_ops,
	    cap,
	    (unsigned long long)elapsed_ns,
	    (unsigned long long)cpu_us,
	    (unsigned long long)rss_kb,
	    (unsigned long long)hist_percentile(&hist,  50.0),
	    (unsigned long long)hist_percentile(&hist,  95.0),
	    (unsigned long long)hist_percentile(&hist,  99.0),
	    (unsigned long long)hist_percentile(&hist,  99.9),
	    total_rejected);

	/* ---- cleanup ---- */
	hist_fini(&hist);
	xtc_chan_mpsc_destroy(chan);
	free(gen_args);
	free(gtids);

	return 0;
}
