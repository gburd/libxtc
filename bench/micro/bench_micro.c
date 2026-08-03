/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * bench/micro/bench_micro.c
 *	Microbenchmark REGRESSION suite -- guards the critical hot paths
 *	release-over-release.  Unlike the exploratory benches in bench,
 *	this driver's job is a STABLE per-op number a checker can compare
 *	against a stored baseline: if a future change slows a hot path
 *	(e.g. a lock acquire going 2x), the release gate flags it.
 *
 *	Each case is a tight, warmed, N-iteration loop reporting ns/op,
 *	measured with the MONOTONIC clock (xtc_clock_mono) and reported
 *	as the MINIMUM of several runs.  Microbench noise is real and
 *	one-sided: outside interference (other processes, cache
 *	eviction, migration) only ever makes a run SLOWER, never faster,
 *	so the fastest run is the least-perturbed estimate of the
 *	primitive's true cost.  On a loaded host the median still absorbs
 *	that interference; the minimum is the robust floor a regression
 *	gate needs.  Single-threaded / single-loop where that isolates
 *	the primitive.
 *
 *	Output: CSV to stdout, one row per benchmark:
 *	    name,ns_per_op,ops_per_sec
 *	with a leading `#`-comment header (ignored by the checker).
 *
 *	The cases:
 *	  proc_send_recv     xtc_send + xtc_recv round-trip, two procs, one loop
 *	  fiber_switch       xtc_yield throughput (full cooperative yield)
 *	  lwlock_excl        xtc_lwlock acquire+release, exclusive, uncontended
 *	  lwlock_shared      xtc_lwlock acquire+release, shared, uncontended
 *	  lrlock_read        xtc_lrlock wait-free read begin/end
 *	  amutex_lock        xtc_amutex lock+unlock, uncontended
 *	  deque_push_pop     xtc_deque push+pop (owner LIFO fast path)
 *	  deque_steal        xtc_deque push+steal (thief FIFO CAS path)
 *	  slab_alloc_free    xtc_slab alloc+free (magazine fast path)
 *	  malloc_free        malloc+free baseline (for the slab comparison)
 *	  chan_mpsc          xtc_chan_mpsc try_send + try_recv
 *	  chan_mpmc          xtc_chan_mpmc try_send + try_recv
 *	  chash_get          xtc_chash get (wait-free reader)
 *	  chash_insert       xtc_chash insert (overwrite, non-allocating)
 *	  cskip_get          xtc_cskip get (lock-free reader)
 *	  cskip_insert       xtc_cskip insert (overwrite)
 *	  timer_set_fire     xtc_timer_set + fire on a loop
 *
 *	Usage: bench_micro [scale]
 *	  scale  multiplies the default iteration counts (default 1).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_async.h"
#include "xtc_proc.h"
#include "xtc_lwlock.h"
#include "xtc_lrlock.h"
#include "xtc_sync.h"
#include "xtc_slab.h"
#include "xtc_chan.h"
#include "xtc_chash.h"
#include "xtc_cskip.h"
#include "xtc_rcu.h"
#include "deque.h"

/* Number of timed runs per benchmark; report the minimum (the robust
 * floor under a loaded machine -- see the file header). */
#define N_RUNS 9

static long g_scale = 1;

static int64_t
now_ns(void)
{
	return xtc_clock_mono();
}

static double
best(const double *v, int n)
{
	double m = v[0];
	int i;
	for (i = 1; i < n; i++)
		if (v[i] < m)
			m = v[i];
	return m;
}

static void
emit(const char *name, double ns_per_op)
{
	double ops = ns_per_op > 0.0 ? 1e9 / ns_per_op : 0.0;
	printf("%s,%.2f,%.0f\n", name, ns_per_op, ops);
	fflush(stdout);
}

/* ---- lwlock (exclusive + shared, uncontended) --------------------- */

