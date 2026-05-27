/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/inject.c
 *	Injection points implementation.  Tiny linear table protected
 *	by a single mutex, plus per-name condvars for "wait" semantics.
 *	Lookups are linear in the number of attached names; we cap at
 *	256 names which is more than enough.
 */

#include "xtc_int.h"
#include "xtc_inject.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define XTC_INJECT_MAX_NAMES   256
#define XTC_INJECT_MAX_PER     4

struct attachment {
	xtc_inject_fn fn;
	void         *user;
};

struct point {
	char              name[XTC_INJECT_NAME_MAX];
	struct attachment cbs[XTC_INJECT_MAX_PER];
	int               n_cbs;
	int               wait_attached;     /* boolean */
	int               wait_release;      /* "go" flag set by wakeup */
	pthread_cond_t    cv;
};

static pthread_mutex_t __pts_lock = PTHREAD_MUTEX_INITIALIZER;
static struct point   *__pts[XTC_INJECT_MAX_NAMES];
static int             __pts_n;
static _Atomic int     __pts_attached_count;

static struct point *
__find_locked(const char *name)
{
	int i;
	for (i = 0; i < __pts_n; i++)
		if (strncmp(__pts[i]->name, name, XTC_INJECT_NAME_MAX) == 0)
			return __pts[i];
	return NULL;
}

static struct point *
__get_or_create_locked(const char *name)
{
	struct point *p = __find_locked(name);
	if (p != NULL) return p;
	if (__pts_n >= XTC_INJECT_MAX_NAMES) return NULL;
	if (__os_calloc(1, sizeof *p, (void **)&p) != XTC_OK) return NULL;
	(void)pthread_cond_init(&p->cv, NULL);
	(void)strncpy(p->name, name, XTC_INJECT_NAME_MAX - 1);
	p->name[XTC_INJECT_NAME_MAX - 1] = '\0';
	__pts[__pts_n++] = p;
	return p;
}

int
xtc_inject_attach(const char *name, xtc_inject_fn fn, void *user)
{
	struct point *p;
	int rc = XTC_OK;
	if (name == NULL || fn == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&__pts_lock);
	p = __get_or_create_locked(name);
	if (p == NULL) { rc = XTC_E_RESOURCE; goto out; }
	if (p->n_cbs >= XTC_INJECT_MAX_PER) { rc = XTC_E_RESOURCE; goto out; }
	p->cbs[p->n_cbs].fn   = fn;
	p->cbs[p->n_cbs].user = user;
	p->n_cbs++;
	atomic_fetch_add_explicit(&__pts_attached_count, 1,
	    memory_order_relaxed);
out:
	(void)pthread_mutex_unlock(&__pts_lock);
	return rc;
}

int
xtc_inject_attach_wait(const char *name)
{
	struct point *p;
	int rc = XTC_OK;
	if (name == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&__pts_lock);
	p = __get_or_create_locked(name);
	if (p == NULL) { rc = XTC_E_RESOURCE; goto out; }
	if (p->wait_attached) goto out;     /* idempotent */
	p->wait_attached = 1;
	p->wait_release  = 0;
	atomic_fetch_add_explicit(&__pts_attached_count, 1,
	    memory_order_relaxed);
out:
	(void)pthread_mutex_unlock(&__pts_lock);
	return rc;
}

int
xtc_inject_detach(const char *name)
{
	struct point *p;
	int rc = XTC_E_INVAL;
	int i;
	if (name == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&__pts_lock);
	p = __find_locked(name);
	if (p != NULL) {
		int sub = p->n_cbs + (p->wait_attached ? 1 : 0);
		p->n_cbs = 0;
		p->wait_attached = 0;
		p->wait_release  = 1;        /* unblock any waiter */
		(void)pthread_cond_broadcast(&p->cv);
		atomic_fetch_sub_explicit(&__pts_attached_count, sub,
		    memory_order_relaxed);
		rc = XTC_OK;
	}
	(void)pthread_mutex_unlock(&__pts_lock);
	(void)i;
	return rc;
}

void
xtc_inject_trigger(const char *name)
{
	struct point *p;
	int i, n_cbs;
	struct attachment cbs_copy[XTC_INJECT_MAX_PER];
	int wait_attached;

	if (name == NULL) return;

	(void)pthread_mutex_lock(&__pts_lock);
	p = __find_locked(name);
	if (p == NULL || (p->n_cbs == 0 && !p->wait_attached)) {
		(void)pthread_mutex_unlock(&__pts_lock);
		return;
	}
	/* Snapshot callbacks under the lock so we can release before
	 * invoking them (avoid recursion-deadlock if a callback calls
	 * back into the inject API). */
	n_cbs = p->n_cbs;
	for (i = 0; i < n_cbs; i++) cbs_copy[i] = p->cbs[i];
	wait_attached = p->wait_attached;

	if (wait_attached) {
		while (!p->wait_release)
			(void)pthread_cond_wait(&p->cv, &__pts_lock);
		p->wait_release = 0;        /* re-arm for next trigger */
	}
	(void)pthread_mutex_unlock(&__pts_lock);

	for (i = 0; i < n_cbs; i++)
		cbs_copy[i].fn(name, cbs_copy[i].user);
}

int
xtc_inject_wakeup(const char *name)
{
	struct point *p;
	int rc = XTC_E_INVAL;
	if (name == NULL) return XTC_E_INVAL;
	(void)pthread_mutex_lock(&__pts_lock);
	p = __find_locked(name);
	if (p != NULL && p->wait_attached) {
		p->wait_release = 1;
		(void)pthread_cond_broadcast(&p->cv);
		rc = XTC_OK;
	}
	(void)pthread_mutex_unlock(&__pts_lock);
	return rc;
}

int
xtc_inject_n_attached(void)
{
	return atomic_load_explicit(&__pts_attached_count,
	    memory_order_relaxed);
}

/*
 * PUBLIC: int xtc_inject_check __P((const char *));
 *
 * Lock-free check: returns 1 if `name` has any attachments
 * (callbacks or wait), 0 otherwise.  Used by fault-injection sites
 * in production code that want to substitute an error path:
 *
 *   if (xtc_inject_check("io.calloc_fail")) {
 *       xtc_inject_trigger("io.calloc_fail");
 *       return XTC_E_NOMEM;
 *   }
 *
 * The check is fast-path-friendly because the global counter is
 * 0 in production builds with no attached injection points.
 */
int
xtc_inject_check(const char *name)
{
	struct point *p;
	int hit = 0;
	if (atomic_load_explicit(&__pts_attached_count,
	    memory_order_relaxed) == 0) return 0;
	(void)pthread_mutex_lock(&__pts_lock);
	p = __find_locked(name);
	if (p != NULL && (p->n_cbs > 0 || p->wait_attached)) hit = 1;
	(void)pthread_mutex_unlock(&__pts_lock);
	return hit;
}
