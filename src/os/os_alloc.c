/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
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

static const struct __os_alloc_hook __default_hook = {
	__default_malloc,
	__default_calloc,
	__default_realloc,
	__default_free,
	__default_aligned,
};

/*
 * The active hook is read-mostly: seq-cst atomic load on the hot path,
 * one-time init via a published pointer.
 */
static const struct __os_alloc_hook *volatile __active_hook = &__default_hook;

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
	p = __hook()->malloc(sz);
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
	p = __hook()->calloc(n, sz);
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
	q = __hook()->realloc(p, sz);
	if (q == NULL && sz != 0)
		return XTC_E_NOMEM;
	*out = q;
	return XTC_OK;
}

/*
 * PUBLIC: void __os_free __P((void *));
 */
void
__os_free(void *p)
{
	if (p == NULL)
		return;
	__hook()->free(p);
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
