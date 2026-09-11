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

#include "xtc_export.h"

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
	XTC_TAIL_WAKE     = 2,   /* an I/O completion was DISPATCHED to a task
	                          * (the waker side).  pid.loop_id is the
	                          * DISPATCHING loop; local_id/gen are 0
	                          * because dispatch has a task, not a proc.
	                          * detail = the xtc_task_t * as an integer,
	                          * which is the join key: match it against
	                          * the `task` column of xtc-procs, or against
	                          * the XTC_TAIL_PARK_TASK event the parking
	                          * fiber emits just before its PARK.
	                          *
	                          * NOTE: XTC_TAIL_PARK's detail is the aio OP,
	                          * NOT a task pointer.  An earlier version of
	                          * this comment said otherwise and sent a
	                          * consumer chasing a join that could not be
	                          * performed; PARK_TASK exists because of it.
	                          *
	                          * This is the event that separates "the wake
	                          * was never generated" from "the wake was
	                          * generated and the task still never ran":
	                          * a WAKE for task T with no following RUN
	                          * for T's pid means dispatch DID run and the
	                          * loss is after it. */
	XTC_TAIL_RUN      = 3,   /* a proc began running after a wake
	                          * (detail = wake-to-run latency, ns) */
	XTC_TAIL_PARK     = 4,   /* a proc parked (blocked on recv/timer/fd) */
	/* MSG source: */
	XTC_TAIL_SEND     = 5,   /* pid sent a message (detail = payload bytes) */
	XTC_TAIL_RECV     = 6,   /* pid received a message (detail = bytes) */
	XTC_TAIL_MBOX_HWM = 7,   /* pid's mailbox depth reached a new high-water
	                          * (detail = the new peak depth) */
	/* Loop liveness (SCHED source).  pid.loop_id identifies the loop;
	 * local_id/gen are 0 because a LOOP is not a proc.  detail = the
	 * number of events the poll dispatched.
	 *
	 * This exists to answer one question a park/run timeline cannot:
	 * when a fiber's park has no matching RUN, did its loop KEEP WORKING
	 * (so the loop is alive and that fiber specifically was skipped) or
	 * did the loop stop entirely?  Those have different causes and
	 * different fixes, and without a per-loop event the only way to tell
	 * them apart is to infer from the absence of other pids' events --
	 * which cannot distinguish "loop dead" from "we stopped recording",
	 * and a consumer hit exactly that ambiguity.
	 *
	 * Emitted after each completed xtc_io_poll on the loop's own ring, so
	 * a loop that is still polling produces a steady stream even when it
	 * dispatches nothing (detail = 0). */
	XTC_TAIL_LOOP_POLL = 8,
	/*
	 * The identity of the task a proc is parked as.  pid is the PARKING
	 * proc; detail is its xtc_task_t * as an integer.
	 *
	 * This exists purely to make XTC_TAIL_WAKE joinable.  WAKE is emitted
	 * from completion dispatch, which holds a task and no pid (there is no
	 * task-to-proc back pointer), so it carries the task pointer.  PARK
	 * carries the pid and, for an aio park, the OP in detail -- which a
	 * consumer relies on to tell an fdatasync park from a read park.
	 * Overwriting that op with the task pointer would trade one key for
	 * the other; emitting this alongside gives both.
	 *
	 * Emitted immediately before the PARK it describes, from the same
	 * fiber, so the pairing is unambiguous:
	 *
	 *     PARK_TASK  pid=25.1.1  detail=<task*>
	 *     PARK       pid=25.1.1  detail=3        (XTC_AIO_FDATASYNC)
	 *     ...
	 *     WAKE       pid=<loop>  detail=<task*>  <- joins on detail
	 *     RUN        pid=25.1.1  detail=<ns>
	 *
	 * A PARK_TASK whose task pointer never appears in a later WAKE means
	 * the completion never reached dispatch.  One that does appear, with
	 * no following RUN for that pid, means dispatch ran and the loss is
	 * downstream of it.  Those are different bugs.
	 */
	XTC_TAIL_PARK_TASK = 9,
	/*
	 * A CQE was REAPED from a ring and its life ended here.  pid.loop_id
	 * is the REAPING loop; local_id/gen are 0 (the reaper holds a ring,
	 * not a proc).  detail is the tag the CQE resolved to -- the same
	 * xtc_task_t * that PARK_TASK and WAKE carry -- or 0 when it resolved
	 * to no tag at all.
	 *
	 * This closes the LAST gap in the chain.  PARK_TASK/WAKE/RUN can prove
	 * a completion never reached dispatch, but not WHY: the CQE might never
	 * have been posted, or been posted and consumed by the reaper without
	 * ever being handed on.  Those are different subsystems.  A REAP event
	 * for task T with no WAKE for T means the reaper consumed T's
	 * completion and dropped it; no REAP at all means the kernel never
	 * posted it (or we never looked).
	 *
	 * Emitted at every point where a CQE is consumed, INCLUDING the paths
	 * that deliberately discard one, because a deliberate discard is
	 * indistinguishable from a bug in a timeline that cannot see it:
	 *
	 *   detail = <task*>  the completion was stored for dispatch
	 *   detail = 0        consumed and NOT handed on.  Expected for the
	 *                     wakeup-pipe CQE and for a poll_remove cancel
	 *                     (user_data NULL by design), and the signature of
	 *                     a completion dropped because its registration was
	 *                     already torn down.
	 *
	 * Rides XTC_TAIL_SCHED like the other kinds.  It is the highest-volume
	 * event here -- one per CQE -- so on a busy ring it WILL dominate the
	 * buffer; check xtc_tail_dropped() before believing any absence, and
	 * prefer a short capture window.  (The LOOP_POLL lesson: an event that
	 * crowds out the data it explains is worse than none.)
	 */
	XTC_TAIL_REAP = 10,
	/*
	 * An SQE was handed to the kernel.  pid.loop_id is the SUBMITTING loop;
	 * local_id/gen are 0.  detail is the task pointer the submission is on
	 * behalf of -- the same key PARK_TASK, REAP and WAKE carry -- or 0 when
	 * the submission is not tied to a parked task.
	 *
	 * This separates the two halves of "no REAP", which REAP alone cannot:
	 * a completion that never came back may never have been ASKED FOR.
	 *
	 *   SUBMIT then REAP        normal.
	 *   SUBMIT, never REAPed    the kernel took the request and no
	 *                           completion came back -- look at the ring,
	 *                           the wait, or the request itself.
	 *   no SUBMIT at all        we never asked.  The fiber parked for a
	 *                           completion that was never queued, which no
	 *                           amount of polling can deliver.
	 */
	XTC_TAIL_SUBMIT = 11,
	/*
	 * A submission did NOT reach the kernel.  pid.loop_id is the submitting
	 * loop; detail is the negated errno, or 0 if there was none.
	 *
	 * This is the one kind here whose mere PRESENCE is a fault -- every
	 * other needs a join to mean anything.  If one appears, a fiber is
	 * parked on a request the kernel never accepted, and no reap-side or
	 * dispatch-side investigation can explain it.
	 *
	 * Reachable today: io_uring_submit returns the number of SQEs consumed
	 * or a negative errno, and every submit site in the uring backend
	 * discarded that value, so a partial or failed submit left a fiber
	 * parked forever with nothing in flight, silently.
	 */
	XTC_TAIL_SUBMIT_FAIL = 12,
	/*
	 * A poll drained its FULL per-call budget and therefore may have left
	 * completions behind.  pid.loop_id is the polling loop; detail is the
	 * budget it filled (the caller's `max`).
	 *
	 * This closes a blind spot in XTC_TAIL_LOOP_POLL, which is emitted only
	 * for an IDLE poll (one that dispatched nothing).  That restriction is
	 * deliberate -- an every-poll event crowded the ring and evicted the
	 * data it existed to explain -- but it means a poll that reaped
	 * some-but-not-all events is INVISIBLE, and "I polled, took what I
	 * could, and left the rest" is exactly the state to look for when a
	 * ring has a pinned backlog while its loop is demonstrably alive.
	 *
	 * Low volume by construction: it fires only when a single poll hits its
	 * ceiling, not on every poll.  A steady stream of these for one loop_id
	 * means that ring is receiving completions faster than one poll can
	 * take them.
	 */
	XTC_TAIL_POLL_FULL = 13
};

