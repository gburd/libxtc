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

#include "munit.h"
#include "xtc.h"
#include "xtc_sync.h"
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

static MunitTest tests[] = {
	{ "/notify_stored",       test_notify_stored,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/notify_cross_thread", test_notify_cross_thread, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/sem_basic",           test_sem_basic,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/abort_source",        test_abort_source,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/amutex_basic",        test_amutex_basic,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/amutex_mutex",        test_amutex_mutex,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/rwlock_basic",        test_rwlock_basic,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/barrier",             test_barrier,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/gate",                test_gate,                NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m9/sync", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
