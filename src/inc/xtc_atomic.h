/*-
 * Copyright (c) 2026, The XTC Project
 * @stability stable
 *
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/inc/xtc_atomic.h
 *	Portable atomics on PLAIN storage, with an explicit memory order
 *	on every operation.  Included by xtc.h.  See xtc_atomic(3).
 *
 *	The object an operation acts on is an ordinary int / uint64_t /
 *	T * -- never `_Atomic`-qualified -- so a struct that holds one is
 *	valid C and C++, has the same layout under every compiler, and
 *	needs no /experimental:c11atomics on MSVC.  Every access to such an
 *	object that can race must go through these functions; a plain read
 *	or write of it is a data race (pre-publication initialization is
 *	the usual exception).
 *
 *	    xtc_atomic_<op>_<type>(p, ..., order)
 *
 *	op:    load store exchange cas cas_weak
 *	       fetch_add fetch_sub fetch_and fetch_or fetch_xor
 *	type:  u8 i32 u32 i64 u64 long ulong llong ullong size iptr uptr
 *	       (uint8_t int32_t uint32_t int64_t uint64_t long
 *	       unsigned long, long long, unsigned long long, size_t,
 *	       intptr_t, uintptr_t; `int` / `unsigned` are i32 / u32)
 *	       and ptr (any T **, object or function pointer; load, store,
 *	       exchange, cas and cas_weak only)
 *
 *	cas is compare-and-swap: if *p == *expect it stores desired and
 *	returns 1 (ordered by `success`), else it writes the current value
 *	to *expect and returns 0 (ordered by `failure`, which must not be
 *	release / acq_rel or stronger than `success`).  cas_weak may also
 *	fail spuriously; use it in a retry loop.  fetch_* return the value
 *	BEFORE the operation.  No 16-byte operations (a pid is published
 *	with xtc_atomic_pid_t, xtc_proc.h).
 *
 *	Backends: gcc / clang (also clang-cl, icx) in C and C++ use the
 *	__atomic builtins, the same ones <stdatomic.h> expands to, so the
 *	code is instruction-for-instruction what C11 atomics give.  MSVC
 *	(C and C++, x64 and ARM64) uses the Interlocked intrinsics and
 *	ISO-volatile loads and stores with explicit barriers; an RMW is
 *	always a full barrier there (never weaker than asked).  Any other C11
 *	compiler: <stdatomic.h> casts.  The order must be a constant for
 *	the expected code: a non-constant order is run as seq_cst by gcc
 *	(stronger, never weaker).
 */

#ifndef XTC_ATOMIC_H
#define XTC_ATOMIC_H

#include <stddef.h>
#include <stdint.h>

/*
 * The memory orders.  The values are the __ATOMIC_* (and, on gcc and
 * clang, memory_order_*) values, so an order passes through unchanged.
 * There is no consume (compilers promote it to acquire).
 */
typedef enum xtc_mo {
	xtc_mo_relaxed = 0,
	xtc_mo_acquire = 2,
	xtc_mo_release = 3,
	xtc_mo_acq_rel = 4,
	xtc_mo_seq_cst = 5
} xtc_mo_t;

/* Backend selection.  XTC_ATOMIC_FORCE_C11 / XTC_ATOMIC_FORCE_MSVC are
 * test hooks (test/m1/test_xtc_atomic.c, test/dist/test_atomic_cxx.sh). */
#if defined(XTC_ATOMIC_FORCE_C11)
#define XTC__ATOMIC_C11 1
#elif defined(_MSC_VER) &&                                                     \
    (!defined(__clang__) || defined(XTC_ATOMIC_FORCE_MSVC))
#define XTC__ATOMIC_MSVC 1
#elif defined(__GNUC__) || defined(__clang__)
#define XTC__ATOMIC_GNU 1
#elif defined(__cplusplus)
#error "xtc_atomic.h: no C++ atomics backend for this compiler"
#elif defined(__STDC_NO_ATOMICS__)
#error "xtc_atomic.h: this C compiler has no atomics"
#else
#define XTC__ATOMIC_C11 1
#endif

