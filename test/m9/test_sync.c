/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m9/test_sync.c -- verifies M9 notify + sem + abort_source.
 */

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE     /* RUSAGE_THREAD (the off-loop no-herd test) */
#endif

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#if defined(__linux__)
#include <sys/syscall.h>  /* SYS_gettid (offloop_release_wakes) */
#include <unistd.h>
#endif
#include <time.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_sync.h"
#include "xtc_future.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_blocking.h"
#include "xtc_exec.h"
#include "proc_int.h"   /* XTC_HAVE_PROC_KILL_WAKE once the proc branch lands */
#include "os_time.h"

/* notify: stored signal, drains on first wait. */
static MunitResult
test_notify_stored(const MunitParameter p[], void *d)
{
	xtc_notify_t *n;
	(void)p; (void)d;
	munit_assert_int(xtc_notify_create(&n), ==, XTC_OK);
	munit_assert_int(xtc_notify_signal(n), ==, XTC_OK);
	munit_assert_int(xtc_notify_signal(n), ==, XTC_OK);   /* coalesce */
	munit_assert_int(xtc_notify_wait(n, 0), ==, XTC_OK);
	munit_assert_int(xtc_notify_wait(n, 0), ==, XTC_E_AGAIN);
	xtc_notify_destroy(n);
	return MUNIT_OK;
}

/* notify: cross-thread wake. */
struct nt { xtc_notify_t *n; int delay_ms; };
static void *
nt_signaler(void *arg)
{
	struct nt *t = arg;
	(void)__os_sleep_ns((int64_t)t->delay_ms * 1000 * 1000);
	(void)xtc_notify_signal(t->n);
	return NULL;
}

static MunitResult
test_notify_cross_thread(const MunitParameter p[], void *d)
{
	xtc_notify_t *n;
	pthread_t th;
	struct nt t;
	int64_t before, after;
	(void)p; (void)d;
	munit_assert_int(xtc_notify_create(&n), ==, XTC_OK);
	t.n = n; t.delay_ms = 20;
	pthread_create(&th, NULL, nt_signaler, &t);
	(void)__os_clock_mono(&before);
	munit_assert_int(xtc_notify_wait(n, -1), ==, XTC_OK);
	(void)__os_clock_mono(&after);
	pthread_join(th, NULL);
	munit_assert_int64(after - before, >=, 15 * 1000 * 1000);
	munit_assert_int64(after - before, <=, 500 * 1000 * 1000);
	xtc_notify_destroy(n);
	return MUNIT_OK;
}

/* sem: basic count semantics. */
static MunitResult
test_sem_basic(const MunitParameter p[], void *d)
{
	xtc_sem_t *s;
	(void)p; (void)d;
	munit_assert_int(xtc_sem_create(3, &s), ==, XTC_OK);
	munit_assert_int(xtc_sem_count(s), ==, 3);
	munit_assert_int(xtc_sem_try_acquire(s, 1), ==, XTC_OK);
	munit_assert_int(xtc_sem_count(s), ==, 2);
	munit_assert_int(xtc_sem_try_acquire(s, 5), ==, XTC_E_AGAIN);
	munit_assert_int(xtc_sem_post(s, 10), ==, XTC_OK);
	munit_assert_int(xtc_sem_count(s), ==, 12);
	munit_assert_int(xtc_sem_acquire(s, 12, 0), ==, XTC_OK);
	munit_assert_int(xtc_sem_count(s), ==, 0);
	xtc_sem_destroy(s);
	return MUNIT_OK;
}

/* abort_source: token observes fire. */
static MunitResult
test_abort_source(const MunitParameter p[], void *d)
{
	xtc_abort_source_t *src;
	xtc_abort_token_t  t1, t2;
	(void)p; (void)d;
	munit_assert_int(xtc_abort_source_create(&src), ==, XTC_OK);
	munit_assert_int(xtc_abort_source_token(src, &t1), ==, XTC_OK);
	munit_assert_int(xtc_abort_token_is_aborted(&t1), ==, 0);
	munit_assert_int(xtc_abort_source_fire(src, 42), ==, XTC_OK);
	munit_assert_int(xtc_abort_token_is_aborted(&t1), ==, 1);
	munit_assert_int(xtc_abort_token_reason(&t1), ==, 42);
	/* Tokens minted after firing also report aborted. */
	munit_assert_int(xtc_abort_source_token(src, &t2), ==, XTC_OK);
	munit_assert_int(xtc_abort_token_is_aborted(&t2), ==, 1);
	xtc_abort_source_destroy(src);
	return MUNIT_OK;
}

/* amutex: basic + try */
static MunitResult
test_amutex_basic(const MunitParameter p[], void *d)
{
	xtc_amutex_t *m;
	(void)p; (void)d;
	munit_assert_int(xtc_amutex_create(&m), ==, XTC_OK);
	munit_assert_int(xtc_amutex_lock(m, 0), ==, XTC_OK);
	munit_assert_int(xtc_amutex_try_lock(m), ==, XTC_E_AGAIN);
	munit_assert_int(xtc_amutex_unlock(m), ==, XTC_OK);
	munit_assert_int(xtc_amutex_try_lock(m), ==, XTC_OK);
	munit_assert_int(xtc_amutex_unlock(m), ==, XTC_OK);
	xtc_amutex_destroy(m);
	return MUNIT_OK;
}

/* amutex: N-thread mutual exclusion. */
#define MX_THREADS 4
#define MX_ITERS   10000
static xtc_amutex_t *mx_m;
static int64_t       mx_counter;
static void *
mx_worker(void *arg) {
	int i;
	(void)arg;
	for (i = 0; i < MX_ITERS; i++) {
		(void)xtc_amutex_lock(mx_m, -1);
		mx_counter++;
		(void)xtc_amutex_unlock(mx_m);
	}
	return NULL;
}
static MunitResult
test_amutex_mutex(const MunitParameter p[], void *d)
{
	pthread_t th[MX_THREADS];
	int i;
	(void)p; (void)d;
	munit_assert_int(xtc_amutex_create(&mx_m), ==, XTC_OK);
	mx_counter = 0;
	for (i = 0; i < MX_THREADS; i++) pthread_create(&th[i], NULL, mx_worker, NULL);
	for (i = 0; i < MX_THREADS; i++) pthread_join(th[i], NULL);
	munit_assert_int64(mx_counter, ==, (int64_t)MX_THREADS * MX_ITERS);
	xtc_amutex_destroy(mx_m);
	return MUNIT_OK;
}

/* rwlock: basic */
static MunitResult
test_rwlock_basic(const MunitParameter p[], void *d)
{
	xtc_rwlock_t *r;
	(void)p; (void)d;
	munit_assert_int(xtc_rwlock_create(&r), ==, XTC_OK);
	munit_assert_int(xtc_rwlock_rdlock(r, 0), ==, XTC_OK);
	munit_assert_int(xtc_rwlock_rdlock(r, 0), ==, XTC_OK);
	munit_assert_int(xtc_rwlock_wrlock(r, 0), ==, XTC_E_AGAIN); /* readers held */
	munit_assert_int(xtc_rwlock_unlock(r), ==, XTC_OK);
	munit_assert_int(xtc_rwlock_unlock(r), ==, XTC_OK);
	munit_assert_int(xtc_rwlock_wrlock(r, 0), ==, XTC_OK);
	munit_assert_int(xtc_rwlock_rdlock(r, 0), ==, XTC_E_AGAIN); /* writer held */
	munit_assert_int(xtc_rwlock_unlock(r), ==, XTC_OK);
	xtc_rwlock_destroy(r);
	return MUNIT_OK;
}

/* barrier: 4 threads rendezvous */
#define BR_N 4
static xtc_barrier_t *br_b;
static _Atomic int    br_phase;
static int            br_after_phase[BR_N];
static void *
br_worker(void *arg) {
	int id = (int)(intptr_t)arg;
	atomic_fetch_add_explicit(&br_phase, 1, memory_order_relaxed);
	(void)xtc_barrier_wait(br_b);
	br_after_phase[id] = atomic_load_explicit(&br_phase, memory_order_relaxed);
	return NULL;
}
static MunitResult
test_barrier(const MunitParameter p[], void *d)
{
	pthread_t th[BR_N];
	int i;
	(void)p; (void)d;
	munit_assert_int(xtc_barrier_create(BR_N, &br_b), ==, XTC_OK);
	atomic_store(&br_phase, 0);
	for (i = 0; i < BR_N; i++) pthread_create(&th[i], NULL, br_worker, (void *)(intptr_t)i);
	for (i = 0; i < BR_N; i++) pthread_join(th[i], NULL);
	/* All threads see the full N count after the barrier. */
	for (i = 0; i < BR_N; i++) munit_assert_int(br_after_phase[i], ==, BR_N);
	xtc_barrier_destroy(br_b);
	return MUNIT_OK;
}

/* gate: enter/leave + drain */
static MunitResult
test_gate(const MunitParameter p[], void *d)
{
	xtc_gate_t *g;
	(void)p; (void)d;
	munit_assert_int(xtc_gate_create(&g), ==, XTC_OK);
	munit_assert_int(xtc_gate_enter(g), ==, XTC_OK);
	munit_assert_int(xtc_gate_enter(g), ==, XTC_OK);
	munit_assert_int(xtc_gate_count(g), ==, 2);
	munit_assert_int(xtc_gate_close(g), ==, XTC_OK);
	munit_assert_int(xtc_gate_enter(g), ==, XTC_E_INVAL);
	munit_assert_int(xtc_gate_drain(g, 0), ==, XTC_E_AGAIN);
	munit_assert_int(xtc_gate_leave(g), ==, XTC_OK);
	munit_assert_int(xtc_gate_leave(g), ==, XTC_OK);
	munit_assert_int(xtc_gate_drain(g, 0), ==, XTC_OK);
	xtc_gate_destroy(g);
	return MUNIT_OK;
}

/* amutex fiber path: procs hold the lock across a real park (a
 * blocking-pool offload) while others contend.  If the lock blocked
 * the OS thread, the first holder's park would wedge the loop the
 * moment a second proc contended -- deadlock.  With fiber yielding,
 * waiters park and the holder is still woken, so all complete and
 * mutual exclusion holds. */
