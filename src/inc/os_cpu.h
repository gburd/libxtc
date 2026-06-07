/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/os_cpu.h
 *	CPU + NUMA topology surface.
 */

#ifndef XTC_OS_CPU_H
#define XTC_OS_CPU_H

/*
 * PUBLIC: int __os_ncpus __P((void));
 * PUBLIC: int __os_numa_nnodes __P((void));
 * PUBLIC: int __os_numa_node_of_cpu __P((int));
 * PUBLIC: int __os_numa_current_node __P((void));
 */
int __os_ncpus(void);
int __os_numa_nnodes(void);
int __os_numa_node_of_cpu(int cpu);
int __os_numa_current_node(void);

/*
 * Spin-loop relaxation hint: tells the CPU we are in a busy-wait so it
 * can save power and yield pipeline/SMT resources to the lock holder
 * (x86 PAUSE, ARM YIELD).  A no-op where unavailable.  Header-only and
 * always inlined -- it must be zero-cost in a tight CAS retry loop.
 */
static inline void
__os_cpu_relax(void)
{
#if defined(__GNUC__) || defined(__clang__)
# if defined(__i386__) || defined(__x86_64__)
	__asm__ __volatile__("pause" ::: "memory");
# elif defined(__aarch64__) || defined(__arm__)
	__asm__ __volatile__("yield" ::: "memory");
# else
	__asm__ __volatile__("" ::: "memory");   /* compiler barrier only */
# endif
#else
	/* MSVC / unknown compiler: no portable hint without <intrin.h>. */
#endif
}

#endif /* XTC_OS_CPU_H */