/* The integer types, as X(suffix, type, msvc-width). */
#define XTC__ATOMIC_TYPES(X)                                                   \
	X(u8, uint8_t, 8)                                                      \
	X(i32, int32_t, 32)                                                    \
	X(u32, uint32_t, 32)                                                   \
	X(i64, int64_t, 64)                                                    \
	X(u64, uint64_t, 64)                                                   \
	X(long, long, XTC__ATOMIC_LONG_W)                                      \
	X(ulong, unsigned long, XTC__ATOMIC_LONG_W)                            \
	X(llong, long long, 64)                                                \
	X(ullong, unsigned long long, 64)                                      \
	X(size, size_t, XTC__ATOMIC_PTR_W)                                     \
	X(iptr, intptr_t, XTC__ATOMIC_PTR_W)                                   \
	X(uptr, uintptr_t, XTC__ATOMIC_PTR_W)

#if defined(XTC__ATOMIC_GNU)
/* ------------------------------------------------------------------ */
/* gcc / clang: the __atomic builtins on the plain object.            */
/* ------------------------------------------------------------------ */
#define XTC__AI static inline __attribute__((__always_inline__))

#define XTC__ATOMIC_GNU_DEF(S, T, W)                                           \
	XTC__AI T xtc_atomic_load_##S(const T *p, xtc_mo_t mo)                 \
	{                                                                      \
		return __atomic_load_n(p, (int)mo);                            \
	}                                                                      \
	XTC__AI void xtc_atomic_store_##S(T *p, T v, xtc_mo_t mo)              \
	{                                                                      \
		__atomic_store_n(p, v, (int)mo);                               \
	}                                                                      \
	XTC__AI T xtc_atomic_exchange_##S(T *p, T v, xtc_mo_t mo)              \
	{                                                                      \
		return __atomic_exchange_n(p, v, (int)mo);                     \
	}                                                                      \
	XTC__AI int xtc_atomic_cas_##S(T *p, T *expect, T desired,             \
	    xtc_mo_t success, xtc_mo_t failure)                                \
	{                                                                      \
		return __atomic_compare_exchange_n(p, expect, desired, 0,      \
		    (int)success, (int)failure);                               \
	}                                                                      \
	XTC__AI int xtc_atomic_cas_weak_##S(T *p, T *expect, T desired,        \
	    xtc_mo_t success, xtc_mo_t failure)                                \
	{                                                                      \
		return __atomic_compare_exchange_n(p, expect, desired, 1,      \
		    (int)success, (int)failure);                               \
	}                                                                      \
	XTC__AI T xtc_atomic_fetch_add_##S(T *p, T v, xtc_mo_t mo)             \
	{                                                                      \
		return __atomic_fetch_add(p, v, (int)mo);                      \
	}                                                                      \
	XTC__AI T xtc_atomic_fetch_sub_##S(T *p, T v, xtc_mo_t mo)             \
	{                                                                      \
		return __atomic_fetch_sub(p, v, (int)mo);                      \
	}                                                                      \
	XTC__AI T xtc_atomic_fetch_and_##S(T *p, T v, xtc_mo_t mo)             \
	{                                                                      \
		return __atomic_fetch_and(p, v, (int)mo);                      \
	}                                                                      \
	XTC__AI T xtc_atomic_fetch_or_##S(T *p, T v, xtc_mo_t mo)              \
	{                                                                      \
		return __atomic_fetch_or(p, v, (int)mo);                       \
	}                                                                      \
	XTC__AI T xtc_atomic_fetch_xor_##S(T *p, T v, xtc_mo_t mo)             \
	{                                                                      \
		return __atomic_fetch_xor(p, v, (int)mo);                      \
	}

XTC__ATOMIC_TYPES(XTC__ATOMIC_GNU_DEF)
#undef XTC__ATOMIC_GNU_DEF

/* The pointer operations are type-generic: pp is a T ** for any object
 * or function pointer type T *, and the value is a T *.  A non-pointer
 * operand is a compile-time error. */
#if defined(__cplusplus)
/* C++: templates whose value parameter is not deduced, so NULL, 0 and a
 * function name convert to the operand's own pointer type, as they do
 * in C. */
extern "C++" {
/* Defined for pointer types only, so a non-pointer operand fails to
 * compile; ::type is also the non-deduced value parameter. */
template <typename P> struct xtc__atomic_id;
template <typename T> struct xtc__atomic_id<T *> {
	typedef T *type;
};
template <typename P>
XTC__AI typename xtc__atomic_id<P>::type
xtc__atomic_load_ptr(P const *pp, int mo)
{
	return __atomic_load_n(pp, mo);
}
template <typename P>
XTC__AI void
xtc__atomic_store_ptr(P *pp, typename xtc__atomic_id<P>::type v, int mo)
{
	__atomic_store_n(pp, v, mo);
}
template <typename P>
XTC__AI typename xtc__atomic_id<P>::type
xtc__atomic_exchange_ptr(P *pp, typename xtc__atomic_id<P>::type v, int mo)
{
	return __atomic_exchange_n(pp, v, mo);
}
template <typename P>
XTC__AI int
xtc__atomic_cas_ptr(P *pp, P *expect, typename xtc__atomic_id<P>::type v,
    bool weak, int success, int failure)
{
	return __atomic_compare_exchange_n(pp, expect, v, weak, success,
	    failure);
}
}
#define xtc_atomic_load_ptr(pp, mo) xtc__atomic_load_ptr((pp), (int)(mo))
#define xtc_atomic_store_ptr(pp, v, mo)                                        \
	xtc__atomic_store_ptr((pp), (v), (int)(mo))
