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

## Statistics: what we include, and what we deliberately do not

Where `xtc_tail` records individual events, `xtc_stats` aggregates them:
counters, gauges, and histograms you register once and update on the hot path,
then read back for a dashboard or a Prometheus scrape. See
[`xtc_stats(3)`]({{ '/reference/api-index/' | relative_url }}) for the full
API.

This section is the honest counterpart to the API reference: it says what
`xtc_stats` is *not*, and why. The design was informed by two mature Java
libraries -- [Apache Commons Statistics](https://commons.apache.org/proper/commons-statistics/)
(a math library) and [Dropwizard Metrics](https://metrics.dropwizard.io/)
(a telemetry library) -- and several of their headline features were
considered and left out on purpose. Each omission below is tied to a concrete
libxtc constraint, not to "we ran out of time."

### What libxtc does provide, and why it fits a concurrent system

The shape of `xtc_stats` follows from one requirement: a highly concurrent,
multi-loop runtime must produce **one cohesive number** without turning the
hot path into a contention point. Every primitive is built on the same
**merge-on-read** model -- update a contention-free per-core (or per-shard)
partial on the hot path, and assemble the system-wide figure only when
something reads it:

- **Counters** are per-CPU sharded. `xtc_counter_inc` is one atomic add to a
  cache-line-isolated shard on the calling CPU; the read sums the shards.
- **Histograms** (`xtc_hist`) are per-CPU sharded fixed-bucket log-linear
  histograms. `xtc_hist_record` hits one shard; `xtc_hist_quantile` merges
  shards on read. The buckets are allocation-free *and* mergeable, which is
  the property that matters below.
- **Gauges** are a single `_Atomic int64_t`: wait-free reads, atomic-store
  writes, for a value that moves up and down (queue depth, live connections).
- **`xtc_dist`** adds mergeable online mean/variance: each shard keeps a
  Welford running moment on its hot path, and the read merges shards with the
  Chan parallel-merge formula for `(n, mean, M2)`. Like the histogram, the
  per-shard partials combine exactly into one system-wide mean and variance.

Merge-on-read is precisely what lets independent per-core and per-fiber
activity roll up into coherent system-wide numbers without the cores fighting
over a shared cache line. That property -- not a feature count -- is the
reason the following things are *out*.

### Deliberately NOT incorporated

**1. EWMA 1/5/15-minute rates (Dropwizard's `Meter`).** Not incorporated. A
Meter decays its rate on a periodic tick -- Dropwizard ticks every 5 seconds
and applies the UNIX load-average alphas -- which means it must read a real
wall clock on a timer. `xtc_stats` is pure value aggregation with **no time
source**: keeping a clock out of the stats path is what makes every operation
trivially correct and side-effect-free. A rate over a window is computed
better one layer out, in the consumer's scrape/monitoring system (for example
Prometheus `rate()` over the raw counter), which is also where cross-host
aggregation belongs. We expose the monotonic counter; the window is the
monitoring layer's job.

**2. Reservoir sampling for approximate quantiles (Dropwizard's
`UniformReservoir` / Vitter Algorithm R, `ExponentiallyDecayingReservoir` /
Cormode forward-decay, and the sliding-window reservoirs).** Not incorporated,
for two reasons. First, the exponentially-decaying reservoir needs a
concurrent sorted map plus a periodic rescale plus a clock -- allocation, a
lock, and a time source, all three of which collide with libxtc's
allocation-free hot paths and its no-clock rule. Second, and more decisively:
**none of these reservoirs are mergeable** across shards or instances. In a
multi-loop system you cannot combine a per-core reservoir into one number
without re-sampling, which defeats the purpose. libxtc's fixed-bucket
log-linear histogram is allocation-free *and* shard-mergeable, so its
approximate quantiles actually combine.

   The honest tradeoff: a fixed-bucket histogram is coarser than a reservoir
   on an arbitrary distribution -- its error is bounded by the bucket width,
   not by a sampling guarantee. If a consumer ever demonstrates that the
   bucket granularity is too coarse for a real decision, the answer is a
   *mergeable sketch* (t-digest, KLL, or Greenwald-Khanna), not a reservoir.
   That is a deliberate deferral, recorded below, not something shipped today.

**3. A `MetricRegistry` + reporter framework (Dropwizard's `ConsoleReporter`,
`JmxReporter`, `GraphiteReporter`, `CsvReporter`).** Not re-incorporated,
because libxtc already has the parts worth having: a registry you walk with
`xtc_metrics_iterate`, and a Prometheus text dump via
`xtc_metrics_dump_prometheus`. Additional transports -- Graphite, JMX, CSV, a
push gateway -- are a consumer integration choice, not a library concern. We
expose the data and the iterator; the consumer wires whatever transport its
deployment already speaks.

**4. Probability distributions, statistical inference, and regression (the
bulk of Apache Commons Statistics: roughly 35 distributions, t-tests, ANOVA,
confidence intervals, ranking, least-squares fitting).** Not incorporated:
out of scope. That is a numerical/math library. `xtc_stats` exists to observe
a running concurrent system, not to do science on the data in-process. A
consumer that needs inference exports the raw figures and runs the statistics
wherever it already does analysis.

**5. Mergeable streaming quantiles (t-digest / KLL / Greenwald-Khanna).** Not
incorporated *yet*, and flagged here as the one real future direction.
Neither surveyed library actually provides them -- Commons Statistics computes
percentiles by full-array quickselect (not streaming), and Dropwizard's
reservoirs are not mergeable -- so there was nothing to adopt wholesale, and a
proper sketch is a sizeable dependency to carry. The existing fixed-bucket
histogram is already shard-mergeable and adequate for latency dashboards. If,
and only if, bucket granularity is ever shown insufficient for a real
decision, a mergeable sketch is the direction we would take -- preserving the
merge-on-read property that everything else here depends on.

### The "Timer" pattern: compose, do not add a type

A Dropwizard `Timer` looks like a fourth metric kind, but it is just a `Meter`
(a rate) plus a `Histogram` (a latency distribution) behind one call site.
libxtc does not ship a `Timer` type because it does not need to: compose the
counter and the histogram you already have. Record both at the one site so the
rate and the distribution can never drift apart, and read them back together
for a cohesive view.

{% include snippet.html file="stats_timer_pattern.c" region="full" %}

The rate over a window is still the monitoring layer's `rate()` over
`demo.ops`; the library's job ends at exposing the raw count and the
mergeable distribution.
