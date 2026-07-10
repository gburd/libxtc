/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/orc/credit.c
 *	Sliding-window credit regulator over xtc_sem.  See xtc_credit.h.
 *
 *	The semaphore holds the FREE credits (initialized to the window).
 *	acquire takes one (blocking when none free); release returns one.
 *	A small mutex-guarded counter tracks in-flight and its peak for
 *	observability -- the semaphore alone cannot report the peak.
 */

#include "xtc_int.h"
#include "xtc_preempt.h"   /* __xtc_mtx_lock/unlock */
#include "xtc_sync.h"
#include "xtc_credit.h"

#include <pthread.h>

struct xtc_credit {
	xtc_sem_t       *sem;        /* free credits */
	pthread_mutex_t  lock;       /* guards the counters */
	unsigned         window;     /* max in flight */
	unsigned         in_flight;  /* acquired, not yet released */
	unsigned         peak;       /* high-water of in_flight */
};

int
xtc_credit_create(unsigned window, xtc_credit_t **out)
{
	struct xtc_credit *c = NULL;
	int rc;
	if (out == NULL || window == 0) return XTC_E_INVAL;
	*out = NULL;
	if ((rc = __os_calloc(1, sizeof *c, (void **)&c)) != XTC_OK)
		return rc;
	if ((rc = xtc_sem_create(window, &c->sem)) != XTC_OK) {
		__os_free(c);
		return rc;
	}
	(void)pthread_mutex_init(&c->lock, NULL);
	c->window = window;
	*out = c;
	return XTC_OK;
}

void
xtc_credit_destroy(xtc_credit_t *c)
{
	if (c == NULL) return;
	if (c->sem != NULL) xtc_sem_destroy(c->sem);
	(void)pthread_mutex_destroy(&c->lock);
	__os_free(c);
}

/* Bump the in-flight counter and its peak, under the lock. */
static void
credit_took_one(struct xtc_credit *c)
{
	(void)__xtc_mtx_lock(&c->lock);
	c->in_flight++;
	if (c->in_flight > c->peak)
		c->peak = c->in_flight;
	(void)__xtc_mtx_unlock(&c->lock);
}

int
xtc_credit_acquire(xtc_credit_t *c, int64_t timeout_ns)
{
	int rc;
	if (c == NULL) return XTC_E_INVAL;
	if (timeout_ns == 0)
		rc = xtc_sem_try_acquire(c->sem, 1);
	else
		rc = xtc_sem_acquire(c->sem, 1, timeout_ns);
	if (rc != XTC_OK)
		return rc;   /* XTC_E_AGAIN on timeout, propagated */
	credit_took_one(c);
	return XTC_OK;
}

int
xtc_credit_try_acquire(xtc_credit_t *c)
{
	int rc;
	if (c == NULL) return XTC_E_INVAL;
	if ((rc = xtc_sem_try_acquire(c->sem, 1)) != XTC_OK)
		return rc;
	credit_took_one(c);
	return XTC_OK;
}

int
xtc_credit_release(xtc_credit_t *c)
{
	if (c == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&c->lock);
	if (c->in_flight == 0) {
		/* release without a matching acquire: caller bug */
		(void)__xtc_mtx_unlock(&c->lock);
		return XTC_E_INVAL;
	}
	c->in_flight--;
	(void)__xtc_mtx_unlock(&c->lock);
	(void)xtc_sem_post(c->sem, 1);   /* free one credit, wake a waiter */
	return XTC_OK;
}

unsigned
xtc_credit_in_flight(const xtc_credit_t *c)
{
	unsigned n;
	if (c == NULL) return 0;
	(void)__xtc_mtx_lock((pthread_mutex_t *)&c->lock);
	n = c->in_flight;
	(void)__xtc_mtx_unlock((pthread_mutex_t *)&c->lock);
	return n;
}

unsigned
xtc_credit_peak(const xtc_credit_t *c)
{
	unsigned n;
	if (c == NULL) return 0;
	(void)__xtc_mtx_lock((pthread_mutex_t *)&c->lock);
	n = c->peak;
	(void)__xtc_mtx_unlock((pthread_mutex_t *)&c->lock);
	return n;
}

unsigned
xtc_credit_window(const xtc_credit_t *c)
{
	return c == NULL ? 0 : c->window;
}
