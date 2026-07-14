/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/os_cpu.c
 *	CPU and NUMA topology probes.  Linux uses /sys/devices/system/cpu
 *	and /sys/devices/system/node; other platforms get a single-node
 *	answer until M5.5+ ports them.
 *
 *	Container/cgroup awareness (PLAN.md 19.15): on Linux, __os_ncpus
 *	and __os_mem_max first consult cgroup v2's cpu.max / memory.max so
 *	a process confined by Kubernetes/Docker/systemd sees the cap it is
 *	actually limited to, not the raw host's hardware -- the number a
 *	default reactor/memory-cap sizing decision needs.  Falls back to
 *	the uncapped host probe (/proc/cpuinfo via sysconf, or
 *	sysconf(_SC_PHYS_PAGES)) when cgroup v2 is absent, unreadable, or
 *	reports "max" (unlimited).  Both file paths are overridable via a
 *	test-only seam (__xtc_os_cgroup_cpu_path_override /
 *	__xtc_os_cgroup_mem_path_override) so a unit test can point them
 *	at a fixture file instead of the real /sys -- no root or real
 *	cgroup required.  This is a one-time host probe, not a hot path
 *	and not called from any DST sim-reachable code path (every
 *	xtc_res_init / xtc_exec_init / xtc_loop_init call site runs before
 *	xtc_sim_activate; see src/evt/sim.c), so no sim guard is needed.
 */

#define _GNU_SOURCE

#include "xtc_int.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(_WIN32)
# include <windows.h>
#else
# include <unistd.h>
# if defined(__linux__)
#  include <sched.h>
# endif
#endif

#if defined(__APPLE__)
# include <sys/sysctl.h>

/* Read an integer hw.* sysctl by name; returns -1 if unavailable. */
static int
__darwin_sysctl_int(const char *name)
{
	int64_t val = 0;
	size_t len = sizeof val;
	if (sysctlbyname(name, &val, &len, NULL, 0) != 0)
		return -1;
	return (int)val;
}
#endif

#if defined(__linux__)
/* Test-only override for the cgroup v2 file paths (see os_cpu.h): a
 * unit test points these at a fixture file so the parsing logic is
 * exercised without root or a real cgroup.  NULL (the default) means
 * "use the real /sys/fs/cgroup path". */
static const char *__cgroup_cpu_path_override;
static const char *__cgroup_mem_path_override;

void
__xtc_os_cgroup_cpu_path_override(const char *path)
{
	__cgroup_cpu_path_override = path;
}

void
__xtc_os_cgroup_mem_path_override(const char *path)
{
	__cgroup_mem_path_override = path;
}

/* Read the first line of "path" into buf (NUL-terminated, trailing
 * newline stripped).  Returns 0 on success, -1 if the file cannot be
 * opened or read. */
static int
__read_first_line(const char *path, char *buf, size_t bufsize)
{
	FILE *f;
	size_t n;

	f = fopen(path, "r");  /* XTC_BLOCKING_OK: one-shot host probe */
	if (f == NULL)
		return -1;
	if (fgets(buf, (int)bufsize, f) == NULL) {  /* XTC_BLOCKING_OK: one-shot host probe */
		(void)fclose(f);
		return -1;
	}
	(void)fclose(f);
	n = strlen(buf);
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = '\0';
	return 0;
}

/*
 * cgroup v2 cpu.max: "<quota> <period>" in microseconds, or
 * "max <period>" for no limit.  Effective CPU count is
 * ceil(quota/period), clamped to [1, hw_ncpus].  Returns -1 if the
 * file is absent/unreadable/unlimited so the caller falls back.
 */