static double
run_lwlock(xtc_lwlock_mode_t mode)
{
	long iters = 5000000L * g_scale, i;
	xtc_lwlock_t lk;
	int64_t t0, t1;

	if (xtc_lwlock_init(&lk, 1) != XTC_OK)
		return -1.0;
	for (i = 0; i < 100000; i++) {	/* warm */
		(void)xtc_lwlock_acquire(&lk, mode);
		xtc_lwlock_release(&lk);
	}
	t0 = now_ns();
	for (i = 0; i < iters; i++) {
		(void)xtc_lwlock_acquire(&lk, mode);
		xtc_lwlock_release(&lk);
	}
	t1 = now_ns();
	xtc_lwlock_destroy(&lk);
	return (double)(t1 - t0) / (double)iters;
}

static double bench_lwlock_excl(void)   { return run_lwlock(XTC_LW_EXCLUSIVE); }
static double bench_lwlock_shared(void) { return run_lwlock(XTC_LW_SHARED); }

/* ---- lrlock wait-free read ---------------------------------------- */

static void
lr_sync(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }

static double
bench_lrlock_read(void)
{
	long iters = 5000000L * g_scale, i;
	xtc_lrlock_t *lr = NULL;
	int64_t t0, t1;
	volatile int64_t sink = 0;

	if (xtc_lrlock_create(sizeof(int64_t), NULL, lr_sync, "bench", &lr)
	    != XTC_OK)
		return -1.0;
	/* Seed a value into both copies. */
	{
		int64_t *w = xtc_lrlock_write_begin(lr);
		*w = 42;
		xtc_lrlock_publish_full_sync(lr);
		xtc_lrlock_write_end(lr);
	}
	for (i = 0; i < 100000; i++) {	/* warm + register reader slot */
		const int64_t *r = xtc_lrlock_read_begin(lr);
		if (r != NULL)
			sink += *r;
		xtc_lrlock_read_end(lr);
	}
	t0 = now_ns();
	for (i = 0; i < iters; i++) {
		const int64_t *r = xtc_lrlock_read_begin(lr);
		if (r != NULL)
			sink += *r;
		xtc_lrlock_read_end(lr);
	}
	t1 = now_ns();
	xtc_lrlock_destroy(lr);
	(void)sink;
	return (double)(t1 - t0) / (double)iters;
}

/* ---- amutex lock/unlock (uncontended, off a loop) ----------------- */

static double
bench_amutex_lock(void)
{
	long iters = 5000000L * g_scale, i;
	xtc_amutex_t *m = NULL;
	int64_t t0, t1;

	if (xtc_amutex_create(&m) != XTC_OK)
		return -1.0;
	for (i = 0; i < 100000; i++) {	/* warm */
		(void)xtc_amutex_lock(m, -1);
		(void)xtc_amutex_unlock(m);
	}
	t0 = now_ns();
	for (i = 0; i < iters; i++) {
		(void)xtc_amutex_lock(m, -1);
		(void)xtc_amutex_unlock(m);
	}
	t1 = now_ns();
	xtc_amutex_destroy(m);
	return (double)(t1 - t0) / (double)iters;
}

/* ---- deque push/pop and push/steal -------------------------------- */

static double
bench_deque_push_pop(void)
{
	long iters = 20000000L * g_scale, i;
	xtc_deque_t d;
	int64_t t0, t1;
	long v = 1;

	xtc_deque_init(&d);
	for (i = 0; i < 100000; i++) {	/* warm */
		(void)xtc_deque_push(&d, &v);
		(void)xtc_deque_pop(&d);
	}
	t0 = now_ns();
	for (i = 0; i < iters; i++) {
		(void)xtc_deque_push(&d, &v);
		(void)xtc_deque_pop(&d);
	}
	t1 = now_ns();
	/* One push+pop pair per iteration. */
	return (double)(t1 - t0) / (double)iters;
}

static double
bench_deque_steal(void)
{
	long iters = 20000000L * g_scale, i;
	xtc_deque_t d;
	int64_t t0, t1;
	long v = 1;

	xtc_deque_init(&d);
	for (i = 0; i < 100000; i++) {	/* warm */
		(void)xtc_deque_push(&d, &v);
		(void)xtc_deque_steal(&d);
	}
	t0 = now_ns();
	for (i = 0; i < iters; i++) {
		(void)xtc_deque_push(&d, &v);
		(void)xtc_deque_steal(&d);
	}
	t1 = now_ns();
	return (double)(t1 - t0) / (double)iters;
}

