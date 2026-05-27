/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m1/test_mutex.c -- verifies M1_CLAIMS.md Mu1-Mu6.
 */

#include "munit.h"
#include "xtc_int.h"

/* [Mu1] basic */
static MunitResult
test_mutex_basic(const MunitParameter p[], void *d)
{
	__os_mutex_t m;
	(void)p; (void)d;
	munit_assert_int(__os_mutex_init(&m), ==, XTC_OK);
	munit_assert_int(__os_mutex_lock(&m), ==, XTC_OK);
	munit_assert_int(__os_mutex_unlock(&m), ==, XTC_OK);
	munit_assert_int(__os_mutex_destroy(&m), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Mu2] trylock */
struct mu2_ctx { __os_mutex_t *m; int rc; };

static void *
mu2_worker(void *arg)
{
	struct mu2_ctx *c = arg;
	c->rc = __os_mutex_trylock(c->m);
	if (c->rc == XTC_OK) (void)__os_mutex_unlock(c->m);
	return NULL;
}

static MunitResult
test_mutex_trylock(const MunitParameter p[], void *d)
{
	__os_mutex_t m;
	__os_thread_t thr = {0};
	struct mu2_ctx c;
	(void)p; (void)d;
	munit_assert_int(__os_mutex_init(&m), ==, XTC_OK);
	munit_assert_int(__os_mutex_lock(&m), ==, XTC_OK);
	c.m = &m;
	munit_assert_int(__os_thread_create(&thr, mu2_worker, &c), ==, XTC_OK);
	munit_assert_int(__os_thread_join(&thr, NULL), ==, XTC_OK);
	munit_assert_int(c.rc, ==, XTC_E_AGAIN);
	munit_assert_int(__os_mutex_unlock(&m), ==, XTC_OK);

	/* Same thread now succeeds. */
	munit_assert_int(__os_mutex_trylock(&m), ==, XTC_OK);
	munit_assert_int(__os_mutex_unlock(&m), ==, XTC_OK);
	munit_assert_int(__os_mutex_destroy(&m), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Mu3] mutual exclusion. */
#define MU3_THREADS  4
#define MU3_ITERS    20000

static __os_mutex_t mu3_lock;
static int64_t      mu3_counter;

static void *
mu3_worker(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < MU3_ITERS; i++) {
		(void)__os_mutex_lock(&mu3_lock);
		mu3_counter++;             /* deliberately non-atomic */
		(void)__os_mutex_unlock(&mu3_lock);
	}
	return NULL;
}

static MunitResult
test_mutex_mutual_exclusion(const MunitParameter p[], void *d)
{
	__os_thread_t th[MU3_THREADS] = {{0}};
	int i;
	(void)p; (void)d;
	munit_assert_int(__os_mutex_init(&mu3_lock), ==, XTC_OK);
	mu3_counter = 0;
	for (i = 0; i < MU3_THREADS; i++)
		munit_assert_int(__os_thread_create(&th[i], mu3_worker, NULL),
		    ==, XTC_OK);
	for (i = 0; i < MU3_THREADS; i++)
		munit_assert_int(__os_thread_join(&th[i], NULL), ==, XTC_OK);
	munit_assert_int64(mu3_counter, ==, (int64_t)MU3_THREADS * MU3_ITERS);
	munit_assert_int(__os_mutex_destroy(&mu3_lock), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Mu4] rwlock */
static MunitResult
test_rwlock_excludes_writer(const MunitParameter p[], void *d)
{
	__os_rwlock_t r;
	(void)p; (void)d;
	munit_assert_int(__os_rwlock_init(&r), ==, XTC_OK);
	munit_assert_int(__os_rwlock_rdlock(&r), ==, XTC_OK);
	munit_assert_int(__os_rwlock_unlock(&r), ==, XTC_OK);
	munit_assert_int(__os_rwlock_wrlock(&r), ==, XTC_OK);
	munit_assert_int(__os_rwlock_unlock(&r), ==, XTC_OK);
	munit_assert_int(__os_rwlock_destroy(&r), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Mu5] cond */
struct mu5 { __os_mutex_t m; __os_cond_t c; int signaled; };

static void *
mu5_signaler(void *arg)
{
	struct mu5 *s = arg;
	(void)__os_sleep_ns(2 * XTC_NS_PER_MS);
	(void)__os_mutex_lock(&s->m);
	s->signaled = 1;
	(void)__os_cond_signal(&s->c);
	(void)__os_mutex_unlock(&s->m);
	return NULL;
}

static MunitResult
test_cond_signal(const MunitParameter p[], void *d)
{
	struct mu5 s = {0};   /* Some pthread impls (illumos) reject
	                       * pthread_mutex_init on uninitialised memory. */
	__os_thread_t thr = {0};
	(void)p; (void)d;
	munit_assert_int(__os_mutex_init(&s.m), ==, XTC_OK);
	munit_assert_int(__os_cond_init(&s.c),  ==, XTC_OK);
	s.signaled = 0;
	munit_assert_int(__os_thread_create(&thr, mu5_signaler, &s), ==, XTC_OK);
	(void)__os_mutex_lock(&s.m);
	while (!s.signaled)
		(void)__os_cond_wait(&s.c, &s.m);
	(void)__os_mutex_unlock(&s.m);
	munit_assert_int(__os_thread_join(&thr, NULL), ==, XTC_OK);
	(void)__os_cond_destroy(&s.c);
	(void)__os_mutex_destroy(&s.m);
	return MUNIT_OK;
}

/* [Mu6] sem */
static MunitResult
test_sem_post_wait(const MunitParameter p[], void *d)
{
	__os_sem_t s;
	(void)p; (void)d;
	munit_assert_int(__os_sem_init(&s, 0), ==, XTC_OK);
	munit_assert_int(__os_sem_trywait(&s), ==, XTC_E_AGAIN);
	munit_assert_int(__os_sem_post(&s), ==, XTC_OK);
	munit_assert_int(__os_sem_wait(&s), ==, XTC_OK);
	munit_assert_int(__os_sem_trywait(&s), ==, XTC_E_AGAIN);
	munit_assert_int(__os_sem_destroy(&s), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/Mu1_basic",           test_mutex_basic,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Mu2_trylock",         test_mutex_trylock,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Mu3_mutual_exclusion",test_mutex_mutual_exclusion, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Mu4_rwlock",          test_rwlock_excludes_writer, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Mu5_cond_signal",     test_cond_signal,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Mu6_sem",             test_sem_post_wait,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m1/mutex", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
