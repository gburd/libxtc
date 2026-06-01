# xtc_lldb.py -- LLDB introspection for libxtc.
#
# Copyright (c) 2026, The XTC Project -- ISC License.
# SPDX-License-Identifier: ISC
#
# The LLDB sibling of tools/gdb/xtc-gdb.py: a live process view of every
# xtc_proc (mailbox depth, run state, links, monitors), for macOS and
# LLDB-based IDE debugging (VS Code, CLion).
#
# Load it:
#     (lldb) command script import tools/lldb/xtc_lldb.py
# or add to ~/.lldbinit:
#     command script import /path/to/libxtc/tools/lldb/xtc_lldb.py
#
# Commands:  xtc-loops  xtc-procs  xtc-proc ADDR  xtc-mailbox ADDR  xtc-self
#            xtc-trace
#
# Run it while STOPPED (a breakpoint or attach), so file-static symbols
# (proc.c's __lt, __current_proc) resolve.  Build with -g.

import lldb

TASK_STATE = {0: "SCHEDULED", 1: "RUNNING", 2: "PARKED", 3: "DONE"}
TRACE_KIND = {0: "SEND", 1: "RECV", 2: "SPAWN", 3: "EXIT"}


def _eval(target, expr):
    v = target.EvaluateExpression(expr)
    if v is None or not v.IsValid() or v.GetError().Fail():
        return None
    return v


def _u(v):
    return v.GetValueAsUnsigned() if v and v.IsValid() else 0


def _pid_str(pid):
    if pid is None or not pid.IsValid():
        return "?"
    return "%d.%d.%d" % (_u(pid.GetChildMemberWithName("loop_id")),
                         _u(pid.GetChildMemberWithName("local_id")),
                         _u(pid.GetChildMemberWithName("gen")))


def _list_len(head, field="next", cap=100000):
    n = 0
    cur = head
    while cur and cur.IsValid() and _u(cur) != 0 and n < cap:
        cur = cur.GetChildMemberWithName(field)
        n += 1
    return n


def _proc_state(p):
    task = p.GetChildMemberWithName("task")
    if _u(task) == 0:
        return "no-task"
    task = task.Dereference()
    st = _u(task.GetChildMemberWithName("state"))
    s = TASK_STATE.get(st, "?%d" % st)
    if st == 2:
        if task.GetChildMemberWithName("park_fd").GetValueAsSigned() >= 0:
            s += "(fd)"
        elif _u(task.GetChildMemberWithName("park_timer")) != 0:
            s += "(timer)"
        elif _u(task.GetChildMemberWithName("park_requested")) != 0:
            s += "(mailbox)"
    return s


def _loop_tables(target):
    """Return list of (loop SBValue, tbl SBValue) for registered loops."""
    out = []
    lt = _eval(target, "__lt")
    if lt is None:
        return out
    n = lt.GetNumChildren()
    for i in range(n):
        e = lt.GetChildAtIndex(i)
        loop = e.GetChildMemberWithName("loop")
        tbl = e.GetChildMemberWithName("tbl")
        if _u(loop) != 0 and _u(tbl) != 0:
            out.append((loop, tbl.Dereference()))
    return out


def _procs_in(target, tbl):
    out = []
    slots = tbl.GetChildMemberWithName("slots")
    cap = _u(tbl.GetChildMemberWithName("cap"))
    if _u(slots) == 0:
        return out
    for i in range(cap):
        slot = slots.GetValueForExpressionPath("[%d]" % i)
        p = slot.GetChildMemberWithName("proc")
        if _u(p) != 0:
            out.append(p)
    return out


def xtc_loops(debugger, command, result, internal_dict):
    target = debugger.GetSelectedTarget()
    tables = _loop_tables(target)
    if not tables:
        print("no loops registered (running? stopped? built -g?)", file=result)
        return
    for loop, tbl in tables:
        ld = loop.Dereference()
        nprocs = len(_procs_in(target, tbl))
        print("loop 0x%x  procs=%d  alive=%d  tasks_run=%d  steals=%d"
              % (_u(loop), nprocs,
                 _u(ld.GetChildMemberWithName("n_alive")),
                 _u(ld.GetChildMemberWithName("n_tasks_run")),
                 _u(ld.GetChildMemberWithName("n_steals"))), file=result)


def xtc_procs(debugger, command, result, internal_dict):
    target = debugger.GetSelectedTarget()
    print("%-18s %-10s %5s %5s %5s %-16s %s"
          % ("proc", "pid", "mbox", "peak", "save", "state", "lnk/mon"),
          file=result)
    total = 0
    for loop, tbl in _loop_tables(target):
        for p in _procs_in(target, tbl):
            pd = p.Dereference()
            total += 1
            links = _list_len(pd.GetChildMemberWithName("links"))
            mons = _list_len(pd.GetChildMemberWithName("monitors"))
            dead = "" if _u(pd.GetChildMemberWithName("alive")) else "  DEAD"
            print("0x%-16x %-10s %5d %5d %5d %-16s %d/%d%s"
                  % (_u(p), _pid_str(pd.GetChildMemberWithName("pid")),
                     _u(pd.GetChildMemberWithName("mbox_n")),
                     _u(pd.GetChildMemberWithName("mbox_peak")),
                     _u(pd.GetChildMemberWithName("mbox_saved")),
                     _proc_state(pd), links, mons, dead), file=result)
    print("(%d procs)" % total, file=result)


