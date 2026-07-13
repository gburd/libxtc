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
    xtc-mailbox ADDR   the queued envelopes (sender, size)
    xtc-self           the proc running on the selected thread
    xtc-trace          the causal message trace, HLC-ordered (SEND/
                       RECV/SPAWN/EXIT, with cause edges) -- seq_trace
    xtc-tail-dump F    write the live xtc_tail runtime-microscope ring
                       to file F in the compact portable format, for
                       the offline viewer below

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

## Graphical DST trace viewer (tools/sim-monitor/)

`tools/sim-monitor/` is a separate, **strictly optional** graphical
viewer for the same `xtc_tail` trace format, in the spirit of
TigerBeetle's VOPR visualizer: it animates a recorded deterministic-
simulation run (lanes = loops, flashes = scheduler events and buggify
activations) instead of printing a timeline. Needs raylib
(`nix develop .#sim-monitor`); not required to build the library, not
wired into `make check`. See `tools/sim-monitor/README.md`. The
SCHED/MSG/IO/OS sources above now have a fifth sibling, `SIM`
(`XTC_TAIL_SIM`), covering DST-specific events -- currently buggify
activations (`xtc-tail.py TRACE --source SIM`); the graphical viewer
consumes the same source.

## MSVC / WinDbg

Native MSVC support (a NatVis file for the value views plus an
enumeration extension) is a planned follow-up.  VS
Code on Windows using the MS C/C++ extension with a GDB/LLDB backend
can use the scripts above today.