/* ---- slab alloc/free vs malloc/free ------------------------------- */

static double
bench_slab_alloc_free(void)
{
	long iters = 10000000L * g_scale, i;
	xtc_slab_t *s = NULL;
	xtc_slab_opts_t o = XTC_SLAB_OPTS_DEFAULT;
	int64_t t0, t1;

	o.name = "bench";
	o.obj_size = 64;
	if (xtc_slab_create(&o, &s) != XTC_OK)
		return -1.0;
	for (i = 0; i < 100000; i++) {	/* warm */
		void *p = xtc_slab_alloc(s);
		if (p != NULL)
			xtc_slab_free(s, p);
	}
	t0 = now_ns();
	for (i = 0; i < iters; i++) {
		void *p = xtc_slab_alloc(s);
		if (p != NULL)
			xtc_slab_free(s, p);
	}
	t1 = now_ns();
	xtc_slab_destroy(s);
	return (double)(t1 - t0) / (double)iters;
}

static double
bench_malloc_free(void)
{
	long iters = 10000000L * g_scale, i;
	int64_t t0, t1;
	volatile unsigned char sink = 0;

	for (i = 0; i < 100000; i++) {	/* warm */
		void *p = malloc(64);
		if (p != NULL) {
			((unsigned char *)p)[0] = 1;
			free(p);
		}
	}
	t0 = now_ns();
	for (i = 0; i < iters; i++) {
		void *p = malloc(64);
		if (p != NULL) {
			sink += ((unsigned char *)p)[0] = 1;
			free(p);
		}
	}
	t1 = now_ns();
	(void)sink;
	return (double)(t1 - t0) / (double)iters;
}

/* ---- channels: mpsc + mpmc try_send/try_recv ---------------------- */

static double
bench_chan_mpsc(void)
{
	long iters = 5000000L * g_scale, i;
	xtc_chan_mpsc_t *c = NULL;
	int64_t t0, t1;
	uintptr_t payload = 0xabc;

	if (xtc_chan_mpsc_create(NULL, 1024, &c) != XTC_OK)
		return -1.0;
	for (i = 0; i < 100000; i++) {	/* warm */
		void *out = NULL;
		(void)xtc_chan_mpsc_try_send(c, (void *)payload);
		(void)xtc_chan_mpsc_try_recv(c, &out);
	}
	t0 = now_ns();
	for (i = 0; i < iters; i++) {
		void *out = NULL;
		(void)xtc_chan_mpsc_try_send(c, (void *)payload);
		(void)xtc_chan_mpsc_try_recv(c, &out);
	}
	t1 = now_ns();
	xtc_chan_mpsc_destroy(c);
	return (double)(t1 - t0) / (double)iters;
}

static double
bench_chan_mpmc(void)
{
	long iters = 5000000L * g_scale, i;
	xtc_chan_mpmc_t *c = NULL;
	int64_t t0, t1;
	uintptr_t payload = 0xabc;

	if (xtc_chan_mpmc_create(NULL, 1024, &c) != XTC_OK)
		return -1.0;
	for (i = 0; i < 100000; i++) {	/* warm */
		void *out = NULL;
		(void)xtc_chan_mpmc_try_send(c, (void *)payload);
		(void)xtc_chan_mpmc_try_recv(c, &out);
	}
	t0 = now_ns();
	for (i = 0; i < iters; i++) {
		void *out = NULL;
		(void)xtc_chan_mpmc_try_send(c, (void *)payload);
		(void)xtc_chan_mpmc_try_recv(c, &out);
	}
	t1 = now_ns();
	xtc_chan_mpmc_destroy(c);
	return (double)(t1 - t0) / (double)iters;
}

/* ---- chash get / insert ------------------------------------------- */

struct kv { int64_t k; int64_t v; };