#define xtc_atomic_exchange_ptr(pp, v, mo)                                     \
	xtc__atomic_exchange_ptr((pp), (v), (int)(mo))
#define xtc_atomic_cas_ptr(pp, expect, desired, success, failure)              \
	xtc__atomic_cas_ptr((pp), (expect), (desired), false, (int)(success),  \
	    (int)(failure))
#define xtc_atomic_cas_weak_ptr(pp, expect, desired, success, failure)         \
	xtc__atomic_cas_ptr((pp), (expect), (desired), true, (int)(success),   \
	    (int)(failure))
#else
#define XTC__PTR_CHK(pp)                                                       \
	((void)sizeof(char[__builtin_classify_type(*(pp)) == 5 ? 1 : -1]))
#define xtc_atomic_load_ptr(pp, mo)                                            \
	(XTC__PTR_CHK(pp), __atomic_load_n((pp), (int)(mo)))
#define xtc_atomic_store_ptr(pp, v, mo)                                        \
	(XTC__PTR_CHK(pp), __atomic_store_n((pp), (v), (int)(mo)))
#define xtc_atomic_exchange_ptr(pp, v, mo)                                     \
	(XTC__PTR_CHK(pp), __atomic_exchange_n((pp), (v), (int)(mo)))
#define xtc_atomic_cas_ptr(pp, expect, desired, success, failure)              \
	(XTC__PTR_CHK(pp),                                                     \
	    (int)__atomic_compare_exchange_n((pp), (expect), (desired), 0,     \
	        (int)(success), (int)(failure)))
#define xtc_atomic_cas_weak_ptr(pp, expect, desired, success, failure)         \
	(XTC__PTR_CHK(pp),                                                     \
	    (int)__atomic_compare_exchange_n((pp), (expect), (desired), 1,     \
	        (int)(success), (int)(failure)))
#endif

XTC__AI void
xtc_atomic_thread_fence(xtc_mo_t mo)
{
	__atomic_thread_fence((int)mo);
}

XTC__AI void
xtc_atomic_signal_fence(xtc_mo_t mo)
{
	__atomic_signal_fence((int)mo);
}

#elif defined(XTC__ATOMIC_MSVC)
/* ------------------------------------------------------------------ */
/* MSVC, x64 and ARM64: Interlocked* for RMW (full barrier), ISO       */
/* volatile loads/stores plus the barrier the order needs.  The       */
/* mapping is the one MSVC's own <atomic> uses.                       */
/* ------------------------------------------------------------------ */
#include <intrin.h>
#if defined(_M_ARM64) && defined(__has_include)
#if __has_include(<arm_acle.h>)
/* clang targeting ARM64 Windows (clang-cl, and the CI cross-check)
 * declares __dmb only in <arm_acle.h> on some versions (19, 21).  MSVC's
 * <intrin.h> declares it and MSVC ships no <arm_acle.h>, so this is
 * clang-only without naming clang (a -U__clang__ build still needs it). */
#include <arm_acle.h>
#endif
#endif

#if !defined(_M_X64) && !defined(_M_ARM64)
#error "xtc_atomic.h: MSVC backend supports x64 and ARM64 only"
#endif
#if defined(_M_ARM64)
#define XTC__MS_BARRIER() __dmb(0xB) /* _ARM64_BARRIER_ISH */
#else
#define XTC__MS_BARRIER() _ReadWriteBarrier()
#endif
#define XTC__ATOMIC_LONG_W 32 /* LLP64 */
#define XTC__ATOMIC_PTR_W 64
#define XTC__AI static __forceinline

/* Per width: the iso-volatile integer type, the Interlocked type and
 * the Interlocked name suffix. */