static xtc_amutex_t *g_am;
static _Atomic int   g_in_cs;
static _Atomic int   g_overlap;
static _Atomic int   g_cs_count;

static int
cs_sleep(void *a)
{
	struct timespec ts = { 0, 8 * 1000 * 1000 };  /* 8 ms */
	(void)a;
	(void)nanosleep(&ts, NULL);
	return 0;
}

static void
cs_proc(void *arg)
{
	int iters = (int)(intptr_t)arg;
	int i, r;
	for (i = 0; i < iters; i++) {
		(void)xtc_amutex_lock(g_am, -1);
		if (atomic_fetch_add(&g_in_cs, 1) + 1 > 1)
			atomic_store(&g_overlap, 1);
		/* Hold the lock across a park: the deadlock scenario. */
		(void)xtc_blocking_run(cs_sleep, NULL, &r);
		atomic_fetch_sub(&g_in_cs, 1);
		atomic_fetch_add(&g_cs_count, 1);
		(void)xtc_amutex_unlock(g_am);
	}
}

static MunitResult
test_amutex_fiber(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t pid;
	int i;
	(void)p; (void)d;

	atomic_store(&g_in_cs, 0);
	atomic_store(&g_overlap, 0);
	atomic_store(&g_cs_count, 0);
	munit_assert_int(xtc_amutex_create(&g_am), ==, XTC_OK);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	for (i = 0; i < 3; i++) {
		opts.name = "cs";
		munit_assert_int(xtc_proc_spawn(loop, cs_proc,
		    (void *)(intptr_t)3, &opts, &pid), ==, XTC_OK);
	}
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	xtc_amutex_destroy(g_am);
	g_am = NULL;
	xtc_blocking_shutdown();

	/* 3 procs * 3 iterations, all serialized, none overlapping. */
	munit_assert_int(atomic_load(&g_cs_count), ==, 9);
	munit_assert_int(atomic_load(&g_overlap), ==, 0);
	return MUNIT_OK;
}

/* arwlock: mutual exclusion under concurrent fiber readers + writers,
 * and the cooperative-safety property -- a writer parks while holding
 * the exclusive latch and contenders park (not thread-block). */
static xtc_arwlock_t *g_arw;
static _Atomic int g_arw_value;        /* protected counter */
static _Atomic int g_arw_inwrite;      /* writers currently inside crit */
static _Atomic int g_arw_inread;       /* readers currently inside crit */
static _Atomic int g_arw_excl_viol;    /* writer overlapped any peer */
static _Atomic int g_arw_read_viol;    /* reader saw a torn/odd value */
static _Atomic int g_arw_writes;

static void
arw_writer(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < 50; i++) {
		(void)xtc_arwlock_wrlock(g_arw, -1);
		if (atomic_fetch_add(&g_arw_inwrite, 1) != 0 ||
		    atomic_load(&g_arw_inread) != 0)
			atomic_store(&g_arw_excl_viol, 1);
		/* Make the value briefly odd, then even -- a reader that sees
		 * odd under a shared latch would prove exclusion broke.  Park
		 * mid-critical-section to exercise holder-parks-while-latched. */
		atomic_fetch_add(&g_arw_value, 1);          /* odd */
		(void)xtc_proc_sleep(200LL * 1000);          /* 0.2ms park */
		atomic_fetch_add(&g_arw_value, 1);          /* even */
		atomic_fetch_sub(&g_arw_inwrite, 1);
		atomic_fetch_add(&g_arw_writes, 1);
		(void)xtc_arwlock_unlock(g_arw);
		{ void *m=NULL; size_t n=0; (void)xtc_recv(&m,&n, 100LL*1000); if(m) m=NULL; }
	}
}
static void
arw_reader(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < 80; i++) {
		int v;
		(void)xtc_arwlock_rdlock(g_arw, -1);
		atomic_fetch_add(&g_arw_inread, 1);
		if (atomic_load(&g_arw_inwrite) != 0)
			atomic_store(&g_arw_read_viol, 1);   /* writer overlapped */
		v = atomic_load(&g_arw_value);
		(void)xtc_proc_sleep(100LL * 1000);
		if ((v & 1) != 0 || atomic_load(&g_arw_value) != v)
			atomic_store(&g_arw_read_viol, 1);   /* torn / odd value */
		atomic_fetch_sub(&g_arw_inread, 1);
		(void)xtc_arwlock_unlock(g_arw);
		{ void *m=NULL; size_t n=0; (void)xtc_recv(&m,&n, 100LL*1000); if(m) m=NULL; }
	}
}

static MunitResult
test_arwlock_fiber(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t o = { 0 };
	xtc_pid_t pid;
	int i;
	(void)p; (void)d;

	munit_assert_int(xtc_arwlock_create(&g_arw), ==, XTC_OK);
	atomic_store(&g_arw_value, 0);
	atomic_store(&g_arw_inwrite, 0); atomic_store(&g_arw_inread, 0);
	atomic_store(&g_arw_excl_viol, 0); atomic_store(&g_arw_read_viol, 0);
	atomic_store(&g_arw_writes, 0);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	for (i = 0; i < 3; i++) {
		o.name = "w";
		munit_assert_int(xtc_proc_spawn(loop, arw_writer, NULL, &o, &pid), ==, XTC_OK);
	}
	for (i = 0; i < 4; i++) {
		o.name = "r";
		munit_assert_int(xtc_proc_spawn(loop, arw_reader, NULL, &o, &pid), ==, XTC_OK);
	}
	/* If the latch thread-blocked instead of parking, a writer parked
	 * mid-critical-section would wedge the single loop and this never
	 * returns. */
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);

	munit_assert_int(atomic_load(&g_arw_excl_viol), ==, 0);  /* exclusion held */
	munit_assert_int(atomic_load(&g_arw_read_viol), ==, 0);  /* no torn reads */
	munit_assert_int(atomic_load(&g_arw_writes), ==, 3 * 50);
	munit_assert_int(atomic_load(&g_arw_value), ==, 2 * 3 * 50);
	xtc_arwlock_destroy(g_arw);
	return MUNIT_OK;
}


/* ---- Fiber timed-wait EXPIRY -----------------------------------------
 *
 * The suite covered only timeout 0 (poll) and -1 (infinite).  The
 * interesting paths -- a fiber that parks on a FINITE timeout, has it
 * actually EXPIRE, and is then unlinked from the primitive's fiber wait
 * queue -- were never entered: sync.c's deadline re-arm loops and its
 * __amutex_wq_remove / __arw_wq_remove waiter-cancellation helpers.
 * Those are exactly the paths a consumer hits when a lock is contended
 * and it does not want to wait forever, so "it compiles" is not enough.
 *
 * Each holder takes the lock and keeps it well past the waiter's
 * deadline, so the waiter's timeout MUST expire.  The holder blocks in
 * xtc_blocking_run (off the loop) rather than parking on the loop, so
 * the waiter's timer can actually fire while the lock is held.
 */
#if !defined(_WIN32)   /* helpers used only by the POSIX-only body below */
static xtc_amutex_t  *g_to_m;
static xtc_arwlock_t *g_to_rw;
static atomic_int g_to_mutex_timedout;
static atomic_int g_to_rd_timedout;
static atomic_int g_to_wr_timedout;
static atomic_int g_to_holder_done;

static int
to_hold_sleep(void *arg)
{
	(void)arg;
	/* Comfortably longer than every waiter deadline below. */
	/* Hold well past every waiter's 20 ms deadline.  The margin has to
	 * absorb a COARSE timer: Windows' default timer granularity is
	 * ~15 ms, so a 20 ms deadline can land anywhere in roughly 20-35 ms,
	 * and the loop only checks deadlines when it next wakes.  400 ms is
	 * ~20x the deadline and still fast, so the assertion "the waiter
	 * timed out while the lock was held" cannot flip on timer slop. */
	(void)xtc_sleep_ns(400 * 1000 * 1000LL);   /* 400 ms */
	return 0;
}

static void
to_mutex_holder(void *arg)
{
	int r;
	(void)arg;
	if (xtc_amutex_lock(g_to_m, -1) != XTC_OK)
		return;
	(void)xtc_blocking_run(to_hold_sleep, NULL, &r);
	(void)xtc_amutex_unlock(g_to_m);
	atomic_fetch_add(&g_to_holder_done, 1);
}

static void
to_mutex_waiter(void *arg)
{
	(void)arg;
	/* Contended + finite deadline => must return XTC_E_AGAIN after the
	 * timer fires, and must unlink itself from the wait queue. */
	if (xtc_amutex_lock(g_to_m, 20 * 1000 * 1000LL) == XTC_E_AGAIN)
		atomic_fetch_add(&g_to_mutex_timedout, 1);
	else
		(void)xtc_amutex_unlock(g_to_m);
}

static void
to_rw_holder(void *arg)
{
	int r;
	(void)arg;
	if (xtc_arwlock_wrlock(g_to_rw, -1) != XTC_OK)
		return;
	(void)xtc_blocking_run(to_hold_sleep, NULL, &r);
	(void)xtc_arwlock_unlock(g_to_rw);
	atomic_fetch_add(&g_to_holder_done, 1);
}

static void
to_rw_waiters(void *arg)
{
	(void)arg;
	/* A writer holds it, so BOTH a reader and a writer must time out. */
	if (xtc_arwlock_rdlock(g_to_rw, 20 * 1000 * 1000LL) == XTC_E_AGAIN)
		atomic_fetch_add(&g_to_rd_timedout, 1);
	else
		(void)xtc_arwlock_unlock(g_to_rw);
	if (xtc_arwlock_wrlock(g_to_rw, 20 * 1000 * 1000LL) == XTC_E_AGAIN)
		atomic_fetch_add(&g_to_wr_timedout, 1);
	else
		(void)xtc_arwlock_unlock(g_to_rw);
}

#endif   /* !_WIN32 */

