/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * src/inc/os_atomic.h
 *	Inline atomics over C11 <stdatomic.h>.  Sequentially-consistent
 *	by default; no relaxed orderings exposed in M1.
 *
 *	Variants are provided for int32_t, int64_t, uint32_t, uint64_t,
 *	and void*.  See M1_CLAIMS.md, A1–A8.
 */

#ifndef XTC_OS_ATOMIC_H
#define XTC_OS_ATOMIC_H

#include <stdint.h>

#if defined(__STDC_NO_ATOMICS__)
# error "xtc requires C11 atomics; rebuild with a compiler that has <stdatomic.h>"
#endif

#include <stdatomic.h>

/*
 * The variable storing the atomic value is plain (non-_Atomic).
 * We use atomic_*_explicit primitives applied to the address; this
 * gives us the same ordering guarantees as _Atomic-qualified types
 * and lets ordinary load/store work in single-threaded init paths
 * before publication.
 *
 * Each macro takes a pointer-to-T as its first argument.
 */

#define __OS_ATOMIC_LOAD_DEFINE(suffix, T)				\
	static inline T							\
	__os_atomic_load_##suffix(const T *p)				\
	{								\
		return atomic_load_explicit(				\
		    (_Atomic T *)(uintptr_t)p,				\
		    memory_order_seq_cst);				\
	}

#define __OS_ATOMIC_STORE_DEFINE(suffix, T)				\
	static inline void						\
	__os_atomic_store_##suffix(T *p, T v)				\
	{								\
		atomic_store_explicit(					\
		    (_Atomic T *)p, v, memory_order_seq_cst);		\
	}

#define __OS_ATOMIC_CAS_DEFINE(suffix, T)				\
	static inline int						\
	__os_atomic_cas_##suffix(T *p, T *expect, T desired)		\
	{								\
		return atomic_compare_exchange_strong_explicit(		\
		    (_Atomic T *)p, expect, desired,			\
		    memory_order_seq_cst, memory_order_seq_cst);	\
	}

#define __OS_ATOMIC_FETCH_ADD_DEFINE(suffix, T)				\
	static inline T							\
	__os_atomic_fetch_add_##suffix(T *p, T delta)			\
	{								\
		return atomic_fetch_add_explicit(			\
		    (_Atomic T *)p, delta,				\
		    memory_order_seq_cst);				\
	}

#define __OS_ATOMIC_DEFINE_ALL(suffix, T)				\
	__OS_ATOMIC_LOAD_DEFINE(suffix, T)				\
	__OS_ATOMIC_STORE_DEFINE(suffix, T)				\
	__OS_ATOMIC_CAS_DEFINE(suffix, T)				\
	__OS_ATOMIC_FETCH_ADD_DEFINE(suffix, T)

__OS_ATOMIC_DEFINE_ALL(i32, int32_t)
__OS_ATOMIC_DEFINE_ALL(i64, int64_t)
__OS_ATOMIC_DEFINE_ALL(u32, uint32_t)
__OS_ATOMIC_DEFINE_ALL(u64, uint64_t)

/*
 * Pointer atomics over intptr_t storage.  Pointer-typed C11 atomics
 * have surprisingly inconsistent codegen on some compilers; integer
 * atomics are reliably correct.
 */
static inline void *
__os_atomic_load_ptr(void *const *p)
{
	return (void *)(intptr_t)atomic_load_explicit(
	    (_Atomic intptr_t *)(uintptr_t)p, memory_order_seq_cst);
}
static inline void
__os_atomic_store_ptr(void **p, void *v)
{
	atomic_store_explicit(
	    (_Atomic intptr_t *)p, (intptr_t)v, memory_order_seq_cst);
}
static inline int
__os_atomic_cas_ptr(void **p, void **expect, void *desired)
{
	intptr_t e = (intptr_t)*expect;
	int ok = atomic_compare_exchange_strong_explicit(
	    (_Atomic intptr_t *)p, &e, (intptr_t)desired,
	    memory_order_seq_cst, memory_order_seq_cst);
	if (!ok) *expect = (void *)e;
	return ok;
}

static inline void
__os_atomic_fence(void)
{
	atomic_thread_fence(memory_order_seq_cst);
}

/*
 * CPU spin hint.  Not a syscall, not a yield.  Tells the CPU we are
 * spinning so it can throttle, save power, and avoid pipeline stalls
 * on the lock release.
 */
#if defined(__x86_64__) || defined(__i386__)
#  define __os_pause()		__asm__ __volatile__("pause" ::: "memory")
#elif defined(__aarch64__)
#  define __os_pause()		__asm__ __volatile__("yield" ::: "memory")
#elif defined(__powerpc64__) || defined(__powerpc__)
#  define __os_pause()		__asm__ __volatile__("or 27,27,27" ::: "memory")
#else
#  define __os_pause()		((void)0)
#endif

#undef __OS_ATOMIC_DEFINE_ALL
#undef __OS_ATOMIC_FETCH_ADD_DEFINE
#undef __OS_ATOMIC_CAS_DEFINE
#undef __OS_ATOMIC_STORE_DEFINE
#undef __OS_ATOMIC_LOAD_DEFINE

#endif /* XTC_OS_ATOMIC_H */
