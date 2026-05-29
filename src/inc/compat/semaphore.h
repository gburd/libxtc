/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/inc/compat/semaphore.h
 *
 *	The POSIX unnamed-semaphore surface libxtc uses (sem_init,
 *	sem_destroy, sem_post, sem_wait, sem_trywait), over a Win32
 *	semaphore HANDLE, for the MSVC build.  sem_t is one HANDLE, so
 *	it fits comfortably in __os_sem_t's 64-byte storage.
 */

#ifndef XTC_COMPAT_SEMAPHORE_H
#define XTC_COMPAT_SEMAPHORE_H

#if !defined(_MSC_VER)
#  error "compat/semaphore.h is the MSVC-only shim"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <errno.h>
#include <limits.h>

typedef struct { HANDLE h; } sem_t;

static __inline int
sem_init(sem_t *s, int pshared, unsigned value)
{
	(void)pshared;     /* process-shared unnamed sems unsupported */
	s->h = CreateSemaphoreA(NULL, (LONG)value, LONG_MAX, NULL);
	return s->h != NULL ? 0 : -1;
}
static __inline int sem_destroy(sem_t *s)
{ return (s->h != NULL && CloseHandle(s->h)) ? 0 : -1; }
static __inline int sem_post(sem_t *s)
{ return ReleaseSemaphore(s->h, 1, NULL) ? 0 : -1; }
static __inline int sem_wait(sem_t *s)
{ return WaitForSingleObject(s->h, INFINITE) == WAIT_OBJECT_0 ? 0 : -1; }
static __inline int sem_trywait(sem_t *s)
{
	DWORD r = WaitForSingleObject(s->h, 0);
	if (r == WAIT_OBJECT_0) return 0;
	errno = EAGAIN;
	return -1;
}

#endif /* XTC_COMPAT_SEMAPHORE_H */
