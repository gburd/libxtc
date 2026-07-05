/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/os_alloc.c
 *	The default backend wraps malloc(3); the hook lets PG (or
 *	libumem, etc.) substitute its own primitives.  The hook
 *	pointer itself is read with seq-cst atomic semantics so a
 *	swap from one thread is observed by every subsequent
 *	allocator call.
 */

#include "xtc_int.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <malloc.h>            /* _msize */
#elif defined(__APPLE__)
#include <malloc/malloc.h>     /* malloc_size */
#elif defined(__linux__)
#include <malloc.h>            /* malloc_usable_size (glibc and musl) */
#endif

/*
 * Default backend.  Implemented as ordinary functions so they take
 * normal addresses (a vtable of malloc directly does not work on all
 * platforms because malloc is sometimes a macro).
 */
static void *
__default_malloc(size_t sz)
{
	return malloc(sz);
}

static void *
__default_calloc(size_t n, size_t sz)
{
	return calloc(n, sz);
}

static void *
__default_realloc(void *p, size_t sz)
{
	return realloc(p, sz);
}

static void
__default_free(void *p)
{
	free(p);
}

static void *
__default_aligned(size_t align, size_t sz)
{
	void *p = NULL;
#if defined(_WIN32)
	p = _aligned_malloc(sz, align);
#elif defined(_ISOC11_SOURCE) || (__STDC_VERSION__ >= 201112L)
	if (sz % align != 0)
		sz += align - (sz % align);   /* aligned_alloc requires it */
	p = aligned_alloc(align, sz);
#else
	if (posix_memalign(&p, align, sz) != 0)
		p = NULL;
#endif
	return p;
}

static void
__default_aligned_free(void *p)
{
#if defined(_WIN32)
	/* Memory from _aligned_malloc lives on a separate heap and
	 * MUST NOT be passed to free(). */
	_aligned_free(p);
#else
	free(p);
#endif
}

static const struct __os_alloc_hook __default_hook = {
	__default_malloc,
	__default_calloc,
	__default_realloc,
	__default_free,
	__default_aligned,
	__default_aligned_free,
};

/*
 * The active hook is read-mostly: seq-cst atomic load on the hot path,
 * one-time init via a published pointer.
 */
static const struct __os_alloc_hook *volatile __active_hook = &__default_hook;

/*
 * Async-signal-unsafe-region bracket (defined in src/ptc/preempt.c, L3).
 * Forward-declared here rather than including the L3 header so the L0
 * allocator keeps its layering -- the same pattern os_time.c uses to
 * consult __xtc_sim_vclock.  Bracketing every allocator call marks the
 * malloc/free window unsafe so the preemption timer defers a
 * signal-context yield across it (and the fault handler does not unwind
 * out of a corrupt arena).  In the full build (autoconf / amalgamation)
 * preempt.c provides these.  The minimal meson M0 library does not link
 * preempt.c; it links src/os/os_unsafe_stub.c instead, which supplies
 * no-op fallbacks so the allocator links standalone there. */
void __xtc_unsafe_enter(void);
void __xtc_unsafe_leave(void);

static const struct __os_alloc_hook *
__hook(void)
{
	const struct __os_alloc_hook *h;
	h = (const struct __os_alloc_hook *)__os_atomic_load_ptr(
	    (void *const *)&__active_hook);
	return h;
}

/*
 * PUBLIC: int __os_malloc __P((size_t, void **));
 */
int
__os_malloc(size_t sz, void **out)
{
	void *p;
	if (out == NULL)
		return XTC_E_INVAL;
	__xtc_unsafe_enter();
	p = __hook()->malloc(sz);
	__xtc_unsafe_leave();
	if (p == NULL && sz != 0)
		return XTC_E_NOMEM;
	*out = p;
	return XTC_OK;
}

/*
 * PUBLIC: int __os_calloc __P((size_t, size_t, void **));
 */
