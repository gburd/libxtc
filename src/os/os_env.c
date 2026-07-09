/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/os_env.c
 *	Thread-safe environment access.  getenv(3) and setenv(3) are not
 *	safe against one another -- setenv may reallocate the environ
 *	block that a concurrent getenv is walking -- so every access is
 *	serialized on a single process-wide mutex, and the getter copies
 *	the value into the caller's buffer under that lock so the result
 *	cannot be invalidated by a later setenv from another thread.
 *
 *	A static PTHREAD_MUTEX_INITIALIZER (rather than an __os_mutex_t)
 *	keeps this file self-contained: no init call, no dependency on
 *	os_mutex.c / os_thread.c, so the minimal meson M0 library links
 *	it with nothing but libpthread.  On Windows the mutex path lands
 *	with the rest of the Win32 threading shim.
 */

#include "xtc_int.h"

#include <string.h>
#include <stdlib.h>

#include "os_sharp.h"

#if !defined(_WIN32)
#include <pthread.h>
static pthread_mutex_t __env_lock = PTHREAD_MUTEX_INITIALIZER;
#define ENV_LOCK()    (void)pthread_mutex_lock(&__env_lock)
#define ENV_UNLOCK()  (void)pthread_mutex_unlock(&__env_lock)
#else
#include <windows.h>
static SRWLOCK __env_lock = SRWLOCK_INIT;
#define ENV_LOCK()    AcquireSRWLockExclusive(&__env_lock)
#define ENV_UNLOCK()  ReleaseSRWLockExclusive(&__env_lock)
#endif

/*
 * PUBLIC: int __os_env_get __P((const char *, char *, size_t));
 */
int
__os_env_get(const char *name, char *buf, size_t bufsize)
{
	const char *v;
	int rc;

	if (name == NULL || (buf == NULL && bufsize != 0))
		return XTC_E_INVAL;

	ENV_LOCK();
	v = getenv(name);   /* XTC_RAW_OK: this IS the __os wrapper */
	if (v == NULL) {
		rc = XTC_E_NOTFOUND;
	} else {
		/* Copy under the lock so a concurrent setenv cannot free the
		 * string out from under us; always NUL-terminate. */
		(void)__os_strlcpy(buf, v, bufsize);
		rc = XTC_OK;
	}
	ENV_UNLOCK();

	if (rc != XTC_OK && bufsize != 0)
		buf[0] = '\0';
	return rc;
}

/*
 * PUBLIC: int __os_env_set __P((const char *, const char *, int));
 */
int
__os_env_set(const char *name, const char *value, int overwrite)
{
	int e;

	if (name == NULL || value == NULL || name[0] == '\0' ||
	    strchr(name, '=') != NULL)
		return XTC_E_INVAL;

	ENV_LOCK();
#if defined(_WIN32)
	/* MSVC's CRT has no setenv(3); _putenv_s is the equivalent.  Honor
	 * the POSIX `overwrite` flag by leaving an existing value in place
	 * when overwrite == 0 (getenv-check under the same lock). */
	if (overwrite == 0 && getenv(name) != NULL) {   /* XTC_RAW_OK: __os wrapper */
		e = 0;
	} else {
		e = _putenv_s(name, value);
	}
#else
	e = setenv(name, value, overwrite);   /* XTC_RAW_OK: this IS the wrapper */
#endif
	ENV_UNLOCK();

	if (e != 0)
		return XTC_E_NOMEM;   /* setenv/_putenv_s fail only with ENOMEM/EINVAL here */
	return XTC_OK;
}
