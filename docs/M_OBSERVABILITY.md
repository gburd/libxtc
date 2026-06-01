# Observability and debugging for libxtc: research and plan

libxtc gives C the BEAM's concurrency model.  This document asks the
follow-on question: how do we give C programmers the BEAM's
*observability* -- the reason Erlang systems are famously debuggable in
production -- using the tools C programmers already have (GDB, LLDB,
the MSVC debugger, and the IDEs that wrap them)?

It is in three parts: what the BEAM gives you, what maps to libxtc, and
the staged plan (with the first stage already shipped).

---

## Part 1 -- how BEAM programmers observe a running system

### Ships with Erlang/OTP

  * **`observer`** -- a GUI: a live, sortable table of every process
    (mailbox length, reductions, memory, current function), the
    supervision tree, ETS tables, application and system metrics, and
    a crash-dump viewer.  The thing newcomers remember.
  * **`observer_cli`** (now bundled-ish) / **`etop`** -- a terminal
    "top" for processes, ranked by message-queue length, memory, or
    reductions.  The first thing you run on a box that is misbehaving.
  * **`sys`** -- `sys:get_state/1`, `sys:get_status/1`,
    `sys:statistics/2`, `sys:trace/2`, `sys:replace_state/2`:
    inspect and even rewrite a live gen_server's state, turn on
    per-process tracing, get its message counts.
  * **`erlang:process_info/1,2`** -- for any pid: `message_queue_len`,
    `current_function`, `current_stacktrace`, `links`, `monitors`,
    `memory`, `status`, `dictionary`.  The programmatic core that
    observer renders.
  * **The trace BIFs** -- `erlang:trace/3` + `erlang:trace_pattern/3`
    with match specifications: trace sends, receives, calls, returns,
    process spawn/exit, GC, scheduling, on selected processes, live,
    with the VM doing the filtering.
  * **`seq_trace`** -- a trace *token* that rides along message sends,
    so you can reconstruct the causal chain of ONE request as it fans
    out across processes -- a built-in distributed trace.
  * **`erlang:system_monitor/2`** -- asynchronous alarms: long GC, large
    heap, **busy port**, **long message queue** -- the runtime tells
    *you* when a process is in trouble.
  * **`msacc`** (microstate accounting) -- where scheduler time goes
    (sleep, aux, gc, send, emulator).
  * **Crash dumps + `crashdump_viewer`** -- on a fatal error the VM
    writes a full snapshot (every process, its stack, mailbox, and
    state) that you load and browse offline.
  * **`dbg`** -- the low-level tracer; **`fprof`/`eprof`/`cprof`** --
    profilers; **`logger`** + SASL reports -- structured logs and
    crash reports.

### Built by the community (the production-safe layer)

  * **`recon`** (Fred Hebert) -- the canonical production toolkit:
    `recon:proc_count/2` (top processes by an attribute),
    `recon:proc_window/3` (who is busy *right now*), `recon_trace`
    (rate-limited tracing that cannot take the node down),
    `recon_alloc` (allocator fragmentation), `recon:bin_leak`
    (refc-binary leaks).  Its design rule -- "never let an
    introspection tool kill production" -- is the one to copy.
  * **`redbug`** -- safe, rate- and time-limited call tracing.
  * **`looking_glass`** / **`eflame`** -- flamegraph profilers from
    trace data.  **`xprof`** -- per-function live latency.
  * **`telemetry`** -- the de-facto metrics/event library everything
    emits to.

### The philosophy, distilled

  1. **Every process is inspectable, live, by identity.**  Given a
     pid you can always ask: what are you running, how deep is your
     mailbox, who are you linked to, what is your state.
  2. **Mailbox length is the vital sign.**  A growing message queue is
     the single most common pathology, and it is always visible.
  3. **Tracing is first-class, filtered, and safe.**  You turn it on
     against a live system, scoped to a few processes, rate-limited so
     it cannot become the outage.
  4. **The runtime reports its own distress** (system_monitor) rather
     than waiting to be asked.
  5. **A crash produces a full, browsable snapshot**, because "let it
     crash" only works if the crash is legible afterward.

