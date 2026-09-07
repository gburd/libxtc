# xtc-gdb.py -- GDB introspection for libxtc.
#
# Copyright (c) 2026, The XTC Project -- ISC License.
# SPDX-License-Identifier: ISC
#
# A "drop-in" debugger extension that gives a libxtc program the kind
# of live process view BEAM programmers get from observer / recon:
# enumerate every xtc_proc, see its mailbox depth, run state, links and
# monitors, and follow a fiber across a park.  Works on a live process
# (attach / breakpoint) and on a core dump.
#
# Load it:
#     (gdb) source tools/gdb/xtc-gdb.py
# or add to ~/.gdbinit:
#     source /path/to/libxtc/tools/gdb/xtc-gdb.py
#
# Commands:
#     xtc-loops              list scheduler loops and their stats
#     xtc-procs [loop]       list all procs (optionally one loop's)
#     xtc-proc  ADDR|PID     detail one proc (struct xtc_proc *)
#     xtc-mailbox ADDR       dump a proc's mailbox (queued envelopes)
#     xtc-self               the proc running on the selected thread
#     xtc-trace              dump the causal message trace (HLC-ordered)
#     xtc-help               this help
#
# The proc enumeration walks proc.c's per-loop slot tables via the
# file-static registry `__lt`; build with -g (the default build does).

import gdb

TASK_STATE = {0: "SCHEDULED", 1: "RUNNING", 2: "PARKED", 3: "DONE"}
TRACE_KIND = {0: "SEND", 1: "RECV", 2: "SPAWN", 3: "EXIT"}


def _sym(name):
    """Read a (possibly file-static) global by name, or None."""
    try:
        return gdb.parse_and_eval(name)
    except gdb.error:
        return None


def _pid_str(pid):
    try:
        return "%d.%d.%d" % (int(pid["loop_id"]),
                             int(pid["local_id"]),
                             int(pid["gen"]))
    except gdb.error:
        return "?"


# ---- pretty-printer for xtc_pid_t -------------------------------------

class PidPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        return "pid<%s>" % _pid_str(self.val)


def _lookup_printer(val):
    t = val.type.strip_typedefs()
    if t.code == gdb.TYPE_CODE_STRUCT and t.tag == "xtc_pid":
        return PidPrinter(val)
    return None


# ---- proc-table walk --------------------------------------------------

def _loop_tables():
    """Yield (loop_ptr, table_ptr) for every registered loop."""
    lt = _sym("__lt")
    if lt is None:
        return
    n = int(lt.type.range()[1]) + 1
    for i in range(n):
        e = lt[i]
        loop = e["loop"]
        tbl = e["tbl"]
        if int(loop) != 0 and int(tbl) != 0:
            yield loop, tbl


def _procs_in(tbl):
    """Yield live struct xtc_proc * in a proc table."""
    slots = tbl["slots"]
    cap = int(tbl["cap"])
    if int(slots) == 0:
        return
    for i in range(cap):
        p = slots[i]["proc"]
        if int(p) != 0:
            yield p


def _list_len(head, nextfield="next", cap=100000):
    n, cur = 0, head
    while int(cur) != 0 and n < cap:
        cur = cur[nextfield]
        n += 1
    return n


def _proc_state(p):
    task = p["task"]
    if int(task) == 0:
        return "no-task"
    st = int(task["state"])
    s = TASK_STATE.get(st, "?%d" % st)
    if st == 2:  # PARKED -- why?
        if int(task["park_fd"]) >= 0:
            s += "(fd %d)" % int(task["park_fd"])
        elif int(task["park_timer"]) != 0:
            s += "(timer)"
        elif int(task["park_requested"]) != 0:
            s += "(mailbox)"
    # wake_pending is THE discriminator for the cross-loop wake-loss family:
    # a task PARKED with wake_pending set means a waker arrived in the
    # prepare/park window, latched the flag, and the PENDING verdict has not
    # yet consumed it -- transient and benign.  A task PARKED with
    # wake_pending CLEAR, whose wake source has already completed, is a LOST
    # WAKE: nothing is left to re-deliver it.  Distinguishing those two is
    # what tells a stranded-fiber report apart from a slow one, so print it
    # whenever it is set (and always in xtc-proc's detail view).
    try:
        if int(task["wake_pending"]) != 0:
            s += " WAKE_PENDING"
    except gdb.error:
        pass    # older libxtc without the field
    return s