static int
__cgroup_cpu_quota_ncpus(int hw_ncpus)
{
	const char *path = __cgroup_cpu_path_override != NULL ?
	    __cgroup_cpu_path_override : "/sys/fs/cgroup/cpu.max";
	char buf[128];
	char *end;
	long long quota, period;
	int n;

	if (__read_first_line(path, buf, sizeof buf) != 0)
		return -1;
	if (strncmp(buf, "max", 3) == 0)
		return -1;   /* unlimited: fall back to the hw count */

	quota = strtoll(buf, &end, 10);
	if (end == buf || quota <= 0)
		return -1;
	while (*end == ' ') end++;
	period = strtoll(end, &end, 10);
	if (period <= 0)
		return -1;

	/* Guard the ceil() arithmetic against a maliciously-huge quota
	 * (a local user who can write the cgroup file): quota+period-1
	 * could overflow long long (signed overflow is UB).  The result is
	 * only ever a CPU count clamped to [1, hw_ncpus], so if quota/period
	 * already meets or exceeds hw_ncpus, just return hw_ncpus.  The
	 * comparison uses division (never multiplication or addition) so it
	 * cannot itself overflow. */
	if (quota / period >= (long long)hw_ncpus)
		return hw_ncpus;

	n = (int)((quota + period - 1) / period);   /* ceil(quota/period) */
	if (n < 1) n = 1;
	if (n > hw_ncpus) n = hw_ncpus;
	return n;
}
#endif /* __linux__ */

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
	int hw = (n < 1) ? 1 : (int)n;
# if defined(__linux__)
	{
		int capped = __cgroup_cpu_quota_ncpus(hw);
		if (capped > 0)
			return capped;
	}
# endif
	return hw;
#endif
}

/*
 * PUBLIC: int64_t __os_mem_max __P((void));
 *
 * Usable memory cap in bytes.  Linux: cgroup v2 memory.max when set;
 * "max" (unlimited) or an absent/unreadable file falls back to
 * sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE), the host's total
 * physical memory.  Other platforms always report the host total
 * (no cgroup concept).
 */
int64_t
__os_mem_max(void)
{
#if defined(_WIN32)
	MEMORYSTATUSEX st;
	st.dwLength = sizeof st;
	if (GlobalMemoryStatusEx(&st))
		return (int64_t)st.ullTotalPhys;
	return 0;
#else
	long pages = sysconf(_SC_PHYS_PAGES);
	long pagesize = sysconf(_SC_PAGESIZE);
	int64_t host_total = (pages > 0 && pagesize > 0) ?
	    (int64_t)pages * (int64_t)pagesize : 0;
# if defined(__linux__)
	{
		const char *path = __cgroup_mem_path_override != NULL ?
		    __cgroup_mem_path_override : "/sys/fs/cgroup/memory.max";
		char buf[64];
		if (__read_first_line(path, buf, sizeof buf) == 0 &&
		    strncmp(buf, "max", 3) != 0) {
			char *end;
			long long lim = strtoll(buf, &end, 10);
			if (end != buf && lim > 0)
				return (int64_t)lim;
		}
	}
# endif
	return host_total;
#endif
}

/*
 * Number of performance ("P") logical CPUs.  On Apple Silicon the
 * scheduler exposes asymmetric "perf levels": level 0 is the
 * highest-performance class (P-cores), higher indices are slower
 * efficiency classes (E-cores).  A latency-sensitive, thread-per-core
 * runtime wants to know how many P-cores exist so it can size or bias
 * its reactor pool toward them.  On hardware with a single perf level
 * (Intel Macs, and every non-Apple platform) every CPU is equivalent,
 * so this returns the full __os_ncpus() count.
 *
 * PUBLIC: int __os_ncpus_perf __P((void));
 */
int
__os_ncpus_perf(void)
{
#if defined(__APPLE__)
	int nlevels = __darwin_sysctl_int("hw.nperflevels");
	if (nlevels > 1) {
		int p = __darwin_sysctl_int("hw.perflevel0.logicalcpu");
		if (p > 0) return p;
	}
#endif
	return __os_ncpus();
}

/*
 * Number of efficiency ("E") logical CPUs: the CPUs that are NOT in
 * the top perf level.  Returns 0 on symmetric hardware (no distinct
 * efficiency class).
 *
 * PUBLIC: int __os_ncpus_effic __P((void));
 */
int
__os_ncpus_effic(void)
{
#if defined(__APPLE__)
	int nlevels = __darwin_sysctl_int("hw.nperflevels");
	if (nlevels > 1) {
		int total = __os_ncpus();
		int perf  = __os_ncpus_perf();
		if (total > perf) return total - perf;
	}
#endif
	return 0;
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
