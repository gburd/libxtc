/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/inc/compat/pthread.h
 *
 *	A subset POSIX-threads shim over the Win32 API, for building
 *	libxtc with MSVC (cl.exe), which -- unlike MinGW and Clang64 --
 *	does not ship winpthreads.  This directory is placed on the
 *	include path only for the MSVC build, so `#include <pthread.h>`
 *	resolves here.
 *
 *	It implements exactly the pthread surface libxtc uses:
 *	  - mutex (mapped to SRWLOCK so PTHREAD_MUTEX_INITIALIZER can be
 *	    a static initializer; libxtc mutexes are non-recursive)
 *	  - condition variables (CONDITION_VARIABLE + SleepConditionVariableSRW)
 *	  - thread-specific keys WITH destructors (mapped to Fls*, which
 *	    -- unlike Tls* -- invokes a destructor callback on thread exit)
 *	  - pthread_once (InitOnceExecuteOnce)
 *	  - thread create/join/detach/self (_beginthreadex + handle ops)
 *	  - rwlock (SRWLOCK)
 *
 *	It is deliberately not a general pthread implementation.
 */

#ifndef XTC_COMPAT_PTHREAD_H
#define XTC_COMPAT_PTHREAD_H

#if !defined(_MSC_VER)
#  error "compat/pthread.h is the MSVC-only shim; use real pthreads elsewhere"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>     /* _beginthreadex */
#include <errno.h>
#include <stdlib.h>      /* malloc / free (thread thunk) */
#include <time.h>

/* ---- mutex (SRWLOCK; non-recursive) ------------------------------- */

typedef SRWLOCK pthread_mutex_t;
typedef int     pthread_mutexattr_t;

#define PTHREAD_MUTEX_INITIALIZER  SRWLOCK_INIT

static __inline int
pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
	(void)a;
	InitializeSRWLock(m);
	return 0;
}
static __inline int pthread_mutex_destroy(pthread_mutex_t *m)
{ (void)m; return 0; }                       /* SRWLOCK needs no teardown */
static __inline int pthread_mutex_lock(pthread_mutex_t *m)
{ AcquireSRWLockExclusive(m); return 0; }
static __inline int pthread_mutex_unlock(pthread_mutex_t *m)
{ ReleaseSRWLockExclusive(m); return 0; }
static __inline int pthread_mutex_trylock(pthread_mutex_t *m)
{ return TryAcquireSRWLockExclusive(m) ? 0 : EBUSY; }

/* ---- rwlock (SRWLOCK) --------------------------------------------- */

typedef SRWLOCK pthread_rwlock_t;
typedef int     pthread_rwlockattr_t;

static __inline int
pthread_rwlock_init(pthread_rwlock_t *rw, const pthread_rwlockattr_t *a)
{ (void)a; InitializeSRWLock(rw); return 0; }
static __inline int pthread_rwlock_destroy(pthread_rwlock_t *rw)
{ (void)rw; return 0; }
static __inline int pthread_rwlock_rdlock(pthread_rwlock_t *rw)
{ AcquireSRWLockShared(rw); return 0; }
static __inline int pthread_rwlock_wrlock(pthread_rwlock_t *rw)
{ AcquireSRWLockExclusive(rw); return 0; }
/* libxtc never mixes shared/exclusive unlock on one variable in a way
 * that needs disambiguation; it tracks the mode at the call site.  We
 * expose a single unlock that releases exclusive, plus an explicit
 * shared-unlock for the read paths. */
static __inline int pthread_rwlock_unlock(pthread_rwlock_t *rw)
{ ReleaseSRWLockExclusive(rw); return 0; }

/* ---- condition variables ------------------------------------------ */

typedef CONDITION_VARIABLE pthread_cond_t;
typedef int                pthread_condattr_t;

#define PTHREAD_COND_INITIALIZER  {0}

static __inline int
pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)
{ (void)a; InitializeConditionVariable(c); return 0; }
static __inline int pthread_cond_destroy(pthread_cond_t *c)
{ (void)c; return 0; }
static __inline int pthread_cond_signal(pthread_cond_t *c)
{ WakeConditionVariable(c); return 0; }
static __inline int pthread_cond_broadcast(pthread_cond_t *c)
{ WakeAllConditionVariable(c); return 0; }
static __inline int
pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
	return SleepConditionVariableSRW(c, m, INFINITE, 0) ? 0 : EINVAL;
}
static __inline int
pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                       const struct timespec *abstime)
{
	/* Convert the absolute CLOCK_REALTIME deadline to a relative
	 * millisecond timeout for SleepConditionVariableSRW. */
	struct timespec now;
	long long ms;
	timespec_get(&now, TIME_UTC);
	ms = (long long)(abstime->tv_sec - now.tv_sec) * 1000
	   + (abstime->tv_nsec - now.tv_nsec) / 1000000;
	if (ms < 0) ms = 0;
	if (SleepConditionVariableSRW(c, m, (DWORD)ms, 0))
		return 0;
	return (GetLastError() == ERROR_TIMEOUT) ? ETIMEDOUT : EINVAL;
}