int
__os_calloc(size_t n, size_t sz, void **out)
{
	void *p;
	if (out == NULL)
		return XTC_E_INVAL;
	if (n != 0 && sz > (size_t)-1 / n)
		return XTC_E_RANGE;
	__xtc_unsafe_enter();
	p = __hook()->calloc(n, sz);
	__xtc_unsafe_leave();
	if (p == NULL && n != 0 && sz != 0)
		return XTC_E_NOMEM;
	*out = p;
	return XTC_OK;
}

/*
 * PUBLIC: int __os_realloc __P((void *, size_t, void **));
 */
int
__os_realloc(void *p, size_t sz, void **out)
{
	void *q;
	if (out == NULL)
		return XTC_E_INVAL;
	__xtc_unsafe_enter();
	q = __hook()->realloc(p, sz);
	__xtc_unsafe_leave();
	if (q == NULL && sz != 0)
		return XTC_E_NOMEM;
	*out = q;
	return XTC_OK;
}

/*
 * PUBLIC: size_t __os_msize __P((void *));
 *
 * Usable byte count of a block from __os_malloc/__os_realloc -- the
 * size actually available to the caller, which may exceed what was
 * requested.  Returns 0 when the active backend cannot report it (any
 * custom hook): only the default malloc(3) backend knows usable size,
 * via the platform's primitive.  Callers treat 0 as "unknown" and fall
 * back to the requested size.
 */
size_t
__os_msize(void *p)
{
	if (p == NULL)
		return 0;
	/* Only the default backend's pointers are valid for the platform
	 * usable-size primitives; a custom hook may use a foreign heap. */
	if (__hook() != &__default_hook)
		return 0;
#if defined(_WIN32)
	return _msize(p);
#elif defined(__APPLE__)
	return malloc_size(p);
#elif defined(__linux__)
	return malloc_usable_size(p);
#else
	return 0;
#endif
}

/*
 * PUBLIC: void __os_free __P((void *));
 */
void
__os_free(void *p)
{
	if (p == NULL)
		return;
	__xtc_unsafe_enter();
	__hook()->free(p);
	__xtc_unsafe_leave();
}

/*
 * PUBLIC: int __os_strdup __P((const char *, char **));
 */
int
__os_strdup(const char *s, char **out)
{
	void *p;
	size_t n;
	int rc;

	if (s == NULL || out == NULL)
		return XTC_E_INVAL;
	n = strlen(s) + 1;
	if ((rc = __os_malloc(n, &p)) != XTC_OK)
		return rc;
	memcpy(p, s, n);
	*out = (char *)p;
	return XTC_OK;
}

/*
 * PUBLIC: int __os_aligned_alloc __P((size_t, size_t, void **));
 */
int
__os_aligned_alloc(size_t align, size_t sz, void **out)
{
	void *p;
	if (out == NULL)
		return XTC_E_INVAL;
	if (align == 0 || (align & (align - 1)) != 0)
		return XTC_E_INVAL;
	if (align < sizeof(void *))
		return XTC_E_INVAL;
	p = __hook()->aligned(align, sz);
	if (p == NULL && sz != 0)
		return XTC_E_NOMEM;
	*out = p;
	return XTC_OK;
}

/*
 * PUBLIC: void __os_aligned_free __P((void *));
 */
void
__os_aligned_free(void *p)
{
	if (p == NULL)
		return;
	__hook()->aligned_free(p);
}

/*
 * PUBLIC: int __os_alloc_set_hook __P((const struct __os_alloc_hook *));
 */
int
__os_alloc_set_hook(const struct __os_alloc_hook *h)
{
	if (h == NULL || h->malloc == NULL || h->calloc == NULL ||
	    h->realloc == NULL || h->free == NULL || h->aligned == NULL)
		return XTC_E_INVAL;
	__os_atomic_store_ptr((void **)&__active_hook, (void *)(uintptr_t)h);
	return XTC_OK;
}

/*
 * PUBLIC: int __os_alloc_get_hook __P((struct __os_alloc_hook *));
 */
int
__os_alloc_get_hook(struct __os_alloc_hook *out)
{
	const struct __os_alloc_hook *h;
	if (out == NULL)
		return XTC_E_INVAL;
	h = __hook();
	*out = *h;
	return XTC_OK;
}
