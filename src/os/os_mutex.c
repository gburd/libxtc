/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/os/os_mutex.c
 *	pthread_{mutex,rwlock,cond}_t-backed locks plus an unnamed
 *	in-process semaphore.
 *
 *	The opaque-storage trick: __os_mutex_t etc. carry a fixed-size
 *	byte array sized to comfortably contain the platform's
 *	pthread_*_t.  At init we placement-construct the pthread type
 *	into that storage.  The caller never sees pthread types and
 *	gets a stable ABI on the xtc surface.
 */

#define _POSIX_C_SOURCE 200809L

#include "xtc_int.h"

#include <pthread.h>
#include <semaphore.h>
#include <errno.h>

#include "os_thread.h"

/* Compile-time assertions that storage is large enough. */
typedef char __chk_mu_size[
    sizeof(pthread_mutex_t)  <= sizeof(((__os_mutex_t  *)0)->storage) ? 1 : -1];
typedef char __chk_rw_size[
    sizeof(pthread_rwlock_t) <= sizeof(((__os_rwlock_t *)0)->storage) ? 1 : -1];
typedef char __chk_cv_size[
    sizeof(pthread_cond_t)   <= sizeof(((__os_cond_t   *)0)->storage) ? 1 : -1];
typedef char __chk_sem_size[
    sizeof(sem_t)            <= sizeof(((__os_sem_t    *)0)->storage) ? 1 : -1];

#define MU(p)  ((pthread_mutex_t  *)(void *)(p)->storage)
#define RW(p)  ((pthread_rwlock_t *)(void *)(p)->storage)
#define CV(p)  ((pthread_cond_t   *)(void *)(p)->storage)
#define SE(p)  ((sem_t            *)(void *)(p)->storage)

/* --- mutex --- */

/* PUBLIC: int __os_mutex_init __P((__os_mutex_t *)); */
int __os_mutex_init(__os_mutex_t *m) {
	if (m == NULL) return XTC_E_INVAL;
	if (pthread_mutex_init(MU(m), NULL) != 0) return XTC_E_INTERNAL;
	return XTC_OK;
}
/* PUBLIC: int __os_mutex_destroy __P((__os_mutex_t *)); */
int __os_mutex_destroy(__os_mutex_t *m) {
	if (m == NULL) return XTC_E_INVAL;
	return pthread_mutex_destroy(MU(m)) == 0 ? XTC_OK : XTC_E_INTERNAL;
}
/* PUBLIC: int __os_mutex_lock __P((__os_mutex_t *)); */
int __os_mutex_lock(__os_mutex_t *m) {
	if (m == NULL) return XTC_E_INVAL;
	return pthread_mutex_lock(MU(m)) == 0 ? XTC_OK : XTC_E_INTERNAL;
}
/* PUBLIC: int __os_mutex_trylock __P((__os_mutex_t *)); */
int __os_mutex_trylock(__os_mutex_t *m) {
	int e;
	if (m == NULL) return XTC_E_INVAL;
	e = pthread_mutex_trylock(MU(m));
	if (e == 0)         return XTC_OK;
	if (e == EBUSY)     return XTC_E_AGAIN;
	return XTC_E_INTERNAL;
}
/* PUBLIC: int __os_mutex_unlock __P((__os_mutex_t *)); */
int __os_mutex_unlock(__os_mutex_t *m) {
	if (m == NULL) return XTC_E_INVAL;
	return pthread_mutex_unlock(MU(m)) == 0 ? XTC_OK : XTC_E_INTERNAL;
}

/* --- rwlock --- */

