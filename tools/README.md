# libxtc debugger tools

Drop-in extensions that give a libxtc program the kind of live process
view BEAM programmers get from `observer` / `recon`, inside the
debugger you already use.  See `docs/guide/debugging.md` for recipes.

## GDB

    (gdb) source tools/gdb/xtc-gdb.py

Or in `~/.gdbinit`:

    source /path/to/libxtc/tools/gdb/xtc-gdb.py

VS Code (`launch.json`) and CLion drive GDB underneath; add the same
`source` line via `setupCommands` / the debugger startup commands.

## LLDB (macOS, and LLDB-based IDE debugging)

    (lldb) command script import tools/lldb/xtc_lldb.py

Or in `~/.lldbinit`:

    command script import /path/to/libxtc/tools/lldb/xtc_lldb.py

## Commands (both)

    xtc-loops          scheduler loops + per-loop stats
    xtc-procs [loop]   every proc: pid, mailbox depth, peak, state,
                       links/monitors -- the observer process table
    xtc-proc  ADDR     one proc in detail
    xtc-stranded       triage a suspected stranded-fiber hang: the
                       park-kind histogram plus every PARKED proc with
                       its park shape and wake_pending, flagging the
                       suspects (see below)
    xtc-mailbox ADDR   the queued envelopes (sender, size)
    xtc-self           the proc running on the selected thread
    xtc-trace          the causal message trace, HLC-ordered (SEND/
                       RECV/SPAWN/EXIT, with cause edges) -- seq_trace
    xtc-tail-dump F    write the live xtc_tail runtime-microscope ring
                       to file F in the compact portable format, for
                       the offline viewer below

## Diagnosing "a fiber is never resumed" (xtc-stranded)

The one command to reach for when something appears parked forever.  It
answers the question that separates a genuinely lost wake from an
operation that is merely slow:

    park='-' + wake_pending CLEAR
        No armed wake source (not an fd, not a timer -- this is the
        xtc_aio completion shape) AND no latched wake.  If the operation
        it waits on has demonstrably finished, this is a LOST WAKE:
        nothing remains to re-deliver it.

    park='-' + WAKE_PENDING
        A wake arrived in the prepare/park window and was latched; the
        PENDING verdict has not consumed it yet.  Transient -- but if it
        persists across samples, the consume path is broken, which is a
        DIFFERENT bug.  Say which of the two you observed.

    park='fd' / 'timer' / 'mailbox'
        Waiting on an armed source.  Check the source (is the fd
        readable? has the deadline passed?) before suspecting the
        runtime.

Sample it three times about a second apart.  A real strand is identical
every time; progress shows up as changing counts.  Pair it with
`thread apply all bt` sampled the same way -- that is what distinguishes
a park (identical, no CPU) from an infinite loop (identical line, burning
CPU).

A suspect is not yet a bug report: confirm the wake source actually
completed first.  For an aio park, the kernel's `iou-wrk-*` threads being
present in `/proc/<pid>/task` shows the op was submitted and serviced; a
still-pending syscall is not a strand.

## Offline trace viewer (xtc_tail)

`tools/xtc-tail.py` reads a trace captured with `xtc_tail_dump(fd)` (or
`xtc-tail-dump` from the debugger) and lets an operator read, filter,
step through, and summarize what the runtime did after the fact -- the
BEAM-observer/recon experience for a captured trace.  The format is
self-describing and portable, so the viewer runs anywhere regardless of
the host that produced the trace.

    xtc-tail.py TRACE                 # human-readable event timeline
    xtc-tail.py TRACE --summary       # per-pid / per-kind rollup
    xtc-tail.py TRACE --pid L.I.G     # only one process's events
    xtc-tail.py TRACE --kind RUN,EXIT # only these event kinds
    xtc-tail.py TRACE --source SCHED  # only one source (SCHED|MSG|IO|OS)
    xtc-tail.py TRACE --wake-latency  # RUN events by park->run ns (worst
                                      # first -- finds lost/late wakeups)
    xtc-tail.py TRACE --around T[:W]  # events within +/-W ns of time T
    xtc-tail.py TRACE --step          # interactive: one event per Enter

Build with `-g` (the default build does).  The tools work on a live
process (run / attach / breakpoint) and on a core dump.  Run them while
stopped so the file-static registry symbols resolve.

## MSVC / WinDbg

Native MSVC support (a NatVis file for the value views plus an
enumeration extension) is a planned follow-up.  VS
Code on Windows using the MS C/C++ extension with a GDB/LLDB backend
can use the scripts above today.
