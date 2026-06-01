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
#     xtc-help               this help
#
# The proc enumeration walks proc.c's per-loop slot tables via the
# file-static registry `__lt`; build with -g (the default build does).

import gdb

TASK_STATE = {0: "SCHEDULED", 1: "RUNNING", 2: "PARKED", 3: "DONE"}


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
    return s


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


class XtcHelp(gdb.Command):
    """xtc-help: list xtc debugger commands."""
    def __init__(self):
        super().__init__("xtc-help", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        print(__doc__ if __doc__ else "see tools/gdb/xtc-gdb.py header")
        print("  xtc-loops | xtc-procs [loop] | xtc-proc A | "
              "xtc-mailbox A | xtc-self")


gdb.pretty_printers.append(_lookup_printer)
XtcLoops()
XtcProcs()
XtcProc()
XtcMailbox()
XtcSelf()
XtcHelp()
print("xtc-gdb loaded: xtc-loops, xtc-procs, xtc-proc, xtc-mailbox, "
      "xtc-self, xtc-help")