/* PUBLIC: int __os_rwlock_init __P((__os_rwlock_t *)); */
int __os_rwlock_init(__os_rwlock_t *r) {
	if (r == NULL) return XTC_E_INVAL;
	return pthread_rwlock_init(RW(r), NULL) == 0 ? XTC_OK : XTC_E_INTERNAL;
}
/* PUBLIC: int __os_rwlock_destroy __P((__os_rwlock_t *)); */
int __os_rwlock_destroy(__os_rwlock_t *r) {
	if (r == NULL) return XTC_E_INVAL;
	return pthread_rwlock_destroy(RW(r)) == 0 ? XTC_OK : XTC_E_INTERNAL;
}
/* PUBLIC: int __os_rwlock_rdlock __P((__os_rwlock_t *)); */
int __os_rwlock_rdlock(__os_rwlock_t *r) {
	if (r == NULL) return XTC_E_INVAL;
	return pthread_rwlock_rdlock(RW(r)) == 0 ? XTC_OK : XTC_E_INTERNAL;
}
/* PUBLIC: int __os_rwlock_wrlock __P((__os_rwlock_t *)); */
int __os_rwlock_wrlock(__os_rwlock_t *r) {
	if (r == NULL) return XTC_E_INVAL;
	return pthread_rwlock_wrlock(RW(r)) == 0 ? XTC_OK : XTC_E_INTERNAL;
}
/* PUBLIC: int __os_rwlock_unlock __P((__os_rwlock_t *)); */
int __os_rwlock_unlock(__os_rwlock_t *r) {
	if (r == NULL) return XTC_E_INVAL;
	return pthread_rwlock_unlock(RW(r)) == 0 ? XTC_OK : XTC_E_INTERNAL;
}

/* --- cond --- */

/* PUBLIC: int __os_cond_init __P((__os_cond_t *)); */
int __os_cond_init(__os_cond_t *c) {
	if (c == NULL) return XTC_E_INVAL;
	return pthread_cond_init(CV(c), NULL) == 0 ? XTC_OK : XTC_E_INTERNAL;
}
/* PUBLIC: int __os_cond_destroy __P((__os_cond_t *)); */
int __os_cond_destroy(__os_cond_t *c) {
	if (c == NULL) return XTC_E_INVAL;
	return pthread_cond_destroy(CV(c)) == 0 ? XTC_OK : XTC_E_INTERNAL;
}
/* PUBLIC: int __os_cond_wait __P((__os_cond_t *, __os_mutex_t *)); */
int __os_cond_wait(__os_cond_t *c, __os_mutex_t *m) {
	if (c == NULL || m == NULL) return XTC_E_INVAL;
	return pthread_cond_wait(CV(c), MU(m)) == 0 ? XTC_OK : XTC_E_INTERNAL;
}
/* PUBLIC: int __os_cond_signal __P((__os_cond_t *)); */
int __os_cond_signal(__os_cond_t *c) {
	if (c == NULL) return XTC_E_INVAL;
	return pthread_cond_signal(CV(c)) == 0 ? XTC_OK : XTC_E_INTERNAL;
}
/* PUBLIC: int __os_cond_broadcast __P((__os_cond_t *)); */
int __os_cond_broadcast(__os_cond_t *c) {
	if (c == NULL) return XTC_E_INVAL;
	return pthread_cond_broadcast(CV(c)) == 0 ? XTC_OK : XTC_E_INTERNAL;
}

/* --- semaphore (unnamed, in-process) --- */

/* PUBLIC: int __os_sem_init __P((__os_sem_t *, unsigned)); */
int __os_sem_init(__os_sem_t *s, unsigned initial) {
	if (s == NULL) return XTC_E_INVAL;
	return sem_init(SE(s), 0, initial) == 0 ? XTC_OK : XTC_E_INTERNAL;
}
/* PUBLIC: int __os_sem_destroy __P((__os_sem_t *)); */
int __os_sem_destroy(__os_sem_t *s) {
	if (s == NULL) return XTC_E_INVAL;
	return sem_destroy(SE(s)) == 0 ? XTC_OK : XTC_E_INTERNAL;
}
/* PUBLIC: int __os_sem_post __P((__os_sem_t *)); */
int __os_sem_post(__os_sem_t *s) {
	if (s == NULL) return XTC_E_INVAL;
	return sem_post(SE(s)) == 0 ? XTC_OK : XTC_E_INTERNAL;
}
/* PUBLIC: int __os_sem_wait __P((__os_sem_t *)); */
int __os_sem_wait(__os_sem_t *s) {
	if (s == NULL) return XTC_E_INVAL;
	while (sem_wait(SE(s)) == -1) {
		if (errno == EINTR) continue;
		return XTC_E_INTERNAL;
	}
	return XTC_OK;
}
/* PUBLIC: int __os_sem_trywait __P((__os_sem_t *)); */
int __os_sem_trywait(__os_sem_t *s) {
	int e;
	if (s == NULL) return XTC_E_INVAL;
	if (sem_trywait(SE(s)) == 0) return XTC_OK;
	e = errno;
	if (e == EAGAIN) return XTC_E_AGAIN;
	return XTC_E_INTERNAL;
}