#define XTC__MS_IT_8 char
#define XTC__MS_IT_32 int
#define XTC__MS_IT_64 __int64
#define XTC__MS_LT_8 char
#define XTC__MS_LT_32 long
#define XTC__MS_LT_64 __int64
#define XTC__MS_CAT(a, b) a##b
#define XTC__MS_X(a, b) XTC__MS_CAT(a, b)
#define XTC__MS_IT(W) XTC__MS_X(XTC__MS_IT_, W)
#define XTC__MS_LT(W) XTC__MS_X(XTC__MS_LT_, W)
#define XTC__MS_LD(W) XTC__MS_X(__iso_volatile_load, W)
#define XTC__MS_ST(W) XTC__MS_X(__iso_volatile_store, W)
#define XTC__MS_IL_8(op) _Interlocked##op##8
#define XTC__MS_IL_32(op) _Interlocked##op
#define XTC__MS_IL_64(op) _Interlocked##op##64
#define XTC__MS_IL(W, op) XTC__MS_X(XTC__MS_IL_, W)(op)

#define XTC__ATOMIC_MS_DEF2(S, T, W)                                           \
	XTC__AI T xtc_atomic_load_##S(const T *p, xtc_mo_t mo)                 \
	{                                                                      \
		T v = (T)XTC__MS_LD(W)((const volatile XTC__MS_IT(W) *)p);     \
		if (mo != xtc_mo_relaxed)                                      \
			XTC__MS_BARRIER();                                     \
		return v;                                                      \
	}                                                                      \
	XTC__AI void xtc_atomic_store_##S(T *p, T v, xtc_mo_t mo)              \
	{                                                                      \
		if (mo == xtc_mo_seq_cst) {                                    \
			(void)XTC__MS_IL(W,                                    \
			    Exchange)((volatile XTC__MS_LT(W) *)p,             \
			    (XTC__MS_LT(W))v);                                 \
			return;                                                \
		}                                                              \
		if (mo != xtc_mo_relaxed)                                      \
			XTC__MS_BARRIER();                                     \
		XTC__MS_ST(W)((volatile XTC__MS_IT(W) *)p, (XTC__MS_IT(W))v);  \
	}                                                                      \
	XTC__AI T xtc_atomic_exchange_##S(T *p, T v, xtc_mo_t mo)              \
	{                                                                      \
		(void)mo;                                                      \
		return (T)XTC__MS_IL(W, Exchange)((volatile XTC__MS_LT(W) *)p, \
		    (XTC__MS_LT(W))v);                                         \
	}                                                                      \
	XTC__AI int xtc_atomic_cas_##S(T *p, T *expect, T desired,             \
	    xtc_mo_t success, xtc_mo_t failure)                                \
	{                                                                      \
		T old = (T)XTC__MS_IL(W,                                       \
		    CompareExchange)((volatile XTC__MS_LT(W) *)p,              \
		    (XTC__MS_LT(W))desired, (XTC__MS_LT(W)) * expect);         \
		(void)success;                                                 \
		(void)failure;                                                 \
		if (old == *expect)                                            \
			return 1;                                              \
		*expect = old;                                                 \
		return 0;                                                      \
	}                                                                      \
	XTC__AI int xtc_atomic_cas_weak_##S(T *p, T *expect, T desired,        \
	    xtc_mo_t success, xtc_mo_t failure)                                \
	{                                                                      \
		return xtc_atomic_cas_##S(p, expect, desired, success,         \
		    failure);                                                  \
	}                                                                      \
	XTC__AI T xtc_atomic_fetch_add_##S(T *p, T v, xtc_mo_t mo)             \
	{                                                                      \
		(void)mo;                                                      \
		return (                                                       \
		    T)XTC__MS_IL(W, ExchangeAdd)((volatile XTC__MS_LT(W) *)p,  \
		    (XTC__MS_LT(W))v);                                         \
	}                                                                      \
	XTC__AI T xtc_atomic_fetch_sub_##S(T *p, T v, xtc_mo_t mo)             \
	{                                                                      \
		(void)mo;                                                      \
		return (                                                       \
		    T)XTC__MS_IL(W, ExchangeAdd)((volatile XTC__MS_LT(W) *)p,  \
		    (XTC__MS_LT(W))(0 - v));                                   \
	}                                                                      \
	XTC__AI T xtc_atomic_fetch_and_##S(T *p, T v, xtc_mo_t mo)             \
	{                                                                      \
		(void)mo;                                                      \
		return (T)XTC__MS_IL(W, And)((volatile XTC__MS_LT(W) *)p,      \
		    (XTC__MS_LT(W))v);                                         \
	}                                                                      \
	XTC__AI T xtc_atomic_fetch_or_##S(T *p, T v, xtc_mo_t mo)              \
	{                                                                      \
		(void)mo;                                                      \
		return (T)XTC__MS_IL(W, Or)((volatile XTC__MS_LT(W) *)p,       \
		    (XTC__MS_LT(W))v);                                         \
	}                                                                      \
	XTC__AI T xtc_atomic_fetch_xor_##S(T *p, T v, xtc_mo_t mo)             \
	{                                                                      \
		(void)mo;                                                      \
		return (T)XTC__MS_IL(W, Xor)((volatile XTC__MS_LT(W) *)p,      \
		    (XTC__MS_LT(W))v);                                         \
	}
