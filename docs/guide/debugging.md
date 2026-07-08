---
title: Debugging and observing
parent: Guide
nav_order: 8
lede: >-
  Finding bugs in the message-passing model with GDB/LLDB and the runtime's own introspection.
permalink: /guide/debugging/
---
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

## The commands, with example output

Every command works on a live process (run / attach / breakpoint) and on
a core dump; run them while stopped so the registry symbols resolve.
This is what each prints (addresses will differ; the shape is the
point). The LLDB commands are identical.

### `xtc-loops` -- the scheduler at a glance

    (gdb) xtc-loops
    loop 0x555555b8e2a0  [running]  procs=3  alive=3  tasks_run=1408  steals=12
    loop 0x555555b9f100  [idle]     procs=1  alive=1  tasks_run=530   steals=9

One line per loop (one per core under the executor). `tasks_run` is how
much work each loop has done; a large `steals` skew means one loop is
feeding the others (uneven placement).

### `xtc-procs` -- the observer process table

    (gdb) xtc-procs
    proc               pid        mbox  peak  save state              lnk/mon
    0x555555b8f420     <0.1.1>       0     4     0 parked(fd)          0/1
    0x555555b8fae0     <0.2.1>    2043  2043     0 parked(mailbox)     1/0
    0x555555b90200     <0.3.1>       0     1     0 running             0/0
    (3 procs)

The columns are the vital signs. Here `<0.2.1>` has a mailbox of 2043
with a matching peak -- a consumer that fell behind its producers. Pass a
loop address to scope the listing: `xtc-procs 0x555555b9f100`.

### `xtc-proc ADDR` -- one process in detail

    (gdb) xtc-proc 0x555555b8fae0
    proc 0x555555b8fae0  pid=<0.2.1>  loop=0x555555b8e2a0
      state        : parked(mailbox)
      alive        : 1   kill_pending=0
      mailbox      : depth=2043 peak=2043 cap=4096 saved=0 recv_total=118 drop_total=0
      wm           : lvl=0 fired=0
      links        : 1   monitors=0  monitored_by=0
      recovery     : armed=1 fired=0 crit_depth=0
      entry fn     : 0x555555559abc <consumer_body>

`recv_total` against `depth` tells the story: 118 received, 2043 still
waiting -- the producer is far ahead. `entry fn` names the body, so you
know which proc this is without a name registry.

### `xtc-mailbox ADDR` -- who is queued, and from whom

    (gdb) xtc-mailbox 0x555555b8fae0
      [  0] env 0x7ffff0001200  from=<0.4.1>  size=64
      [  1] env 0x7ffff0001260  from=<0.4.1>  size=64
      [  2] env 0x7ffff00012c0  from=<0.5.1>  size=64
      ...
      2043 message(s); save-queue=0

The `from=` column identifies the producers -- here two of them
(`<0.4.1>` and `<0.5.1>`) are flooding one consumer.

### `xtc-self` -- map a thread back to its proc

    (gdb) thread 3
    [Switching to thread 3 (LWP 12807)]
    (gdb) xtc-self
    proc 0x555555b90200  pid=<0.3.1>  loop=0x555555b8e2a0
      state        : running
      ...

Use it after `thread apply all bt` shows a thread you want to tie back
to a libxtc process.

### `xtc-trace` -- the causal message trace

Covered in full in the causal-trace recipe below; it prints the
send/receive ring in happens-before (HLC) order so you can follow which
message caused which.

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

## Recipe: tracing what caused what (causal message trace)

Symptom: a request misbehaves and the cause is spread across several
procs -- you can see the symptom in one mailbox but not which message,
from which proc, set it off.  This is the hardest class of bug in a
message-passing system: "who sent this, and what caused that."

libxtc's `seq_trace` answers it.  It is an opt-in, bounded ring of
send/receive events, each stamped with a hybrid logical clock (HLC) so
the causal order survives even when the wall-clock order the records
arrived in lies.  It is OFF by default and costs a single relaxed
atomic load on the message hot path when disabled, so you turn it on
for a specific investigation and off again -- it is not an always-on
tax.

From inside the program, enable it, run the suspect workload, and dump
the records in causal order (`xtc_trace(3)`):

    int was_on = xtc_trace_enable(1);   /* opt in */
    xtc_trace_reset();                  /* start from a clean ring */
    run_suspect_workload();

    static int on_rec(const xtc_trace_rec_t *r, void *u) {
        (void)u;
        printf("HLC=%llu kind=%d self=%llu peer=%llu cause=%llu\n",
               (unsigned long long)r->hlc, r->kind,
               (unsigned long long)r->self,
               (unsigned long long)r->peer,
               (unsigned long long)r->cause);
        return 0;                       /* 0 = keep going */
    }
    xtc_trace_dump(on_rec, NULL);       /* visits in HLC-ascending order */
    if (!was_on)
        xtc_trace_enable(0);            /* stop paying for it */

From a stopped program or a core dump, the same ring is in the
debugger:

    (gdb) xtc-trace
    HLC now: 1700000042
    hlc          kind  self         peer         cause/detail
    HLC100       SEND  self=0x..a   peer=0x..b               detail=64
    HLC205       RECV  self=0x..b   peer=0x..a   cause=100    detail=64
    HLC206       SEND  self=0x..b   peer=0x..c               detail=16
    HLC310       RECV  self=0x..c   peer=0x..b   cause=206    detail=16
    (4 events)