static int
kv_cmp(const void *a, const void *b)
{
	int64_t x = ((const struct kv *)a)->k, y = ((const struct kv *)b)->k;
	return x < y ? -1 : (x > y ? 1 : 0);
}
static uint64_t
kv_hash(const void *key)
{
	uint64_t x = (uint64_t)((const struct kv *)key)->k;
	x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
	return x;
}

#define MAP_KEYS 4096

static double
bench_chash(int do_insert)
{
	long iters = 5000000L * g_scale, i;
	xtc_chash_t *h = NULL;
	struct kv *boxes;
	int64_t t0, t1;
	volatile int64_t sink = 0;

	if (xtc_chash_create(kv_cmp, kv_hash, 64, &h) != XTC_OK)
		return -1.0;
	boxes = calloc(MAP_KEYS, sizeof *boxes);
	if (boxes == NULL) {
		xtc_chash_destroy(h);
		return -1.0;
	}
	for (i = 0; i < MAP_KEYS; i++) {
		void *old = NULL;
		boxes[i].k = i;
		boxes[i].v = i * 7 + 1;
		(void)xtc_chash_insert(h, &boxes[i], &boxes[i], &old);
	}
	t0 = now_ns();
	if (do_insert) {
		for (i = 0; i < iters; i++) {
			void *old = NULL;
			long k = i % MAP_KEYS;
			(void)xtc_chash_insert(h, &boxes[k], &boxes[k], &old);
		}
	} else {
		for (i = 0; i < iters; i++) {
			struct kv q;
			void *v = NULL;
			q.k = i % MAP_KEYS;
			xtc_rcu_read_lock();
			if (xtc_chash_get(h, &q, &v) == XTC_OK && v != NULL)
				sink += ((struct kv *)v)->v;
			xtc_rcu_read_unlock();
		}
	}
	t1 = now_ns();
	xtc_chash_destroy(h);
	xtc_rcu_synchronize();
	free(boxes);
	(void)sink;
	return (double)(t1 - t0) / (double)iters;
}

static double bench_chash_get(void)    { return bench_chash(0); }
static double bench_chash_insert(void) { return bench_chash(1); }

/* ---- cskip get / insert ------------------------------------------- */

static double
bench_cskip(int do_insert)
{
	long iters = 2000000L * g_scale, i;
	xtc_cskip_t *s = NULL;
	struct kv *boxes;
	int64_t t0, t1;
	volatile int64_t sink = 0;

	if (xtc_cskip_create(kv_cmp, &s) != XTC_OK)
		return -1.0;
	boxes = calloc(MAP_KEYS, sizeof *boxes);
	if (boxes == NULL) {
		xtc_cskip_destroy(s);
		return -1.0;
	}
	for (i = 0; i < MAP_KEYS; i++) {
		void *old = NULL;
		boxes[i].k = i;
		boxes[i].v = i * 7 + 1;
		(void)xtc_cskip_insert(s, &boxes[i], &boxes[i], &old);
	}
	t0 = now_ns();
	if (do_insert) {
		for (i = 0; i < iters; i++) {
			void *old = NULL;
			long k = i % MAP_KEYS;
			(void)xtc_cskip_insert(s, &boxes[k], &boxes[k], &old);
		}
	} else {
		for (i = 0; i < iters; i++) {
			struct kv q;
			void *v = NULL;
			q.k = i % MAP_KEYS;
			xtc_rcu_read_lock();
			if (xtc_cskip_get(s, &q, &v) == XTC_OK && v != NULL)
				sink += ((struct kv *)v)->v;
			xtc_rcu_read_unlock();
		}
	}
	t1 = now_ns();
	xtc_cskip_destroy(s);
	xtc_rcu_synchronize();
	free(boxes);
	(void)sink;
	return (double)(t1 - t0) / (double)iters;
}

static double bench_cskip_get(void)    { return bench_cskip(0); }
static double bench_cskip_insert(void) { return bench_cskip(1); }

/* ---- fiber switch (xtc_yield throughput) -------------------------- */

static long        g_yield_iters;
static int64_t     g_yield_t0, g_yield_t1;