/* Expand W before the per-width token pasting. */
#define XTC__ATOMIC_MS_DEF(S, T, W) XTC__ATOMIC_MS_DEF2(S, T, W)

XTC__ATOMIC_TYPES(XTC__ATOMIC_MS_DEF)
#undef XTC__ATOMIC_MS_DEF

XTC__AI void *
xtc__ms_load_ptr(void *const *p, xtc_mo_t mo)
{
	void *v = (void *)__iso_volatile_load64((const volatile __int64 *)p);

	if (mo != xtc_mo_relaxed)
		XTC__MS_BARRIER();
	return v;
}
XTC__AI void
xtc__ms_store_ptr(void **p, void *v, xtc_mo_t mo)
{
	if (mo == xtc_mo_seq_cst) {
		(void)_InterlockedExchangePointer((void *volatile *)p, v);
		return;
	}
	if (mo != xtc_mo_relaxed)
		XTC__MS_BARRIER();
	__iso_volatile_store64((volatile __int64 *)p, (__int64)v);
}
XTC__AI int
xtc__ms_cas_ptr(void **p, void **expect, void *desired)
{
	void *old = _InterlockedCompareExchangePointer((void *volatile *)p,
	    desired, *expect);

	if (old == *expect)
		return 1;
	*expect = old;
	return 0;
}
/* The result is void * in C (it converts to T * on assignment); C++
 * casts it back to the operand's own type. */
#if defined(__cplusplus)
#define XTC__MS_PCAST(pp, e) ((decltype(+*(pp)))(e))
#else
#define XTC__MS_PCAST(pp, e) (e)
#endif
#define xtc_atomic_load_ptr(pp, mo)                                            \
	XTC__MS_PCAST(pp, xtc__ms_load_ptr((void *const *)(pp), (mo)))
#define xtc_atomic_store_ptr(pp, v, mo)                                        \
	xtc__ms_store_ptr((void **)(pp), (void *)(v), (mo))
#define xtc_atomic_exchange_ptr(pp, v, mo)                                     \
	XTC__MS_PCAST(pp,                                                      \
	    _InterlockedExchangePointer((void *volatile *)(pp), (void *)(v)))
#define xtc_atomic_cas_ptr(pp, expect, desired, success, failure)              \
	xtc__ms_cas_ptr((void **)(pp), (void **)(expect), (void *)(desired))
#define xtc_atomic_cas_weak_ptr(pp, expect, desired, success, failure)         \
	xtc__ms_cas_ptr((void **)(pp), (void **)(expect), (void *)(desired))

XTC__AI void
xtc_atomic_thread_fence(xtc_mo_t mo)
{
	if (mo == xtc_mo_relaxed)
		return;
#if defined(_M_X64)
	if (mo == xtc_mo_seq_cst) {
		__faststorefence();
		return;
	}
#endif
	XTC__MS_BARRIER();
}

XTC__AI void
xtc_atomic_signal_fence(xtc_mo_t mo)
{
	if (mo != xtc_mo_relaxed)
		_ReadWriteBarrier();
}

#else /* XTC__ATOMIC_C11 */
/* ------------------------------------------------------------------ */
/* Any other C11 compiler: <stdatomic.h> through a cast of the plain  */
/* object's address (the representation of an atomic integer is the  */
/* plain one on every lock-free implementation).                      */
/* ------------------------------------------------------------------ */
#include <stdatomic.h>

#define XTC__AI static inline
#define XTC__MO(mo) ((memory_order)(mo))