static MunitResult
test_fiber_timeout_expiry(const MunitParameter p[], void *d)
{
#if defined(_WIN32)
	(void)p; (void)d;
	/* This case asserts a STRICT ORDERING between two timescales: each
	 * waiter's 20 ms deadline must expire while a holder still owns the
	 * lock.  That needs the waiter to be scheduled, and its timer to be
	 * observed, DURING the hold -- and Windows' ~15 ms coarse timer floor
	 * makes a 20 ms deadline land anywhere in ~20-35 ms, with the loop
	 * only checking deadlines when it next wakes.  Widening the hold does
	 * not make the ordering deterministic, it just moves the coin flip,
	 * so this asserts a timing property the platform does not provide
	 * rather than the lock property under test.
	 *
	 * The timed-wait EXPIRY paths themselves are platform-independent and
	 * are covered on Linux/macOS, where the timer is fine-grained enough
	 * for the ordering to be reliable.  Same reasoning (and the same
	 * remedy) as m5/test_exec's Blk5/Blk6 cases. */
	return MUNIT_SKIP;
#else
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t pid;
	(void)p; (void)d;

	atomic_store(&g_to_mutex_timedout, 0);
	atomic_store(&g_to_rd_timedout, 0);
	atomic_store(&g_to_wr_timedout, 0);
	atomic_store(&g_to_holder_done, 0);

	munit_assert_int(xtc_amutex_create(&g_to_m), ==, XTC_OK);
	munit_assert_int(xtc_arwlock_create(&g_to_rw), ==, XTC_OK);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);

	/* Holders first, so they own the locks before the waiters run. */
	opts.name = "hold-m";
	munit_assert_int(xtc_proc_spawn(loop, to_mutex_holder, NULL, &opts,
	    &pid), ==, XTC_OK);
	opts.name = "wait-m";
	munit_assert_int(xtc_proc_spawn(loop, to_mutex_waiter, NULL, &opts,
	    &pid), ==, XTC_OK);
	opts.name = "hold-rw";
	munit_assert_int(xtc_proc_spawn(loop, to_rw_holder, NULL, &opts,
	    &pid), ==, XTC_OK);
	opts.name = "wait-rw";
	munit_assert_int(xtc_proc_spawn(loop, to_rw_waiters, NULL, &opts,
	    &pid), ==, XTC_OK);

	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	munit_assert_int(atomic_load(&g_to_mutex_timedout), ==, 1);
	munit_assert_int(atomic_load(&g_to_rd_timedout), ==, 1);
	munit_assert_int(atomic_load(&g_to_wr_timedout), ==, 1);
	/* Both holders must have released cleanly -- a timed-out waiter
	 * must not corrupt the queue or strand the holder. */
	munit_assert_int(atomic_load(&g_to_holder_done), ==, 2);

	(void)xtc_loop_fini(loop);
	xtc_amutex_destroy(g_to_m);
	xtc_arwlock_destroy(g_to_rw);
	return MUNIT_OK;
#endif   /* !_WIN32 */
}

/* ---- B1: two fiber waiters, two posts -------------------------------
 *
 * Two fibers wait on one sem (count 0) / notify; a poster posts once,
 * lets both run, posts again.  Both must be released promptly.  The
 * 1.55.0 wake detached the whole queue, so the loser of the first post
 * re-parked unqueued and the second post woke nobody: with a finite
 * timeout it was only released by its own timer (latency == timeout).
 */
#define TW_TIMEOUT (3000LL * 1000 * 1000)   /* 3 s: far above any wake */
#define TW_PROMPT  (1000LL * 1000 * 1000)   /* 1 s: "prompt" bound */
static xtc_sem_t    *g_tw_sem;
static xtc_notify_t *g_tw_n;
static int           g_tw_mode;             /* 0 sem, 1 notify */
static atomic_int    g_tw_ok;
static atomic_int    g_tw_late;

static void
tw_waiter(void *a)
{
	int64_t t0, t1;
	int rc;
	(void)a;
	(void)__os_clock_mono(&t0);
	rc = g_tw_mode == 0 ? xtc_sem_acquire(g_tw_sem, 1, TW_TIMEOUT)
	                    : xtc_notify_wait(g_tw_n, TW_TIMEOUT);
	(void)__os_clock_mono(&t1);
	if (rc == XTC_OK) atomic_fetch_add(&g_tw_ok, 1);
	if (t1 - t0 > TW_PROMPT) atomic_fetch_add(&g_tw_late, 1);
}

static void
tw_post(void)
{
	if (g_tw_mode == 0) (void)xtc_sem_post(g_tw_sem, 1);
	else (void)xtc_notify_signal(g_tw_n);
}

static void
tw_poster(void *a)
{
	int i;
	(void)a;
	for (i = 0; i < 5; i++) xtc_yield();      /* both waiters park */
	tw_post();
	for (i = 0; i < 20; i++) xtc_yield();     /* one wins, one re-parks */
	tw_post();
}

static MunitResult
tw_run(int mode)
{
	xtc_loop_t *loop = NULL;
	xtc_pid_t pid;
	g_tw_mode = mode;
	atomic_store(&g_tw_ok, 0);
	atomic_store(&g_tw_late, 0);
	munit_assert_int(xtc_sem_create(0, &g_tw_sem), ==, XTC_OK);
	munit_assert_int(xtc_notify_create(&g_tw_n), ==, XTC_OK);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, tw_waiter, NULL, NULL, &pid), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, tw_waiter, NULL, NULL, &pid), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, tw_poster, NULL, NULL, &pid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	xtc_sem_destroy(g_tw_sem);
	xtc_notify_destroy(g_tw_n);
	munit_assert_int(atomic_load(&g_tw_ok), ==, 2);
	munit_assert_int(atomic_load(&g_tw_late), ==, 0);
	return MUNIT_OK;
}

static MunitResult
test_sem_two_waiters(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return tw_run(0);
}

static MunitResult
test_notify_two_waiters(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return tw_run(1);
}

/* ---- B1: conservation under N waiters / M posts ---------------------
 *
 * CW_WAITERS fibers spread over CW_LOOPS loops each take CW_ROUNDS
 * acquires of 1 or 2 units; a poster posts exactly the total demand in
 * random chunks with random yields between.  Property: every acquire
 * succeeds promptly (none stranded until its timeout), units acquired
 * == units posted, and the count ends at 0.  Repeated with fresh seeds.
 */
#define CW_LOOPS   4
#define CW_WAITERS 12
#define CW_ROUNDS  20
static xtc_sem_t  *g_cw_sem;
static atomic_int  g_cw_taken;     /* units acquired */
static atomic_int  g_cw_fail;      /* acquire returned != OK */
static atomic_int  g_cw_late;      /* acquire took > TW_PROMPT */
static atomic_int  g_cw_done;      /* waiters that finished */
static int         g_cw_need[CW_WAITERS][CW_ROUNDS];
static int         g_cw_chunks[CW_WAITERS * CW_ROUNDS * 2];
static int         g_cw_yields[CW_WAITERS * CW_ROUNDS * 2];
static int         g_cw_nchunks;

static void
cw_waiter(void *a)
{
	int id = (int)(intptr_t)a, r;
	for (r = 0; r < CW_ROUNDS; r++) {
		int64_t t0, t1;
		int rc;
		(void)__os_clock_mono(&t0);
		rc = xtc_sem_acquire(g_cw_sem, (unsigned)g_cw_need[id][r],
		    TW_TIMEOUT);
		(void)__os_clock_mono(&t1);
		if (rc != XTC_OK) { atomic_fetch_add(&g_cw_fail, 1); continue; }
		atomic_fetch_add(&g_cw_taken, g_cw_need[id][r]);
		if (t1 - t0 > TW_PROMPT) atomic_fetch_add(&g_cw_late, 1);
		if (r & 1) xtc_yield();
	}
	atomic_fetch_add(&g_cw_done, 1);
}

static void
cw_poster(void *a)
{
	int i, y;
	(void)a;
	for (i = 0; i < g_cw_nchunks; i++) {
		for (y = 0; y < g_cw_yields[i]; y++) xtc_yield();
		(void)xtc_sem_post(g_cw_sem, (unsigned)g_cw_chunks[i]);
	}
}

static MunitResult
test_sem_conservation(const MunitParameter p[], void *d)
{
	int iter, i, r, total, left;
	(void)p; (void)d;
	for (iter = 0; iter < 10; iter++) {
		xtc_exec_t *e = NULL;
		total = 0;
		for (i = 0; i < CW_WAITERS; i++)
			for (r = 0; r < CW_ROUNDS; r++) {
				g_cw_need[i][r] = 1 + (munit_rand_uint32() & 1);
				total += g_cw_need[i][r];
			}
		g_cw_nchunks = 0;
		for (left = total; left > 0; left -= g_cw_chunks[g_cw_nchunks++]) {
			int c = 1 + (int)(munit_rand_uint32() % 3);
			g_cw_chunks[g_cw_nchunks] = c < left ? c : left;
			g_cw_yields[g_cw_nchunks] = (int)(munit_rand_uint32() % 4);
		}
		atomic_store(&g_cw_taken, 0);
		atomic_store(&g_cw_fail, 0);
		atomic_store(&g_cw_late, 0);
		atomic_store(&g_cw_done, 0);
		munit_assert_int(xtc_sem_create(0, &g_cw_sem), ==, XTC_OK);
		munit_assert_int(xtc_exec_init(&e, CW_LOOPS), ==, XTC_OK);
		for (i = 0; i < CW_WAITERS; i++)
			munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e,
			    i % CW_LOOPS), cw_waiter, (void *)(intptr_t)i,
			    NULL, NULL), ==, XTC_OK);
		munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 0),
		    cw_poster, NULL, NULL, NULL), ==, XTC_OK);
		munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
		(void)xtc_exec_fini(e);
		munit_assert_int(atomic_load(&g_cw_done), ==, CW_WAITERS);
		munit_assert_int(atomic_load(&g_cw_fail), ==, 0);
		munit_assert_int(atomic_load(&g_cw_late), ==, 0);
		munit_assert_int(atomic_load(&g_cw_taken), ==, total);
		munit_assert_int(xtc_sem_count(g_cw_sem), ==, 0);
		xtc_sem_destroy(g_cw_sem);
	}
	return MUNIT_OK;
}

