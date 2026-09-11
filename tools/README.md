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
    xtc-rings          each loop -> its io_uring ring fd, the thread that
                       polls it, and how many completions the KERNEL has
                       posted that we have not reaped (see below)
    xtc-cqes [fd]      the UNREAPED CQEs in a ring's CQ, each decoded to the
                       aio/fd registration and the TASK it belongs to -- the
                       join that turns a count into a named owner (see below)
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

## Diagnosing "a completion arrived but nobody reaped it" (xtc-rings)

`/proc/<pid>/fdinfo/<ring_fd>` reports `CqHead` and `CqTail`; the difference
is the number of completions the KERNEL has already posted that userspace has
not consumed.  A non-zero value with a stalled process means the I/O finished
and the loop that owns that ring never drained it.

`xtc-rings` maps the kernel's view back to libxtc's:

    (gdb) xtc-rings
    loop               id     ring_fd  owner_tid            unreaped  alive
    0x5163c0           0      5        140737339500224      45        36
    0x519840           3      14       140737314322112      21        0

Match `ring_fd` against the `fdinfo` sweep, then use `owner_tid` to find which
thread is supposed to be polling that ring and ask what it is actually doing
(`thread apply all bt`).  The `io` column is what a blocked poller's
`xtc_io_poll` frame shows as `io=`, so the join is direct.

**A single sample showing `unreaped > 0` proves nothing.**  Under load,
completions arrive continuously and any snapshot catches some in flight:
measured on a healthy-but-busy 8-loop run, one ring read 218 unreaped and then
1 three seconds later.  Sample at least three times a few seconds apart --
a count that stays *pinned* while the process makes no progress is the signal;
a count that moves means the ring is being serviced.

One inference worth knowing: a poller **cannot** be blocked in
`io_uring_wait_cqe` on a ring whose CQ is non-empty -- liburing checks the CQ
before entering the kernel.  So if `unreaped > 0`, that loop's worker is
somewhere OTHER than its own poll: running a task, blocked on a different
ring, or gone.  That distinction is usually the whole bug.

**And `unreaped == 0` does not mean the ring is empty.**  This kernel reports
`IORING_FEAT_NODROP`, so once the visible CQ fills, further completions go to a
kernel-side *overflow list* that `CqTail - CqHead` cannot see.  Measured
directly: with 4000 completions outstanding on a CQ of 128, the number read
**128**.  It saturates.  The `ovf` column (the `IORING_SQ_CQ_OVERFLOW` bit)
is printed beside it for exactly this reason -- `unreaped=0` with `ovf=1`
means completions ARE pending and simply invisible in that number.

For the record, a full CQ is *not* itself a lost-completion mechanism: a
bounded 16-per-pass drain recovered 4000 of 4000 with no duplicates, and
`io_uring_wait_cqe_timeout` with a backlog present returned in 0.0 ms rather
than blocking.  Overflow is a reason to distrust the counter, not a bug.

## Naming the owner of a stuck completion (xtc-cqes)

`xtc-rings` gives a COUNT, and a count can never say *whose* completion it is.
`xtc-cqes` walks the visible CQ from `CqHead` to `CqTail` and decodes each
entry's `user_data` the way `xtc_io_poll` does:

    (gdb) xtc-cqes
    loop 0x514350 id=0 ring_fd=5  CqHead=3046 CqTail=3051 unreaped=5
        [ 0] user_data=0x7ffff6df4c81 res=0    flags=0x0 aio 0x7ffff6df4c80  task=0x51ab90 op=3
        [ 1] user_data=0x7ffff6dc1c81 res=4096 flags=0x0 aio 0x7ffff6dc1c80  task=0x51db00 op=1

Low bit set means an `xtc_aio_t *` (the pointer with bit 0 masked off), whose
`->tag` is the `xtc_task_t *`; low bit clear means a `struct __xtc_uring_fd *`,
whose `->tag` is the parked task and `->fd` the descriptor; `user_data=0` is a
discarded `poll_remove` cancel CQE.  `op` is the `XTC_AIO_*` opcode -- `3` is
`XTC_AIO_FDATASYNC`.

That `task=0x...` is the SAME key `XTC_TAIL_PARK_TASK`, `XTC_TAIL_SUBMIT`,
`XTC_TAIL_REAP` and `XTC_TAIL_WAKE` carry, so a stranded fiber in an
`xtc_tail` trace joins straight to the completion sitting in the ring.  If a
strand's task pointer appears here, the kernel posted the completion, it is
visible in the CQ, and the drain did not take it -- a **drain-side** bug, not a
lost completion.  Like `xtc-rings`, this walks only the visible CQ, so read it
together with `ovf`.

Note that `alive` counts tasks HOMED on the loop, which is not the same as
tasks it can currently run -- a loop can show `alive=0` and still hold both
queued work and unreaped completions.

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

A long-lived idle receiver is indistinguishable from a lost wake by shape
alone: a fiber blocked in `xtc_recv(..., -1)` parks with no source and no
latched wake too.  Procs with `local_id == 0` (the per-loop service fiber)
are excluded from the suspect count for that reason.  Service fibers with a
NON-zero local_id will still be listed -- cross-check against what you know
parks forever by design.

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
