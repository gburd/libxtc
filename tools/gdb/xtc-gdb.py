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


def _all_loops():
    """Yield EVERY loop, including ones with no procs registered.

    _loop_tables() only sees loops that have a proc table, so a loop whose
    procs have all exited (or that never spawned one) is invisible to it --
    and those are exactly the loops worth inspecting when a ring is not
    being drained.  Start from any registered loop, hop to its executor,
    and walk exec->loops[0..n_loops-1]; fall back to the proc-table walk
    for a standalone loop with no executor.
    """
    seen = []
    for loop, _tbl in _loop_tables():
        ex = loop["exec"]
        if int(ex) != 0:
            try:
                n = int(ex["n_loops"])
                arr = ex["loops"]
                for i in range(n):
                    lp = arr[i]
                    if int(lp) != 0 and int(lp) not in [int(x) for x in seen]:
                        seen.append(lp)
                if seen:
                    for lp in seen:
                        yield lp
                    return
            except gdb.error:
                pass
    # No executor (or the walk failed): fall back to the proc-table view.
    for loop, _tbl in _loop_tables():
        yield loop


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


class XtcRings(gdb.Command):
    """xtc-rings: map each loop to its io_uring ring fd, owner thread, and
    how many completions are sitting UNREAPED in its CQ.

    This is the command for "a ring has posted completions that nobody
    drained".  /proc/<pid>/fdinfo/<ring_fd> gives CqTail - CqHead (the
    kernel's own count of posted-but-unconsumed CQEs); this maps that fd
    back to the loop that owns it, so you can then ask what THAT loop's
    worker is doing.

    Columns:
      loop      the xtc_loop_t *
      ring_fd   io->ring.ring_fd -- match this against fdinfo
      owner     the pthread id recorded as the ring's polling thread
                (0 = never polled yet)
      unreaped  CqTail - CqHead read from the live ring memory: completions
                the kernel has PLACED IN THE VISIBLE CQ and we have not
                consumed.  It SATURATES AT THE CQ SIZE -- see the second
                caveat below, which cuts the other way from the first.
      alive     loop->n_alive (tasks HOMED here, incl. parked ones)

    READ THE CAVEAT BEFORE CONCLUDING ANYTHING FROM unreaped > 0.

    A single sample showing unreaped > 0 proves NOTHING.  Under load,
    completions arrive continuously and any snapshot catches some of them
    in flight; measured on a healthy-but-busy 8-loop run, one ring read
    218 unreaped and then 1 three seconds later.  That is a ring draining
    normally, not a stuck one.

    AND unreaped == 0 DOES NOT MEAN "NOTHING IS PENDING".

    This kernel reports IORING_FEAT_NODROP, so when the visible CQ fills,
    further completions go to a kernel-side OVERFLOW LIST rather than being
    dropped -- and CqTail - CqHead cannot see that list.  Measured directly:
    with 4000 completions outstanding on a CQ of 128, this number read 128,
    not 4000.  So it saturates, and a reading of 0 taken just after a drain
    is compatible with a backlog the kernel has not yet flushed forward.

    The two caveats point in opposite directions and both matter:
      unreaped > 0  does not prove a ring is stuck (it is the normal state
                    of a busy ring);
      unreaped == 0 does not prove a ring is empty (it saturates, and the
                    overflow list is invisible to it).
    Cross-check with io_uring_cq_has_overflow() -- or, from gdb, the
    IORING_SQ_CQ_OVERFLOW bit in *io->ring.sq.kflags -- before treating a
    zero as evidence of anything.

    (For the record, overflow was MEASURED NOT to lose completions on this
    kernel: a bounded 16-per-pass drain recovered 4000 of 4000 with no
    duplicates, and io_uring_wait_cqe_timeout with a backlog present returned
    in 0.0 ms rather than blocking.  So a full CQ is not itself a lost-wake
    mechanism -- it is only a reason not to trust this counter.)

    To show a ring is genuinely STUCK, sample it at least three times a
    few seconds apart and show the count does NOT fall:

        (gdb) xtc-rings
        (gdb) shell sleep 3
        (gdb) xtc-rings
        (gdb) shell sleep 3
        (gdb) xtc-rings

    A count that stays pinned at the same value across samples, while the
    process makes no progress, is the real signal.  A count that moves --
    in either direction -- means that ring is being serviced.

    One inference that IS sound once you have a stuck ring: a poller
    cannot be blocked in io_uring_wait_cqe on a ring whose CQ is
    non-empty, because liburing checks the CQ before entering the kernel.
    So a persistently non-empty CQ means that loop's worker is somewhere
    OTHER than its own poll -- running a task, blocked on a peer's ring,
    or gone.  Use the `io` column to join against the `io=` argument in a
    blocked thread's xtc_io_poll frame.
    """
    def __init__(self):
        super().__init__("xtc-rings", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        print("%-18s %-6s %-18s %-8s %-20s %-9s %-5s %s"
              % ("loop", "id", "io", "ring_fd", "owner_tid", "unreaped",
                 "ovf", "alive"))
        n = 0
        for loop in _all_loops():
            io = loop["io"]
            if int(io) == 0:
                continue
            n += 1
            fd = owner = unreaped = "?"
            try:
                fd = int(io["ring"]["ring_fd"])
            except gdb.error:
                pass
            try:
                owner = ("%d" % int(io["owner_tid"])
                         if int(io["owner_set"]) else "unset")
            except gdb.error:
                owner = "n/a"
            # CqTail - CqHead straight out of the mapped ring.
            try:
                cq = io["ring"]["cq"]
                khead = int(cq["khead"].dereference())
                ktail = int(cq["ktail"].dereference())
                unreaped = "%d" % (ktail - khead)
            except gdb.error:
                unreaped = "?"
            # IORING_SQ_CQ_OVERFLOW (bit 1) in the SQ kflags.  Printed next
            # to unreaped because it is the ONLY thing that makes a zero
            # there meaningful: unreaped saturates at the CQ size and cannot
            # see the kernel's overflow list, so "0" plus "ovf=1" means
            # completions ARE pending and simply not visible here.
            try:
                kflags = int(io["ring"]["sq"]["kflags"].dereference())
                ovf = "1" if (kflags & 2) else "0"
            except gdb.error:
                ovf = "?"
            try:
                lid = int(loop["exec_id"])
            except gdb.error:
                lid = -1
            # The io pointer is what a blocked poller's xtc_io_poll frame
            # shows as `io=`, so printing it here makes the join to
            # `thread apply all bt` direct instead of guesswork.
            print("%-18s %-6s %-18s %-8s %-20s %-9s %-5s %s"
                  % (str(loop), lid if lid >= 0 else "solo", str(io), fd,
                     owner, unreaped, ovf, int(loop["n_alive"])))
        if n == 0:
            print("no loops with an io backend (running? built -g?)")
        else:
            print("(%d ring(s).  unreaped > 0 is the NORMAL state of a busy "
                  "ring -- sample 3x before" % n)
            print(" calling one stuck.  unreaped == 0 does NOT mean empty: it "
                  "saturates at the CQ")
            print(" size and cannot see the kernel overflow list, so read it "
                  "with ovf -- 0 with")
            print(" ovf=1 means completions ARE pending and invisible here.)")


class XtcCqes(gdb.Command):
    """xtc-cqes [ring_fd|loop-addr]: dump the UNREAPED CQEs in a ring's CQ,
    with the user_data each one carries.

    This is the join that turns "a ring has N unreaped completions" into
    "THIS fiber's completion is sitting in the CQ and the drain is not taking
    it".  xtc-rings gives a COUNT; a count cannot name the owner, so it can
    only ever be suggestive.  Here the user_data is decoded the same way
    xtc_io_poll decodes it:

      low bit SET    an xtc_aio_t * (the pointer with bit 0 masked off).  Its
                     ->tag is the xtc_task_t * -- the SAME key PARK_TASK,
                     SUBMIT, REAP and WAKE carry, so it joins straight to a
                     strand in an xtc_tail trace.
      low bit CLEAR  a struct __xtc_uring_fd * (an fd registration).  Its
                     ->tag is the parked task, ->fd the descriptor.
      NULL           a discarded cancel CQE (poll_remove sets no data).

    If a stranded fiber's task pointer appears here, the completion was
    posted by the kernel, is visible in the CQ, and was never consumed --
    which is a DRAIN-side bug, not a lost completion.

    Read it together with the ovf column from xtc-rings: this walks only the
    VISIBLE CQ, so it cannot see a kernel overflow list either.
    """

    def __init__(self):
        super(XtcCqes, self).__init__("xtc-cqes", gdb.COMMAND_USER)

    def _dump(self, io, label):
        try:
            cq = io["ring"]["cq"]
            head = int(cq["khead"].dereference())
            tail = int(cq["ktail"].dereference())
            mask = int(cq["ring_mask"])
            cqes = cq["cqes"]
        except gdb.error as e:
            print("  %s: cannot read cq (%s)" % (label, e))
            return
        n = tail - head
        print("%s  CqHead=%u CqTail=%u unreaped=%d" % (label, head, tail, n))
        if n <= 0:
            print("    (nothing unreaped in the visible CQ)")
            return
        # Bound the walk: a corrupt head/tail should not spin forever.
        if n > 4096:
            print("    (unreaped=%d exceeds 4096 -- refusing to walk, the "
                  "head/tail pair looks wrong)" % n)
            return
        for i in range(n):
            try:
                cqe = cqes[(head + i) & mask]
                ud = int(cqe["user_data"])
                res = int(cqe["res"])
                flags = int(cqe["flags"])
            except gdb.error as e:
                print("    [%d] unreadable (%s)" % (i, e))
                continue
            if ud == 0:
                what = "NULL (discarded cancel CQE)"
            elif ud & 1:
                aio = ud & ~1
                what = "aio 0x%x" % aio
                try:
                    a = gdb.Value(aio).cast(
                        gdb.lookup_type("xtc_aio_t").pointer())
                    what += "  task=0x%x op=%d" % (
                        int(a["tag"]), int(a["op"]))
                except gdb.error:
                    what += "  (tag unreadable)"
            else:
                what = "uring_fd 0x%x" % ud
                try:
                    uf = gdb.Value(ud).cast(
                        gdb.lookup_type("struct __xtc_uring_fd").pointer())
                    what += "  fd=%d task=0x%x%s" % (
                        int(uf["fd"]), int(uf["tag"]),
                        " DEAD" if int(uf["dead"]) else "")
                except gdb.error:
                    what += "  (fd/tag unreadable)"
            print("    [%2d] user_data=0x%-16x res=%-6d flags=0x%-4x %s"
                  % (i, ud, res, flags, what))

    def invoke(self, arg, from_tty):
        arg = arg.strip()
        want_fd = None
        if arg:
            try:
                want_fd = int(arg, 0)
            except ValueError:
                want_fd = None
            if want_fd is None or want_fd > 100000:
                # treat it as a loop address
                loop = gdb.parse_and_eval(arg)
                self._dump(loop["io"], "loop %s" % arg)
                return
        n = 0
        for loop in _all_loops():
            io = loop["io"]
            if int(io) == 0:
                continue
            try:
                fd = int(io["ring"]["ring_fd"])
            except gdb.error:
                continue
            if want_fd is not None and fd != want_fd:
                continue
            try:
                lid = int(loop["exec_id"])
            except gdb.error:
                lid = -1
            self._dump(io, "loop %s id=%s ring_fd=%d" % (loop, lid, fd))
            n += 1
        if n == 0:
            print("no matching ring (try xtc-rings first)")
        else:
            print("(task=0x... joins to PARK_TASK / SUBMIT / REAP / WAKE in "
                  "an xtc_tail trace)")


class XtcProcs(gdb.Command):
    """xtc-procs [loop-addr]: list every proc (or one loop's)."""
    def __init__(self):
        super().__init__("xtc-procs", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        want = None
        if arg.strip():
            want = int(gdb.parse_and_eval(arg.strip()))
        # `task` is printed because XTC_TAIL_WAKE records the dispatched
        # task POINTER in its detail field (dispatch has a task, not a
        # proc).  Without this column there is no way to turn a WAKE
        # event back into a pid.
        hdr = "%-18s %-10s %-18s %5s %5s %5s %-18s %s" % (
            "proc", "pid", "task", "mbox", "peak", "save", "state",
            "lnk/mon")
        print(hdr)
        total = 0
        for loop, tbl in _loop_tables():
            if want is not None and int(loop) != want:
                continue
            for p in _procs_in(tbl):
                total += 1
                links = _list_len(p["links"])
                mons = _list_len(p["monitors"])
                print("%-18s %-10s %-18s %5d %5d %5d %-18s %d/%d%s"
                      % (str(p), _pid_str(p["pid"]), str(p["task"]),
                         int(p["mbox_n"]), int(p["mbox_peak"]),
                         int(p["mbox_saved"]), _proc_state(p),
                         links, mons,
                         "" if int(p["alive"]) else "  DEAD"))
        print("(%d procs)" % total)


class XtcTailDropped(gdb.Command):
    """xtc-tail-dropped: how many xtc_tail records the ring has overwritten.

    Reads the ring bookkeeping directly, so it works on a HUNG process
    where calling xtc_tail_dropped() would be awkward or unsafe.

    CHECK THIS BEFORE BELIEVING ANY ABSENCE.  With a non-zero dropped
    count, "pid X has no events", "this loop never polled" and "no WAKE
    for this task" are all unfalsifiable -- the records may simply have
    been evicted.  A consumer had to withdraw two verdicts that were
    exactly this artifact.  Conclusions drawn from events that are
    PRESENT stay valid regardless.
    """
    def __init__(self):
        super().__init__("xtc-tail-dropped", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        seq = _sym("__tail_seq")
        cap = _sym("XTC_TAIL_RING")
        if seq is None:
            print("no __tail_seq symbol (not linked with xtc_tail, or "
                  "stripped -- build with -g)")
            return
        try:
            n = int(seq)
        except gdb.error:
            print("could not read __tail_seq")
            return
        # XTC_TAIL_RING is a #define, so it is usually absent from the
        # debug info; fall back to the array's own length.
        ring = 0
        if cap is not None:
            try:
                ring = int(cap)
            except gdb.error:
                ring = 0
        if ring == 0:
            arr = _sym("__tail_ring")
            if arr is not None:
                try:
                    ring = int(arr.type.range()[1]) + 1
                except gdb.error:
                    ring = 0
        if ring == 0:
            print("emitted=%d  (ring capacity unknown -- cannot compute "
                  "dropped)" % n)
            return
        dropped = n - ring if n > ring else 0
        print("emitted=%d  ring=%d  buffered=%d  dropped=%d"
              % (n, ring, n if n < ring else ring, dropped))
        if dropped:
            print("WARNING: %d record(s) were OVERWRITTEN.  Any claim that "
                  "rests on an event being ABSENT is unfalsifiable for this "
                  "capture." % dropped)
        else:
            print("ring did not wrap: absence claims are meaningful.")


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
                # local_id 0 is the loop's own service fiber (a supervisor /
                # acceptor that sits in xtc_recv(-1) for the process lifetime).
                # park='-' with no latched wake is its NORMAL idle state, so
                # counting it as a suspect buries the real ones: a consumer
                # reported 31 of 33 "suspects" were exactly these.
                svc = 0
                try:
                    svc = 1 if int(p["pid"]["local_id"]) == 0 else 0
                except gdb.error:
                    pass
                rows.append((str(p), _pid_str(p["pid"]), kind, wp,
                             int(loop), svc))

        print("proc states:")
        for k in sorted(states):
            print("    %-12s %d" % (k, states[k]))
        print("park kinds (PARKED procs only):")
        for k in sorted(kinds):
            print("    park=%-8s %d" % (k, kinds[k]))

        # A suspect is park='-' with no latched wake AND not a service fiber.
        susp = [r for r in rows if r[2] == "-" and r[3] == 0 and not r[5]]
        idle_svc = [r for r in rows if r[2] == "-" and r[3] == 0 and r[5]]
        print("")
        print("%-18s %-10s %-8s %-14s %s"
              % ("proc", "pid", "park", "wake_pending", "verdict"))
        for r in rows:
            verdict = ""
            if r[2] == "-" and r[3] == 0:
                if r[5]:
                    verdict = "(local_id 0: service fiber, idle is normal)"
                else:
                    verdict = "<-- SUSPECT: no source, no latched wake"
            elif r[2] == "-" and r[3] != 0:
                verdict = "latched wake, should resume"
            print("%-18s %-10s %-8s %-14s %s"
                  % (r[0], r[1], r[2], "SET" if r[3] else "clear", verdict))
        print("")
        print("(%d parked, %d suspect, %d idle service fiber(s) excluded)"
              % (len(rows), len(susp), len(idle_svc)))
        if idle_svc:
            print("NOTE: %d proc(s) with local_id 0 park with no source and no "
                  "latched wake.  That is the NORMAL idle state of a per-loop "
                  "service fiber blocked in xtc_recv(..., -1), so they are "
                  "excluded above.  If YOUR long-lived receivers have a "
                  "non-zero local_id they will still appear as suspects -- "
                  "check them against what you know parks forever by design."
                  % len(idle_svc))
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
        print("  xtc-loops | xtc-rings | xtc-cqes [fd] | xtc-procs [loop] | xtc-proc A | "
              "xtc-stranded | "
              "xtc-mailbox A | xtc-self | xtc-trace")


gdb.pretty_printers.append(_lookup_printer)
XtcLoops()
XtcRings()
XtcCqes()
XtcProcs()
XtcProc()
XtcStranded()
XtcTailDropped()
XtcMailbox()
XtcSelf()
XtcTrace()
XtcTailDump()
XtcHelp()
print("xtc-gdb loaded: xtc-loops, xtc-rings, xtc-cqes, xtc-procs, xtc-proc, "
      "xtc-stranded, xtc-tail-dropped, "
      "xtc-mailbox, "
      "xtc-self, xtc-trace, xtc-tail-dump, xtc-help")
