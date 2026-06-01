# libxtc debugger tools

Drop-in extensions that give a libxtc program the kind of live process
view BEAM programmers get from `observer` / `recon`, inside the
debugger you already use.  See `docs/guide/debugging.md` for recipes
and `docs/M_OBSERVABILITY.md` for the full plan.

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

Build with `-g` (the default build does).  The tools work on a live
process (run / attach / breakpoint) and on a core dump.  Run them while
stopped so the file-static registry symbols resolve.

## MSVC / WinDbg

Native MSVC support (a NatVis file for the value views plus an
enumeration extension) is stage 4 of `docs/M_OBSERVABILITY.md`.  VS
Code on Windows using the MS C/C++ extension with a GDB/LLDB backend
can use the scripts above today.
