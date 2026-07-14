/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_iosched.h
 *	Adaptive write-batching scheduler for a single writer over the
 *	async file path (xtc_aio_*), intended for the direct-I/O
 *	writeback hot path (a buffer-manager flusher, a WAL writer).
 *
 *	Queued writes are coalesced into batches; each flush issues the
 *	batch via xtc_aio_pwrite and measures throughput.  When adaptive
 *	mode is on, a genetic tuner (xtc_dio_sched) evolves the batch size
 *	to maximise observed throughput and re-adapts when the workload
 *	or device behaviour shifts.  Opt-in per scheduler (per device):
 *	with adaptive off it uses a fixed batch size.
 *
 *	Single-writer contract: all calls for one scheduler must come
 *	from one fiber (the device's writer).  This keeps batching free
 *	of cross-fiber coordination; flushing parks only that fiber.
 */

#ifndef XTC_IOSCHED_H
#define XTC_IOSCHED_H

#include "xtc_export.h"

#include <stdint.h>

#include "xtc.h"

typedef struct xtc_iosched xtc_iosched_t;

typedef struct xtc_iosched_opts {
	int      fd;            /* target descriptor (direct or buffered) */
	int      adaptive;      /* 1 = GA-tune the batch size; 0 = fixed */
	int      batch_size;    /* fixed (adaptive=0) or initial (adaptive=1) */
	int      min_batch;     /* gene bounds when adaptive (>=1) */
	int      max_batch;
	uint64_t seed;          /* tuner PRNG seed (0 = default) */
} xtc_iosched_opts_t;

typedef struct xtc_iosched_stats {
	uint64_t writes;        /* total writes queued */
	uint64_t bytes;         /* total bytes written */
	uint64_t flushes;       /* batches issued */
	int      cur_batch;     /* batch size currently in use */
	double   last_mbps;     /* throughput of the last flush, MiB/s */
	double   mutation_rate; /* tuner mutation rate (adaptive only) */
} xtc_iosched_stats_t;

/*
 * PUBLIC: int  xtc_iosched_create __P((const xtc_iosched_opts_t *, xtc_iosched_t **));
 * PUBLIC: void xtc_iosched_destroy __P((xtc_iosched_t *));
 * PUBLIC: int  xtc_iosched_write __P((xtc_iosched_t *, const void *, uint32_t, int64_t));
 * PUBLIC: int  xtc_iosched_flush __P((xtc_iosched_t *));
 * PUBLIC: void xtc_iosched_get_stats __P((const xtc_iosched_t *, xtc_iosched_stats_t *));
 */
XTC_API int  xtc_iosched_create(const xtc_iosched_opts_t *opts, xtc_iosched_t **out);
XTC_API void xtc_iosched_destroy(xtc_iosched_t *s);

/* Queue a write at off.  The buffer must remain valid until the next
 * flush completes.  When the queue reaches the current batch size an
 * implicit flush runs (issuing the batch and possibly parking the
 * writer fiber).  Returns XTC_OK, or a negative errno from a flush. */
XTC_API int  xtc_iosched_write(xtc_iosched_t *s, const void *buf, uint32_t len,
                               int64_t off);

/* Issue all queued writes now.  Returns XTC_OK or a negative errno. */
XTC_API int  xtc_iosched_flush(xtc_iosched_t *s);

XTC_API void xtc_iosched_get_stats(const xtc_iosched_t *s, xtc_iosched_stats_t *out);

#endif /* XTC_IOSCHED_H */
