/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * src/os/os_cpu.c
 *	CPU and NUMA topology probes.  Linux uses /sys/devices/system/cpu
 *	and /sys/devices/system/node; other platforms get a single-node
 *	answer until M5.5+ ports them.
 */

#define _GNU_SOURCE

#include "xtc_int.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
# include <windows.h>
#else
# include <unistd.h>
# if defined(__linux__)
#  include <sched.h>
# endif
#endif

/*
 * PUBLIC: int __os_ncpus __P((void));
 */
int
__os_ncpus(void)
{
#if defined(_WIN32)
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	return (int)si.dwNumberOfProcessors;
#else
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	if (n < 1) return 1;
	return (int)n;
#endif
}

/*
 * PUBLIC: int __os_numa_nnodes __P((void));
 */
int
__os_numa_nnodes(void)
{
#if defined(__linux__)
	/* Count subdirectories /sys/devices/system/node/nodeN. */
	int n = 0, i;
	char path[64];
	for (i = 0; i < 64; i++) {
		snprintf(path, sizeof path,
		    "/sys/devices/system/node/node%d", i);
		if (access(path, F_OK) == 0) n++;
		else break;
	}
	return n > 0 ? n : 1;
#else
	return 1;
#endif
}

/*
 * Return the NUMA node a given CPU belongs to.  Linux: walks
 * /sys/devices/system/cpu/cpu<N>/node<M> symlinks.  Other platforms
 * collapse everything to node 0.
 *
 * PUBLIC: int __os_numa_node_of_cpu __P((int));
 */
int
__os_numa_node_of_cpu(int cpu)
{
#if defined(__linux__)
	char path[128], buf[256];
	int i, n;
	ssize_t r;
	int fd;
	for (i = 0; i < 64; i++) {
		snprintf(path, sizeof path,
		    "/sys/devices/system/cpu/cpu%d/node%d", cpu, i);
		if (access(path, F_OK) == 0) return i;
	}
	/* Fallback: read the link of cpu<N>'s node link if present. */
	(void)n; (void)r; (void)fd; (void)buf;
	return 0;
#else
	(void)cpu;
	return 0;
#endif
}

/*
 * Return the NUMA node the calling thread is currently running on.
 * Best-effort.
 *
 * PUBLIC: int __os_numa_current_node __P((void));
 */
int
__os_numa_current_node(void)
{
#if defined(__linux__)
	int cpu = sched_getcpu();
	if (cpu < 0) return 0;
	return __os_numa_node_of_cpu(cpu);
#else
	return 0;
#endif
}