#define XTC__ATOMIC_C11_DEF(S, T, W)                                           \
	XTC__AI T xtc_atomic_load_##S(const T *p, xtc_mo_t mo)                 \
	{                                                                      \
		return atomic_load_explicit((_Atomic(T) *)(uintptr_t)p,        \
		    XTC__MO(mo));                                              \
	}                                                                      \
	XTC__AI void xtc_atomic_store_##S(T *p, T v, xtc_mo_t mo)              \
	{                                                                      \
		atomic_store_explicit((_Atomic(T) *)p, v, XTC__MO(mo));        \
	}                                                                      \
	XTC__AI T xtc_atomic_exchange_##S(T *p, T v, xtc_mo_t mo)              \
	{                                                                      \
		return atomic_exchange_explicit((_Atomic(T) *)p, v,            \
		    XTC__MO(mo));                                              \
	}                                                                      \
	XTC__AI int xtc_atomic_cas_##S(T *p, T *expect, T desired,             \
	    xtc_mo_t success, xtc_mo_t failure)                                \
	{                                                                      \
		return atomic_compare_exchange_strong_explicit((_Atomic(T) *)  \
		                                                   p,          \
		    expect, desired, XTC__MO(success), XTC__MO(failure));      \
	}                                                                      \
	XTC__AI int xtc_atomic_cas_weak_##S(T *p, T *expect, T desired,        \
	    xtc_mo_t success, xtc_mo_t failure)                                \
	{                                                                      \
		return atomic_compare_exchange_weak_explicit((_Atomic(T) *)p,  \
		    expect, desired, XTC__MO(success), XTC__MO(failure));      \
	}                                                                      \
	XTC__AI T xtc_atomic_fetch_add_##S(T *p, T v, xtc_mo_t mo)             \
	{                                                                      \
		return atomic_fetch_add_explicit((_Atomic(T) *)p, v,           \
		    XTC__MO(mo));                                              \
	}                                                                      \
	XTC__AI T xtc_atomic_fetch_sub_##S(T *p, T v, xtc_mo_t mo)             \
	{                                                                      \
		return atomic_fetch_sub_explicit((_Atomic(T) *)p, v,           \
		    XTC__MO(mo));                                              \
	}                                                                      \
	XTC__AI T xtc_atomic_fetch_and_##S(T *p, T v, xtc_mo_t mo)             \
	{                                                                      \
		return atomic_fetch_and_explicit((_Atomic(T) *)p, v,           \
		    XTC__MO(mo));                                              \
	}                                                                      \
	XTC__AI T xtc_atomic_fetch_or_##S(T *p, T v, xtc_mo_t mo)              \
	{                                                                      \
		return atomic_fetch_or_explicit((_Atomic(T) *)p, v,            \
		    XTC__MO(mo));                                              \
	}                                                                      \
	XTC__AI T xtc_atomic_fetch_xor_##S(T *p, T v, xtc_mo_t mo)             \
	{                                                                      \
		return atomic_fetch_xor_explicit((_Atomic(T) *)p, v,           \
		    XTC__MO(mo));                                              \
	}

XTC__ATOMIC_TYPES(XTC__ATOMIC_C11_DEF)
#undef XTC__ATOMIC_C11_DEF

/* Pointers go through _Atomic(void *); the result is void *, which
 * converts to the operand's T * on assignment. */
#define XTC__PV(pp) ((_Atomic(void *) *)(uintptr_t)(pp))
#define xtc_atomic_load_ptr(pp, mo)                                            \
	atomic_load_explicit(XTC__PV(pp), XTC__MO(mo))
#define xtc_atomic_store_ptr(pp, v, mo)                                        \
	atomic_store_explicit(XTC__PV(pp), (void *)(v), XTC__MO(mo))
#define xtc_atomic_exchange_ptr(pp, v, mo)                                     \
	atomic_exchange_explicit(XTC__PV(pp), (void *)(v), XTC__MO(mo))
#define xtc_atomic_cas_ptr(pp, expect, desired, success, failure)              \
	atomic_compare_exchange_strong_explicit(XTC__PV(pp),                   \
	    (void **)(expect), (void *)(desired), XTC__MO(success),            \
	    XTC__MO(failure))
#define xtc_atomic_cas_weak_ptr(pp, expect, desired, success, failure)         \
	atomic_compare_exchange_weak_explicit(XTC__PV(pp), (void **)(expect),  \
	    (void *)(desired), XTC__MO(success), XTC__MO(failure))

XTC__AI void
xtc_atomic_thread_fence(xtc_mo_t mo)
{
	atomic_thread_fence(XTC__MO(mo));
}

XTC__AI void
xtc_atomic_signal_fence(xtc_mo_t mo)
{
	atomic_signal_fence(XTC__MO(mo));
}
#endif

#endif /* XTC_ATOMIC_H */