static intptr_t
yield_coro(void *arg)
{
	long i;
	(void)arg;
	for (i = 0; i < 100000; i++)	/* warm */
		xtc_yield();
	g_yield_t0 = now_ns();
	for (i = 0; i < g_yield_iters; i++)
		xtc_yield();
	g_yield_t1 = now_ns();
	return 0;
}

static double
bench_fiber_switch(void)
{
	xtc_loop_t *loop = NULL;
	xtc_task_t *t = NULL;

	g_yield_iters = 10000000L * g_scale;
	if (xtc_loop_init(&loop) != XTC_OK)
		return -1.0;
	if (xtc_async(loop, yield_coro, NULL, &t) != XTC_OK) {
		(void)xtc_loop_fini(loop);
		return -1.0;
	}
	(void)xtc_loop_run(loop);
	(void)xtc_loop_fini(loop);
	/*
	 * Report ns per xtc_yield -- the full cooperative yield the
	 * scheduler actually executes (fiber -> loop -> fiber), which
	 * includes stack-tail reclaim, per-fiber TLS save/restore, and
	 * the run-queue turn, NOT just the raw fcontext register swap.
	 * The ~7.6 ns/swap claim is the bare __xtc_jump_fcontext cost;
	 * this number is deliberately the higher, consumer-visible one
	 * (what a fiber that yields really pays), so a regression in any
	 * part of the yield path -- not only the asm swap -- is caught.
	 */
	return (double)(g_yield_t1 - g_yield_t0) / (double)g_yield_iters;
}

/* ---- proc send/recv round-trip ------------------------------------ */

static long      g_pp_rounds;
static xtc_pid_t g_pp_pong;
static int64_t   g_pp_t0, g_pp_t1;

struct pp_msg { xtc_pid_t from; long n; };

static void
pp_pong(void *arg)
{
	(void)arg;
	for (;;) {
		void *m = NULL;
		size_t sz = 0;
		struct pp_msg req, reply;
		if (xtc_recv(&m, &sz, 1000LL * 1000 * 1000) != XTC_OK)
			return;
		if (sz != sizeof req) {
			if (m != NULL)
				xtc_free(m);
			continue;
		}
		memcpy(&req, m, sizeof req);
		xtc_free(m);
		if (req.n < 0)
			return;	/* sentinel: stop */
		reply.from = xtc_self();
		reply.n = req.n;
		(void)xtc_send(req.from, &reply, sizeof reply);
	}
}

static void
pp_ping(void *arg)
{
	long i;
	(void)arg;
	/* Warm. */
	for (i = 0; i < 50000; i++) {
		void *m = NULL;
		size_t sz = 0;
		struct pp_msg req = { xtc_self(), i };
		(void)xtc_send(g_pp_pong, &req, sizeof req);
		if (xtc_recv(&m, &sz, 1000LL * 1000 * 1000) != XTC_OK)
			return;
		if (m != NULL)
			xtc_free(m);
	}
	g_pp_t0 = now_ns();
	for (i = 0; i < g_pp_rounds; i++) {
		void *m = NULL;
		size_t sz = 0;
		struct pp_msg req = { xtc_self(), i };
		(void)xtc_send(g_pp_pong, &req, sizeof req);
		if (xtc_recv(&m, &sz, 1000LL * 1000 * 1000) != XTC_OK)
			return;
		if (m != NULL)
			xtc_free(m);
	}
	g_pp_t1 = now_ns();
	/* Stop pong. */
	{
		struct pp_msg stop = { xtc_self(), -1 };
		(void)xtc_send(g_pp_pong, &stop, sizeof stop);
	}
}

static double
bench_proc_send_recv(void)
{
	xtc_loop_t *loop = NULL;
	xtc_pid_t ping_pid;

	g_pp_rounds = 2000000L * g_scale;
	if (xtc_loop_init(&loop) != XTC_OK)
		return -1.0;
	if (xtc_proc_spawn(loop, pp_pong, NULL, NULL, &g_pp_pong) != XTC_OK ||
	    xtc_proc_spawn(loop, pp_ping, NULL, NULL, &ping_pid) != XTC_OK) {
		(void)xtc_loop_fini(loop);
		return -1.0;
	}
	(void)xtc_loop_run(loop);
	(void)xtc_loop_fini(loop);
	/* One round = one send + one recv + one reply send + one recv. */
	return (double)(g_pp_t1 - g_pp_t0) / (double)g_pp_rounds;
}