Read it by following the `cause` edges.  A RECV's `cause` is the HLC of
the SEND that produced the message it received, so a RECV with
`cause=100` is the delivery of the SEND stamped `HLC100`.  Chain them:
proc A sends (HLC100), proc B receives it (cause=100) and in turn sends
to C (HLC206), C receives that (cause=206).  That chain is the path of
one request as it fanned out across A, B, and C -- the trail you could
not see in any single mailbox.

The HLC order is the truth.  A single global clock ticks once per
traced event (its high 48 bits are microseconds, its low 16 a logical
counter), so a send's stamp is always strictly less than the stamp of
the receive it causes.  When two cores disagree about which message
landed first -- because wall-clock arrival across threads is not a
reliable order -- sort by HLC and the happens-before is restored.
Both `xtc_trace_dump` and `xtc-trace` already present the records in
HLC-ascending order for exactly this reason.

The ring is bounded and overwrites its oldest records when full, so if
the interesting events scroll off, narrow the window: `xtc_trace_reset`
immediately before the action, and dump immediately after.

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
  * `xtc_inspect` -- `xtc_inspect_procs(cb, user)` and
    `xtc_proc_info(pid, &info)`: the in-process form of the debugger's
    `xtc-procs`, snapshotting every live proc (or one by pid) without
    a debugger attached.
  * `xtc_exec_loop_stats(exec, i, &st)` -- per-loop tasks_run / steals
    (scheduler utilization).
  * `xtc_reg` -- a name registry to enumerate well-known procs.
  * `xtc_pdict` -- a per-proc dictionary for ad-hoc state you want to
    read back.

`xtc_inspect_procs` is the data behind a "SHOW PROCESSES" admin
command: it walks the same per-loop slot tables `xtc-procs` does, takes
a best-effort snapshot under their locks, then runs your callback after
the locks are released (so the callback may call back into the proc and
loop APIs).  Mailbox depth (`info.mbox_len`) is the vital sign, exactly
as in the debugger view:

    static int show(const xtc_proc_info_t *p, void *u) {
        (void)u;
        printf("pid=%llu mbox=%zu peak=%zu state=%d\n",
               (unsigned long long)p->pid,
               p->mbox_len, p->mbox_peak, p->run_state);
        return 0;            /* 0 = keep going, nonzero = stop */
    }
    xtc_inspect_procs(show, NULL);

Link/monitor topology is deliberately absent from the live snapshot (a
proc mutates those lists lock-free, so a cross-thread walk would race);
for that you still need the debugger's `xtc-proc`, which runs against a
stopped program.  See `xtc_inspect(3)`.

The causal message trace (libxtc's `seq_trace`) is shipped and opt-in;
see the recipe above and `xtc_trace(3)`.

## In-process dumps without a debugger (xtc_dump / XTC_PANIC)

When you cannot attach a debugger or collect a core -- a container with
`core_pattern` disabled, a CI runner, a customer site -- the same proc,
loop, and mailbox view is available from inside the program:

    #include "xtc_dump.h"

    xtc_dump(STDERR_FILENO);   /* loops, procs, mailbox depths, backtrace */

The output uses the same field names as `xtc-procs`/`xtc-loops`, so a
dump and a `gdb` session read alike:

    === xtc runtime dump ===
    thread backtrace (4 frames):
    ...
    loops:
      loop 0  procs=2 alive=2 tasks_run=3 steals=0
    procs:
      <0.0.1> parked    park=timer   mbox=2/4096 peak=2 recv=2 drop=0
      <0.1.1> running   park=-       mbox=0/4096 peak=0 recv=0 drop=0
    === end dump ===

The C backtrace is symbolized on glibc, macOS, and the BSDs (execinfo)
and on musl via the compiler's builtin `_Unwind_Backtrace` (frames
symbolized best-effort via `dladdr`), which needs no third-party
library.  The optional libunwind backend (`--with-libunwind=yes`) is a
fallback for the rare target whose toolchain ships neither execinfo nor
a working `_Unwind_Backtrace`.  Where none is available the frames are
addresses-only, and on a platform with no backend at all the
dump prints `thread backtrace: unavailable (no backtrace backend; attach
a debugger to a core)` and still reports the full proc/loop/mailbox
state.  The Windows DbgHelp backend is compiled and reviewed but has not
yet been runtime-verified on a Windows host.

To abort with that dump on a failed invariant, use the panic/assert
macros (always compiled in -- they are diagnostics, not debug-only):

    XTC_ASSERT(frame->pin > 0);
    XTC_ASSERT_F(n <= cap, "overflow: n=%zu cap=%zu", n, cap);
    XTC_PANIC("unreachable: state=%d", st);

To dump automatically on a fault, install the crash handler at startup:

    xtc_crash_handler_install();   /* SIGSEGV/SIGBUS/SIGABRT -> dump -> core */

The dump reports each proc's *state* and *mailbox*, not the C stack of
every parked fiber (those live in saved coroutine contexts, not on a
live OS thread -- only the faulting thread's C stack is unwound).  This
makes it strongest for the hang/wedge class above ("which proc is
waiting on whom"); for deep per-fiber stacks attach a debugger to the
core.  See `xtc_dump(3)`.

## When you file a libxtc bug

Include the output of `xtc-loops`, `xtc-procs`, and `thread apply all
bt`, plus whether the program holds any lock across a `recv`/await and
whether any proc does CPU work without yielding.  Those three artifacts
resolve most reports immediately -- usually to one of the anti-patterns
in `docs/guide/transitioning.md`.
