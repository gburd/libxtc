/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_inspect.h
 *	Live process introspection -- the programmatic form of the
 *	debugger's xtc-procs / xtc-loops, callable from a running program
 *	(an admin command, a metrics scraper, a TUI).  Modeled on
 *	Erlang's erlang:process_info/2 + the data observer renders.
 *
 *	Each call takes a best-effort snapshot under the per-loop slot
 *	locks: the proc set is consistent for the duration of the call,
 *	the per-proc counters are read under the mailbox lock, and the
 *	scheduler-owned run state is sampled (it may change the instant
 *	after).  Callbacks run AFTER all internal locks are released, so
 *	they may freely call back into the proc/loop APIs.
 *
 *	See docs/guide/debugging.md.
 */

#ifndef XTC_INSPECT_H
#define XTC_INSPECT_H

#include "xtc_export.h"

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_proc.h"

/* Run state of a proc (mirrors the scheduler task state). */
enum xtc_proc_run_state {
	XTC_PROC_SCHEDULED = 0,
	XTC_PROC_RUNNING   = 1,
	XTC_PROC_PARKED    = 2,
	XTC_PROC_DONE      = 3
};

/* Why a PARKED proc is parked (XTC_PARK_NONE otherwise). */
enum xtc_proc_park {
	XTC_PARK_NONE    = 0,
	XTC_PARK_FD      = 1,
	XTC_PARK_TIMER   = 2,
	XTC_PARK_MAILBOX = 3
};

/*
 * One proc's snapshot.  mbox_len is the live mailbox depth -- the
 * single most useful health signal in a message-passing system; a
 * large or growing value is the most common pathology.
 */
typedef struct xtc_proc_info {
	xtc_pid_t pid;
	int       run_state;        /* enum xtc_proc_run_state */
	int       park_reason;      /* enum xtc_proc_park (valid if PARKED) */
	int       alive;
	int       kill_pending;
	size_t    mbox_len;         /* current depth */
	size_t    mbox_peak;        /* high-water mark */
	size_t    mbox_cap;
	size_t    mbox_saved;       /* selective-receive save queue */
	uint64_t  mbox_recv_total;  /* messages ever accepted */
	uint64_t  mbox_drop_total;  /* messages ever rejected (full/dead) */
} xtc_proc_info_t;

/*
 * Link/monitor topology is intentionally NOT in the live snapshot: a
 * proc mutates its own link/monitor lists without a lock, so walking
 * them from another thread would race.  Use the debugger (xtc-proc,
 * which runs against a stopped program) to inspect link/monitor
 * topology; the live API reports only fields that are safe to read
 * concurrently (mailbox counters under the mailbox lock, plus the
 * sampled run state).
 */

/* One loop's scheduler snapshot. */
typedef struct xtc_loop_info {
	int      loop_id;           /* 0 standalone, exec slot + 1 otherwise */
	int      n_procs;           /* live procs homed on this loop */
	int      n_alive;           /* live tasks (procs + plain tasks) */
	uint64_t tasks_run;
	uint64_t steals;
} xtc_loop_info_t;

/* Enumeration callbacks: return 0 to continue, nonzero to stop early. */
typedef int (*xtc_inspect_proc_fn)(const xtc_proc_info_t *info, void *user);
typedef int (*xtc_inspect_loop_fn)(const xtc_loop_info_t *info, void *user);

/*
 * PUBLIC: int xtc_inspect_procs __P((xtc_inspect_proc_fn, void *));
 * PUBLIC: int xtc_inspect_loops __P((xtc_inspect_loop_fn, void *));
 * PUBLIC: int xtc_proc_info __P((xtc_pid_t, xtc_proc_info_t *));
 */

/* Invoke `cb` once per live proc (across all loops).  Returns the
 * number of procs visited, or a negative XTC_E_* on error. */
XTC_API int xtc_inspect_procs(xtc_inspect_proc_fn cb, void *user);

/* Invoke `cb` once per registered loop.  Returns the loop count or a
 * negative XTC_E_*. */
XTC_API int xtc_inspect_loops(xtc_inspect_loop_fn cb, void *user);

/* Snapshot one proc by pid into *out.  XTC_OK on success,
 * XTC_E_NOTFOUND if no live proc has that pid, XTC_E_INVAL on a NULL
 * out-pointer. */
XTC_API int xtc_proc_info(xtc_pid_t pid, xtc_proc_info_t *out);

#endif /* XTC_INSPECT_H */
