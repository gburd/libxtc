/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_tail.h
 *	A runtime microscope: cheap, high-volume recording of what the
 *	scheduler and runtime are actually doing -- "tail -f a system" --
 *	so hard async bugs (lost/late wakeups, long polls, mailbox backups,
 *	scheduler imbalance) become obvious after the fact.  Inspired by
 *	dial9 ("a microscope for Tokio").
 *
 *	Unlike xtc_stats (aggregate counters) and xtc_trace (the causal
 *	message trace), xtc_tail records every INDIVIDUAL runtime event
 *	tied to a precise instant, so a degraded window can be diffed
 *	against a normal one.  It is OFF by default and one branch when
 *	disabled -- observability must not tax production.
 *
 *	Phase 1 (this): the SCHED source (proc spawn / exit / wake / run)
 *	over a bounded per-process ring, an in-process read callback, and a
 *	versioned binary dump to an fd.  The IO/OS sources, on-disk spill
 *	with rotation, and the offline viewer are staged follow-ons (see
 *	the roadmap); the record format and API are designed to accept them
 *	without a break.
 */

#ifndef XTC_TAIL_H
#define XTC_TAIL_H

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_proc.h"

/* Event sources, an enable mask (dial9-style: pay only for what you turn
 * on).  Phase 1 implements SCHED; the others are reserved so the mask is
 * stable across the staged build-out. */
#define XTC_TAIL_SCHED  (1u << 0)   /* proc spawn/exit/wake/run */
#define XTC_TAIL_MSG    (1u << 1)   /* reserved: send/recv/mailbox depth */
#define XTC_TAIL_IO     (1u << 2)   /* reserved: fd reg/del/completion */
#define XTC_TAIL_OS     (1u << 3)   /* reserved: per-loop CPU/RSS sampling */
#define XTC_TAIL_ALL    (XTC_TAIL_SCHED | XTC_TAIL_MSG | XTC_TAIL_IO | XTC_TAIL_OS)

/* Event kinds recorded by the SCHED source. */
enum xtc_tail_kind {
	XTC_TAIL_SPAWN    = 0,   /* a proc was spawned */
	XTC_TAIL_EXIT     = 1,   /* a proc exited (detail = reason) */
	XTC_TAIL_WAKE     = 2,   /* a parked proc was woken (armed) */
	XTC_TAIL_RUN      = 3,   /* a proc began running after a wake
	                          * (detail = wake-to-run latency, ns) */
	XTC_TAIL_PARK     = 4    /* a proc parked (blocked on recv/timer/fd) */
};

/* One recorded event.  Fixed layout; the binary dump writes it verbatim
 * behind a versioned header, so a reader across the wire/disk decodes it
 * without guessing. */
typedef struct xtc_tail_rec {
	uint64_t  ts_ns;    /* monotonic timestamp (ns) */
	uint32_t  source;   /* which XTC_TAIL_* source produced it */
	uint32_t  kind;     /* enum xtc_tail_kind */
	xtc_pid_t pid;      /* the proc the event concerns */
	uint64_t  detail;   /* kind-specific (EXIT reason / RUN latency ns) */
} xtc_tail_rec_t;

/* Visit callback for xtc_tail_read: return 0 to continue, nonzero stops. */
typedef int (*xtc_tail_fn)(const xtc_tail_rec_t *rec, void *user);

/*
 * PUBLIC: unsigned xtc_tail_enable __P((unsigned));
 * PUBLIC: void     xtc_tail_disable __P((void));
 * PUBLIC: int      xtc_tail_reset __P((void));
 * PUBLIC: int      xtc_tail_read __P((xtc_tail_fn, void *));
 * PUBLIC: int      xtc_tail_dump __P((int));
 * PUBLIC: size_t   xtc_tail_count __P((void));
 */

/* Enable the named sources (a bitwise-OR of XTC_TAIL_*).  Returns the
 * previously enabled mask.  Enabling is idempotent; call with the full
 * mask you want each time (it replaces, not ORs). */
unsigned xtc_tail_enable(unsigned source_mask);

/* Disable all recording (equivalent to xtc_tail_enable(0)). */
void     xtc_tail_disable(void);

/* Drop all buffered records.  Returns XTC_OK. */
int      xtc_tail_reset(void);

/* Visit every buffered record oldest-first (a stable snapshot). */
int      xtc_tail_read(xtc_tail_fn cb, void *user);

/* Write the buffered records to `fd` as a versioned binary trace:
 * a small header (magic, version, record count, record size) followed
 * by the records verbatim.  A separate offline tool renders it. */
int      xtc_tail_dump(int fd);

/* Number of records currently buffered. */
size_t   xtc_tail_count(void);

/* Internal: record one event from a runtime hook point (proc spawn/exit,
 * etc.).  Not part of the public API -- callers use xtc_tail_enable to
 * turn recording on and the read/dump/count functions to consume it.
 * A no-op fast path (one branch) when `source` is disabled. */
void __xtc_tail_emit(unsigned source, unsigned kind, xtc_pid_t pid,
                     uint64_t detail);

/* Internal: 1 if `source` is currently enabled.  A hook point uses this
 * to guard extra work (e.g. a clock read) so a disabled tail costs one
 * branch and has no side effects. */
int  __xtc_tail_on(unsigned source);

/* On-disk binary dump header (also used by the offline reader). */
#define XTC_TAIL_MAGIC   0x5854434Cu   /* "XTCL" */
#define XTC_TAIL_VERSION 1u
typedef struct xtc_tail_hdr {
	uint32_t magic;      /* XTC_TAIL_MAGIC */
	uint32_t version;    /* XTC_TAIL_VERSION */
	uint32_t rec_size;   /* sizeof(xtc_tail_rec_t) -- guards layout skew */
	uint32_t count;      /* number of records that follow */
} xtc_tail_hdr_t;

#endif /* XTC_TAIL_H */
