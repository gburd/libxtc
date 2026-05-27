/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/os_thread.h
 *	Thread, TLS, mutex, rwlock, cond, sem abstractions.
 *	The pthreads implementation lives in src/os/os_*.c; a Win32
 *	implementation lands later behind the same surface.
 *	See M1_CLAIMS.md, T1-T7, L1-L5, Mu1-Mu6.
 */

#ifndef XTC_OS_THREAD_H
#define XTC_OS_THREAD_H

#include <stddef.h>
#include <stdint.h>

/*
 * Opaque handles.  The full struct lives in the implementation file;
 * callers see only an opaque pointer plus a sentinel-zero state.
 *
 * We reserve a small in-line "state" struct rather than allocating
 * via __os_malloc -- initialisation must work before the allocator
 * is initialised on some platforms.
 */
struct __os_thread { void *opaque; };
/*
 * Opaque storage with explicit alignment.  pthread_mutex_t / cond_t /
 * rwlock_t / sem_t carry stricter alignment requirements than `char`
 * on some platforms (notably illumos / Solaris where uninitialized or
 * misaligned storage causes pthread_mutex_init to return EINVAL).
 * `_Alignas(long long)` gives us 8-byte alignment which covers every
 * known pthread implementation.
 */
struct __os_mutex  { _Alignas(long long) unsigned char storage[64];  };
struct __os_rwlock { _Alignas(long long) unsigned char storage[128]; };
struct __os_cond   { _Alignas(long long) unsigned char storage[64];  };
struct __os_sem    { _Alignas(long long) unsigned char storage[64];  };

typedef struct __os_thread  __os_thread_t;
typedef struct __os_mutex   __os_mutex_t;
typedef struct __os_rwlock  __os_rwlock_t;
typedef struct __os_cond    __os_cond_t;
typedef struct __os_sem     __os_sem_t;
typedef unsigned long       __os_tls_key_t;

typedef void *(*__os_thread_fn)(void *);
typedef void  (*__os_tls_dtor)(void *);

/*
 * --- Threads ---
 *
 * PUBLIC: int  __os_thread_create __P((__os_thread_t *, __os_thread_fn, void *));
 * PUBLIC: int  __os_thread_join __P((__os_thread_t *, void **));
 * PUBLIC: int  __os_thread_detach __P((__os_thread_t *));
 * PUBLIC: int  __os_thread_self __P((__os_thread_t *));
 * PUBLIC: void __os_thread_yield __P((void));
 * PUBLIC: int  __os_thread_setname __P((const char *));
 */
int  __os_thread_create(__os_thread_t *thr, __os_thread_fn fn, void *arg);
int  __os_thread_join(__os_thread_t *thr, void **retval);
int  __os_thread_detach(__os_thread_t *thr);
int  __os_thread_self(__os_thread_t *out);
void __os_thread_yield(void);
int  __os_thread_setname(const char *name);

/*
 * --- TLS ---
 *
 * PUBLIC: int   __os_tls_create __P((__os_tls_key_t *, __os_tls_dtor));
 * PUBLIC: int   __os_tls_destroy __P((__os_tls_key_t));
 * PUBLIC: int   __os_tls_set __P((__os_tls_key_t, void *));
 * PUBLIC: void *__os_tls_get __P((__os_tls_key_t));
 */
int   __os_tls_create(__os_tls_key_t *key, __os_tls_dtor dtor);
int   __os_tls_destroy(__os_tls_key_t key);
int   __os_tls_set(__os_tls_key_t key, void *value);
void *__os_tls_get(__os_tls_key_t key);

/*
 * --- Mutex ---
 *
 * PUBLIC: int  __os_mutex_init __P((__os_mutex_t *));
 * PUBLIC: int  __os_mutex_destroy __P((__os_mutex_t *));
 * PUBLIC: int  __os_mutex_lock __P((__os_mutex_t *));
 * PUBLIC: int  __os_mutex_trylock __P((__os_mutex_t *));
 * PUBLIC: int  __os_mutex_unlock __P((__os_mutex_t *));
 */
int __os_mutex_init(__os_mutex_t *m);
int __os_mutex_destroy(__os_mutex_t *m);
int __os_mutex_lock(__os_mutex_t *m);
int __os_mutex_trylock(__os_mutex_t *m);
int __os_mutex_unlock(__os_mutex_t *m);

/*
 * --- RWLock ---
 *
 * PUBLIC: int __os_rwlock_init __P((__os_rwlock_t *));
 * PUBLIC: int __os_rwlock_destroy __P((__os_rwlock_t *));
 * PUBLIC: int __os_rwlock_rdlock __P((__os_rwlock_t *));
 * PUBLIC: int __os_rwlock_wrlock __P((__os_rwlock_t *));
 * PUBLIC: int __os_rwlock_unlock __P((__os_rwlock_t *));
 */
int __os_rwlock_init(__os_rwlock_t *r);
int __os_rwlock_destroy(__os_rwlock_t *r);
int __os_rwlock_rdlock(__os_rwlock_t *r);
int __os_rwlock_wrlock(__os_rwlock_t *r);
int __os_rwlock_unlock(__os_rwlock_t *r);

/*
 * --- Condition variable ---
 *
 * PUBLIC: int __os_cond_init __P((__os_cond_t *));
 * PUBLIC: int __os_cond_destroy __P((__os_cond_t *));
 * PUBLIC: int __os_cond_wait __P((__os_cond_t *, __os_mutex_t *));
 * PUBLIC: int __os_cond_signal __P((__os_cond_t *));
 * PUBLIC: int __os_cond_broadcast __P((__os_cond_t *));
 */
int __os_cond_init(__os_cond_t *c);
int __os_cond_destroy(__os_cond_t *c);
int __os_cond_wait(__os_cond_t *c, __os_mutex_t *m);
int __os_cond_signal(__os_cond_t *c);
int __os_cond_broadcast(__os_cond_t *c);

/*
 * --- Semaphore (counting; unnamed, in-process) ---
 *
 * PUBLIC: int __os_sem_init __P((__os_sem_t *, unsigned));
 * PUBLIC: int __os_sem_destroy __P((__os_sem_t *));
 * PUBLIC: int __os_sem_post __P((__os_sem_t *));
 * PUBLIC: int __os_sem_wait __P((__os_sem_t *));
 * PUBLIC: int __os_sem_trywait __P((__os_sem_t *));
 */
int __os_sem_init(__os_sem_t *s, unsigned initial);
int __os_sem_destroy(__os_sem_t *s);
int __os_sem_post(__os_sem_t *s);
int __os_sem_wait(__os_sem_t *s);
int __os_sem_trywait(__os_sem_t *s);

#endif /* XTC_OS_THREAD_H */
