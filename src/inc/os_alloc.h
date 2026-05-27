/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/os_alloc.h
 *	Hookable allocator abstraction.  Default backend is malloc(3);
 *	a vtable lets PG (or libumem, or jemalloc) substitute its own
 *	primitives.  See M1_CLAIMS.md, M1-M8.
 */

#ifndef XTC_OS_ALLOC_H
#define XTC_OS_ALLOC_H

#include <stddef.h>

/*
 * The allocator vtable.  All five callbacks must be set together; we
 * do not interleave callbacks from different backends.
 *
 * Contract on each callback:
 *	malloc(sz)       -> non-NULL pointer of >= sz bytes, or NULL on OOM.
 *	calloc(n, sz)    -> zeroed; return NULL on OOM or n*sz overflow.
 *	realloc(p, sz)   -> like malloc; if p != NULL, contents are preserved.
 *	free(p)          -> p may be NULL.
 *	aligned(a, sz)   -> a is a power of two and >= sizeof(void *); NULL on OOM.
 */
struct __os_alloc_hook {
	void *(*malloc)(size_t sz);
	void *(*calloc)(size_t n, size_t sz);
	void *(*realloc)(void *p, size_t sz);
	void  (*free)(void *p);
	void *(*aligned)(size_t align, size_t sz);
};

/*
 * Public-internal API.  Every function returns int per the BDB
 * convention except __os_free which has no failure mode.
 *
 * PUBLIC: int  __os_malloc __P((size_t, void **));
 * PUBLIC: int  __os_calloc __P((size_t, size_t, void **));
 * PUBLIC: int  __os_realloc __P((void *, size_t, void **));
 * PUBLIC: void __os_free __P((void *));
 * PUBLIC: int  __os_strdup __P((const char *, char **));
 * PUBLIC: int  __os_aligned_alloc __P((size_t, size_t, void **));
 * PUBLIC: int  __os_alloc_set_hook __P((const struct __os_alloc_hook *));
 * PUBLIC: int  __os_alloc_get_hook __P((struct __os_alloc_hook *));
 */
int  __os_malloc(size_t sz, void **out);
int  __os_calloc(size_t n, size_t sz, void **out);
int  __os_realloc(void *p, size_t sz, void **out);
void __os_free(void *p);
int  __os_strdup(const char *s, char **out);
int  __os_aligned_alloc(size_t align, size_t sz, void **out);
int  __os_alloc_set_hook(const struct __os_alloc_hook *hook);
int  __os_alloc_get_hook(struct __os_alloc_hook *out);

#endif /* XTC_OS_ALLOC_H */
