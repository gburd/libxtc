/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/os_atomic.h
 *	The original internal atomics: sequentially consistent only, on
 *	int32_t, int64_t, uint32_t, uint64_t and void *.  Now thin aliases
 *	over the public xtc_atomic.h (explicit orders, more types), which
 *	new code uses directly.  See __os_atomic(3), xtc_atomic(3).
 */

#ifndef XTC_OS_ATOMIC_H
#define XTC_OS_ATOMIC_H

#include <stdint.h>

#include "xtc_atomic.h"
#include "xtc_compiler.h"

/* shortcut: transitional -- library files still written against C11
 * atomics get <stdatomic.h> from here; dropped once src/ is converted. */
#include <stdatomic.h>

/*
 * Each takes a pointer to PLAIN storage (atomic operations applied to
 * its address), so ordinary loads and stores stay usable on the
 * single-threaded init path before publication.
 */
#define __OS_ATOMIC_DEFINE_ALL(S, T)                                           \
	static inline T __os_atomic_load_##S(const T *p)                       \
	{                                                                      \
		return xtc_atomic_load_##S(p, xtc_mo_seq_cst);                 \
	}                                                                      \
	static inline void __os_atomic_store_##S(T *p, T v)                    \
	{                                                                      \
		xtc_atomic_store_##S(p, v, xtc_mo_seq_cst);                    \
	}                                                                      \
	static inline int __os_atomic_cas_##S(T *p, T *expect, T desired)      \
	{                                                                      \
		return xtc_atomic_cas_##S(p, expect, desired, xtc_mo_seq_cst,  \
		    xtc_mo_seq_cst);                                           \
	}                                                                      \
	static inline T __os_atomic_fetch_add_##S(T *p, T delta)               \
	{                                                                      \
		return xtc_atomic_fetch_add_##S(p, delta, xtc_mo_seq_cst);     \
	}

__OS_ATOMIC_DEFINE_ALL(i32, int32_t)
__OS_ATOMIC_DEFINE_ALL(i64, int64_t)
__OS_ATOMIC_DEFINE_ALL(u32, uint32_t)
__OS_ATOMIC_DEFINE_ALL(u64, uint64_t)
#undef __OS_ATOMIC_DEFINE_ALL

static inline void *
__os_atomic_load_ptr(void *const *p)
{
	return xtc_atomic_load_ptr((void **)(uintptr_t)p, xtc_mo_seq_cst);
}
static inline void
__os_atomic_store_ptr(void **p, void *v)
{
	xtc_atomic_store_ptr(p, v, xtc_mo_seq_cst);
}
static inline int
__os_atomic_cas_ptr(void **p, void **expect, void *desired)
{
	return xtc_atomic_cas_ptr(p, expect, desired, xtc_mo_seq_cst,
	    xtc_mo_seq_cst);
}

static inline void
__os_atomic_fence(void)
{
	xtc_atomic_thread_fence(xtc_mo_seq_cst);
}

/* CPU spin hint: XTC_CPU_RELAX (xtc_compiler.h). */
#define __os_pause() XTC_CPU_RELAX()

#endif /* XTC_OS_ATOMIC_H */