/* ---- M5: a timed lock grant must not leave its park timer armed -----
 *
 * The holder keeps the lock 10 ms; the waiter takes it with a 2 s
 * timeout, then does ONE plain xtc_yield.  1.55.0 left the timed wait's
 * park timer in the task slot on the grant path, so that yield parked
 * until the stale deadline (measured 1.991 s).
 */
#define TL_TIMEOUT (2000LL * 1000 * 1000)
static xtc_amutex_t  *g_tl_m;
static xtc_arwlock_t *g_tl_rw;
static int            g_tl_kind;           /* 0 amutex, 1 rd, 2 wr */
static int64_t        g_tl_yield_ns;
static int            g_tl_rc;

static void
tl_holder(void *a)
{
	(void)a;
	if (g_tl_kind == 0) (void)xtc_amutex_lock(g_tl_m, -1);
	else (void)xtc_arwlock_wrlock(g_tl_rw, -1);
	(void)xtc_proc_sleep(10LL * 1000 * 1000);
	if (g_tl_kind == 0) (void)xtc_amutex_unlock(g_tl_m);
	else (void)xtc_arwlock_unlock(g_tl_rw);
}

static void
tl_waiter(void *a)
{
	int64_t t0, t1;
	(void)a;
	(void)xtc_proc_sleep(1LL * 1000 * 1000);   /* holder goes first */
	g_tl_rc = g_tl_kind == 0 ? xtc_amutex_lock(g_tl_m, TL_TIMEOUT) :
	    g_tl_kind == 1 ? xtc_arwlock_rdlock(g_tl_rw, TL_TIMEOUT) :
	                     xtc_arwlock_wrlock(g_tl_rw, TL_TIMEOUT);
	(void)__os_clock_mono(&t0);
	xtc_yield();
	(void)__os_clock_mono(&t1);
	g_tl_yield_ns = t1 - t0;
	if (g_tl_rc != XTC_OK) return;
	if (g_tl_kind == 0) (void)xtc_amutex_unlock(g_tl_m);
	else (void)xtc_arwlock_unlock(g_tl_rw);
}

static MunitResult
test_timed_lock_no_stale_timer(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	for (g_tl_kind = 0; g_tl_kind < 3; g_tl_kind++) {
		xtc_loop_t *loop = NULL;
		g_tl_rc = -999;
		g_tl_yield_ns = -1;
		munit_assert_int(xtc_amutex_create(&g_tl_m), ==, XTC_OK);
		munit_assert_int(xtc_arwlock_create(&g_tl_rw), ==, XTC_OK);
		munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
		munit_assert_int(xtc_proc_spawn(loop, tl_holder, NULL, NULL, NULL), ==, XTC_OK);
		munit_assert_int(xtc_proc_spawn(loop, tl_waiter, NULL, NULL, NULL), ==, XTC_OK);
		munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
		(void)xtc_loop_fini(loop);
		xtc_amutex_destroy(g_tl_m);
		xtc_arwlock_destroy(g_tl_rw);
		munit_logf(MUNIT_LOG_INFO, "kind %d: rc=%d yield %.3f ms",
		    g_tl_kind, g_tl_rc, (double)g_tl_yield_ns / 1e6);
		munit_assert_int(g_tl_rc, ==, XTC_OK);
		munit_assert_int64(g_tl_yield_ns, <, 50LL * 1000 * 1000);
	}
	return MUNIT_OK;
}

/* ---- gate: drain on an OPEN gate wakes when the count reaches 0 ---- */
static xtc_gate_t *g_gd;
static int64_t     g_gd_ns;
static int         g_gd_rc;

static void
gd_drainer(void *a)
{
	int64_t t0, t1;
	(void)a;
	(void)__os_clock_mono(&t0);
	g_gd_rc = xtc_gate_drain(g_gd, TW_TIMEOUT);
	(void)__os_clock_mono(&t1);
	g_gd_ns = t1 - t0;
}

static void
gd_worker(void *a)
{
	(void)a;
	(void)xtc_proc_sleep(5LL * 1000 * 1000);
	(void)xtc_gate_leave(g_gd);
}

static MunitResult
test_gate_drain_open(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	(void)p; (void)d;
	munit_assert_int(xtc_gate_create(&g_gd), ==, XTC_OK);
	munit_assert_int(xtc_gate_enter(g_gd), ==, XTC_OK);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, gd_drainer, NULL, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, gd_worker, NULL, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	xtc_gate_destroy(g_gd);
	munit_assert_int(g_gd_rc, ==, XTC_OK);
	munit_assert_int64(g_gd_ns, <, TW_PROMPT);
	return MUNIT_OK;
}

/* ---- kill a parked waiter, then wake it -----------------------------
 *
 * A waiter queued on a lock is killed (xtc_exit_pid) and is then handed
 * the lock by the holder's unlock.  It must die WITHOUT keeping the
 * grant: a third proc must get the lock.  In 1.55.0 the resume hook
 * inside xtc_yield unwound the killed waiter before it could give the
 * grant back, so the lock stayed held by a dead fiber forever.
 */
static xtc_amutex_t  *g_kw_m;
static xtc_arwlock_t *g_kw_rw;
static xtc_sem_t     *g_kw_sem;
static xtc_notify_t  *g_kw_n;
static xtc_barrier_t *g_kw_b;
static xtc_gate_t    *g_kw_g;
static int            g_kw_kind;   /* index into kw_kinds */
static xtc_loop_t    *g_kw_loop;
static xtc_pid_t      g_kw_victim;
static atomic_int     g_kw_victim_ret;    /* victim's wait returned */
static int            g_kw_third_rc;
static int            g_kw_down;

static int64_t        g_kw_tmo;    /* victim's timeout: -1 or 50 ms */

static void
kw_victim(void *a)
{
	(void)a;
	switch (g_kw_kind) {
	case 0: (void)xtc_amutex_lock(g_kw_m, g_kw_tmo); break;
	case 1: (void)xtc_arwlock_wrlock(g_kw_rw, g_kw_tmo); break;
	case 2: (void)xtc_sem_acquire(g_kw_sem, 1, g_kw_tmo); break;
	case 3: (void)xtc_notify_wait(g_kw_n, g_kw_tmo); break;
	case 4: (void)xtc_barrier_wait(g_kw_b); break;
	default: (void)xtc_gate_drain(g_kw_g, g_kw_tmo); break;
	}
	atomic_store(&g_kw_victim_ret, 1);       /* must not be reached */
}

static void
kw_driver(void *a)
{
	uint64_t ref;
	void *m = NULL;
	size_t n = 0;
	int i;
	(void)a;
	if (g_kw_kind == 0) (void)xtc_amutex_lock(g_kw_m, -1);
	if (g_kw_kind == 1) (void)xtc_arwlock_wrlock(g_kw_rw, -1);
	if (g_kw_kind == 5) (void)xtc_gate_enter(g_kw_g);
	munit_assert_int(xtc_proc_spawn_monitor(g_kw_loop, kw_victim,
	    NULL, NULL, &g_kw_victim, &ref), ==, XTC_OK);
	for (i = 0; i < 10; i++) xtc_yield();    /* victim parks */
	if (g_kw_kind == 5) (void)xtc_gate_close(g_kw_g);
	munit_assert_int(xtc_exit_pid(g_kw_victim, 9), ==, XTC_OK);
	/* Past a timed victim's 50 ms deadline, so its own timer resumes
	 * it while it is still queued. */
	(void)xtc_proc_sleep(100LL * 1000 * 1000);
	/* The later wake: hand the victim the lock / a unit / a signal. */
	switch (g_kw_kind) {
	case 0: (void)xtc_amutex_unlock(g_kw_m); break;
	case 1: (void)xtc_arwlock_unlock(g_kw_rw); break;
	case 2: (void)xtc_sem_post(g_kw_sem, 1); break;
	case 3: (void)xtc_notify_signal(g_kw_n); break;
	case 4: g_kw_third_rc = xtc_barrier_wait(g_kw_b); break;
	default: (void)xtc_gate_leave(g_kw_g); break;
	}
	g_kw_down = xtc_recv(&m, &n, 2000LL * 1000 * 1000) == XTC_OK;
	if (m != NULL) xtc_free(m);
	/* The victim's grant must have been given back. */
	switch (g_kw_kind) {
	case 0:
		g_kw_third_rc = xtc_amutex_lock(g_kw_m, 500LL * 1000 * 1000);
		if (g_kw_third_rc == XTC_OK) (void)xtc_amutex_unlock(g_kw_m);
		break;
	case 1:
		g_kw_third_rc = xtc_arwlock_rdlock(g_kw_rw, 500LL * 1000 * 1000);
		if (g_kw_third_rc == XTC_OK) (void)xtc_arwlock_unlock(g_kw_rw);
		break;
	case 2:
		g_kw_third_rc = xtc_sem_acquire(g_kw_sem, 1, 500LL * 1000 * 1000);
		break;
	case 3:
		g_kw_third_rc = xtc_notify_wait(g_kw_n, 500LL * 1000 * 1000);
		break;
	case 4:
		break;          /* the generation completed: rc set above */
	default:
		g_kw_third_rc = xtc_gate_drain(g_kw_g, 0);
		break;
	}
}

static const char *kw_kinds[] = { "amutex", "arwlock", "sem", "notify",
    "barrier", "gate", NULL };
static const char *kw_waits[] = { "inf", "timed", NULL };
static MunitParameterEnum kw_params[] = {
	{ (char *)"kind", (char **)kw_kinds },
	{ (char *)"wait", (char **)kw_waits },
	{ NULL, NULL }
};
#if defined(XTC_HAVE_PROC_KILL_WAKE)
static MunitParameterEnum kw_kind_params[] = {
	{ (char *)"kind", (char **)kw_kinds },
	{ NULL, NULL }
};
#endif

static int
kw_param_kind(const MunitParameter p[])
{
	const char *k = munit_parameters_get(p, "kind");
	int i;
	for (i = 0; k != NULL && kw_kinds[i] != NULL; i++)
		if (strcmp(k, kw_kinds[i]) == 0) return i;
	return 0;
}

