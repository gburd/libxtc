/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m9/test_sync.c -- verifies M9 notify + sem + abort_source.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_sync.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_blocking.h"
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
	(void)xtc_sleep_ns(120 * 1000 * 1000LL);   /* 120 ms */
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

static MunitResult
test_fiber_timeout_expiry(const MunitParameter p[], void *d)
{
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
}

static MunitTest tests[] = {
	{ "/notify_stored",       test_notify_stored,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/notify_cross_thread", test_notify_cross_thread, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/sem_basic",           test_sem_basic,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/abort_source",        test_abort_source,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/amutex_basic",        test_amutex_basic,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/amutex_mutex",        test_amutex_mutex,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/amutex_fiber",        test_amutex_fiber,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/rwlock_basic",        test_rwlock_basic,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/arwlock_fiber",       test_arwlock_fiber,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/barrier",             test_barrier,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/gate",                test_gate,                NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fiber_timeout_expiry", test_fiber_timeout_expiry, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m9/sync", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
