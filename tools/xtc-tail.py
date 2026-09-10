#!/usr/bin/env python3
# xtc-tail.py -- offline viewer for xtc_tail runtime-microscope traces.
#
# Copyright (c) 2026, The XTC Project -- ISC License.
# SPDX-License-Identifier: ISC
#
# Reads the compact, portable binary trace an xtc program writes with
# xtc_tail_dump(fd) (see src/inc/xtc_tail.h) and lets an operator
# read / filter / step through / summarize what the runtime did, after
# the fact -- the "tail -f a system" / BEAM-observer-recon experience for
# a captured trace.
#
# The trace is self-describing and portable (little-endian header + the
# LEB128 delta-encoded event stream), so this reader runs anywhere,
# regardless of the host that produced the trace.
#
# Usage:
#     xtc-tail.py TRACE                 # human-readable event dump
#     xtc-tail.py TRACE --summary       # per-pid / per-kind rollup
#     xtc-tail.py TRACE --pid L.I.G     # only events for one pid
#     xtc-tail.py TRACE --kind RUN,EXIT # only these event kinds
#     xtc-tail.py TRACE --source SCHED  # only this source
#     xtc-tail.py TRACE --wake-latency  # RUN events sorted by park->run ns
#     xtc-tail.py TRACE --around T[:W]  # events within +/-W ns of time T
#     xtc-tail.py TRACE --step          # interactive: one event per Enter
#
# The gdb/lldb extensions (xtc-tail-dump) can write a live program's ring
# to a file this reads.

import sys
import struct
import argparse

MAGIC = 0x5854434C          # "XTCL"
FLAG_LE = 1

SOURCES = {1: "SCHED", 2: "MSG", 4: "IO", 8: "OS"}
KINDS = {
    0: "SPAWN", 1: "EXIT", 2: "WAKE", 3: "RUN", 4: "PARK",
    5: "SEND", 6: "RECV", 7: "MBOX_HWM", 8: "LOOP_POLL",
    9: "PARK_TASK", 10: "REAP", 11: "SUBMIT", 12: "SUBMIT_FAIL",
}
# detail-field meaning per kind, for the human column
DETAIL = {
    "EXIT": "reason", "RUN": "park->run ns", "SEND": "bytes",
    "RECV": "bytes", "MBOX_HWM": "peak depth",
    "LOOP_POLL": "events dispatched",
    # PARK's detail is source-dependent: the fd for an xtc_proc_wait_fd
    # readiness park, the aio opcode for a native async-file park, 0 for a
    # mailbox recv park.  Label it neutrally rather than guess.
    "PARK": "fd/op",
    # Both of these carry a task POINTER and are rendered as hex (see
    # _PTR_DETAIL): PARK_TASK is the key a WAKE joins to.
    "PARK_TASK": "task",
    "WAKE": "task",
    # REAP: a CQE was consumed by the reaping loop.  detail is the tag it
    # resolved to, or 0 when it was consumed WITHOUT being handed to dispatch
    # (the wakeup pipe, a poll_remove cancel, or a completion dropped because
    # its registration was already gone).  0 is common and not by itself a
    # bug -- the signal is a REAP whose task never appears in a later WAKE.
    "REAP": "task",
    # SUBMIT carries the task the request is on behalf of (0 for the wakeup
    # pipe).  SUBMIT_FAIL carries the negated errno and is a fault by its
    # mere presence -- a fiber is parked on a request the kernel refused.
    "SUBMIT": "task",
    "SUBMIT_FAIL": "errno/short",
}


class Event:
    __slots__ = ("ts", "source", "kind", "loop", "local", "gen", "detail")

    def __init__(self, ts, source, kind, loop, local, gen, detail):
        self.ts, self.source, self.kind = ts, source, kind
        self.loop, self.local, self.gen, self.detail = loop, local, gen, detail

    @property
    def pid(self):
        return "%d.%d.%d" % (self.loop, self.local, self.gen)

    @property
    def source_name(self):
        return SOURCES.get(self.source, "?%d" % self.source)

    @property
    def kind_name(self):
        return KINDS.get(self.kind, "?%d" % self.kind)


def _read_leb128(buf, off):
    """Decode one unsigned LEB128 varint from buf at off; return (val, new_off)."""
    val = 0
    shift = 0
    while True:
        b = buf[off]
        off += 1
        val |= (b & 0x7F) << shift
        if (b & 0x80) == 0:
            break
        shift += 7
    return val, off


