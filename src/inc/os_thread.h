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
 * Thread-local storage-class specifier.  GCC and clang spell it
 * __thread; MSVC spells it __declspec(thread); C11 has _Thread_local.
 * XTC_THREAD_LOCAL is the portable spelling used throughout the
 * source.  (Defined here because both xtc_int.h and loop_int.h
 * include this header.)
 */
#if defined(_MSC_VER)
#  define XTC_THREAD_LOCAL  __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define XTC_THREAD_LOCAL  __thread
#else
#  define XTC_THREAD_LOCAL  _Thread_local
#endif

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
 * known pthread implementation.  The rwlock buffer is 256 bytes:
 * macOS's pthread_rwlock_t is ~200 (vs ~56 on glibc), so 128 was too
 * small there.
 */
struct __os_mutex  { _Alignas(long long) unsigned char storage[64];  };
struct __os_rwlock { _Alignas(long long) unsigned char storage[256]; };
struct __os_cond   { _Alignas(long long) unsigned char storage[64];  };
struct __os_sem    { _Alignas(long long) unsigned char storage[64];  };

/*
 * Static initializers for file-scope-static locks (no _init() call).
 * POSIX has PTHREAD_MUTEX_INITIALIZER / PTHREAD_RWLOCK_INITIALIZER; on
 * the coming Win32 backing the mutex is an SRWLock (which HAS a static
 * initializer, SRWLOCK_INIT -- unlike CRITICAL_SECTION), so this shape
 * stays portable.  Storage is zero-padded after the pthread type; both
 * POSIX initializers are all-zero on the platforms libxtc targets, so
 * a braced-zero initializer is correct and also matches SRWLOCK_INIT
 * (a zeroed pointer).  A run-time __os_mutex_init remains available and
 * is required where a mutex is heap-allocated or needs attributes.
 */
#define XTC_OS_MUTEX_INIT   { { 0 } }
#define XTC_OS_RWLOCK_INIT  { { 0 } }

typedef struct __os_thread  __os_thread_t;
typedef struct __os_mutex   __os_mutex_t;
typedef struct __os_rwlock  __os_rwlock_t;
typedef struct __os_cond    __os_cond_t;
typedef struct __os_sem     __os_sem_t;
typedef unsigned long       __os_tls_key_t;

typedef void *(*__os_thread_fn)(void *);
typedef void  (*__os_tls_dtor)(void *);

/*
 * One-time initialization.  __os_once_t is a flag with a static
 * initializer (XTC_OS_ONCE_INIT); __os_call_once runs fn exactly once
 * across all threads racing on the same flag.  POSIX maps to
 * pthread_once; Windows to InitOnceExecuteOnce.  Replaces hand-rolled
 * set-once guards (sig_atomic_t + double-checked stores) with one
 * portable, race-free primitive.
 */
#if defined(_WIN32)
/* INIT_ONCE is a single pointer-sized slot; keep the header pthread-free. */
typedef void *__os_once_t;
#define XTC_OS_ONCE_INIT  NULL
#else
#include <pthread.h>
typedef pthread_once_t __os_once_t;
#define XTC_OS_ONCE_INIT  PTHREAD_ONCE_INIT
#endif

/*
 * --- Threads ---
 *
 * PUBLIC: int  __os_thread_create __P((__os_thread_t *, __os_thread_fn, void *));
 * PUBLIC: int  __os_thread_join __P((__os_thread_t *, void **));
 * PUBLIC: int  __os_thread_detach __P((__os_thread_t *));
 * PUBLIC: int  __os_thread_self __P((__os_thread_t *));
 * PUBLIC: void __os_thread_yield __P((void));
 * PUBLIC: int  __os_thread_setname __P((const char *));
 * PUBLIC: void __os_thread_apply_default_qos __P((void));
 * PUBLIC: int  __os_thread_set_affinity __P((int));
 */
int  __os_thread_create(__os_thread_t *thr, __os_thread_fn fn, void *arg);

/* Create a raw pthread with all signals blocked (mask restored after),
 * for the few call sites that hold a raw pthread_t rather than an
 * __os_thread_t (the PSI slab thread, the deadlock detector).  Keeps
 * every runtime thread from inheriting a permissive signal mask.
 * Declared with a pthread_t; callers already include <pthread.h>. */
#include <pthread.h>
int  __os_pthread_create_masked(pthread_t *out, void *(*fn)(void *),
         void *arg);
int  __os_thread_join(__os_thread_t *thr, void **retval);
int  __os_thread_detach(__os_thread_t *thr);
int  __os_thread_self(__os_thread_t *out);
void __os_thread_yield(void);
int  __os_thread_setname(const char *name);
void __os_thread_apply_default_qos(void);
int  __os_thread_set_affinity(int cpu);

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
 * Register fn(arg) to run when the CALLING thread exits -- including
 * threads libxtc did NOT create (an embedding host's threads, or
 * PostgreSQL's pg_threads.h carrier threads).  This is the narrow
 * "clean up my thread_local resources on the way out" contract, built
 * on the same pthread_key_t / FlsAlloc destructor mechanism as TLS but
 * without exposing a full key.  Multiple registrations on one thread
 * are all run, in reverse order of registration (last-registered
 * first), the way C atexit composes.  Returns XTC_OK, or XTC_E_NOMEM
 * if the per-thread record could not be allocated.
 *
 * PUBLIC: int __os_thread_atexit __P((void (*)(void *), void *));
 */
int __os_thread_atexit(void (*fn)(void *), void *arg);

/*
 * --- One-time init ---
 *
 * PUBLIC: int __os_call_once __P((__os_once_t *, void (*)(void)));
 */
int __os_call_once(__os_once_t *once, void (*fn)(void));

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
 * unlock is SPLIT into read/write forms: POSIX pthread_rwlock_unlock is
 * mode-agnostic, but Windows SRWLock has NO mode-agnostic release --
 * ReleaseSRWLockShared vs ReleaseSRWLockExclusive are distinct calls
 * chosen by how the lock was taken.  Callers already know which lock
 * they hold, so the split keeps the surface portable to the coming
 * Win32 backing without per-lock held-mode tracking.
 *
 * PUBLIC: int __os_rwlock_init __P((__os_rwlock_t *));
 * PUBLIC: int __os_rwlock_destroy __P((__os_rwlock_t *));
 * PUBLIC: int __os_rwlock_rdlock __P((__os_rwlock_t *));
 * PUBLIC: int __os_rwlock_wrlock __P((__os_rwlock_t *));
 * PUBLIC: int __os_rwlock_rdunlock __P((__os_rwlock_t *));
 * PUBLIC: int __os_rwlock_wrunlock __P((__os_rwlock_t *));
 */
int __os_rwlock_init(__os_rwlock_t *r);
int __os_rwlock_destroy(__os_rwlock_t *r);
int __os_rwlock_rdlock(__os_rwlock_t *r);
int __os_rwlock_wrlock(__os_rwlock_t *r);
int __os_rwlock_rdunlock(__os_rwlock_t *r);
int __os_rwlock_wrunlock(__os_rwlock_t *r);

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
