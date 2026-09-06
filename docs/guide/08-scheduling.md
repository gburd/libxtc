---
title: Scheduling and CPU shares
parent: Guide
nav_order: 8
permalink: /guide/08-scheduling/
lede: >-
  Weight CPU between workloads with proportional-share scheduling, and
  catch a fiber that hogs a core -- both opt-in, both inspired by Glommio.
---
1. TOC
{:toc}

---

By default libxtc's run queue is a plain FIFO plus a work-stealing
deque: equally busy fibers get roughly equal CPU, and there is no way
to say "the flush workload should get 3x the query workload" or "this
class must be scheduled within 500us". This chapter covers the two
opt-in scheduler features that add exactly that -- **proportional-share
scheduling** and the **over-budget stall watchdog** -- both off by
default with zero overhead until you use them.

Both are directly **inspired by [Glommio](https://github.com/DataDog/glommio)**
(Glauber Costa / ScyllaDB), the thread-per-core io_uring runtime whose
executor gives each task queue *shares* and a *latency* class and picks
the next queue by a CFS-style virtual-runtime heap, and whose stall
detector reports which task monopolized a core. libxtc's north star is
the same multi-tenant / mixed-workload database use, so these were the
one capability worth adopting outright.

## The gap: FIFO splits CPU evenly

Put two groups of equally busy, well-behaved worker fibers on one loop
-- call them class A and class B, each doing equal compute chunks with a
cooperative `xtc_yield` between them. Under the plain FIFO run queue the
CPU split is about **1:1**, no matter what weighting you want. There is
no knob. That is the gap.

## Proportional share: weighting CPU between classes

Create a scheduling **class** with `xtc_exec_class_create`, give it
`shares` in 1..1000, and place a process's tasks in it with
`xtc_proc_opts_t.sched_class` at spawn (or `xtc_proc_set_class` from
inside the process). A loop that carries more than one class picks the
minimum-virtual-runtime class on each dispatch, so each class gets a CPU
fraction proportional to its shares:

{% include snippet.html file="10_sched_shares.c" region="full" %}

Class A has 3 shares and class B has 1, so A is picked more often. The
virtual-runtime accrual is Glommio's exact formula:
`vruntime += (cost_ns * reciprocal) >> 12`, where
`reciprocal = (1<<22)/shares`, so a higher-shares class accrues virtual
time more slowly and is chosen more often.

Be precise about what the snippet above prints, because the honest claim
is narrower than "3:1 shares gives 3:1 run counts" on real hardware:

- Accrual is weighted by **measured** run time, floored to a minimum
  quantum. For a yield-only worker the real elapsed time is dominated by
  scheduling jitter, so the observed run ratio is noisy -- it is not a
  stable 3:1.
- A shared work budget adds a counter-effect: the favoured class drains
  it faster, finishes sooner, and then stops running, after which only
  the slower class accumulates runs. A raw end-of-budget run count is
  therefore not a clean CPU-share signal.

Strict proportionality -- run ratio equal to share ratio, exactly and
reproducibly -- is proven under the deterministic simulator, where the
virtual clock makes every run cost exactly the floor:
`test/sim/test_sim_sched_shares.c`. That is where the numeric guarantee
lives. On real hardware, treat shares as a weighting that holds in
aggregate over a sustained contended workload, not as an exact ratio you
can assert over a few hundred dispatches.

Untagged work (any task with no class) races an implicit default lane in
the same pick, so background work is never starved by always-ready class
work -- it simply gets a default weight.

### Latency classes

Pass a non-zero `latency_ns` to `xtc_exec_class_create` and the loop's
cooperative yield interval shrinks to that bound (like Glommio's
`reevaluate_preempt_timer`), so a latency-sensitive class is serviced
promptly instead of waiting behind a throughput backlog. This is how you
say "compaction gets 20% of the CPU, but queries must be scheduled
within 1ms" on a single core.

### Zero overhead when unused

A loop with no class created runs the exact plain-FIFO + work-stealing
path, byte-for-byte; the virtual-runtime accounting activates only once
a class exists. You pay nothing until you opt in.

## The stall watchdog: who hogged the core?

libxtc is cooperatively scheduled -- a fiber that never yields
monopolizes its loop. The **over-budget stall watchdog** turns that from
a silent tail-latency mystery into an alertable signal: arm a per-loop
budget with `xtc_loop_set_stall_budget` (or every loop of an executor at
once with `xtc_exec_set_stall_budget`), and when a single run exceeds it
the runtime reports which task did it.

```c
xtc_loop_set_stall_budget(loop, 50 * 1000 * 1000LL);  /* 50 ms */
xtc_loop_set_stall_cb(loop, my_report_fn, ctx);       /* or NULL to log */
```

With no callback the runtime logs a warning and emits a backtrace of the
loop to stderr. `xtc_loop_stall_count` returns how many overruns fired.
The check is a cheap wall-clock comparison at the run-end boundary of
the loop step -- no watcher thread, no signal -- so it is a single
branch on a disabled flag when off. This is inspired by Glommio's stall
detector.

## When to reach for this

- Mixed workloads on one core (compaction vs queries, flush vs read
  path): give each a class and weight the shares.
- A latency SLA on one workload while another does bulk throughput: a
  latency class.
- Diagnosing "something occasionally pins a core for 200ms": arm the
  stall budget and let it name the culprit.

If your workload is a single kind of task, or you are happy with even
sharing, leave both off -- the default FIFO + work stealing is simpler
and faster.

## Demonstrated in

- [`examples/03_supervised_app.c`](https://codeberg.org/gregburd/libxtc/src/branch/main/examples/03_supervised_app.c)
  -- the minimal case: two supervised children share one loop as two
  classes (a latency-bounded foreground counter with 3x shares vs a
  best-effort background stats printer), plus the stall watchdog.
- [`examples/06_sqlxtc`](https://codeberg.org/gregburd/libxtc/src/branch/main/examples/06_sqlxtc)
  -- the real case: the SQL server enables the over-budget stall
  watchdog so a runaway query or structure-modification that hogs a
  worker loop is logged with a backtrace instead of silently stalling
  every other connection on that loop.

## See also

- [`xtc_exec(3)`]({{ '/man/xtc_exec/' | relative_url }}) -- the class and
  stall-watchdog API in full.
- [`xtc_proc(3)`]({{ '/man/xtc_proc/' | relative_url }}) --
  `xtc_proc_set_class` and `xtc_proc_opts_t.sched_class`.
