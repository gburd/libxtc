/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/orc/pool.c
 *	Bounded resource pool over a counting semaphore + a mutex-protected
 *	free array.  See src/inc/xtc_pool.h.
 *
 *	Design: the semaphore counts FREE resources.  xtc_pool_add pushes a
 *	resource onto the free array and posts one unit.  xtc_pool_checkout
 *	acquires one unit (blocking the fiber until a resource frees, or
 *	timing out) and then pops a free slot under the mutex.
 *	xtc_pool_checkin pushes the slot back and posts one unit, waking one
 *	waiter.  The semaphore does the fiber-blocking and fairness; the
 *	mutex only guards the tiny free array, held for O(1).
 */

#include "xtc_int.h"
#include "xtc_preempt.h"   /* __xtc_mtx_lock/unlock: preemption-safe */
#include "xtc_sync.h"
#include "xtc_pool.h"

#include <pthread.h>

struct xtc_pool {
	pthread_mutex_t  lock;
	xtc_sem_t       *sem;       /* counts free resources */
	void           **slots;     /* [capacity] resource pointers */
	int             *free_map;  /* [capacity] 1 = slot is free */
	size_t           cap;       /* total slots (== resources added max) */
	size_t           n_added;   /* resources added so far (<= cap) */
	size_t           n_free;    /* currently free (== sem count, tracked) */
};

int
xtc_pool_create(size_t capacity, xtc_pool_t **out)
{
	struct xtc_pool *p = NULL;
	int rc;
	if (out == NULL || capacity == 0) return XTC_E_INVAL;
	*out = NULL;
	if ((rc = __os_calloc(1, sizeof *p, (void **)&p)) != XTC_OK)
		return rc;
	if ((rc = __os_calloc(capacity, sizeof *p->slots,
	    (void **)&p->slots)) != XTC_OK)
		goto fail1;
	if ((rc = __os_calloc(capacity, sizeof *p->free_map,
	    (void **)&p->free_map)) != XTC_OK)
		goto fail2;
	if ((rc = xtc_sem_create(0, &p->sem)) != XTC_OK)
		goto fail3;
	(void)pthread_mutex_init(&p->lock, NULL);
	p->cap = capacity;
	*out = p;
	return XTC_OK;
fail3:	__os_free(p->free_map);
fail2:	__os_free(p->slots);
fail1:	__os_free(p);
	return rc;
}

void
xtc_pool_destroy(xtc_pool_t *p)
{
	if (p == NULL) return;
	if (p->sem != NULL) xtc_sem_destroy(p->sem);
	(void)pthread_mutex_destroy(&p->lock);
	__os_free(p->free_map);
	__os_free(p->slots);
	__os_free(p);
}

int
xtc_pool_add(xtc_pool_t *p, void *resource)
{
	int rc = XTC_OK;
	if (p == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&p->lock);
	if (p->n_added == p->cap) {
		rc = XTC_E_RESOURCE;
		goto out;
	}
	p->slots[p->n_added] = resource;
	p->free_map[p->n_added] = 1;
	p->n_added++;
	p->n_free++;
out:
	(void)__xtc_mtx_unlock(&p->lock);
	if (rc == XTC_OK)
		(void)xtc_sem_post(p->sem, 1);   /* one more free resource */
	return rc;
}

int
xtc_pool_checkout(xtc_pool_t *p, int64_t timeout_ns, void **out)
{
	size_t i;
	int rc;
	if (p == NULL || out == NULL) return XTC_E_INVAL;
	*out = NULL;
	/* Wait for a free unit.  timeout_ns == 0 -> try once. */
	if (timeout_ns == 0)
		rc = xtc_sem_try_acquire(p->sem, 1);
	else
		rc = xtc_sem_acquire(p->sem, 1, timeout_ns);
	if (rc != XTC_OK)
		return rc;   /* XTC_E_AGAIN on timeout/empty, propagated */
	/* We hold one unit; there is guaranteed to be a free slot. */
	(void)__xtc_mtx_lock(&p->lock);
	for (i = 0; i < p->n_added; i++) {
		if (p->free_map[i]) {
			p->free_map[i] = 0;
			p->n_free--;
			*out = p->slots[i];
			break;
		}
	}
	(void)__xtc_mtx_unlock(&p->lock);
	return XTC_OK;
}

int
xtc_pool_checkin(xtc_pool_t *p, void *resource)
{
	size_t i;
	int rc = XTC_E_INVAL;
	if (p == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&p->lock);
	for (i = 0; i < p->n_added; i++) {
		if (p->slots[i] == resource && !p->free_map[i]) {
			p->free_map[i] = 1;
			p->n_free++;
			rc = XTC_OK;
			break;
		}
	}
	(void)__xtc_mtx_unlock(&p->lock);
	if (rc == XTC_OK)
		(void)xtc_sem_post(p->sem, 1);   /* wake one waiter */
	return rc;
}

size_t
xtc_pool_available(const xtc_pool_t *p)
{
	size_t n;
	if (p == NULL) return 0;
	(void)__xtc_mtx_lock((pthread_mutex_t *)&p->lock);
	n = p->n_free;
	(void)__xtc_mtx_unlock((pthread_mutex_t *)&p->lock);
	return n;
}

size_t
xtc_pool_capacity(const xtc_pool_t *p)
{
	size_t n;
	if (p == NULL) return 0;
	(void)__xtc_mtx_lock((pthread_mutex_t *)&p->lock);
	n = p->n_added;
	(void)__xtc_mtx_unlock((pthread_mutex_t *)&p->lock);
	return n;
}