def xtc_proc(debugger, command, result, internal_dict):
    target = debugger.GetSelectedTarget()
    p = _eval(target, "(struct xtc_proc *)(%s)" % command.strip())
    if p is None or _u(p) == 0:
        print("usage: xtc-proc <struct xtc_proc *>", file=result)
        return
    pd = p.Dereference()
    print("proc 0x%x  pid=%s" % (_u(p), _pid_str(pd.GetChildMemberWithName("pid"))),
          file=result)
    print("  state   : %s" % _proc_state(pd), file=result)
    print("  alive   : %d  kill_pending=%d"
          % (_u(pd.GetChildMemberWithName("alive")),
             _u(pd.GetChildMemberWithName("kill_pending"))), file=result)
    print("  mailbox : depth=%d peak=%d cap=%d saved=%d recv=%d drop=%d"
          % (_u(pd.GetChildMemberWithName("mbox_n")),
             _u(pd.GetChildMemberWithName("mbox_peak")),
             _u(pd.GetChildMemberWithName("mbox_cap")),
             _u(pd.GetChildMemberWithName("mbox_saved")),
             _u(pd.GetChildMemberWithName("mbox_recv_total")),
             _u(pd.GetChildMemberWithName("mbox_drop_total"))), file=result)
    print("  links   : %d  monitors=%d  monitored_by=%d"
          % (_list_len(pd.GetChildMemberWithName("links")),
             _list_len(pd.GetChildMemberWithName("monitors")),
             _list_len(pd.GetChildMemberWithName("monitored_by"))), file=result)
    print("  recovery: armed=%d fired=%d crit_depth=%d"
          % (_u(pd.GetChildMemberWithName("recovery_armed")),
             _u(pd.GetChildMemberWithName("recovery_fired")),
             _u(pd.GetChildMemberWithName("crit_depth"))), file=result)


def xtc_mailbox(debugger, command, result, internal_dict):
    target = debugger.GetSelectedTarget()
    p = _eval(target, "(struct xtc_proc *)(%s)" % command.strip())
    if p is None or _u(p) == 0:
        print("usage: xtc-mailbox <struct xtc_proc *>", file=result)
        return
    e = p.Dereference().GetChildMemberWithName("mbox_head")
    i = 0
    while _u(e) != 0 and i < 1000:
        ed = e.Dereference()
        print("  [%3d] env 0x%x  from=%s  size=%d"
              % (i, _u(e), _pid_str(ed.GetChildMemberWithName("from")),
                 _u(ed.GetChildMemberWithName("size"))), file=result)
        e = ed.GetChildMemberWithName("next")
        i += 1
    print("  %d message(s)" % i, file=result)


def xtc_self(debugger, command, result, internal_dict):
    target = debugger.GetSelectedTarget()
    cur = _eval(target, "__current_proc")
    if cur is None or _u(cur) == 0:
        print("no current proc on this thread", file=result)
        return
    xtc_proc(debugger, "0x%x" % _u(cur), result, internal_dict)


def xtc_trace(debugger, command, result, internal_dict):
    target = debugger.GetSelectedTarget()
    seq = _eval(target, "__trace_seq")
    if seq is None:
        print("no trace ring (built -g? library linked?)", file=result)
        return
    hlc = _eval(target, "__g_hlc")
    if hlc is not None:
        print("HLC now: %d" % _u(hlc), file=result)
    seq = _u(seq)
    if seq == 0:
        print("no trace events (is tracing enabled? "
              "xtc_trace_enable(1))", file=result)
        return
    ring = _eval(target, "__trace_ring")
    if ring is None:
        print("no trace ring (built -g? library linked?)", file=result)
        return
    esz = ring.GetType().GetArrayElementType().GetByteSize()
    cap = ring.GetType().GetByteSize() // esz if esz else 8192
    n = seq if seq < cap else cap
    start = seq - n
    recs = []
    for i in range(n):
        r = ring.GetValueForExpressionPath("[%d]" % ((start + i) % cap))
        recs.append((_u(r.GetChildMemberWithName("hlc")),
                     _u(r.GetChildMemberWithName("cause")),
                     r.GetChildMemberWithName("kind").GetValueAsSigned(),
                     _pid_str(r.GetChildMemberWithName("self")),
                     _pid_str(r.GetChildMemberWithName("peer")),
                     _u(r.GetChildMemberWithName("detail"))))
    recs.sort(key=lambda t: t[0])
    print("%-20s %-5s %-12s %-12s %s"
          % ("hlc", "kind", "self", "peer", "cause/detail"), file=result)
    for hlcv, cause, kind, selfs, peers, detail in recs:
        ks = TRACE_KIND.get(kind, "?%d" % kind)
        cs = "  cause=%d" % cause if cause else ""
        print("HLC%-17d %-5s self=%-11s peer=%-11s%s  detail=%d"
              % (hlcv, ks, selfs, peers, cs, detail), file=result)
    print("(%d events)" % n, file=result)


def __lldb_init_module(debugger, internal_dict):
    for name, fn in (("xtc-loops", "xtc_loops"), ("xtc-procs", "xtc_procs"),
                     ("xtc-proc", "xtc_proc"), ("xtc-mailbox", "xtc_mailbox"),
                     ("xtc-self", "xtc_self"), ("xtc-trace", "xtc_trace")):
        debugger.HandleCommand("command script add -f %s.%s %s"
                               % (__name__, fn, name))
    print("xtc-lldb loaded: xtc-loops, xtc-procs, xtc-proc, xtc-mailbox, "
          "xtc-self, xtc-trace")
