/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/iosched.c
 *	Adaptive write-batching scheduler.  See xtc_iosched.h.
 */

#include "xtc_int.h"
#include "xtc_iosched.h"
#include "xtc_aio.h"
#include "xtc_dio_sched.h"
#include "os_time.h"

#include <string.h>

struct iow {
	const void *buf;
	uint32_t    len;
	int64_t     off;
};

struct xtc_iosched {
	int            fd;
	int            adaptive;
	int            batch;        /* current batch size in use */
	int            min_batch;
	int            max_batch;

	struct iow    *queue;        /* up to max_batch entries */
	int            n;            /* queued */

	xtc_dio_sched_t  *tuner;        /* NULL unless adaptive */

	uint64_t       writes;
	uint64_t       bytes;
	uint64_t       flushes;
	double         last_mbps;
};

int
xtc_iosched_create(const xtc_iosched_opts_t *opts, xtc_iosched_t **out)
{
	xtc_iosched_t *s;
	int rc, cap;

	if (opts == NULL || out == NULL || opts->fd < 0) return XTC_E_INVAL;

	if ((rc = __os_calloc(1, sizeof *s, (void **)&s)) != XTC_OK) return rc;
	s->fd = opts->fd;
	s->adaptive = opts->adaptive ? 1 : 0;
	s->min_batch = opts->min_batch > 0 ? opts->min_batch : 1;
	s->max_batch = opts->max_batch > s->min_batch ? opts->max_batch : 256;
	s->batch = opts->batch_size > 0 ? opts->batch_size : 32;
	if (s->batch < s->min_batch) s->batch = s->min_batch;
	if (s->batch > s->max_batch) s->batch = s->max_batch;

	if (s->adaptive) {
		xtc_dio_sched_spec_t spec;
		memset(&spec, 0, sizeof spec);
		spec.n_genes = 1;
		spec.min[0] = s->min_batch;
		spec.max[0] = s->max_batch;
		spec.init[0] = s->batch;
		spec.population = 8;
		spec.seed = opts->seed;
		if ((rc = xtc_dio_sched_create(&spec, &s->tuner)) != XTC_OK) {
			__os_free(s);
			return rc;
		}
		{
			int g[XTC_DIO_SCHED_MAX_GENES];
			xtc_dio_sched_current(s->tuner, g);
			s->batch = g[0];
		}
	}

	cap = s->max_batch;
	if ((rc = __os_calloc((size_t)cap, sizeof s->queue[0],
	    (void **)&s->queue)) != XTC_OK) {
		xtc_dio_sched_destroy(s->tuner);
		__os_free(s);
		return rc;
	}
	*out = s;
	return XTC_OK;
}

void
xtc_iosched_destroy(xtc_iosched_t *s)
{
	if (s == NULL) return;
	xtc_dio_sched_destroy(s->tuner);
	__os_free(s->queue);
	__os_free(s);
}

int
xtc_iosched_flush(xtc_iosched_t *s)
{
	int64_t t0 = 0, t1 = 0;
	uint64_t batch_bytes = 0;
	int i, rc = XTC_OK;

	if (s == NULL) return XTC_E_INVAL;
	if (s->n == 0) return XTC_OK;

	(void)__os_clock_mono(&t0);
	for (i = 0; i < s->n; i++) {
		int w = xtc_aio_pwrite(s->fd, s->queue[i].buf,
		    s->queue[i].len, s->queue[i].off);
		if (w < 0) { rc = w; break; }   /* negative errno */
		batch_bytes += (uint64_t)w;
	}
	(void)__os_clock_mono(&t1);

	s->bytes += batch_bytes;
	s->flushes++;
	{
		double secs = (t1 > t0) ? (double)(t1 - t0) / 1e9 : 1e-9;
		double mbps = ((double)batch_bytes / (1024.0 * 1024.0)) / secs;
		s->last_mbps = mbps;
		/* Adaptive: this flush is one evaluation of the current batch
		 * size; report throughput as fitness and adopt the tuner's
		 * next candidate batch size. */
		if (s->adaptive && s->tuner != NULL && rc == XTC_OK) {
			int g[XTC_DIO_SCHED_MAX_GENES];
			xtc_dio_sched_report(s->tuner, mbps);
			xtc_dio_sched_current(s->tuner, g);
			s->batch = g[0];
			if (s->batch < s->min_batch) s->batch = s->min_batch;
			if (s->batch > s->max_batch) s->batch = s->max_batch;
		}
	}
	s->n = 0;
	return rc;
}

int
xtc_iosched_write(xtc_iosched_t *s, const void *buf, uint32_t len, int64_t off)
{
	if (s == NULL || buf == NULL) return XTC_E_INVAL;
	s->queue[s->n].buf = buf;
	s->queue[s->n].len = len;
	s->queue[s->n].off = off;
	s->n++;
	s->writes++;
	if (s->n >= s->batch)
		return xtc_iosched_flush(s);
	return XTC_OK;
}

void
xtc_iosched_get_stats(const xtc_iosched_t *s, xtc_iosched_stats_t *out)
{
	if (s == NULL || out == NULL) return;
	out->writes = s->writes;
	out->bytes = s->bytes;
	out->flushes = s->flushes;
	out->cur_batch = s->batch;
	out->last_mbps = s->last_mbps;
	out->mutation_rate = s->tuner ? xtc_dio_sched_mutation_rate(s->tuner) : 0.0;
}