def _park_kind(task):
    """The park SHAPE, matching what xtc_dump's histogram reports:
    'fd' / 'timer' / 'mailbox' / '-' (no armed source).

    '-' is the interesting one for AIO: xtc_aio parks with no fd and no
    timer, waiting only for its completion to be reaped and dispatched, so
    a fiber stuck at park='-' with its io_uring worker present is the
    aio-completion shape rather than a wait_fd or timer shape.
    """
    if int(task) == 0:
        return "no-task"
    if int(task["park_fd"]) >= 0:
        return "fd"
    if int(task["park_timer"]) != 0:
        return "timer"
    if int(task["park_requested"]) != 0:
        return "mailbox"
    return "-"


# ---- commands ---------------------------------------------------------

class XtcLoops(gdb.Command):
    """xtc-loops: list scheduler loops and per-loop stats."""
    def __init__(self):
        super().__init__("xtc-loops", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        any_loop = False
        for loop, tbl in _loop_tables():
            any_loop = True
            n_procs = sum(1 for _ in _procs_in(tbl))
            exec_id = int(loop["exec_id"])
            where = "exec#%d" % exec_id if exec_id >= 0 else "standalone"
            print("loop %s  [%s]  procs=%d  alive=%d  tasks_run=%d  steals=%d"
                  % (str(loop), where, n_procs,
                     int(loop["n_alive"]), int(loop["n_tasks_run"]),
                     int(loop["n_steals"])))
        if not any_loop:
            print("no loops registered (is the program running? built -g?)")


class XtcProcs(gdb.Command):
    """xtc-procs [loop-addr]: list every proc (or one loop's)."""
    def __init__(self):
        super().__init__("xtc-procs", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        want = None
        if arg.strip():
            want = int(gdb.parse_and_eval(arg.strip()))
        hdr = "%-18s %-10s %5s %5s %5s %-18s %s" % (
            "proc", "pid", "mbox", "peak", "save", "state", "lnk/mon")
        print(hdr)
        total = 0
        for loop, tbl in _loop_tables():
            if want is not None and int(loop) != want:
                continue
            for p in _procs_in(tbl):
                total += 1
                links = _list_len(p["links"])
                mons = _list_len(p["monitors"])
                print("%-18s %-10s %5d %5d %5d %-18s %d/%d%s"
                      % (str(p), _pid_str(p["pid"]),
                         int(p["mbox_n"]), int(p["mbox_peak"]),
                         int(p["mbox_saved"]), _proc_state(p),
                         links, mons,
                         "" if int(p["alive"]) else "  DEAD"))
        print("(%d procs)" % total)


class XtcStranded(gdb.Command):
    """xtc-stranded: triage a suspected stranded-fiber hang.

    Prints the park-kind histogram (matching xtc_dump's) and then every
    PARKED proc with its park shape and wake_pending, flagging the ones
    that look stranded.

    This is the one command to run when a fiber appears never to be
    resumed.  It answers, in one shot, the question that distinguishes the
    cross-loop wake-loss family from a merely slow operation:

      park='-' + wake_pending CLEAR
          The aio-completion shape with NO latched wake.  If the operation
          it was waiting for has demonstrably completed (io_uring workers
          present, the syscall finished), this is a LOST WAKE: nothing is
          left to re-deliver it.  Report this.

      park='-' + WAKE_PENDING
          A wake arrived in the prepare/park window and was latched; the
          PENDING verdict has not consumed it yet.  Transient.  If it
          persists across several samples the consume path is broken,
          which is a different bug -- say which you saw.

      park='fd' / 'timer'
          Waiting on an armed source.  Not the aio shape; check whether
          the fd is readable / the deadline has passed before suspecting
          libxtc.

    Sample it 3x a second apart: a genuine strand is byte-identical every
    time, while progress shows up as changing counts.
    """
    def __init__(self):
        super().__init__("xtc-stranded", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        kinds = {}
        states = {}
        rows = []
        for loop, tbl in _loop_tables():
            for p in _procs_in(tbl):
                task = p["task"]
                if int(task) == 0:
                    continue
                st = int(task["state"])
                states[TASK_STATE.get(st, "?%d" % st)] = \
                    states.get(TASK_STATE.get(st, "?%d" % st), 0) + 1
                if st != 2:      # only PARKED procs can be stranded
                    continue
                kind = _park_kind(task)
                kinds[kind] = kinds.get(kind, 0) + 1
                wp = 0
                try:
                    wp = int(task["wake_pending"])
                except gdb.error:
                    pass
                rows.append((str(p), _pid_str(p["pid"]), kind, wp,
                             int(loop)))

        print("proc states:")
        for k in sorted(states):
            print("    %-12s %d" % (k, states[k]))
        print("park kinds (PARKED procs only):")
        for k in sorted(kinds):
            print("    park=%-8s %d" % (k, kinds[k]))

        # The suspects: no armed wake source AND no latched wake.
        susp = [r for r in rows if r[2] == "-" and r[3] == 0]
        print("")
        print("%-18s %-10s %-8s %-14s %s"
              % ("proc", "pid", "park", "wake_pending", "verdict"))
        for r in rows:
            verdict = ""
            if r[2] == "-" and r[3] == 0:
                verdict = "<-- SUSPECT: no source, no latched wake"
            elif r[2] == "-" and r[3] != 0:
                verdict = "latched wake, should resume"
            print("%-18s %-10s %-8s %-14s %s"
                  % (r[0], r[1], r[2], "SET" if r[3] else "clear", verdict))
        print("")
        print("(%d parked, %d suspect)" % (len(rows), len(susp)))
        if susp:
            print("For each suspect, confirm its wake source actually "
                  "completed (e.g. iou-wrk threads present for an aio "
                  "park) before reporting a lost wake -- a pending "
                  "syscall is not a strand.")


class XtcProc(gdb.Command):
    """xtc-proc ADDR: detail one proc (struct xtc_proc *)."""
    def __init__(self):
        super().__init__("xtc-proc", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        if not arg.strip():
            print("usage: xtc-proc <struct xtc_proc *>")
            return
        v = gdb.parse_and_eval(arg.strip())
        p = v.cast(gdb.lookup_type("struct xtc_proc").pointer())
        print("proc %s  pid=%s  loop=%s"
              % (str(p), _pid_str(p["pid"]), str(p["loop"])))
        print("  state        : %s" % _proc_state(p))
        print("  alive        : %d   kill_pending=%d"
              % (int(p["alive"]), int(p["kill_pending"])))
        print("  mailbox      : depth=%d peak=%d cap=%d saved=%d recv_total=%d drop_total=%d"
              % (int(p["mbox_n"]), int(p["mbox_peak"]), int(p["mbox_cap"]),
                 int(p["mbox_saved"]), int(p["mbox_recv_total"]),
                 int(p["mbox_drop_total"])))
        print("  wm           : lvl=%d fired=%d"
              % (int(p["mbox_wm_lvl"]), int(p["mbox_wm_fired"])))
        print("  links        : %d   monitors=%d  monitored_by=%d"
              % (_list_len(p["links"]), _list_len(p["monitors"]),
                 _list_len(p["monitored_by"])))
        print("  recovery     : armed=%d fired=%d crit_depth=%d"
              % (int(p["recovery_armed"]), int(p["recovery_fired"]),
                 int(p["crit_depth"])))
        fn = p["fn"]
        print("  entry fn     : %s" % str(fn))


class XtcMailbox(gdb.Command):
    """xtc-mailbox ADDR: dump a proc's queued envelopes."""
    def __init__(self):
        super().__init__("xtc-mailbox", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        if not arg.strip():
            print("usage: xtc-mailbox <struct xtc_proc *>")
            return
        p = gdb.parse_and_eval(arg.strip()).cast(
            gdb.lookup_type("struct xtc_proc").pointer())
        e = p["mbox_head"]
        i = 0
        while int(e) != 0 and i < 1000:
            print("  [%3d] env %s  from=%s  size=%d"
                  % (i, str(e), _pid_str(e["from"]), int(e["size"])))
            e = e["next"]
            i += 1
        print("  %d message(s); save-queue=%d" % (i, int(p["mbox_saved"])))


class XtcSelf(gdb.Command):
    """xtc-self: the proc running on the selected thread (if any)."""
    def __init__(self):
        super().__init__("xtc-self", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        cur = _sym("__current_proc")
        if cur is None or int(cur) == 0:
            print("no current proc on this thread "
                  "(not inside a fiber, or off-loop)")
            return
        gdb.execute("xtc-proc 0x%x" % int(cur))


class XtcTrace(gdb.Command):
    """xtc-trace: dump the causal message trace ring (HLC-ordered)."""
    def __init__(self):
        super().__init__("xtc-trace", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        seq = _sym("__trace_seq")
        if seq is None:
            print("no trace ring (built -g? library linked?)")
            return
        hlc = _sym("__g_hlc")
        if hlc is not None:
            print("HLC now: %d" % int(hlc))
        seq = int(seq)
        if seq == 0:
            print("no trace events (is tracing enabled? "
                  "xtc_trace_enable(1))")
            return
        ring = _sym("__trace_ring")
        if ring is None:
            print("no trace ring (built -g? library linked?)")
            return
        cap = int(ring.type.range()[1]) + 1
        n = seq if seq < cap else cap
        start = seq - n
        recs = [ring[(start + i) % cap] for i in range(n)]
        recs.sort(key=lambda r: int(r["hlc"]))
        print("%-20s %-5s %-12s %-12s %s"
              % ("hlc", "kind", "self", "peer", "cause/detail"))
        for r in recs:
            kind = TRACE_KIND.get(int(r["kind"]), "?%d" % int(r["kind"]))
            cause = int(r["cause"])
            cs = "  cause=%d" % cause if cause else ""
            print("HLC%-17d %-5s self=%-11s peer=%-11s%s  detail=%d"
                  % (int(r["hlc"]), kind, _pid_str(r["self"]),
                     _pid_str(r["peer"]), cs, int(r["detail"])))
        print("(%d events)" % n)


class XtcTailDump(gdb.Command):
    """xtc-tail-dump FILE: write the live xtc_tail ring to FILE in the
    compact portable format, for the offline viewer (tools/xtc-tail.py)."""
    def __init__(self):
        super().__init__("xtc-tail-dump", gdb.COMMAND_USER)

    def _leb128(self, out, v):
        while True:
            b = v & 0x7F
            v >>= 7
            if v:
                out.append(b | 0x80)
            else:
                out.append(b)
                break

    def invoke(self, arg, from_tty):
        path = arg.strip()
        if not path:
            print("usage: xtc-tail-dump FILE")
            return
        seq = _sym("__tail_seq")
        ring = _sym("__tail_ring")
        if seq is None or ring is None:
            print("no xtc_tail ring (built -g? library linked? tail enabled?)")
            return
        seq = int(seq)
        cap = int(ring.type.range()[1]) + 1
        n = seq if seq < cap else cap
        start = 0 if seq < cap else seq % cap
        recs = [ring[(start + i) % cap] for i in range(n)]
        base = int(recs[0]["ts_ns"]) if recs else 0
        # header: magic "XTCL", version 2, flags LE(1), count, base_ts u64
        import struct
        blob = bytearray()
        blob += struct.pack("<IIII", 0x5854434C, 2, 1, n)
        blob += struct.pack("<Q", base)
        prev = base
        for r in recs:
            ts = int(r["ts_ns"])
            dts = ts - prev if ts >= prev else 0
            prev = ts
            pid = r["pid"]
            blob.append(int(r["kind"]) & 0xFF)
            blob.append(int(r["source"]) & 0xFF)
            self._leb128(blob, dts)
            self._leb128(blob, int(pid["loop_id"]))
            self._leb128(blob, int(pid["local_id"]))
            self._leb128(blob, int(pid["gen"]))
            self._leb128(blob, int(r["detail"]))
        with open(path, "wb") as f:
            f.write(blob)
        print("wrote %d events (%d bytes) to %s -- view with "
              "tools/xtc-tail.py %s" % (n, len(blob), path, path))


class XtcHelp(gdb.Command):
    """xtc-help: list xtc debugger commands."""
    def __init__(self):
        super().__init__("xtc-help", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        print(__doc__ if __doc__ else "see tools/gdb/xtc-gdb.py header")
        print("  xtc-loops | xtc-procs [loop] | xtc-proc A | "
              "xtc-stranded | "
              "xtc-mailbox A | xtc-self | xtc-trace")


gdb.pretty_printers.append(_lookup_printer)
XtcLoops()
XtcProcs()
XtcProc()
XtcStranded()
XtcMailbox()
XtcSelf()
XtcTrace()
XtcTailDump()
XtcHelp()
print("xtc-gdb loaded: xtc-loops, xtc-procs, xtc-proc, xtc-stranded, "
      "xtc-mailbox, "
      "xtc-self, xtc-trace, xtc-tail-dump, xtc-help")