/*
 * XTC_TAIL_SUBMIT_FAIL detail encoding.  Below this value the detail is a
 * plain errno (the submit was refused); at or above it, subtract the base to
 * get the number of SQEs left UNSUBMITTED by a short submit, which has no
 * errno of its own.  Two failure shapes, one field, no ambiguity: a short
 * submit returns a POSITIVE count, so an errno-only report would call it
 * success while an SQE never reached the kernel.
 */
#define XTC_TAIL_SHORT_SUBMIT_BASE 4096

/*
 * loop_id reported for a ring that has NO exec-relative loop id -- a loop
 * created by a bare xtc_loop_init rather than as part of an executor.
 *
 * This exists because the obvious fallback is a lie: reporting 0 makes an
 * unlabelled ring indistinguishable from the executor's REAL loop 0, and a
 * consumer read a stack of "loop 0" SUBMIT/REAP events as evidence that
 * loop 0 owned a stranded completion.  That inference may well be right, but
 * the number they read could not distinguish the two cases, so it could not
 * support it.  0xFFFF cannot be a real exec loop id (an exec with 65535
 * loops is not a thing) and is visibly not an index.
 */
#define XTC_TAIL_LOOP_NONE 0xFFFFu

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
 * PUBLIC: uint64_t xtc_tail_dropped __P((void));
 */