/* Fresh primitives + one loop running `driver`; asserts the victim never
 * returned from its wait and its DOWN arrived. */
static void
kw_run(xtc_proc_fn driver)
{
	xtc_loop_t *loop = NULL;
	atomic_store(&g_kw_victim_ret, 0);
	g_kw_third_rc = -999;
	g_kw_down = 0;
	munit_assert_int(xtc_amutex_create(&g_kw_m), ==, XTC_OK);
	munit_assert_int(xtc_arwlock_create(&g_kw_rw), ==, XTC_OK);
	munit_assert_int(xtc_sem_create(0, &g_kw_sem), ==, XTC_OK);
	munit_assert_int(xtc_notify_create(&g_kw_n), ==, XTC_OK);
	munit_assert_int(xtc_barrier_create(2, &g_kw_b), ==, XTC_OK);
	munit_assert_int(xtc_gate_create(&g_kw_g), ==, XTC_OK);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	g_kw_loop = loop;
	munit_assert_int(xtc_proc_spawn(loop, driver, NULL, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	xtc_amutex_destroy(g_kw_m);
	xtc_arwlock_destroy(g_kw_rw);
	xtc_sem_destroy(g_kw_sem);
	xtc_notify_destroy(g_kw_n);
	xtc_barrier_destroy(g_kw_b);
	xtc_gate_destroy(g_kw_g);
	munit_logf(MUNIT_LOG_INFO, "%s: down=%d third_rc=%d",
	    kw_kinds[g_kw_kind], g_kw_down, g_kw_third_rc);
	munit_assert_int(atomic_load(&g_kw_victim_ret), ==, 0);
	munit_assert_int(g_kw_down, ==, 1);
}

static MunitResult
test_kill_then_wake(const MunitParameter p[], void *d)
{
	const char *wt = munit_parameters_get(p, "wait");
	(void)d;
	g_kw_kind = kw_param_kind(p);
	g_kw_tmo = (wt != NULL && strcmp(wt, "timed") == 0) ?
	    50LL * 1000 * 1000 : -1;
	kw_run(kw_driver);
	munit_assert_int(g_kw_third_rc, ==, XTC_OK);
	return MUNIT_OK;
}

#if defined(XTC_HAVE_PROC_KILL_WAKE)
/* ---- prompt kill of a parked waiter (no other wake) -----------------
 *
 * Needs xtc_exit_pid to wake a target parked anywhere (proc branch).
 * The victim waits forever on a sem / notify / amutex nobody releases;
 * the kill alone must end it within 1 s, and leave the primitive
 * usable (no stale waiter left linked).
 */
static int64_t g_kp_ns;

static void
kp_driver(void *a)
{
	uint64_t ref;
	void *m = NULL;
	size_t n = 0;
	int64_t t0, t1;
	int i;
	(void)a;
	if (g_kw_kind == 0) (void)xtc_amutex_lock(g_kw_m, -1);
	if (g_kw_kind == 1) (void)xtc_arwlock_wrlock(g_kw_rw, -1);
	if (g_kw_kind == 5) (void)xtc_gate_enter(g_kw_g);
	munit_assert_int(xtc_proc_spawn_monitor(g_kw_loop, kw_victim,
	    NULL, NULL, &g_kw_victim, &ref), ==, XTC_OK);
	for (i = 0; i < 10; i++) xtc_yield();
	(void)__os_clock_mono(&t0);
	munit_assert_int(xtc_exit_pid(g_kw_victim, 9), ==, XTC_OK);
	g_kw_down = xtc_recv(&m, &n, 1000LL * 1000 * 1000) == XTC_OK;
	(void)__os_clock_mono(&t1);
	g_kp_ns = t1 - t0;
	if (m != NULL) xtc_free(m);
	switch (g_kw_kind) {
	case 4:                 /* nothing to release */
		g_kw_third_rc = XTC_OK;
		break;
	case 5: (void)xtc_gate_leave(g_kw_g);
		g_kw_third_rc = xtc_gate_drain(g_kw_g, 0);
		break;
	case 0: (void)xtc_amutex_unlock(g_kw_m);
		g_kw_third_rc = xtc_amutex_try_lock(g_kw_m);
		if (g_kw_third_rc == XTC_OK) (void)xtc_amutex_unlock(g_kw_m);
		break;
	case 1: (void)xtc_arwlock_unlock(g_kw_rw);
		g_kw_third_rc = xtc_arwlock_wrlock(g_kw_rw, 0);
		if (g_kw_third_rc == XTC_OK) (void)xtc_arwlock_unlock(g_kw_rw);
		break;
	case 2: (void)xtc_sem_post(g_kw_sem, 1);
		g_kw_third_rc = xtc_sem_try_acquire(g_kw_sem, 1);
		break;
	default: (void)xtc_notify_signal(g_kw_n);    /* notify */
		g_kw_third_rc = xtc_notify_wait(g_kw_n, 0);
		break;
	}
}

static MunitResult
test_kill_prompt(const MunitParameter p[], void *d)
{
	(void)d;
	g_kw_kind = kw_param_kind(p);
	g_kw_tmo = -1;
	kw_run(kp_driver);
	munit_assert_int64(g_kp_ns, <, 1000LL * 1000 * 1000);
	munit_assert_int(g_kw_third_rc, ==, XTC_OK);
	return MUNIT_OK;
}

/*
 * A kill that lands while the target is RUNNING after an earlier park
 * (here a recv), before it enters a sync wait, must still unwind that
 * wait promptly.  The resume hook skips delivery after a park yield, so
 * the wait's own delivery point has to deliver the kill directly
 * (__xtc_proc_kill_poll); calling the resume hook there returns early
 * because the earlier recv park left yield_park set, and the wait then
 * parks for good.  Caught by the 1.55.x re-review (kp.c) after the fix
 * that switches the wait sites to __xtc_proc_kill_poll was left out of
 * a merge: no DOWN within 3 s for sem, notify, amutex and future.
 */
static xtc_exec_t *g_kr_e;
static int64_t g_kr_ns;
static xtc_promise_t *g_kr_pr;
static xtc_future_t *g_kr_f;

static void
kr_victim(void *a)
{
	void *m;
	size_t n;
	int64_t t0 = 0, t = 0;
	intptr_t v;
	(void)a;
	if (xtc_recv(&m, &n, -1) == XTC_OK)
		xtc_free(m);
	(void)__os_clock_mono(&t0);
	do {                         /* busy, no yield: the kill lands here */
		(void)__os_clock_mono(&t);
	} while (t - t0 < 50LL * 1000 * 1000);
	switch (g_kw_kind) {
	case 0: (void)xtc_amutex_lock(g_kw_m, -1); break;
	case 2: (void)xtc_sem_acquire(g_kw_sem, 1, -1); break;
	case 3: (void)xtc_notify_wait(g_kw_n, -1); break;
	default: (void)xtc_future_wait(g_kr_f, &v, -1); break;
	}
}

static void
kr_holder(void *a)
{
	(void)a;
	if (xtc_amutex_lock(g_kw_m, -1) == XTC_OK) {
		(void)xtc_proc_sleep(2LL * 1000 * 1000 * 1000);
		(void)xtc_amutex_unlock(g_kw_m);
	}
}

static void
kr_killer(void *a)
{
	xtc_pid_t v, h;
	uint64_t ref;
	void *m;
	size_t n;
	char c = 'x';
	int64_t t0 = 0, t1 = 0;
	(void)a;
	if (g_kw_kind == 0) {
		(void)xtc_proc_spawn(xtc_exec_loop(g_kr_e, 1), kr_holder, NULL,
		    NULL, &h);
		(void)xtc_proc_sleep(1000 * 1000);
	}
	if (xtc_proc_spawn_monitor(xtc_exec_loop(g_kr_e, 0), kr_victim, NULL,
	    NULL, &v, &ref) != XTC_OK)
		return;
	(void)xtc_proc_sleep(5 * 1000 * 1000);
	(void)xtc_send(v, &c, 1);
	(void)xtc_proc_sleep(10 * 1000 * 1000);
	(void)__os_clock_mono(&t0);
	(void)xtc_exit_pid(v, 9);
	if (xtc_recv(&m, &n, 1000LL * 1000 * 1000) == XTC_OK) {
		(void)__os_clock_mono(&t1);
		g_kr_ns = t1 - t0;
		xtc_free(m);
	} else {
		/* Release whatever the victim is stuck on so the run ends. */
		if (g_kw_kind == 2) (void)xtc_sem_post(g_kw_sem, 1);
		else if (g_kw_kind == 3) (void)xtc_notify_signal(g_kw_n);
		else if (g_kw_kind == 1) (void)xtc_promise_set(g_kr_pr, 1, 0);
		if (xtc_recv(&m, &n, 4LL * 1000 * 1000 * 1000) == XTC_OK)
			xtc_free(m);
	}
}

static char *kr_kinds[] = { "amutex", "future", "sem", "notify", NULL };
static MunitParameterEnum kr_params[] = { { "kind", kr_kinds }, { NULL, NULL } };

static MunitResult
test_kill_after_park_while_running(const MunitParameter p[], void *d)
{
	const char *k = munit_parameters_get(p, "kind");
	xtc_pid_t pid;
	(void)d;
	g_kw_kind = !strcmp(k, "amutex") ? 0 : !strcmp(k, "future") ? 1
	    : !strcmp(k, "sem") ? 2 : 3;
	/* Valgrind memcheck flags every access on a libxtc fiber stack once
	 * a fiber resumes after a cross-loop amutex hand-off (the stacks are
	 * plain mmap regions it does not know about); the same two-loop
	 * amutex shape reports thousands of errors on 1.55.0 with no kill at
	 * all.  The CI memcheck job sets XTC_UNDER_VALGRIND; skip only this
	 * kind there.  Every other build runs it. */
	if (g_kw_kind == 0 && getenv("XTC_UNDER_VALGRIND") != NULL)
		return MUNIT_SKIP;
	g_kr_ns = -1;
	munit_assert_int(xtc_sem_create(0, &g_kw_sem), ==, XTC_OK);
	munit_assert_int(xtc_notify_create(&g_kw_n), ==, XTC_OK);
	munit_assert_int(xtc_amutex_create(&g_kw_m), ==, XTC_OK);
	munit_assert_int(xtc_future_new_pair(&g_kr_pr, &g_kr_f), ==, XTC_OK);
	munit_assert_int(xtc_exec_init(&g_kr_e, 2), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(xtc_exec_loop(g_kr_e, 1), kr_killer,
	    NULL, NULL, &pid), ==, XTC_OK);
	munit_assert_int(xtc_exec_run(g_kr_e), ==, XTC_OK);
	munit_assert_int(xtc_exec_fini(g_kr_e), ==, XTC_OK);
	/* The killed waiter never consumed the future (and only kind 1 used
	 * it); settle and consume it so the cell is released.  The set may
	 * already have happened on the killer's fallback path. */
	(void)xtc_promise_set(g_kr_pr, 1, 0);
	(void)xtc_future_wait(g_kr_f, NULL, 0);
	xtc_amutex_destroy(g_kw_m);
	xtc_notify_destroy(g_kw_n);
	xtc_sem_destroy(g_kw_sem);
	munit_assert_int64(g_kr_ns, >=, 0);
	munit_assert_int64(g_kr_ns, <, 500LL * 1000 * 1000);
	return MUNIT_OK;
}
#endif /* XTC_HAVE_PROC_KILL_WAKE */

/*
 * A recursive amutex held by a proc that exits is not re-entrant for
 * the next proc on the loop.  The owner was the task pointer alone, and
 * a finished proc's task struct is recycled for the next proc, so that
 * proc "re-entered" a lock it never took.  Proc A takes the lock and
 * exits holding it; each later proc B must time out.
 */
static xtc_amutex_t *g_ra_m;
static int g_ra_got, g_ra_runs, g_ra_held;

static void
ra_holder(void *a)
{
	(void)a;
	if (xtc_amutex_lock(g_ra_m, -1) == XTC_OK) g_ra_held = 1;
}

static void
ra_other(void *a)
{
	(void)a;
	g_ra_runs++;
	if (xtc_amutex_lock(g_ra_m, 1000000) == XTC_OK) {
		g_ra_got++;
		(void)xtc_amutex_unlock(g_ra_m);
	}
}

static void
ra_driver(void *a)
{
	xtc_loop_t *loop = a;
	int i;
	if (xtc_proc_spawn(loop, ra_holder, NULL, NULL, NULL) != XTC_OK)
		return;
	for (i = 0; i < 10; i++) {
		/* The previous proc has finished: its task struct is free. */
		(void)xtc_proc_sleep(3 * 1000 * 1000);
		if (xtc_proc_spawn(loop, ra_other, NULL, NULL, NULL) != XTC_OK)
			return;
	}
	(void)xtc_proc_sleep(3 * 1000 * 1000);
}

static MunitResult
test_amutex_recursive_exit(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	(void)p; (void)d;
	g_ra_got = g_ra_runs = g_ra_held = 0;
	munit_assert_int(xtc_amutex_create_ex(&g_ra_m, XTC_AMUTEX_RECURSIVE),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, ra_driver, loop, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	munit_assert_int(g_ra_held, ==, 1);
	munit_assert_int(g_ra_runs, ==, 10);
	munit_assert_int(g_ra_got, ==, 0);
	/* Still held by the dead proc: off a loop, try_lock must refuse. */
	munit_assert_int(xtc_amutex_try_lock(g_ra_m), ==, XTC_E_AGAIN);
	xtc_amutex_destroy(g_ra_m);
	return MUNIT_OK;
}

/*
 * A recursive amutex won by an OFF-LOOP thread on the contended (condvar)
 * path must record that thread as its owner, so the thread can re-enter
 * it.  __amutex_lock_thread set held = 1 but left owner/count unset, so
 * the winner's nested lock saw "held by someone else" and waited on
 * itself forever: sqlxtc's in-process TPC-C hung at 8 threads in
 * vx_prepare_select (VX_LK_CC re-entered by a correlated subquery).
 * Two threads contend; each re-enters while holding.  With the bug one
 * of them never finishes (the timed re-entry reports it as a failure
 * instead of hanging the suite).
 */
static xtc_amutex_t *g_rt_m;
static _Atomic int g_rt_ok, g_rt_bad;

static void *
rt_thread(void *a)
{
	int i;
	(void)a;
	for (i = 0; i < 2000; i++) {
		if (xtc_amutex_lock(g_rt_m, -1) != XTC_OK) { g_rt_bad++; continue; }
		/* nested re-entry by the owner: must succeed at once */
		if (xtc_amutex_lock(g_rt_m, 2LL * 1000 * 1000 * 1000) != XTC_OK) {
			g_rt_bad++;
			(void)xtc_amutex_unlock(g_rt_m);
			return NULL;
		}
		g_rt_ok++;
		(void)xtc_amutex_unlock(g_rt_m);
		(void)xtc_amutex_unlock(g_rt_m);
	}
	return NULL;
}

static MunitResult
test_amutex_recursive_thread(const MunitParameter p[], void *d)
{
	pthread_t th[4];
	int i;
	(void)p; (void)d;
	g_rt_ok = 0; g_rt_bad = 0;
	munit_assert_int(xtc_amutex_create_ex(&g_rt_m, XTC_AMUTEX_RECURSIVE),
	    ==, XTC_OK);
	for (i = 0; i < 4; i++)
		munit_assert_int(pthread_create(&th[i], NULL, rt_thread, NULL), ==, 0);
	for (i = 0; i < 4; i++)
		(void)pthread_join(th[i], NULL);
	munit_assert_int(g_rt_bad, ==, 0);
	munit_assert_int(g_rt_ok, ==, 4 * 2000);
	/* Released for good: an off-loop try_lock now succeeds. */
	munit_assert_int(xtc_amutex_try_lock(g_rt_m), ==, XTC_OK);
	(void)xtc_amutex_unlock(g_rt_m);
	xtc_amutex_destroy(g_rt_m);
	return MUNIT_OK;
}

/*
 * OFF-LOOP (plain-thread) contention on xtc_arwlock / xtc_amutex.
 *
 * 1. No thundering herd (offloop_no_herd).  K threads sleep in wrlock
 *    (then in amutex_lock) on a lock the main thread holds.
 *    The main thread holds it ~1 ms, releases and takes it straight back,
 *    R times; the waiters loop on lock/unlock.  A release may usefully
 *    wake ONE waiter (which usually finds the lock taken again and
 *    sleeps).  The arwlock used to broadcast to every off-loop waiter on
 *    every release, so the waiters' summed voluntary context switches
 *    came to about K per release (measured 8478-9347 for K = 8, R = 200);
 *    a targeted wake keeps it near 1 (207-208).  Asserted bound: K/2 per
 *    release.  Needs 2+ CPUs (on one, the main thread's release runs the
 *    woken waiter at once and the waiters rarely all sleep).  Readers are
 *    not tested this way: an exclusive release SHOULD wake every sleeping
 *    reader, since they can all enter together.
 * 2. Spin before sleep (offloop_spin).  Two threads alternate on one lock
 *    with ~2 us holds.  A waiter that sleeps at once (the old off-loop
 *    path) pays a context switch on most contended acquires; one that
 *    polls the lock briefly first takes it without sleeping.  Asserted:
 *    fewer than 1 voluntary switch per 10 acquires.  Needs 2+ CPUs.
 * 3. Every release wakes (offloop_release_wakes).  HERD_K threads sleep
 *    in wrlock (then amutex_lock) on a lock the main thread holds; the
 *    main thread releases and re-takes it HERD_K times back to back
 *    (far faster than a woken thread can run), then holds it.  Each of
 *    those releases freed the lock with sleepers, so each must wake one:
 *    HERD_K distinct waiters run.  A version that kept at most one wake
 *    in flight per lock woke ONE here; on a host where a woken thread
 *    needs ~100 us to leave a deep CPU idle state, the lock then sat free
 *    with sleepers for that long.  Asserted: at least HERD_K/2 woke.
 * 4. Notify spin (offloop_notify_spin).  One thread waits on a notify
 *    NS_N times; another signals it ~2 us after each wait starts (it
 *    busy-waits for the waiter's ack, so only one thread ever waits on
 *    a notify and the process-wide spin cap of 1 on a 2-4 CPU host
 *    suffices).  A waiter that sleeps at once pays a futex sleep + wake
 *    per signal; one that polls first takes it without sleeping.
 *    Asserted: fewer than 1 voluntary switch per 10 waits.  Needs 2+
 *    CPUs.
 * Context switches are read per thread (RUSAGE_THREAD, and
 * /proc/self/task/TID/status for another thread's): Linux only.
 * 5. Liveness + exclusion (arwlock_threads, offloop_timed_handoff):
 *    every acquire is timed, so a lost wakeup fails instead of hanging.
 */
#if defined(__linux__) && defined(RUSAGE_THREAD)
#define HERD_K 8
#define HERD_R 200
enum { HD_WR, HD_RD, HD_AM };
static xtc_arwlock_t *g_hd_rw;
static xtc_amutex_t  *g_hd_m;
static _Atomic int    g_hd_ready, g_hd_stop, g_hd_got;
static _Atomic long   g_hd_csw;

static void
hd_lock(int kind, int64_t to)
{
	int rc;
	if (kind == HD_AM) rc = xtc_amutex_lock(g_hd_m, to);
	else if (kind == HD_RD) rc = xtc_arwlock_rdlock(g_hd_rw, to);
	else rc = xtc_arwlock_wrlock(g_hd_rw, to);
	munit_assert_int(rc, ==, XTC_OK);
}

static void
hd_unlock(int kind)
{
	if (kind == HD_AM) (void)xtc_amutex_unlock(g_hd_m);
	else (void)xtc_arwlock_unlock(g_hd_rw);
}

static void *
hd_waiter(void *arg)
{
	struct rusage u0, u1;
	int kind = (int)(intptr_t)arg;
	(void)getrusage(RUSAGE_THREAD, &u0);
	atomic_fetch_add(&g_hd_ready, 1);
	while (!atomic_load(&g_hd_stop)) {
		hd_lock(kind, 30LL * 1000 * 1000 * 1000);
		atomic_fetch_add(&g_hd_got, 1);
		hd_unlock(kind);
	}
	(void)getrusage(RUSAGE_THREAD, &u1);
	atomic_fetch_add(&g_hd_csw, u1.ru_nvcsw - u0.ru_nvcsw);
	return NULL;
}

static long
herd_run(int kind)
{
	pthread_t th[HERD_K];
	int i;
	g_hd_ready = 0; g_hd_stop = 0; g_hd_got = 0; g_hd_csw = 0;
	hd_lock(kind, -1);
	for (i = 0; i < HERD_K; i++)
		munit_assert_int(pthread_create(&th[i], NULL, hd_waiter,
		    (void *)(intptr_t)kind), ==, 0);
	while (atomic_load(&g_hd_ready) < HERD_K)
		(void)__os_sleep_ns(1000 * 1000);
	(void)__os_sleep_ns(50LL * 1000 * 1000);   /* all asleep in the lock */
	for (i = 0; i < HERD_R; i++) {
		(void)__os_sleep_ns(1000 * 1000);
		hd_unlock(kind);
		hd_lock(kind, -1);
	}
	atomic_store(&g_hd_stop, 1);
	hd_unlock(kind);
	for (i = 0; i < HERD_K; i++)
		(void)pthread_join(th[i], NULL);
	munit_assert_int(atomic_load(&g_hd_got), >=, HERD_K);
	return atomic_load(&g_hd_csw);
}

static MunitResult
test_offloop_no_herd(const MunitParameter p[], void *d)
{
	static const char *name[] = { "arwlock wr", "arwlock rd", "amutex" };
	long csw;
	int kind;
	(void)p; (void)d;
	if (xtc_ncpus() < 2)
		return MUNIT_SKIP;
	munit_assert_int(xtc_arwlock_create(&g_hd_rw), ==, XTC_OK);
	munit_assert_int(xtc_amutex_create(&g_hd_m), ==, XTC_OK);
	for (kind = HD_WR; kind <= HD_AM; kind += 2) {
		csw = herd_run(kind);
		fprintf(stderr, "  %s: %ld waiter csw over %d releases\n",
		    name[kind], csw, HERD_R);
		munit_assert_long(csw, <=, (long)HERD_R * HERD_K / 2);
	}
	xtc_arwlock_destroy(g_hd_rw);
	xtc_amutex_destroy(g_hd_m);
	return MUNIT_OK;
}

#define SP_N 20000
static _Atomic int  g_sp_go;
static _Atomic long g_sp_csw;
static volatile long g_sp_val;

static void
sp_burn(int n)
{
	volatile int k = 0;
	int i;
	for (i = 0; i < n; i++) k += i;
}

static void *
sp_thread(void *arg)
{
	struct rusage u0, u1;
	int kind = (int)(intptr_t)arg, i;
	while (!atomic_load(&g_sp_go))
		;
	(void)getrusage(RUSAGE_THREAD, &u0);
	for (i = 0; i < SP_N; i++) {
		hd_lock(kind, 30LL * 1000 * 1000 * 1000);
		g_sp_val++;
		sp_burn(2000);                     /* ~2 us hold */
		hd_unlock(kind);
		sp_burn(1000);
	}
	(void)getrusage(RUSAGE_THREAD, &u1);
	atomic_fetch_add(&g_sp_csw, u1.ru_nvcsw - u0.ru_nvcsw);
	return NULL;
}

static MunitResult
test_offloop_spin(const MunitParameter p[], void *d)
{
	static const char *name[] = { "arwlock wr", "arwlock rd", "amutex" };
	pthread_t th[2];
	int kind, i;
	(void)p; (void)d;
	if (xtc_ncpus() < 2)
		return MUNIT_SKIP;
	munit_assert_int(xtc_arwlock_create(&g_hd_rw), ==, XTC_OK);
	munit_assert_int(xtc_amutex_create(&g_hd_m), ==, XTC_OK);
	for (kind = HD_WR; kind <= HD_AM; kind += 2) {
		g_sp_go = 0; g_sp_csw = 0; g_sp_val = 0;
		for (i = 0; i < 2; i++)
			munit_assert_int(pthread_create(&th[i], NULL, sp_thread,
			    (void *)(intptr_t)kind), ==, 0);
		atomic_store(&g_sp_go, 1);
		for (i = 0; i < 2; i++)
			(void)pthread_join(th[i], NULL);
		fprintf(stderr, "  %s: %ld csw over %d acquires\n", name[kind],
		    atomic_load(&g_sp_csw), 2 * SP_N);
		munit_assert_long(g_sp_val, ==, 2L * SP_N);
		munit_assert_long(atomic_load(&g_sp_csw), <, 2L * SP_N / 10);
	}
	xtc_arwlock_destroy(g_hd_rw);
	xtc_amutex_destroy(g_hd_m);
	return MUNIT_OK;
}

static _Atomic int g_rw_tid[HERD_K];

/* Voluntary context switches of thread `tid` of this process, or -1. */
static long
task_vcsw(int tid)
{
	char path[64], ln[128];
	long v = -1;
	FILE *f;
	(void)snprintf(path, sizeof path, "/proc/self/task/%d/status", tid);
	if ((f = fopen(path, "r")) == NULL)
		return -1;
	while (fgets(ln, sizeof ln, f) != NULL)
		if (sscanf(ln, "voluntary_ctxt_switches: %ld", &v) == 1)
			break;
	(void)fclose(f);
	return v;
}

static void *
rw_waiter(void *arg)
{
	int kind = (int)((intptr_t)arg & 0xff), idx = (int)((intptr_t)arg >> 8);
	atomic_store(&g_rw_tid[idx], (int)syscall(SYS_gettid));
	atomic_fetch_add(&g_hd_ready, 1);
	while (!atomic_load(&g_hd_stop)) {
		hd_lock(kind, 30LL * 1000 * 1000 * 1000);
		hd_unlock(kind);
	}
	return NULL;
}

static MunitResult
test_offloop_release_wakes(const MunitParameter p[], void *d)
{
	static const char *name[] = { "arwlock wr", "arwlock rd", "amutex" };
	pthread_t th[HERD_K];
	long v0[HERD_K], v1;
	int kind, i, woke;
	(void)p; (void)d;
	if (xtc_ncpus() < 2)
		return MUNIT_SKIP;
	munit_assert_int(xtc_arwlock_create(&g_hd_rw), ==, XTC_OK);
	munit_assert_int(xtc_amutex_create(&g_hd_m), ==, XTC_OK);
	for (kind = HD_WR; kind <= HD_AM; kind += 2) {
		g_hd_ready = 0; g_hd_stop = 0;
		hd_lock(kind, -1);
		for (i = 0; i < HERD_K; i++)
			munit_assert_int(pthread_create(&th[i], NULL, rw_waiter,
			    (void *)(intptr_t)(kind | (i << 8))), ==, 0);
		while (atomic_load(&g_hd_ready) < HERD_K)
			(void)__os_sleep_ns(1000 * 1000);
		(void)__os_sleep_ns(50LL * 1000 * 1000);   /* all asleep */
		for (i = 0; i < HERD_K; i++)
			v0[i] = task_vcsw(atomic_load(&g_rw_tid[i]));
		for (i = 0; i < HERD_K; i++) {
			hd_unlock(kind);
			hd_lock(kind, -1);
		}
		(void)__os_sleep_ns(50LL * 1000 * 1000);   /* woken ones ran */
		for (woke = 0, i = 0; i < HERD_K; i++) {
			v1 = task_vcsw(atomic_load(&g_rw_tid[i]));
			munit_assert_long(v1, >=, 0);
			woke += v1 > v0[i];
		}
		atomic_store(&g_hd_stop, 1);
		hd_unlock(kind);
		for (i = 0; i < HERD_K; i++)
			(void)pthread_join(th[i], NULL);
		fprintf(stderr, "  %s: %d of %d sleepers woke over %d releases\n",
		    name[kind], woke, HERD_K, HERD_K);
		munit_assert_int(woke, >=, HERD_K / 2);
	}
	xtc_arwlock_destroy(g_hd_rw);
	xtc_amutex_destroy(g_hd_m);
	return MUNIT_OK;
}

#define NS_N 20000
static xtc_notify_t *g_ns;
static _Atomic int   g_ns_ack;

static void *
ns_waiter(void *arg)
{
	struct rusage u0, u1;
	int i;
	(void)arg;
	(void)getrusage(RUSAGE_THREAD, &u0);
	for (i = 0; i < NS_N; i++) {
		munit_assert_int(xtc_notify_wait(g_ns, -1), ==, XTC_OK);
		atomic_store(&g_ns_ack, i + 1);
	}
	(void)getrusage(RUSAGE_THREAD, &u1);
	atomic_store(&g_sp_csw, u1.ru_nvcsw - u0.ru_nvcsw);
	return NULL;
}

static MunitResult
test_offloop_notify_spin(const MunitParameter p[], void *d)
{
	pthread_t th;
	int i;
	(void)p; (void)d;
	if (xtc_ncpus() < 2)
		return MUNIT_SKIP;
	munit_assert_int(xtc_notify_create(&g_ns), ==, XTC_OK);
	g_sp_csw = 0; g_ns_ack = 0;
	munit_assert_int(pthread_create(&th, NULL, ns_waiter, NULL), ==, 0);
	for (i = 0; i < NS_N; i++) {
		sp_burn(2000);                     /* ~2 us into the wait */
		munit_assert_int(xtc_notify_signal(g_ns), ==, XTC_OK);
		while (atomic_load(&g_ns_ack) <= i)
			;                          /* the waiter took it */
	}
	(void)pthread_join(th, NULL);
	fprintf(stderr, "  notify: %ld waiter csw over %d waits\n",
	    atomic_load(&g_sp_csw), NS_N);
	munit_assert_long(atomic_load(&g_sp_csw), <, (long)NS_N / 10);
	xtc_notify_destroy(g_ns);
	return MUNIT_OK;
}
#endif

#define ART_N    3000
#define ART_WR   8
#define ART_RD   8
static xtc_arwlock_t *g_art;
static _Atomic int g_art_inw, g_art_inr, g_art_viol, g_art_lost;
static _Atomic long g_art_wcnt, g_art_rcnt;
static volatile long g_art_val;

static void *
art_writer(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < ART_N; i++) {
		if (xtc_arwlock_wrlock(g_art, 10LL * 1000 * 1000 * 1000) != XTC_OK) {
			atomic_fetch_add(&g_art_lost, 1);
			break;
		}
		if (atomic_fetch_add(&g_art_inw, 1) != 0 ||
		    atomic_load(&g_art_inr) != 0)
			atomic_store(&g_art_viol, 1);
		g_art_val++;
		g_art_val++;
		atomic_fetch_sub(&g_art_inw, 1);
		(void)xtc_arwlock_unlock(g_art);
		atomic_fetch_add(&g_art_wcnt, 1);
	}
	return NULL;
}

static void *
art_reader(void *arg)
{
	int i;
	long v;
	(void)arg;
	for (i = 0; i < ART_N; i++) {
		if (xtc_arwlock_rdlock(g_art, 10LL * 1000 * 1000 * 1000) != XTC_OK) {
			atomic_fetch_add(&g_art_lost, 1);
			break;
		}
		atomic_fetch_add(&g_art_inr, 1);
		if (atomic_load(&g_art_inw) != 0)
			atomic_store(&g_art_viol, 1);
		v = g_art_val;
		if ((v & 1) != 0)
			atomic_store(&g_art_viol, 1);
		atomic_fetch_sub(&g_art_inr, 1);
		(void)xtc_arwlock_unlock(g_art);
		atomic_fetch_add(&g_art_rcnt, 1);
	}
	return NULL;
}

static MunitResult
test_arwlock_threads(const MunitParameter p[], void *d)
{
	pthread_t th[ART_WR + ART_RD];
	int i;
	(void)p; (void)d;
	g_art_viol = 0; g_art_lost = 0; g_art_wcnt = 0; g_art_rcnt = 0;
	g_art_val = 0;
	munit_assert_int(xtc_arwlock_create(&g_art), ==, XTC_OK);
	for (i = 0; i < ART_WR; i++)
		munit_assert_int(pthread_create(&th[i], NULL, art_writer, NULL),
		    ==, 0);
	for (i = 0; i < ART_RD; i++)
		munit_assert_int(pthread_create(&th[ART_WR + i], NULL,
		    art_reader, NULL), ==, 0);
	for (i = 0; i < ART_WR + ART_RD; i++)
		(void)pthread_join(th[i], NULL);
	munit_assert_int(atomic_load(&g_art_viol), ==, 0);
	munit_assert_int(atomic_load(&g_art_lost), ==, 0);
	munit_assert_long(atomic_load(&g_art_wcnt), ==, (long)ART_WR * ART_N);
	munit_assert_long(atomic_load(&g_art_rcnt), ==, (long)ART_RD * ART_N);
	munit_assert_long(g_art_val, ==, 2L * ART_WR * ART_N);
	xtc_arwlock_destroy(g_art);
	return MUNIT_OK;
}

/*
 * Timed off-loop waiters that give up must not strand the rest: 6
 * threads take the amutex / the arwlock (exclusive) with a 1 ms timeout
 * while the holder keeps it 0-3 ms, and retry on E_AGAIN; then all of
 * them must still get it within a 10 s bound.  A timed-out waiter that
 * had consumed the one in-flight wake without passing it on would leave
 * the others asleep behind a free lock until their own deadline.
 */
#define TT_N 6
static xtc_amutex_t  *g_tt_m;
static xtc_arwlock_t *g_tt_rw;
static _Atomic int    g_tt_ok, g_tt_bad;

static void *
tt_thread(void *arg)
{
	int am = (int)(intptr_t)arg, i, rc;
	int64_t t0, t1;
	for (i = 0; i < 300; i++) {
		rc = am ? xtc_amutex_lock(g_tt_m, 1000 * 1000)
		        : xtc_arwlock_wrlock(g_tt_rw, 1000 * 1000);
		if (rc == XTC_E_AGAIN)
			continue;
		if (rc != XTC_OK) { atomic_fetch_add(&g_tt_bad, 1); break; }
		(void)__os_sleep_ns((int64_t)(i % 4) * 1000 * 1000 / 4);
		if (am) (void)xtc_amutex_unlock(g_tt_m);
		else (void)xtc_arwlock_unlock(g_tt_rw);
	}
	(void)__os_clock_mono(&t0);
	rc = am ? xtc_amutex_lock(g_tt_m, 10LL * 1000 * 1000 * 1000)
	        : xtc_arwlock_wrlock(g_tt_rw, 10LL * 1000 * 1000 * 1000);
	(void)__os_clock_mono(&t1);
	if (rc != XTC_OK || t1 - t0 > 2LL * 1000 * 1000 * 1000) {
		atomic_fetch_add(&g_tt_bad, 1);
	} else {
		atomic_fetch_add(&g_tt_ok, 1);
		if (am) (void)xtc_amutex_unlock(g_tt_m);
		else (void)xtc_arwlock_unlock(g_tt_rw);
	}
	return NULL;
}

static MunitResult
test_offloop_timed_handoff(const MunitParameter p[], void *d)
{
	pthread_t th[TT_N];
	int am, i;
	(void)p; (void)d;
	munit_assert_int(xtc_amutex_create(&g_tt_m), ==, XTC_OK);
	munit_assert_int(xtc_arwlock_create(&g_tt_rw), ==, XTC_OK);
	for (am = 0; am < 2; am++) {
		g_tt_ok = 0; g_tt_bad = 0;
		for (i = 0; i < TT_N; i++)
			munit_assert_int(pthread_create(&th[i], NULL, tt_thread,
			    (void *)(intptr_t)am), ==, 0);
		for (i = 0; i < TT_N; i++)
			(void)pthread_join(th[i], NULL);
		munit_assert_int(atomic_load(&g_tt_bad), ==, 0);
		munit_assert_int(atomic_load(&g_tt_ok), ==, TT_N);
	}
	munit_assert_int(xtc_amutex_try_lock(g_tt_m), ==, XTC_OK);
	(void)xtc_amutex_unlock(g_tt_m);
	munit_assert_int(xtc_arwlock_wrlock(g_tt_rw, 0), ==, XTC_OK);
	(void)xtc_arwlock_unlock(g_tt_rw);
	xtc_amutex_destroy(g_tt_m);
	xtc_arwlock_destroy(g_tt_rw);
	return MUNIT_OK;
}

/*
 * Readers of DIFFERENT locks must not contend.  sqlxtc keeps one arwlock
 * per buffer frame; when these objects were packed by malloc, the hot
 * words of neighbouring locks shared cache lines, and 8 threads each
 * reading its own page bounced them (the off-loop rewrite's larger
 * object made it worse: test_snap_registry 8-thread read-only rate
 * 1.89M -> 1.51M; aligned: 2.68M).  Each object now starts on a cache
 * line; checked directly on 64 consecutively created locks of each kind
 * (the behaviour itself is a performance property, measured by
 * test_snap_registry and bench_sync_offloop, not asserted here).
 */
static MunitResult
test_sync_objects_line_aligned(const MunitParameter p[], void *d)
{
	xtc_arwlock_t *rw[64];
	xtc_amutex_t *am[64];
	int i;
	(void)p; (void)d;
	for (i = 0; i < 64; i++) {
		munit_assert_int(xtc_arwlock_create(&rw[i]), ==, XTC_OK);
		munit_assert_int(xtc_amutex_create(&am[i]), ==, XTC_OK);
	}
	for (i = 0; i < 64; i++) {
		munit_assert_size((uintptr_t)rw[i] % 64, ==, 0);
		munit_assert_size((uintptr_t)am[i] % 64, ==, 0);
	}
	for (i = 0; i < 64; i++) {
		xtc_arwlock_destroy(rw[i]);
		xtc_amutex_destroy(am[i]);
	}
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/amutex_recursive_exit", test_amutex_recursive_exit, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/amutex_recursive_thread", test_amutex_recursive_thread, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/notify_stored",       test_notify_stored,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/notify_cross_thread", test_notify_cross_thread, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/sem_basic",           test_sem_basic,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/abort_source",        test_abort_source,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/amutex_basic",        test_amutex_basic,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/amutex_mutex",        test_amutex_mutex,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/amutex_fiber",        test_amutex_fiber,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/rwlock_basic",        test_rwlock_basic,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/arwlock_fiber",       test_arwlock_fiber,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/arwlock_threads",     test_arwlock_threads,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/offloop_timed_handoff", test_offloop_timed_handoff, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/objects_line_aligned", test_sync_objects_line_aligned, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#if defined(__linux__) && defined(RUSAGE_THREAD)
	{ "/offloop_no_herd",     test_offloop_no_herd,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/offloop_spin",        test_offloop_spin,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/offloop_release_wakes", test_offloop_release_wakes, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/offloop_notify_spin", test_offloop_notify_spin, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#endif
	{ "/barrier",             test_barrier,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/gate",                test_gate,                NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fiber_timeout_expiry", test_fiber_timeout_expiry, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/sem_two_waiters",     test_sem_two_waiters,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/notify_two_waiters",  test_notify_two_waiters,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/sem_conservation",    test_sem_conservation,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/timed_lock_no_stale_timer", test_timed_lock_no_stale_timer, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/gate_drain_open",     test_gate_drain_open,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/kill_then_wake",      test_kill_then_wake,      NULL, NULL, MUNIT_TEST_OPTION_NONE, kw_params },
#if defined(XTC_HAVE_PROC_KILL_WAKE)
	{ "/kill_prompt",         test_kill_prompt,         NULL, NULL, MUNIT_TEST_OPTION_NONE, kw_kind_params },
	{ "/kill_after_park_while_running", test_kill_after_park_while_running, NULL, NULL, MUNIT_TEST_OPTION_NONE, kr_params },
#endif
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m9/sync", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
