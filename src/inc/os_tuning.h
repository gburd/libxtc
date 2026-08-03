/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/os_tuning.h
 *	Power/kernel-tuning advisor surface (PLAN.md 19.21).
 */

#ifndef XTC_OS_TUNING_H
#define XTC_OS_TUNING_H

#include "xtc_export.h"

/*
 * Run a battery of cheap, read-only host-tuning probes and log one
 * NOTICE-level (XTC_LOG_INFO, the closest level this logger has) line
 * per probe that is NOT already in its recommended state.  Silent on
 * a well-tuned host.  Meaningful only on Linux; a no-op everywhere
 * else.  Never writes to /proc or /sys.  A probe whose source file is
 * missing or unreadable (absent on this kernel, or hidden inside a
 * container) is skipped silently -- that is "unknown", not "a
 * finding".  Safe to call more than once (each call re-probes); meant
 * to be called once, at startup.
 *
 * INTERNAL: this is the __os_* implementation.  The public entry is
 * xtc_tuning_check() (xtc_stats.h), a thin wrapper -- so consumers on
 * the xtc_exec / xtc_loop path can reach the advisor.  Not exported
 * (no XTC_API): the __os_* symbol stays library-local per the ABI
 * gate; xtc_app_start and stats.c's xtc_tuning_check call it directly.
 */
void __os_tuning_check(void);

#if defined(__linux__)
/*
 * Internal / test hooks: point each probe at a fixture file instead
 * of the real /sys or /proc path, so the parsing + threshold logic is
 * unit-testable without root or a real kernel-tuning knob.  NULL (the
 * default) restores the real path.  Not part of the stable API.
 */
void __xtc_os_tuning_governor_path_override(const char *path);
void __xtc_os_tuning_pstate_status_path_override(const char *path);
void __xtc_os_tuning_cmdline_path_override(const char *path);
void __xtc_os_tuning_thp_path_override(const char *path);
void __xtc_os_tuning_swappiness_path_override(const char *path);
void __xtc_os_tuning_autogroup_path_override(const char *path);

/* Test-only: force the io_uring-under-seccomp probe's verdict instead
 * of issuing the real syscall, so the "blocked" log line is
 * exercisable without actually running under a blocking seccomp
 * filter.  0 = do the real probe (default), 1 = pretend EPERM,
 * -1 = pretend "usable". */
void __xtc_os_tuning_uring_force(int mode);
#endif

#endif /* XTC_OS_TUNING_H */
