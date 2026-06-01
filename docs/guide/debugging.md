# Debugging and observing libxtc programs

This guide is task-oriented: a libxtc program is misbehaving, and you
want to find out why.  It pairs the native-debugger tooling
(`tools/gdb/xtc-gdb.py`, `tools/lldb/xtc_lldb.py`) with the
programmatic introspection libxtc already exposes (`xtc_stats`,
`xtc_reg`, `xtc_pdict`, `xtc_alloc_audit`, the yield watchdog).  Read
`docs/guide/transitioning.md` first if you have not -- most "libxtc
bugs" are one of the anti-patterns there, and knowing them tells you
what to look for.

## Loading the tools

### GDB (and VS Code / CLion, which drive GDB)

    (gdb) source /path/to/libxtc/tools/gdb/xtc-gdb.py

or once, in `~/.gdbinit`:

    source /path/to/libxtc/tools/gdb/xtc-gdb.py

In **VS Code** add to the C/C++ launch configuration:

    "setupCommands": [
      { "text": "source ${workspaceFolder}/tools/gdb/xtc-gdb.py" }
    ]

In **CLion**: Settings -> Build, Execution, Deployment -> Debugger ->
GDB, add the `source` line to the startup commands, or put it in
`~/.gdbinit`.

### LLDB (and VS Code / CLion on macOS)

    (lldb) command script import /path/to/libxtc/tools/lldb/xtc_lldb.py

Build with `-g` (the default build does).  The tools work on a live
process (run/attach/breakpoint) and on a core dump.

## The five-minute triage

When you do not yet know what is wrong, attach (or load the core) and:

    (gdb) xtc-loops      # how many loops, how busy, how many procs
    (gdb) xtc-procs      # every proc: mailbox depth, state, links

`xtc-procs` is the single most useful command.  Its columns are the
vital signs:

  * **mbox** -- live mailbox depth.  A large or growing number is the
    most common pathology (see "stuck or growing mailbox" below).
  * **peak** -- the high-water mark; a peak far above the current depth
    means a burst the proc fell behind on.
  * **state** -- SCHEDULED / RUNNING / PARKED(reason) / DONE.  The park
    reason (fd, timer, mailbox) tells you what the proc is waiting for.
  * **lnk/mon** -- links and monitors, for tracing supervision.

## Recipe: the whole program hangs

Symptom: nothing progresses; CPU may be idle (everyone parked) or one
core pinned (a proc not yielding).

    (gdb) thread apply all bt        # what is each OS thread doing
    (gdb) xtc-procs                  # what is each proc doing

Then reason:

  * **One proc RUNNING, everyone else SCHEDULED and not advancing, one
    core hot.**  A CPU-bound proc is not yielding (Shift 1).  The
    `bt` of the running thread shows where it is spinning.
  * **A thread stuck in a blocking syscall** (`read`, `fsync`,
    `pthread_mutex_lock`, `nanosleep`) in `thread apply all bt`, while
    procs sit PARKED.  Something blocked the loop thread (Shift 2).
    The backtrace names the offending call; it should have been
    `xtc_blocking_run` / `xtc_proc_sleep` / an `xtc_amutex`.
  * **Everyone PARKED(mailbox), no timers pending, no progress.**  A
    message that should have been sent never was, or two procs are each
    waiting for the other (a request/reply cycle).  `xtc-mailbox` on
    the suspects shows who is waiting for what.

Map a thread to the proc it is running: select the thread and

    (gdb) thread 3
    (gdb) xtc-self

## Recipe: a stuck or growing mailbox (the classic)

Symptom: memory climbs; one proc lags; eventually the OOM killer.

    (gdb) xtc-procs        # find the proc with the large/growing mbox

A proc whose **mbox** is large and whose **peak** keeps rising is a
consumer that cannot keep up with its producers (Shift 4).  Confirm the
senders:

    (gdb) xtc-mailbox 0x55...   # the from= column identifies producers

Fixes are architectural: bound the mailbox (`mailbox_cap`), convert
fire-and-forget `xtc_send` to a request/reply `xtc_svr_call` (which
rate-limits the producer to the consumer's pace), or shed load at a
watermark.  In production, the same depth is visible without a debugger
via the inspection API / `xtc_stats` counters.

## Recipe: a leak

libxtc can attribute live allocations to the proc that made them.  In
the program, early:

    xtc_alloc_audit_enable(1);

then at any point (or from a test):

    size_t live_bytes, live_count;
    xtc_alloc_audit_proc_leaks(suspect_pid, &live_bytes, &live_count);

A proc that has exited but whose `live_count` is nonzero leaked.  This
is the `recon_alloc` / `bin_leak` analog.  Globally,
`xtc_alloc_audit_stats(&bytes, &count)` is the total live footprint.

## Recipe: a fiber that ran too long (latency spikes, p99)

Turn on the cooperative-yield watchdog:

    xtc_yield_set_budget(loop, 5LL * 1000 * 1000);   /* 5ms quantum */

Then `xtc_yield_due_count(loop)` counts how often a fiber blew its
budget, and `xtc_yield_check()` is the in-fiber "am I over budget?"
query a long computation can poll to decide to yield or to honor a
cancellation.  A high due-count points at a proc that needs to yield
more or offload (Shift 1).

## Recipe: a contained fault (R1)

If a proc took a SIGSEGV/SIGBUS and was contained, its recovery frame
records it:

    (gdb) xtc-proc 0x55...
      recovery     : armed=1 fired=1 crit_depth=0

`fired=1` means the fault handler ran and the proc was unwound to its
recovery point (or aborted if `crit_depth > 0`, preserving
critical-section semantics).  The faulting backtrace is on the thread
at fault time; in a core dump from the abort path it is the top frame.

## Recipe: who holds / waits on a lock

For lock-order bugs, libxtc has an opt-in WITNESS-style tracker on
`xtc_lwlock`:

    xtc_lwlock_track_enable(1);
    /* ... run the workload ... */
    int v = xtc_lwlock_track_violations();   /* lock-order inversions seen */

It records the acquire-order graph keyed by tranche and reports
inversions, the classic source of deadlocks when you must hold more
than one lock.  Prefer, of course, not holding two (Shift 3).

## Naming procs in the output

libxtc does not store a name on the proc, so `xtc-procs` shows pids.
If you register procs by name (`xtc_reg_register(reg, "wal", pid)`),
cross-reference with `xtc_reg_whereis`.  A future revision may add a
short name field to the proc for zero-setup labels.

## Programmatic observability (no debugger)

For dashboards and always-on monitoring rather than a stuck-process
post-mortem:

  * `xtc_stats` -- counters and latency histograms; dump them in
    Prometheus text form with `xtc_metrics_dump_prometheus(fd)`.
  * `xtc_exec_loop_stats(exec, i, &st)` -- per-loop tasks_run / steals
    (scheduler utilization).
  * `xtc_reg` -- a name registry to enumerate well-known procs.
  * `xtc_pdict` -- a per-proc dictionary for ad-hoc state you want to
    read back.

The roadmap to a live `xtc-top` and an always-on trace ring (libxtc's
`seq_trace`) is in `docs/M_OBSERVABILITY.md`.

## When you file a libxtc bug

Include the output of `xtc-loops`, `xtc-procs`, and `thread apply all
bt`, plus whether the program holds any lock across a `recv`/await and
whether any proc does CPU work without yielding.  Those three artifacts
resolve most reports immediately -- usually to one of the anti-patterns
in `docs/guide/transitioning.md`.
