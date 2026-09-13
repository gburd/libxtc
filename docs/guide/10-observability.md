---
title: Observing a running application (dial9)
parent: Guide
nav_order: 10
permalink: /guide/10-observability/
lede: >-
  xtc_tail is a production microscope: record every scheduler event cheaply,
  ship a trace off the box, and open it in the dial9 GUI to see what the
  runtime was actually doing.
---

# Observing a running application

When an async service misbehaves in production -- a request that should take
microseconds takes milliseconds, a fiber that never wakes, a loop that stops
making progress -- aggregate metrics tell you *that* something is wrong but
rarely *what*. `xtc_tail` is the answer to "what was the runtime actually
doing?": it records every individual scheduler event (spawn, run, park, wake,
the io_uring submit/reap chain) into a bounded in-process ring, cheaply enough
to leave compiled in, and lets you analyze it after the fact.

It is modelled on [dial9](https://github.com/dial9-rs/dial9), a post-hoc
microscope for Tokio, and it **produces traces the dial9 GUI viewer opens
unchanged**. You get a battle-tested visual timeline for free; libxtc's job is
to emit the events in dial9's wire format.

Compared to [`xtc_stats`]({{ '/reference/api-index/' | relative_url }})
(aggregate counters, for dashboards) and `xtc_trace` (the causal message
trace), `xtc_tail` is the event-level tool: `xtc_stats` says the p99 poll time
regressed; `xtc_tail` shows you the exact fiber, on the exact worker, that sat
off-CPU and why.

## Turning it on

`xtc_tail` is **off by default** and costs a single predictable branch when
disabled, so it is safe to compile into a production binary. Turn it on in
code:

```c
xtc_tail_enable(XTC_TAIL_SCHED);   /* record scheduler events */
```

or, for a deployed binary you do not want to rebuild, from the environment at
startup:

```c
xtc_tail_from_env();   /* honors XTC_TAIL_ENABLE */
```

Then the operator arms it without a code change:

```sh
XTC_TAIL_ENABLE=1      # or "sched" for the SCHED source, "all" for every source
```

Unset, `0`, `off`, or `false` leave it disabled. `xtc_tail_from_env` returns
the mask it enabled (0 when off), so you can log whether tracing is live.

## Getting a trace off the box

The ring lives in memory. To analyze it you write it out in the dial9 format.
For a one-shot capture (a test, a repro, a shutdown hook):

```c
int fd = open("/tmp/traces/app.d9", O_WRONLY | O_CREAT | O_TRUNC, 0600);
xtc_tail_dump_dial9(fd);
close(fd);
```

For a deployed service, spill a **segment** into a directory that a sidecar or
`scp` ships to wherever you run the viewer:

```c
xtc_tail_spill_dial9("/var/lib/xtc-traces");
/* writes /var/lib/xtc-traces/xtc-tail-<pid>-<ns>.d9 */
```

`xtc_tail_spill_dial9` names each segment uniquely (pid + monotonic
nanoseconds), so successive spills never collide and sort by time. There is
**no background thread**: you decide when to spill -- at shutdown, on a signal,
on a health-check trigger, or from your own timer. A crash-surviving trace is a
spill from a fault handler; the call only writes and does not allocate beyond
the single ring snapshot.

Rotation and the on-disk byte budget are the deployment's job -- a directory
plus a `find -mtime +1 -delete` or a logrotate rule -- exactly as a dial9 disk
buffer is ultimately bounded by the deployment, not the library.

Putting it together:

{% include snippet.html file="11_observability.c" region="full" %}

## Opening the trace in the dial9 GUI

Install the dial9 viewer (a pre-built binary or `cargo install --locked dial9
--features cli`), point it at your trace directory, and browse:

```sh
dial9 serve --local-dir /var/lib/xtc-traces
# open http://localhost:3000
```

A libxtc trace shows up with a native, Tokio-shaped timeline because the events
carry dial9's built-in schema names:

| libxtc event | dial9 schema | what the GUI shows |
| --- | --- | --- |
| a proc began running | `PollStartEvent` | a poll span on the worker's timeline |
| a proc was spawned | `TaskSpawnEvent` | a task appears |
| a proc exited | `TaskTerminateEvent` | the task ends |
| a completion was dispatched | `WakeEventEvent` | wake causality (who woke whom) |

libxtc's own events -- the io_uring reap/submit chain (`XtcReapEvent`,
`XtcSubmitEvent`, `XtcSubmitFailEvent`, `XtcParkTaskEvent`, ...) -- have no
Tokio analogue, so they ride the same format as **custom events**: the viewer
shows them on the timeline pinned to the right worker and task, even though it
has no built-in rendering for them. Every trace also carries a `ClockSyncEvent`
(so the viewer recovers wall-clock time) and a `SegmentMetadataEvent` naming
the service and the active I/O backend.

## Headless / scripted analysis

If you are debugging without a GUI (a CI failure, an agent-driven
investigation), `tools/xtc-tail.py` reads the *native* XTCL dump
(`xtc_tail_dump`) and offers scripted views -- `--summary`, `--wake-latency`,
and `--strands`, which classifies every parked fiber by how far its wakeup got
(never submitted / no reap / reaped-but-not-dispatched / dispatched-but-not-run)
to localize a lost wakeup to one stage. See
[Debugging and observing]({{ '/guide/debugging/' | relative_url }}) for the
gdb/lldb helpers (`xtc-rings`, `xtc-cqes`, `xtc-stranded`) that read the same
event vocabulary from a live or core-dumped process.

## Cost

Recording is a branch when off and a bounded ring write when on (no allocation,
no clock read beyond the one timestamp each event already needs). The ring is
fixed-size and wraps; `xtc_tail_dropped()` reports how many records were
overwritten, and you should check it before trusting the *absence* of an event
-- in a wrapped ring, "this fiber has no events" may only mean they were
evicted. For a busy service, spill frequently and keep the capture window
short.
