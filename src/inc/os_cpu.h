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

#endif /* XTC_OS_CPU_H */