/* Enable the named sources (a bitwise-OR of XTC_TAIL_*).  Returns the
 * previously enabled mask.  Enabling is idempotent; call with the full
 * mask you want each time (it replaces, not ORs). */
XTC_API unsigned xtc_tail_enable(unsigned source_mask);

/* Disable all recording (equivalent to xtc_tail_enable(0)). */
XTC_API void     xtc_tail_disable(void);

/* Drop all buffered records.  Returns XTC_OK. */
XTC_API int      xtc_tail_reset(void);

/* Visit every buffered record oldest-first (a stable snapshot). */
XTC_API int      xtc_tail_read(xtc_tail_fn cb, void *user);

/* Write the buffered records to `fd` as a versioned binary trace:
 * a small header (magic, version, record count, record size) followed
 * by the records verbatim.  A separate offline tool renders it. */
XTC_API int      xtc_tail_dump(int fd);

/* Number of records currently buffered. */
XTC_API size_t   xtc_tail_count(void);

/*
 * How many records have been OVERWRITTEN (evicted) because the ring
 * wrapped -- total emitted minus what is still buffered.
 *
 * Check this before drawing any conclusion from the ABSENCE of an event.
 * With a non-zero dropped count, "pid X has no events" and "loop L never
 * polled" are unfalsifiable: the events may simply have been evicted.  A
 * consumer reported two verdicts that were exactly this artifact -- 23 of
 * 33 loops appeared to have stopped polling when their events had merely
 * been overwritten -- so this accessor exists to make a real zero
 * distinguishable from an evicted one.
 *
 * Conclusions drawn from events that are PRESENT stay valid regardless.
 */
XTC_API uint64_t xtc_tail_dropped(void);

/* The internal hook-point primitives __xtc_tail_emit / __xtc_tail_on
 * are library-internal (the __ prefix) and live in "tail_int.h", not in
 * this installed public header.  Consumers use the public xtc_tail_*
 * API (enable + read/dump/count) below. */

/* On-disk binary dump header (also used by the offline reader).
 *
 * Format v2 is COMPACT and PORTABLE (dial9-style): all header fields are
 * written as explicit little-endian bytes (no struct memcpy, so it is
 * byte-identical across endianness and padding), and each event is
 * varint/delta encoded rather than a fixed 32-byte record:
 *
 *   header:  magic[4]="XTCL"  version(LE u32)=2  flags(LE u32)
 *            count(LE u32)  base_ts_ns(LE u64)
 *     flags bit0 = 1 -> little-endian canonical stream (always set today)
 *   per event (oldest first):
 *     kind    : 1 byte
 *     source  : 1 byte
 *     ts_delta: LEB128 varint, ns since the previous event (base for the
 *               first) -- monotonic timestamps make this 1-2 bytes
 *     loop_id : LEB128 varint (pid.loop_id)
 *     local_id: LEB128 varint (pid.local_id)
 *     gen     : LEB128 varint (pid.gen)
 *     detail  : LEB128 varint (EXIT reason / RUN latency ns)
 *
 * Typical ~6-12 bytes/event vs 32 for the raw struct, and portable.
 * xtc_tail_read (in-process) still hands back the fixed xtc_tail_rec_t. */
#define XTC_TAIL_MAGIC   0x5854434Cu   /* "XTCL" */
#define XTC_TAIL_VERSION 2u
#define XTC_TAIL_FLAG_LE 1u            /* little-endian canonical stream */
typedef struct xtc_tail_hdr {
	uint32_t magic;      /* XTC_TAIL_MAGIC */
	uint32_t version;    /* XTC_TAIL_VERSION */
	uint32_t flags;      /* XTC_TAIL_FLAG_* (endianness marker) */
	uint32_t count;      /* number of events that follow */
	uint64_t base_ts_ns; /* timestamp of the first event (deltas from here) */
} xtc_tail_hdr_t;

#endif /* XTC_TAIL_H */
