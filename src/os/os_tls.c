/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/os_tls.c
 *	pthread_key_t-backed TLS.  We expose pthread_key_t as
 *	__os_tls_key_t (an unsigned long) to keep the public type
 *	opaque and stable across libc.
 */

#include "xtc_int.h"

#include <pthread.h>

#include "os_thread.h"

/*
 * pthread_key_t is unsigned int on most platforms; we widen to
 * unsigned long for header stability.
 */

/*
 * PUBLIC: int __os_tls_create __P((__os_tls_key_t *, __os_tls_dtor));
 */
int
__os_tls_create(__os_tls_key_t *key, __os_tls_dtor dtor)
{
	pthread_key_t k;
	if (key == NULL)
		return XTC_E_INVAL;
	if (pthread_key_create(&k, dtor) != 0)
		return XTC_E_INTERNAL;
	*key = (__os_tls_key_t)k;
	return XTC_OK;
}

/*
 * PUBLIC: int __os_tls_destroy __P((__os_tls_key_t));
 */
int
__os_tls_destroy(__os_tls_key_t key)
{
	if (pthread_key_delete((pthread_key_t)key) != 0)
		return XTC_E_INTERNAL;
	return XTC_OK;
}

/*
 * PUBLIC: int __os_tls_set __P((__os_tls_key_t, void *));
 */
int
__os_tls_set(__os_tls_key_t key, void *value)
{
	if (pthread_setspecific((pthread_key_t)key, value) != 0)
		return XTC_E_INTERNAL;
	return XTC_OK;
}

/*
 * PUBLIC: void *__os_tls_get __P((__os_tls_key_t));
 */
void *
__os_tls_get(__os_tls_key_t key)
{
	return pthread_getspecific((pthread_key_t)key);
}

/*
 * --- Thread-exit cleanup (__os_thread_atexit) ---
 *
 * A single lazily-created pthread_key holds a per-thread stack of
 * cleanup records; the key's destructor (which pthreads runs on thread
 * exit, once the value is non-NULL) walks the stack calling each fn.
 * This runs for ANY thread that registered, not just libxtc-created
 * ones -- the whole point of the primitive (interop with host /
 * pg_threads.h carrier threads).  Windows will back the same shape with
 * FlsAlloc + a fiber/thread-local-storage callback.
 */
struct __atexit_node {
	void (*fn)(void *);
	void *arg;
	struct __atexit_node *next;
};

static pthread_key_t __atexit_key;
static __os_once_t   __atexit_once = XTC_OS_ONCE_INIT;

/* Key destructor: run the thread's cleanup stack (LIFO), free nodes. */
static void
__atexit_run(void *head)
{
	struct __atexit_node *n = head, *next;
	while (n != NULL) {
		next = n->next;
		n->fn(n->arg);
		__os_free(n);
		n = next;
	}
}

static void
__atexit_key_init(void)
{
	(void)pthread_key_create(&__atexit_key, __atexit_run);
}

/*
 * PUBLIC: int __os_thread_atexit __P((void (*)(void *), void *));
 */
int
__os_thread_atexit(void (*fn)(void *), void *arg)
{
	struct __atexit_node *n;
	int rc;
	if (fn == NULL)
		return XTC_E_INVAL;
	(void)__os_call_once(&__atexit_once, __atexit_key_init);
	if ((rc = __os_malloc(sizeof(*n), (void **)&n)) != XTC_OK)
		return rc;
	n->fn = fn;
	n->arg = arg;
	/* Push onto this thread's stack (LIFO: last registered runs first). */
	n->next = pthread_getspecific(__atexit_key);
	if (pthread_setspecific(__atexit_key, n) != 0) {
		__os_free(n);
		return XTC_E_INTERNAL;
	}
	return XTC_OK;
}