/* ---- timer set + fire --------------------------------------------- */

static long        g_tm_iters;
static long        g_tm_fired;
static xtc_loop_t *g_tm_loop;
static int64_t     g_tm_t0, g_tm_t1;

static void
tm_fire(void *user)
{
	(void)user;
	g_tm_fired++;
	if (g_tm_fired < g_tm_iters) {
		xtc_timer_t *nt = NULL;
		(void)xtc_timer_set(g_tm_loop, 0, tm_fire, NULL, &nt);
	} else {
		g_tm_t1 = now_ns();
		(void)xtc_loop_stop(g_tm_loop);
	}
}

static double
bench_timer_set_fire(void)
{
	xtc_timer_t *t = NULL;

	g_tm_iters = 2000000L * g_scale;
	g_tm_fired = 0;
	if (xtc_loop_init(&g_tm_loop) != XTC_OK)
		return -1.0;
	g_tm_t0 = now_ns();
	if (xtc_timer_set(g_tm_loop, 0, tm_fire, NULL, &t) != XTC_OK) {
		(void)xtc_loop_fini(g_tm_loop);
		return -1.0;
	}
	(void)xtc_loop_run(g_tm_loop);
	(void)xtc_loop_fini(g_tm_loop);
	/* Each iteration is one set + one fire. */
	return (double)(g_tm_t1 - g_tm_t0) / (double)g_tm_iters;
}

/* ---- driver ------------------------------------------------------- */

struct case_t {
	const char *name;
	double    (*fn)(void);
};

static const struct case_t g_cases[] = {
	{ "proc_send_recv",  bench_proc_send_recv },
	{ "fiber_switch",    bench_fiber_switch },
	{ "lwlock_excl",     bench_lwlock_excl },
	{ "lwlock_shared",   bench_lwlock_shared },
	{ "lrlock_read",     bench_lrlock_read },
	{ "amutex_lock",     bench_amutex_lock },
	{ "deque_push_pop",  bench_deque_push_pop },
	{ "deque_steal",     bench_deque_steal },
	{ "slab_alloc_free", bench_slab_alloc_free },
	{ "malloc_free",     bench_malloc_free },
	{ "chan_mpsc",       bench_chan_mpsc },
	{ "chan_mpmc",       bench_chan_mpmc },
	{ "chash_get",       bench_chash_get },
	{ "chash_insert",    bench_chash_insert },
	{ "cskip_get",       bench_cskip_get },
	{ "cskip_insert",    bench_cskip_insert },
	{ "timer_set_fire",  bench_timer_set_fire },
};

int
main(int argc, char **argv)
{
	size_t k;
	int rc = 0;

	if (argc > 1) {
		g_scale = atol(argv[1]);
		if (g_scale < 1)
			g_scale = 1;
	}
	if (xtc_rcu_init() != XTC_OK) {
		fprintf(stderr, "rcu_init failed\n");
		return 1;
	}

	printf("# xtc microbenchmark suite (scale=%ld, min of %d runs)\n",
	    g_scale, N_RUNS);
	printf("name,ns_per_op,ops_per_sec\n");

	for (k = 0; k < sizeof g_cases / sizeof g_cases[0]; k++) {
		double runs[N_RUNS];
		int r;
		for (r = 0; r < N_RUNS; r++) {
			runs[r] = g_cases[k].fn();
			if (runs[r] < 0.0) {
				fprintf(stderr, "FAIL: %s errored\n",
				    g_cases[k].name);
				rc = 1;
				break;
			}
		}
		if (r == N_RUNS)
			emit(g_cases[k].name, best(runs, N_RUNS));
	}

	xtc_rcu_fini();
	return rc;
}