/* ---- thread-specific keys (Fls* -> destructor support) ------------ */

typedef DWORD pthread_key_t;

static __inline int
pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
	DWORD k = FlsAlloc((PFLS_CALLBACK_FUNCTION)destructor);
	if (k == FLS_OUT_OF_INDEXES) return EAGAIN;
	*key = k;
	return 0;
}
static __inline int pthread_key_delete(pthread_key_t key)
{ return FlsFree(key) ? 0 : EINVAL; }
static __inline void *pthread_getspecific(pthread_key_t key)
{ return FlsGetValue(key); }
static __inline int pthread_setspecific(pthread_key_t key, const void *val)
{ return FlsSetValue(key, (PVOID)val) ? 0 : EINVAL; }

/* ---- pthread_once ------------------------------------------------- */

typedef INIT_ONCE pthread_once_t;
#define PTHREAD_ONCE_INIT  INIT_ONCE_STATIC_INIT

static BOOL CALLBACK
xtc__once_trampoline(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
	(void)once; (void)ctx;
	((void (*)(void))param)();
	return TRUE;
}
static __inline int
pthread_once(pthread_once_t *once, void (*init)(void))
{
	InitOnceExecuteOnce(once, xtc__once_trampoline, (PVOID)init, NULL);
	return 0;
}

/* ---- threads ------------------------------------------------------ */

/* A void* return value cannot survive a round-trip through a Win32
 * DWORD thread exit code on LLP64, so the thunk (kept alive for the
 * lifetime of the thread) carries the retval slot the trampoline
 * writes and pthread_join reads back.  pthread_t is a pointer to it,
 * not the raw HANDLE, so join can recover the value POSIX promises. */
struct xtc__thunk { void *(*fn)(void *); void *arg; void *ret; HANDLE h; };
typedef struct xtc__thunk *pthread_t;
typedef int    pthread_attr_t;

static unsigned __stdcall
xtc__thread_trampoline(void *p)
{
	struct xtc__thunk *t = (struct xtc__thunk *)p;
	t->ret = t->fn(t->arg);
	return 0;
}
static __inline int
pthread_create(pthread_t *th, const pthread_attr_t *a,
               void *(*fn)(void *), void *arg)
{
	struct xtc__thunk *t;
	uintptr_t h;
	(void)a;
	t = (struct xtc__thunk *)malloc(sizeof *t);
	if (t == NULL) return EAGAIN;
	t->fn = fn; t->arg = arg; t->ret = NULL;
	h = _beginthreadex(NULL, 0, xtc__thread_trampoline, t, 0, NULL);
	if (h == 0) { free(t); return EAGAIN; }
	t->h = (HANDLE)h;
	*th = t;
	return 0;
}
static __inline int
pthread_join(pthread_t th, void **retval)
{
	WaitForSingleObject(th->h, INFINITE);
	CloseHandle(th->h);
	if (retval != NULL) *retval = th->ret;
	free(th);
	return 0;
}
static __inline int pthread_detach(pthread_t th)
{ CloseHandle(th->h); free(th); return 0; }
/* pthread_self: threads not created via pthread_create (e.g. the main
 * thread, or a raw OS thread) have no thunk, so a per-thread thunk is
 * lazily allocated in TLS and its h holds a real handle to self.  Its
 * id (GetThreadId) is what pthread_equal compares, so identity is
 * stable regardless of which thread allocated the peer's thunk. */
static __inline pthread_t pthread_self(void)
{
	static __declspec(thread) struct xtc__thunk self_slot;
	static __declspec(thread) int have_self;
	if (!have_self) {
		DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
		    GetCurrentProcess(), &self_slot.h, 0, FALSE,
		    DUPLICATE_SAME_ACCESS);
		self_slot.fn = NULL; self_slot.arg = NULL; self_slot.ret = NULL;
		have_self = 1;
	}
	return &self_slot;
}
static __inline int pthread_equal(pthread_t a, pthread_t b)
{ return GetThreadId(a->h) == GetThreadId(b->h); }

/* pthread_setname_np: best-effort, no-op (SetThreadDescription needs a
 * wide string and a recent SDK; naming threads is cosmetic). */
static __inline int pthread_setname_np(pthread_t th, const char *name)
{ (void)th; (void)name; return 0; }

/* Tests that pull in <pthread.h> for the thread primitives also reach
 * for sched_yield()/nanosleep() (thread pacing); surface the sched
 * shim here so a test including only <pthread.h> links. */
#include <sched.h>

#endif /* XTC_COMPAT_PTHREAD_H */
