---
title: Continuous batching
parent: Examples
nav_order: 12
permalink: /examples/continuous-batching/
lede: >-
  Refill a numerical batch between iterations without holding short requests behind the longest member.
---

The [complete example source](https://codeberg.org/gregburd/libxtc/src/branch/main/examples/12_continuous_batching/main.c)
runs two numerical batch groups and an unrelated heartbeat process on one
loop. It uses only public APIs from `xtc_batch.h`, `xtc_sched.h`, and the
process/loop interfaces. Each group has a distinct scheduling class.

## One workload, two admission rules

Each request carries eight unsigned 64-bit components. In each vector
step, the callback prepares eight coefficients once and uses them to
apply an affine recurrence to every ready request. Requests need different
numbers of rounds: 64 for the original long member, 4 for the original
short member, and 2 for a late short member. The example checks the
result against an independent scalar calculation, including defined
unsigned wraparound.

With `--mode=static`, the application holds the late request until the
original cohort finishes. With `--mode=continuous`, it submits the late
request between iterations, as soon as the original short member has
finished. Both modes use the same callback and perform 1120 component
updates across the two groups. The late short member finishes before the
original long member only in continuous mode.

The batch engine owns membership and invokes the callback with a vector
of ready requests. The application owns numerical state and decides when
to submit a new request. Static mode needs no second engine: withholding
new submissions until the cohort drains supplies the static baseline.

## Scheduling is a separate choice

`--policy=default` uses ordinary scheduling; `--policy=continuous` installs
`xtc_sched_continuous_set` before running the loop, with `max_bypass=3`.
The built-in policy prefers ready batch lanes while bounding bypass of
other runnable lanes. It does not change the arithmetic or admission
rule. Continuous refill works with either policy, and the self-test runs
all four mode/policy combinations.

The default-lane heartbeat yields while the two groups work. No part of
the example sleeps or uses elapsed time to manufacture an advantage.
Completion order demonstrates the admission trade-off; it is not evidence
of higher throughput or a promise about wall-clock latency.

## Backpressure and lifecycle

Each group allows three live requests, two active members, and two resource
units. A full-capacity submit returns `XTC_E_AGAIN`; the caller retains the
request and retries later. A queued request is cancelled before doing
arithmetic. After two rounds, the long request returns `XTC_BATCH_BLOCKED`
to represent an unavailable dependency. It retains its reservation but
leaves the ready vector until the owner process calls `xtc_batch_resume`.

Callbacks only compute or record results. The owner process performs
submission, resume, and close operations outside callbacks. The example
uses no cross-thread batch calls. It creates groups before the first loop
run and keeps borrowed request storage alive until after completion.

For teardown, the owner closes with `XTC_BATCH_DRAIN` and lets the loop
finish naturally. After the drivers exit, the application reads final
statistics, destroys the groups, and finalizes the loop. `xtc_loop_stop`
would not provide that drain guarantee.

## Build and check

Configure libxtc with `--enable-sched-policy` and build `libxtc.a`. In
`examples/12_continuous_batching`, run
`make XTC_BUILD=/absolute/path/to/configured-build test`.
See the [README](https://codeberg.org/gregburd/libxtc/src/branch/main/examples/12_continuous_batching/README.md)
and [Makefile](https://codeberg.org/gregburd/libxtc/src/branch/main/examples/12_continuous_batching/Makefile)
for the build contract and command-line options.

`--self-test` checks exact scalar results, one terminal callback per accepted
request, cancellation, capacity retry, BLOCKED/resume, heartbeat progress,
released resources, and per-group completion order. Success ends with
`continuous batching self-test: PASS (4 mode/policy combinations)`.
These checks remain enabled with `NDEBUG` and use no timing thresholds.
Feature-enabled builds run the self-test through root `make check` (or
`make check-continuous-batching`). The scheduler-policy CI job also runs it;
feature-disabled builds explicitly skip this example.