def parse(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 24:
        raise ValueError("trace too short for a header")
    magic, version, flags, count = struct.unpack_from("<IIII", data, 0)
    base_ts = struct.unpack_from("<Q", data, 16)[0]
    if magic != MAGIC:
        raise ValueError("bad magic 0x%08x (not an xtc_tail trace)" % magic)
    if not (flags & FLAG_LE):
        raise ValueError("trace is not little-endian canonical (flags=0x%x)"
                         % flags)
    off = 24
    prev = base_ts
    events = []
    for _ in range(count):
        kind = data[off]; off += 1
        source = data[off]; off += 1
        dts, off = _read_leb128(data, off)
        loop, off = _read_leb128(data, off)
        local, off = _read_leb128(data, off)
        gen, off = _read_leb128(data, off)
        detail, off = _read_leb128(data, off)
        ts = prev + dts
        prev = ts
        events.append(Event(ts, source, kind, loop, local, gen, detail))
    return version, events


def matches(ev, args):
    if args.pid and ev.pid != args.pid:
        return False
    if args.source and ev.source_name != args.source:
        return False
    if args.kinds and ev.kind_name not in args.kinds:
        return False
    if args.around is not None:
        lo, hi = args.around
        if not (lo <= ev.ts <= hi):
            return False
    return True


# Kinds whose detail is a POINTER: render hex so a WAKE and the
# PARK_TASK it joins to are visually comparable, and so the value lines
# up with xtc-procs' `task` column.
_PTR_DETAIL = ("WAKE", "PARK_TASK", "REAP", "SUBMIT")


SHORT_SUBMIT_BASE = 4096


def _submit_fail_detail(d):
    """Render a SUBMIT_FAIL detail as the two shapes it encodes."""
    if d >= SHORT_SUBMIT_BASE:
        return "SHORT submit, %d SQE(s) not taken" % (d - SHORT_SUBMIT_BASE)
    return "errno=%d" % d


def fmt(ev, base):
    d = DETAIL.get(ev.kind_name)
    if ev.kind_name == "SUBMIT_FAIL":
        return "%12d ns  %-5s %-9s pid=%-10s  %s" % (
            ev.ts - base, ev.source_name, ev.kind_name, ev.pid,
            _submit_fail_detail(ev.detail))
    if ev.kind_name in _PTR_DETAIL:
        dstr = "  %s=0x%x" % (d or "detail", ev.detail)
    else:
        dstr = ("  %s=%d" % (d, ev.detail)) if d else (
            "  detail=%d" % ev.detail if ev.detail else "")
    return "%12d ns  %-5s %-9s pid=%-10s%s" % (
        ev.ts - base, ev.source_name, ev.kind_name, ev.pid, dstr)


def cmd_dump(events, args):
    base = events[0].ts if events else 0
    shown = 0
    for ev in events:
        if not matches(ev, args):
            continue
        print(fmt(ev, base))
        shown += 1
    print("-- %d of %d events --" % (shown, len(events)))


def cmd_step(events, args):
    base = events[0].ts if events else 0
    sel = [e for e in events if matches(e, args)]
    print("-- %d events; Enter to step, q to quit --" % len(sel))
    for i, ev in enumerate(sel):
        line = "[%d/%d] %s" % (i + 1, len(sel), fmt(ev, base))
        try:
            r = input(line)
        except EOFError:
            break
        if r.strip().lower() == "q":
            break


def cmd_summary(events, args):
    by_kind = {}
    by_pid = {}
    for ev in events:
        if not matches(ev, args):
            continue
        by_kind[ev.kind_name] = by_kind.get(ev.kind_name, 0) + 1
        by_pid.setdefault(ev.pid, {})
        by_pid[ev.pid][ev.kind_name] = by_pid[ev.pid].get(ev.kind_name, 0) + 1
    span = (events[-1].ts - events[0].ts) if len(events) > 1 else 0
    print("=== %d events over %d ns ===" % (len(events), span))
    print("by kind:")
    for k in sorted(by_kind, key=lambda x: -by_kind[x]):
        print("  %-9s %d" % (k, by_kind[k]))
    print("by pid:")
    for pid in sorted(by_pid):
        kinds = ", ".join("%s=%d" % (k, v)
                          for k, v in sorted(by_pid[pid].items()))
        print("  %-10s %s" % (pid, kinds))


def _in_window(times, lo, hi):
    """Is there a timestamp in the half-open window (lo, hi)?

    SUBMIT runs the OTHER WAY from the rest of the chain, and getting that
    backwards was the second time-direction bug here.  WAKE, REAP and RUN all
    happen AFTER a fiber parks, so they match with `ts > pk.ts`.  A SUBMIT
    happens BEFORE it -- you queue the SQE, then park waiting for its
    completion -- so its timestamp is always LESS than the park's.  The first
    version reused `ts >= pk.ts` for SUBMIT too, which can essentially never
    match, so every genuinely-submitted request fell through to "never
    submitted": a blanket 33-of-33 a consumer correctly refused to publish.

    A LOWER bound is needed too, and "most recent SUBMIT before the park" is
    not enough.  A task pointer is REUSED across the fiber's cycles, so an
    unbounded look backwards finds a SUBMIT from an earlier, healthy cycle
    and calls a never-submitted park submitted.  `lo` fences the search at
    the end of the fiber's previous cycle -- its last RUN before this park --
    so only a submission that could belong to THIS park counts.
    """
    for ts in times:
        if lo < ts < hi:
            return True
    return False


def cmd_strands(events, args):
    """Classify every parked task by how far its wake got.

    This is the branch question, automated.  A fiber that parks and never runs
    again failed at exactly one of three steps, and the fix differs per step:

      no REAP           the kernel never posted the completion (or we never
                        looked) -- look at submission / the ring
      REAP, no WAKE     the reaper consumed it and never handed it to dispatch
                        -- look at the reap->dispatch path
      WAKE, no RUN      dispatch ran; the loss is downstream, in the waker CAS
                        or the enqueue

    Doing this by hand across thousands of events is error-prone: a consumer
    once got a decisive-looking but structurally impossible answer that way,
    by keying the join on a pid the event does not carry.  PARK_TASK shares
    the task pointer with both REAP and WAKE, so it is the join that holds.
    """
    # A task pointer is stable for the fiber's whole LIFE, so every one of
    # these lookups must be time-scoped to "after the final park".  The first
    # version scoped only the `resumed` test and used timeless sets for WAKE
    # and REAP: any fiber with a healthy history was therefore guaranteed to
    # be in the WAKE set (from its own earlier, successful cycles) and got
    # filed under "WAKE, no RUN" before the REAP test was ever reached.
    #
    # That is self-concealing -- the MORE normal work a fiber did before
    # stranding, the more confidently it was misclassified -- and it inverted
    # the answer on real traces: 28 of 30 strands reported as "WAKE, no RUN"
    # when all 30 were "no REAP".  A consumer caught it by hand-tracing one
    # pid (699 events, 207 WAKEs, none after the strand).  The planted-bug
    # gate missed it because its fibers have short histories.
    parked = {}
    for e in events:
        if e.kind_name == "PARK_TASK":
            parked[e.detail] = e
    reaped_at, waked_at, ran_at, subm_at = {}, {}, {}, {}
    n_submit_fail = 0
    for e in events:
        if e.kind_name == "REAP" and e.detail:
            reaped_at.setdefault(e.detail, []).append(e.ts)
        elif e.kind_name == "WAKE":
            waked_at.setdefault(e.detail, []).append(e.ts)
        elif e.kind_name == "RUN":
            ran_at.setdefault(e.pid, []).append(e.ts)
        elif e.kind_name == "SUBMIT" and e.detail:
            subm_at.setdefault(e.detail, []).append(e.ts)
        elif e.kind_name == "SUBMIT_FAIL":
            n_submit_fail += 1

    def _prev_cycle_end(pk):
        """Timestamp of this pid's last RUN before pk -- the cycle fence.

        Without it, a task pointer reused across cycles lets an old SUBMIT
        vouch for a park that never submitted anything.  0 when the fiber has
        no prior RUN (its first park), which correctly leaves the whole
        preceding trace in scope.
        """
        best = 0
        for ts in ran_at.get(pk.pid, ()):
            if ts < pk.ts and ts > best:
                best = ts
        return best

    order = ["never submitted", "no REAP", "REAP, no WAKE", "WAKE, no RUN",
             "resumed"]
    buckets = dict((k, []) for k in order)
    for task, pk in sorted(parked.items(), key=lambda kv: kv[1].ts):
        if any(ts > pk.ts for ts in ran_at.get(pk.pid, ())):
            buckets["resumed"].append((pk, task))
        elif any(ts > pk.ts for ts in waked_at.get(task, ())):
            buckets["WAKE, no RUN"].append((pk, task))
        elif any(ts > pk.ts for ts in reaped_at.get(task, ())):
            buckets["REAP, no WAKE"].append((pk, task))
        elif _in_window(subm_at.get(task, ()), _prev_cycle_end(pk), pk.ts):
            buckets["no REAP"].append((pk, task))
        else:
            # Nothing was submitted for THIS park: we never asked the kernel
            # for the completion this fiber is waiting on.  Distinguished
            # from "no REAP" because no amount of polling can fix it.
            buckets["never submitted"].append((pk, task))

    print("=== %d parked task(s), classified by how far the wake got ==="
          % len(parked))
    for name in order:
        print("  %-16s %d" % (name, len(buckets[name])))
    print("")
    why = {
        "never submitted": "no SQE was queued for this park -- we never asked",
        "no REAP": "submitted, but the completion never came back",
        "REAP, no WAKE": "consumed by the reaper, never handed to dispatch",
        "WAKE, no RUN": "dispatch ran; the loss is after it",
    }
    for name in order[:4]:
        rows = buckets[name]
        if not rows:
            continue
        print("--- %s ---" % name)
        print("    %s" % why[name])
        for pk, task in rows[:args.top]:
            print("    pid=%-10s task=0x%x  parked at %d ns"
                  % (pk.pid, task, pk.ts - events[0].ts))
        if len(rows) > args.top:
            print("    ... %d more" % (len(rows) - args.top))
        print("")
    if n_submit_fail:
        print("*** %d XTC_TAIL_SUBMIT_FAIL event(s): a submission was REFUSED"
              % n_submit_fail)
        print("    by the kernel.  Any fiber parked on one of those requests")
        print("    can never be woken; this is a fault on its own, with no")
        print("    join required.")
        print("")
    if not any(buckets[n] for n in order[:4]):
        print("  no stranded tasks: every park was followed by a RUN")
    print("NOTE: every bucket here is an ABSENCE claim.  Check dropped first")
    print("      (xtc-tail-dropped, or the dump header) -- in a wrapped ring")
    print("      an absence may only mean the event was evicted.")


def cmd_wake_latency(events, args):
    runs = [e for e in events
            if e.kind_name == "RUN" and matches(e, args)]
    runs.sort(key=lambda e: -e.detail)
    print("=== %d RUN events by wake->run latency (worst first) ===" % len(runs))
    if not runs:
        print("  (none -- enable XTC_TAIL_SCHED and record park/run events)")
        return
    for ev in runs[:args.top]:
        print("  %10d ns  pid=%-10s  at %d ns"
              % (ev.detail, ev.pid, ev.ts - events[0].ts))
    worst = runs[0].detail
    med = runs[len(runs) // 2].detail
    print("  worst=%d ns  median=%d ns  (a park->run far above median is a"
          " lost/late wakeup or scheduler stall)" % (worst, med))


def main():
    ap = argparse.ArgumentParser(description="offline xtc_tail trace viewer")
    ap.add_argument("trace")
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--step", action="store_true")
    ap.add_argument("--wake-latency", action="store_true")
    ap.add_argument("--strands", action="store_true",
                    help="classify parked tasks: no REAP / REAP,no WAKE / "
                         "WAKE,no RUN -- the branch question, automated")
    ap.add_argument("--pid")
    ap.add_argument("--source")
    ap.add_argument("--kind", help="comma-separated: SPAWN,EXIT,RUN,...")
    ap.add_argument("--around", help="T or T:W -- events within +/-W ns of T")
    ap.add_argument("--top", type=int, default=20)
    args = ap.parse_args()

    args.kinds = set(k.strip().upper()
                     for k in args.kind.split(",")) if args.kind else None
    if args.around:
        parts = args.around.split(":")
        t = int(parts[0])
        w = int(parts[1]) if len(parts) > 1 else 1000
        args.around = (t - w, t + w)
    else:
        args.around = None

    try:
        version, events = parse(args.trace)
    except (OSError, ValueError) as e:
        sys.stderr.write("xtc-tail: %s\n" % e)
        return 1

    if args.summary:
        cmd_summary(events, args)
    elif args.strands:
        cmd_strands(events, args)
    elif args.wake_latency:
        cmd_wake_latency(events, args)
    elif args.step:
        cmd_step(events, args)
    else:
        cmd_dump(events, args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
