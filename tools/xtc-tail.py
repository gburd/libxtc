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

SOURCES = {1: "SCHED", 2: "MSG", 4: "IO", 8: "OS", 16: "SIM"}
KINDS = {
    0: "SPAWN", 1: "EXIT", 2: "WAKE", 3: "RUN", 4: "PARK",
    5: "SEND", 6: "RECV", 7: "MBOX_HWM",
    8: "BUGGIFY", 9: "PARTITION", 10: "MACHINE_DEATH",
}
# detail-field meaning per kind, for the human column
DETAIL = {
    "EXIT": "reason", "RUN": "wake->run ns", "SEND": "bytes",
    "RECV": "bytes", "MBOX_HWM": "peak depth",
    "BUGGIFY": "site name hash (FNV-1a)",
    "PARTITION": "group B << 1 | healed", "MACHINE_DEATH": "1=kill 0=reboot",
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


def fmt(ev, base):
    d = DETAIL.get(ev.kind_name)
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
    elif args.wake_latency:
        cmd_wake_latency(events, args)
    elif args.step:
        cmd_step(events, args)
    else:
        cmd_dump(events, args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
