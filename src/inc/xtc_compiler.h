/*-
 * Copyright (c) 2026, The XTC Project
 * @stability stable
 *
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/inc/xtc_compiler.h
 *	Portable compiler directives: branch hints, cache-line size and
 *	alignment, thread-local storage, the spin-wait hint, and bit
 *	scans.  One spelling for gcc, clang, MSVC and other C11 compilers,
 *	in C and C++, so neither the library nor a consumer writes a raw
 *	__builtin_* / __attribute__ / __declspec.  Included by xtc.h.
 *	See xtc_atomic(3).
 */

#ifndef XTC_COMPILER_H
#define XTC_COMPILER_H

#include <stdint.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#if defined(_M_ARM64) && defined(__has_include)
#if __has_include(<arm_acle.h>)
#include <arm_acle.h> /* __yield: see xtc_atomic.h */
#endif
#endif
#endif

/*
 * XTC_LIKELY / XTC_UNLIKELY -- branch hints for the hot path and the
 * error path.  No effect where the compiler has no hint.
 */
#if defined(__GNUC__) || defined(__clang__)
#define XTC_LIKELY(x) __builtin_expect(!!(x), 1)
#define XTC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define XTC_LIKELY(x) (x)
#define XTC_UNLIKELY(x) (x)
#endif

/*
 * XTC_CACHE_LINE -- the cache-line size the library pads and aligns
 * to.  Default 64; some ARM cores (Apple M-series, Cortex-X1) use 128:
 * build with -DXTC_CACHE_LINE=128 there (the library and its consumers
 * must agree, since public structs do not depend on it).
 */
#ifndef XTC_CACHE_LINE
#define XTC_CACHE_LINE 64
#endif

/*
 * XTC_ALIGNED(n) -- align a declaration to n bytes (a power of two);
 * written FIRST, like _Alignas: `XTC_ALIGNED(XTC_CACHE_LINE) uint64_t x;`.
 * An object stricter than max_align_t on the heap needs
 * xtc_aligned_alloc, not xtc_malloc.
 */
#if defined(__cplusplus)
#define XTC_ALIGNED(n) alignas(n)
#else
#define XTC_ALIGNED(n) _Alignas(n)
#endif

/*
 * XTC_THREAD_LOCAL -- thread-local storage class.  Spelled the way each
 * compiler implements it most cheaply (MSVC and clang-cl
 * __declspec(thread), gcc/clang __thread); the C11 / C++11 keyword
 * elsewhere.
 */
#if defined(_MSC_VER)
#define XTC_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define XTC_THREAD_LOCAL __thread
#elif defined(__cplusplus)
#define XTC_THREAD_LOCAL thread_local
#else
#define XTC_THREAD_LOCAL _Thread_local
#endif

/*
 * XTC_CPU_RELAX() -- the spin-wait hint (x86 PAUSE, arm64 YIELD, POWER
 * low-priority nop).  Not a yield and not a syscall: put it in the body
 * of a spin loop.  Where it is an instruction it is also a compiler
 * barrier; elsewhere it is nothing.
 */
#if defined(__GNUC__) || defined(__clang__)
#if defined(__x86_64__) || defined(__i386__)
#define XTC_CPU_RELAX() __asm__ __volatile__("pause" ::: "memory")
#elif defined(__aarch64__)
#define XTC_CPU_RELAX() __asm__ __volatile__("yield" ::: "memory")
#elif defined(__powerpc64__) || defined(__powerpc__)
#define XTC_CPU_RELAX() __asm__ __volatile__("or 27,27,27" ::: "memory")
#else
#define XTC_CPU_RELAX() ((void)0)
#endif
#elif defined(_MSC_VER)
#if defined(_M_ARM64) || defined(_M_ARM)
#define XTC_CPU_RELAX() __yield()
#else
#define XTC_CPU_RELAX() _mm_pause()
#endif
#else
#define XTC_CPU_RELAX() ((void)0)
#endif

/*
 * Bit scans.  xtc_ctz32/64: index of the lowest set bit; xtc_clz32/64:
 * number of leading zero bits.  Both are UNDEFINED for v == 0 (as the
 * instructions are); test first.  xtc_popcount32/64: number of set bits
 * (defined for every v).
 */
#if defined(__GNUC__) || defined(__clang__)
static inline int
xtc_ctz32(uint32_t v)
{
	return __builtin_ctz(v);
}
static inline int
xtc_ctz64(uint64_t v)
{
	return __builtin_ctzll(v);
}
static inline int
xtc_clz32(uint32_t v)
{
	return __builtin_clz(v);
}
static inline int
xtc_clz64(uint64_t v)
{
	return __builtin_clzll(v);
}
static inline int
xtc_popcount32(uint32_t v)
{
	return __builtin_popcount(v);
}
static inline int
xtc_popcount64(uint64_t v)
{
	return __builtin_popcountll(v);
}
#else
#if defined(_MSC_VER)
static __forceinline int
xtc_ctz32(uint32_t v)
{
	unsigned long i;

	_BitScanForward(&i, v);
	return (int)i;
}
static __forceinline int
xtc_clz32(uint32_t v)
{
	unsigned long i;

	_BitScanReverse(&i, v);
	return 31 - (int)i;
}
#if defined(_M_X64) || defined(_M_ARM64)
static __forceinline int
xtc_ctz64(uint64_t v)
{
	unsigned long i;

	_BitScanForward64(&i, v);
	return (int)i;
}
static __forceinline int
xtc_clz64(uint64_t v)
{
	unsigned long i;

	_BitScanReverse64(&i, v);
	return 63 - (int)i;
}
#else
static __forceinline int
xtc_ctz64(uint64_t v)
{
	return (uint32_t)v != 0 ? xtc_ctz32((uint32_t)v) :
	                          32 + xtc_ctz32((uint32_t)(v >> 32));
}
static __forceinline int
xtc_clz64(uint64_t v)
{
	return (v >> 32) != 0 ? xtc_clz32((uint32_t)(v >> 32)) :
	                        32 + xtc_clz32((uint32_t)v);
}
#endif
#else /* any other C11 compiler: portable loops */
static inline int
xtc_ctz64(uint64_t v)
{
	int n = 0;

	while ((v & 1) == 0) {
		v >>= 1;
		n++;
	}
	return n;
}
static inline int
xtc_clz64(uint64_t v)
{
	int n = 0;

	while ((v & ((uint64_t)1 << 63)) == 0) {
		v <<= 1;
		n++;
	}
	return n;
}
static inline int
xtc_ctz32(uint32_t v)
{
	return xtc_ctz64(v);
}
static inline int
xtc_clz32(uint32_t v)
{
	return xtc_clz64(v) - 32;
}
#endif
/* No POPCNT intrinsic: MSVC's __popcnt faults on a CPU without it. */
static inline int
xtc_popcount64(uint64_t v)
{
	v = v - ((v >> 1) & 0x5555555555555555ULL);
	v = (v & 0x3333333333333333ULL) + ((v >> 2) & 0x3333333333333333ULL);
	v = (v + (v >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
	return (int)((v * 0x0101010101010101ULL) >> 56);
}
static inline int
xtc_popcount32(uint32_t v)
{
	return xtc_popcount64(v);
}
#endif

#endif /* XTC_COMPILER_H */
