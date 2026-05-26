/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * src/os/os_thread.c
 *	pthread implementation of the thread surface.
 */

#define _GNU_SOURCE  /* for pthread_setname_np on glibc */

#include "xtc_int.h"

#include <pthread.h>
#include <sched.h>
#include <string.h>

#include "os_thread.h"

#include <stdlib.h>

/* Heap-allocated descriptor so the pthread_t outlives the call frame. */
struct __os_thread_state {
	pthread_t pth;
};

/*
 * PUBLIC: int __os_thread_create __P((__os_thread_t *, __os_thread_fn, void *));
 */
int
__os_thread_create(__os_thread_t *thr, __os_thread_fn fn, void *arg)
{
	struct __os_thread_state *st;
	int rc;

	if (thr == NULL || fn == NULL)
		return XTC_E_INVAL;
	if ((rc = __os_malloc(sizeof(*st), (void **)&st)) != XTC_OK)
		return rc;
	if (pthread_create(&st->pth, NULL, fn, arg) != 0) {
		__os_free(st);
		return XTC_E_INTERNAL;
	}
	thr->opaque = st;
	return XTC_OK;
}

/*
 * PUBLIC: int __os_thread_join __P((__os_thread_t *, void **));
 */
int
__os_thread_join(__os_thread_t *thr, void **retval)
{
	struct __os_thread_state *st;
	void *r;
	if (thr == NULL || thr->opaque == NULL)
		return XTC_E_INVAL;
	st = thr->opaque;
	if (pthread_join(st->pth, &r) != 0)
		return XTC_E_INTERNAL;
	if (retval != NULL)
		*retval = r;
	__os_free(st);
	thr->opaque = NULL;
	return XTC_OK;
}

/*
 * PUBLIC: int __os_thread_detach __P((__os_thread_t *));
 */
int
__os_thread_detach(__os_thread_t *thr)
{
	struct __os_thread_state *st;
	if (thr == NULL || thr->opaque == NULL)
		return XTC_E_INVAL;
	st = thr->opaque;
	if (pthread_detach(st->pth) != 0)
		return XTC_E_INTERNAL;
	__os_free(st);
	thr->opaque = NULL;
	return XTC_OK;
}

/*
 * Self handles do not own a state struct; the opaque pointer holds
 * the pthread_t directly via a small static cell so the comparison
 * "this is me" works.
 *
 * PUBLIC: int __os_thread_self __P((__os_thread_t *));
 */
int
__os_thread_self(__os_thread_t *out)
{
	pthread_t *me;
	int rc;
	if (out == NULL)
		return XTC_E_INVAL;
	if ((rc = __os_malloc(sizeof(*me), (void **)&me)) != XTC_OK)
		return rc;
	*me = pthread_self();
	out->opaque = me;
	return XTC_OK;
}

/*
 * PUBLIC: void __os_thread_yield __P((void));
 */
void
__os_thread_yield(void)
{
	(void)sched_yield();
}

/*
 * PUBLIC: int __os_thread_setname __P((const char *));
 */
int
__os_thread_setname(const char *name)
{
	if (name == NULL)
		return XTC_E_INVAL;
#if defined(__linux__)
	{
		char buf[16];   /* glibc truncates at 15 + NUL. */
		strncpy(buf, name, sizeof buf - 1);
		buf[sizeof buf - 1] = '\0';
		(void)pthread_setname_np(pthread_self(), buf);
	}
#elif defined(__APPLE__)
	(void)pthread_setname_np(name);
#else
	/* No-op on platforms without a portable name API. */
	(void)name;
#endif
	return XTC_OK;
}
