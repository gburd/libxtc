/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/os_tuning.c
 *	Power/kernel-tuning advisor (PLAN.md 19.21).  A battery of cheap,
 *	READ-ONLY host probes run once at app startup; each probe that is
 *	NOT already in its recommended state emits one advisory log line
 *	(XTC_LOG_INFO -- the closest level this logger has to PG's
 *	NOTICE; xtc_log.h's enum stops at INFO/WARN/ERROR, see PLAN.md
 *	15.6 for the NOTICE level planned for the future structured
 *	logger).  Silent on a well-tuned host: this is an advisor, not a
 *	monitor, and must never spam an operator who already did the
 *	work.
 *
 *	Every probe is a single fopen/fgets/fclose (or, for the io_uring
 *	probe, a single syscall immediately undone) -- never a write to
 *	/proc or /sys -- and FAILS SAFE: a missing or unreadable file
 *	(not on this kernel, hidden inside a container, wrong
 *	architecture) is treated as "unknown", not "a finding", and is
 *	skipped with no log line and no error.  Linux-only: every probe
 *	targets a Linux-specific /proc or /sys knob, so this whole file
 *	is a no-op on every other platform (matches the __linux__ guard
 *	style of src/os/os_pkey.c: one big #if block with real logic,
 *	trivial stub in the #else).
 *
 *	intel_pstate (PLAN.md 19.21 asks for "an honest recommendation
 *	or skip with a documented reason"): the status file only ever
 *	reports "active" or "passive" when the intel_pstate driver is
 *	loaded at all -- if it is not loaded (disabled via the
 *	intel_pstate=disable cmdline param, non-Intel CPU, or a kernel
 *	built without it) the status file simply does not exist, and the
 *	fail-safe read skips silently, which is the right answer: a
 *	deliberately-disabled intel_pstate is usually an intentional
 *	choice (e.g. cycle-accurate MSR-level frequency pinning for
 *	microbenchmarking) and this advisor has no business second-
 *	guessing it.  "passive" IS actionable: passive mode hands control
 *	to a generic cpufreq governor and gives up the HWP-aware
 *	autonomous boost management intel_pstate's "active" mode
 *	provides, which is worse for the throughput/latency workloads
 *	this runtime targets, so this probe recommends switching back to
 *	active.  "active" is already optimal and stays silent.  There is
 *	deliberately no /proc/cmdline probe for intel_pstate=disable: by
 *	the time we would read it, the sysfs status file already told us
 *	everything actionable (see above), so a second check would only
 *	ever reach the same "skip silently, nothing to say" verdict --
 *	dead code that never changes behavior.
 *
 *	This is a one-time startup probe, not a hot path and not called
 *	from any DST sim-reachable code path (xtc_app_start runs before
 *	xtc_sim_activate), so no sim guard is needed -- consistent with
 *	__os_ncpus / __os_mem_max in os_cpu.c.
 */

#include "xtc_int.h"
#include "os_tuning.h"
#include "xtc_log.h"

#if defined(__linux__)

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

/* Test-only fixture-path overrides (see os_tuning.h).  NULL (the
 * default) means "use the real /proc or /sys path". */
static const char *__governor_path_override;
static const char *__pstate_status_path_override;
static const char *__thp_path_override;
static const char *__swappiness_path_override;
static const char *__autogroup_path_override;
static int          __uring_force_mode;   /* 0 = real probe (default) */

void __xtc_os_tuning_governor_path_override(const char *path)
{ __governor_path_override = path; }
void __xtc_os_tuning_pstate_status_path_override(const char *path)
{ __pstate_status_path_override = path; }
void __xtc_os_tuning_thp_path_override(const char *path)
{ __thp_path_override = path; }
void __xtc_os_tuning_swappiness_path_override(const char *path)
{ __swappiness_path_override = path; }
void __xtc_os_tuning_autogroup_path_override(const char *path)
{ __autogroup_path_override = path; }
void __xtc_os_tuning_uring_force(int mode)
{ __uring_force_mode = mode; }

#define GOVERNOR_PATH \
	(__governor_path_override != NULL ? __governor_path_override : \
	    "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")
#define PSTATE_STATUS_PATH \
	(__pstate_status_path_override != NULL ? \
	    __pstate_status_path_override : \
	    "/sys/devices/system/cpu/intel_pstate/status")
#define THP_PATH \
	(__thp_path_override != NULL ? __thp_path_override : \
	    "/sys/kernel/mm/transparent_hugepage/enabled")
#define SWAPPINESS_PATH \
	(__swappiness_path_override != NULL ? __swappiness_path_override : \
	    "/proc/sys/vm/swappiness")
#define AUTOGROUP_PATH \
	(__autogroup_path_override != NULL ? __autogroup_path_override : \
	    "/proc/sys/kernel/sched_autogroup_enabled")

/* Read the first line of "path" into buf (NUL-terminated, trailing
 * newline stripped).  Returns 0 on success, -1 if the file cannot be
 * opened or read -- the fail-safe "skip this probe" signal. */
static int
read_first_line(const char *path, char *buf, size_t bufsize)
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

/* Each *_needs_rec probe returns: -1 = skip (missing/unreadable/not
 * parseable -- not a finding), 0 = already tuned, 1 = recommend. */

static int
governor_needs_rec(void)
{
	char buf[64];
	if (read_first_line(GOVERNOR_PATH, buf, sizeof buf) != 0)
		return -1;
	return strcmp(buf, "performance") != 0;
}

static int
pstate_needs_rec(void)
{
	char buf[16];
	if (read_first_line(PSTATE_STATUS_PATH, buf, sizeof buf) != 0)
		return -1;   /* driver not loaded: not our call, see header */
	return strcmp(buf, "passive") == 0;
}

/* Format: "always madvise [never]" -- the bracketed word is the
 * active setting.  Recommend "madvise" when "always" or "never" is
 * active; an unexpected format (no brackets) is skipped, not flagged. */
static int
thp_needs_rec(void)
{
	char buf[128];
	char *lb, *rb;
	if (read_first_line(THP_PATH, buf, sizeof buf) != 0)
		return -1;
	lb = strchr(buf, '[');
	rb = strchr(buf, ']');
	if (lb == NULL || rb == NULL || rb <= lb)
		return -1;
	*rb = '\0';
	lb++;
	return strcmp(lb, "always") == 0 || strcmp(lb, "never") == 0;
}

/* Threshold: above 10.  10 is the commonly recommended ceiling for a
 * database/latency-sensitive server (default is 60); above that the
 * kernel starts reclaiming anonymous pages under mild memory pressure
 * well before it needs to, trading throughput for headroom this kind
 * of workload does not want. */
static int
swappiness_needs_rec(void)
{
	char buf[16];
	char *end;
	long v;
	if (read_first_line(SWAPPINESS_PATH, buf, sizeof buf) != 0)
		return -1;
	v = strtol(buf, &end, 10);
	if (end == buf)
		return -1;   /* not numeric: unexpected format, skip */
	return v > 10;
}

static int
autogroup_needs_rec(void)
{
	char buf[8];
	char *end;
	long v;
	if (read_first_line(AUTOGROUP_PATH, buf, sizeof buf) != 0)
		return -1;
	v = strtol(buf, &end, 10);
	if (end == buf)
		return -1;
	return v != 0;
}

/*
 * io_uring under seccomp: the cheapest possible real check is to
 * attempt an actual io_uring_setup(2) with a single-entry ring and
 * immediately close it (or, on failure, do nothing -- the syscall
 * never touches any fd on error).  EPERM specifically is seccomp (or
 * an LSM) denying the syscall; every other failure (ENOSYS on a
 * pre-5.1 kernel, EINVAL, EFAULT) means "not applicable here", not "a
 * finding", and is skipped.  The struct is a local ABI mirror of the
 * kernel's struct io_uring_params (stable UAPI layout since 5.1) so
 * this probe needs neither liburing nor <linux/io_uring.h> --
 * skip it entirely at compile time (return "not blocked") if the
 * running libc does not even define the syscall number.
 */
struct __os_uring_params {
	uint32_t sq_entries, cq_entries, flags, sq_thread_cpu, sq_thread_idle;
	uint32_t features, wq_fd, resv[3];
	unsigned char sq_off[40];   /* opaque struct io_sqring_offsets */
	unsigned char cq_off[40];   /* opaque struct io_cqring_offsets */
};

static int
uring_blocked(void)
{
	if (__uring_force_mode == 1) return 1;    /* test: force EPERM */
	if (__uring_force_mode == -1) return 0;   /* test: force usable */
#if defined(SYS_io_uring_setup)
	{
		struct __os_uring_params p;
		long rc;
		memset(&p, 0, sizeof p);
		p.sq_entries = 1;
		rc = syscall(SYS_io_uring_setup, 1u, &p);  /* XTC_BLOCKING_OK: one-shot host probe */
		if (rc >= 0) {
			(void)close((int)rc);
			return 0;
		}
		return errno == EPERM;
	}
#else
	return 0;   /* no syscall number on this libc/arch: not our finding */
#endif
}

void
__os_tuning_check(void)
{
	if (governor_needs_rec() > 0)
		XTC_LOG_INFO_F(
		    "[tuning] cpu governor is not 'performance' -- "
		    "set it (e.g. cpupower frequency-set -g performance) "
		    "for a latency-sensitive workload");
	if (pstate_needs_rec() > 0)
		XTC_LOG_INFO_F(
		    "[tuning] intel_pstate is in 'passive' mode -- switch "
		    "back to 'active' (drop intel_pstate=passive from the "
		    "kernel cmdline) unless you deliberately pinned "
		    "frequencies yourself; passive mode gives up "
		    "intel_pstate's HWP-aware boost management");
	if (thp_needs_rec() > 0)
		XTC_LOG_INFO_F(
		    "[tuning] transparent_hugepage is not 'madvise' -- set "
		    "it (echo madvise > "
		    "/sys/kernel/mm/transparent_hugepage/enabled) to avoid "
		    "'always'/'never' compaction stalls and latency spikes");
	if (swappiness_needs_rec() > 0)
		XTC_LOG_INFO_F(
		    "[tuning] vm.swappiness is above 10 -- lower it "
		    "(sysctl vm.swappiness=10 or less) so the kernel does "
		    "not reclaim anonymous pages under mild memory pressure");
	if (autogroup_needs_rec() > 0)
		XTC_LOG_INFO_F(
		    "[tuning] kernel.sched_autogroup_enabled is 1 -- "
		    "disable it (sysctl kernel.sched_autogroup_enabled=0) "
		    "for a latency-sensitive server workload");
	if (uring_blocked())
		XTC_LOG_INFO_F(
		    "[tuning] io_uring_setup(2) returned EPERM -- seccomp "
		    "(or an LSM) is blocking io_uring; xtc falls back to "
		    "epoll, which costs throughput/latency under load");
}

#else /* !__linux__: every probe targets a Linux-only knob. */

void
__os_tuning_check(void)
{
}

#endif
