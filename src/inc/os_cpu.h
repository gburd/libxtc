/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/os_cpu.h
 *	CPU + NUMA topology surface.
 */

#ifndef XTC_OS_CPU_H
#define XTC_OS_CPU_H

#include "xtc_export.h"

#include <stdint.h>

/*
 * PUBLIC: int __os_ncpus __P((void));
 * PUBLIC: int __os_ncpus_perf __P((void));
 * PUBLIC: int __os_ncpus_effic __P((void));
 * PUBLIC: int __os_numa_nnodes __P((void));
 * PUBLIC: int __os_numa_node_of_cpu __P((int));
 * PUBLIC: int __os_numa_current_node __P((void));
 * PUBLIC: int64_t __os_mem_max __P((void));
 */
XTC_API int __os_ncpus(void);
XTC_API int __os_ncpus_perf(void);
XTC_API int __os_ncpus_effic(void);
XTC_API int __os_numa_nnodes(void);
XTC_API int __os_numa_node_of_cpu(int cpu);
XTC_API int __os_numa_current_node(void);
XTC_API int64_t __os_mem_max(void);

/*
 * Internal / test hooks (Linux only): point __os_ncpus / __os_mem_max
 * at a fixture file instead of the real /sys/fs/cgroup/cpu.max or
 * memory.max, so cgroup v2 parsing is unit-testable without root or a
 * real cgroup.  NULL restores the real path.  Not part of the stable
 * API.  Linux-only: there is no cgroup path to override elsewhere.
 */
#if defined(__linux__)
void __xtc_os_cgroup_cpu_path_override(const char *path);
void __xtc_os_cgroup_mem_path_override(const char *path);
#endif

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
