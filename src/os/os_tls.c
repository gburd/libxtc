/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
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