---

## Part 2 -- mapping to libxtc

C has no managed VM, so we cannot get this for free.  But libxtc's
structure makes most of it reconstructable, and it already has the
raw materials:

  * **Identity + enumeration.**  Every `xtc_proc` lives in a per-loop
    slot table; the loops are registered in a small global.  A
    debugger script can walk them -- this is the `observer` process
    list.  (Shipped; see Part 3.)
  * **The vital sign is already recorded.**  `mbox_n` (live depth),
    `mbox_peak`, `mbox_saved`, `mbox_recv_total`, `mbox_drop_total`
    are fields on the proc -- `process_info(message_queue_len)` for
    free, plus a peak and a drop count BEAM does not even keep.
  * **State + scheduling.**  The task carries `state`
    (SCHEDULED/RUNNING/PARKED/DONE) and, when parked, *why*
    (`park_fd`, `park_timer`, `park_requested`) -- this is
    `process_info(status, current_function)`.
  * **Links and monitors** are lists on the proc -- `process_info`'s
    `links`/`monitors`.
  * **Names.**  libxtc has no name field on the proc; `xtc_reg`
    holds name->pid.  A debugger/observer cross-references the
    registry.  (Gap: consider a short name on the proc for zero-setup
    labelling -- tracked in `M_SQLXTC_XTC_GAPS.md` style.)
  * **Per-loop work stats.**  `n_tasks_run`, `n_steals`,
    `n_alive`, `n_yield_due` -- `msacc`/scheduler view.
  * **Existing programmatic surface.**  `xtc_stats` (counters +
    histograms, with `xtc_metrics_dump_prometheus`), `xtc_log`
    (structured logs), `xtc_reg` (name registry), `xtc_pdict` (process
    dictionary, like Erlang's), `xtc_alloc_audit` (per-proc live
    allocation attribution -- `recon_alloc` + `bin_leak`), and the
    cooperative-yield watchdog (`xtc_yield_*`, the "this fiber ran too
    long" signal -- a piece of `system_monitor`).

What is missing and worth building: a *live* enumeration API (not just
the debugger walk), an always-on *trace ring* (spawn/exit/send/recv/
park/resume/lock), a *causal trace token* in the envelope
(`seq_trace`), a *fault snapshot* (R1 already catches the fault -- dump
the proc tree + mailboxes + recent trace on the way down), and a
terminal *observer* built on all of it.

---

## Part 3 -- the plan (staged; stage 1 shipped)

### Stage 1 -- native-debugger introspection (SHIPPED)

`tools/gdb/xtc-gdb.py` (and the LLDB sibling) give, on a live process
or a core dump:

  * `xtc-loops`   -- loops + procs/alive/tasks_run/steals (scheduler view)
  * `xtc-procs`   -- every proc: pid, **mailbox depth**, peak, save-queue,
                     run state (+ park reason), link/monitor counts, DEAD flag
  * `xtc-proc A`  -- one proc in full (mailbox stats, watermark, recovery
                     frame, entry function)
  * `xtc-mailbox A` -- the queued envelopes (sender pid, size)
  * `xtc-self`    -- the proc running on the selected thread

This is `observer`'s process table and `process_info`, in GDB, working
today against the stock `-g` build.  It is the highest-leverage piece
because a debugger is already where a C programmer is when things go
wrong, and it needs ZERO changes to the program under inspection.

The MSVC / VS Code / CLion story: VS Code and CLion drive GDB or LLDB
underneath, so the same scripts load via `.gdbinit` / launch.json
`setupCommands`.  For native MSVC (cdb/WinDbg) the equivalent is a
NatVis file for the value pretty-printers plus a small JS/Python
extension for the enumeration commands -- stage 4.

### Stage 2 -- a live introspection API (SHIPPED)

A thread-safe `xtc_inspect_*` surface (`src/inc/xtc_inspect.h`,
`xtc_inspect(3)`) snapshots the same data the debugger walks, callable
from inside a running program -- the `process_info/2` + `observer`
analog without a debugger attached:

  * `xtc_inspect_procs(cb, user)` -- invoke `cb` once per live proc
    (across all loops); returns the count, or a negative `XTC_E_*`.
  * `xtc_inspect_loops(cb, user)` -- invoke `cb` once per loop with
    `{loop_id, n_procs, n_alive, tasks_run, steals}`; returns the loop
    count.
  * `xtc_proc_info(pid, xtc_proc_info_t *)` -- the `process_info/2`
    analog for one pid (`XTC_OK` / `XTC_E_NOTFOUND` / `XTC_E_INVAL`).

The per-proc struct carries `{pid, run_state, park_reason, alive,
kill_pending, mbox_len, mbox_peak, mbox_cap, mbox_saved,
mbox_recv_total, mbox_drop_total}` -- the mailbox depth (`mbox_len`) is
the vital sign, plus a peak and a drop count BEAM does not keep.  Each
call is a best-effort snapshot taken under the per-loop slot locks (the
proc set is consistent for the call, the mailbox counters are read
under the mailbox lock, the run state is sampled); the callbacks run
*after* every internal lock is released, so they may call back into the
proc/loop APIs.  Callbacks return 0 to continue or non-zero to stop
early.

Link/monitor topology is deliberately NOT in the live snapshot: a proc
mutates its own link/monitor lists without a lock, so a cross-thread
walk would race.  That topology stays a debugger-only view (stage 1,
which runs against a stopped program).

Built on the existing slot tables under their locks.  This is what an
admin command ("SHOW PROCESSES" in sqlxtc) or a metrics scraper calls,
and what the terminal observer (stage 5) renders.

### Stage 3 -- an always-on trace ring + causal token

A per-loop lock-free ring buffer of fixed-size trace records
(timestamp, causal stamp, kind, pid, peer, detail), with kinds
SPAWN/EXIT/SEND/RECV/PARK/RESUME/LINK/MONITOR/LOCK.  Always on (a ring
write is a few stores; cost is bounded and constant), dumpable via the
debugger (`xtc-trace`) and the inspection API.  A **causal token**
(the HLC/ITC stamp from `M_CAUSALITY.md`) rides in the message
envelope so the trace reconstructs happens-before across procs and
cores -- libxtc's `seq_trace`.  Filtering + rate limits follow
`recon`'s rule: tracing must never be the outage.

### Stage 4 -- fault snapshot + MSVC/IDE polish

On a contained fault (R1) or a fatal abort, write a crash snapshot:
the proc tree, every mailbox depth, the recent trace ring, the
faulting fiber's backtrace -- the `crashdump_viewer` analog, browsable
offline.  Ship a NatVis + extension for native MSVC/WinDbg, and
documented launch.json / CLion settings that auto-load the scripts.

### Stage 5 -- xtc-top (terminal observer)

A `recon`/`observer_cli`-style live terminal view on the stage-2 API:
procs ranked by mailbox depth (the vital sign), loops by utilization,
lock waits, with the same "cannot take the system down" discipline.

### Stage 6 -- system monitor

`xtc_system_monitor(opts)`: asynchronous alarms for a mailbox past a
threshold, a fiber over its yield budget (already detected), a loop
starved -- the runtime reporting its own distress, like
`erlang:system_monitor`.

---

## Why this order

Stage 1 needs no library change and serves the programmer exactly when
they are stuck, so it ships first.  Stage 2 turns the same data into a
programmatic surface for ops.  Stage 3 (tracing) is where the deepest
bugs in a message-passing system hide -- "who sent this, and what
caused that" -- and it is also where the causality work pays a second
dividend.  Stages 4-6 are polish and production hardening.

The guides that accompany this plan:
`docs/guide/debugging.md` (how to use the tools to find real bugs in
this model) and `docs/guide/transitioning.md` (how to think in libxtc
and the anti-patterns that bite C/C++/Rust programmers).
