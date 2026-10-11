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

## Optional lane policies and continuous batching

Configure with `--enable-sched-policy` (Meson: `-Dsched_policy=true`) to
get two more interfaces. A default build still exports every function;
each returns `XTC_E_NOSYS`, and the scheduler hot path is unchanged. An
enabled build with no policy installed keeps the ordinary picker, queue
order, and weighted accounting.

- `xtc_sched.h` -- a lane policy that chooses which ready lane runs next.
- `xtc_batch.h` -- a batch engine that calls one step function per
  iteration with the vector of ready requests, and refills that vector
  between iterations.

The two are independent. Batching works under the ordinary picker, and a
policy works without any batch group.

### Runtime-owned queues

A lane is a queue the loop already has: lane -1 is the default
FIFO/deque, lanes 0..15 are the class queues created by
`xtc_exec_class_create`. A policy's `select` callback receives an
immutable snapshot of the ready lanes, ordered by lane ID, with each
lane's shares, runs, bypass count, and advisory batch counts. It returns
a lane ID, or `XTC_SCHED_DEFAULT` to use the ordinary picker. The
snapshot holds no task or queue pointers. The runtime still removes the
task, dispatches it, owns its lifetime, and handles wakes and stealing.
An invalid choice falls back to the ordinary picker and is counted by
`xtc_sched_get_stats`.

Installation is setup-only. `xtc_sched_policy_set` and
`xtc_sched_continuous_set` must run before the loop's first run or step,
even an empty one; afterwards they return `XTC_E_AGAIN`. Callbacks must
be bounded, nonblocking, allocation-free, and deterministic under
simulation.

### Bounded bypass

Every policy carries a positive `max_bypass`. For each ready lane the
runtime counts consecutive selection opportunities in which that lane was
not served. When a continuously ready lane reaches `max_bypass`, the
runtime overrides the policy and serves the oldest such lane (ties go to
the lower lane ID). With L ready lanes, every ready lane is served within
`max_bypass + L` opportunities, provided callbacks and dispatched tasks
return.

The bound counts opportunities, not time. A task that runs for 50 ms
without yielding stretches it by 50 ms. It is not preemption, and it is
not per-task fairness inside the default LIFO deque. A custom policy may
skew weighted shares; it does not promise exact proportions.

### The continuous policy

`xtc_sched_continuous_set(loop, max_bypass)` installs a built-in policy
with loop-owned state and no allocation. Among registered batch classes
that are ready and report ready members, it prefers the greatest bypass
age and rotates equal ages by class ID. With no such class it returns
`XTC_SCHED_DEFAULT`. A group whose members are all BLOCKED gets no
preference. The policy picks a class, not a group member, and the
bypass bound still covers ordinary tasks and the default lane.

Each explicit class holds at most one batch group; ordinary tasks may
share the class. A group created with a NULL class works but gets no
preference from this policy.

### Batch engine lifecycle

A group owns a pinned driver process on its loop. Requests are borrowed
pointers with a fixed resource charge. Admission to the active set is
first-come, first-served against `resource_capacity`; a full or closed
group returns `XTC_E_AGAIN` and the caller keeps the request.

The step callback receives the whole ready vector and writes one result
per member: `CONTINUE`, `DONE`, `BLOCKED`, or `ERROR`. The complete
callback runs exactly once per accepted request:

{% include snippet.html file="15_continuous_batching.c" region="step" %}

Create the group and submit work before the loop's first run. The
continuous policy is optional; the snippet installs it to show the call:

{% include snippet.html file="15_continuous_batching.c" region="create" %}

Shutdown has a fixed order:

{% include snippet.html file="15_continuous_batching.c" region="lifecycle" %}

The four requests need 5, 1, 2, and 1 rounds, with at most two active.
Each finished member frees a slot that the next waiting request takes at
the following iteration boundary, so the program runs 9 member-rounds in
5 iterations and all three short requests finish before the long one. A
static batch of two would hold requests 2 and 3 until request 0
finished. In a default build the snippet prints a note and exits 0.

The rules the snippet follows:

- **Create pre-run.** `xtc_batch_create` after the first run or step
  returns `XTC_E_AGAIN`.
- **Owner-only mutation.** After the loop starts, submit, cancel,
  resume, close, and destroy are valid only on the owner loop; another
  loop gets `XTC_E_INVAL`. Route cross-loop work to an owner process with
  messaging.
- **Callbacks cannot mutate.** While a step or complete callback is in
  flight, every mutator on that group returns `XTC_E_DEADLK`, including
  destroy and including peer processes that run while the callback is
  suspended. The snippet's step callback checks this. Return a result
  state instead, and mutate from an owner process after the callback
  returns. Statistics stay readable.
- **Close, run, destroy, fini.** `XTC_BATCH_DRAIN` waits for accepted
  work (BLOCKED members need resume or cancel); `XTC_BATCH_CANCEL`
  cancels it. The driver exits only when the loop runs, even for an
  empty group closed before the first run. `xtc_loop_stop` is not a
  drain: destroy returns `XTC_E_AGAIN` while the driver exists. Destroy
  the group before `xtc_loop_fini`.

### Alternatives not chosen

- **Policy-owned queues.** A vtable that owns enqueue, dequeue,
  stealing, and placement would duplicate task lifecycle and wake
  handling, and widen what has to be proven. The policy only chooses
  among lanes the runtime owns.
- **Orca's GPU results.** The engine adopts the iteration-level
  admission idea from Orca (OSDI '22). It runs CPU fibers, makes no claim
  to Orca's GPU throughput, and its lane rotation is not Orca's
  scheduler.
- **Timely dataflow.** There are no timestamps, frontiers, progress
  tracking, or differential updates. A group is bounded admission plus a
  vector step. Build pipelines from processes and channels with explicit
  backpressure.
- **Wall-time fairness.** A time-based bound would put clock reads in
  the picker and would not replay under deterministic simulation.

### When not to use it

- The requests share no per-iteration work, so a vector call saves
  nothing. One process per request is simpler.
- Requests are equal length and arrive together. Submitting a cohort and
  draining it gives the same result without refill.
- You need a latency bound in time. Use a latency class and the stall
  watchdog; the bypass bound counts opportunities.
- Many threads submit directly. The engine requires an owner process to
  receive that work.

Leaving the feature off costs nothing. The
[continuous batching example]({{ '/examples/continuous-batching/' | relative_url }})
runs two groups, a heartbeat, cancellation, capacity retry, and
BLOCKED/resume, and compares static and continuous admission.

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
- [`xtc_sched(3)`]({{ '/reference/api/xtc_sched/' | relative_url }}) --
  lane policies, the bypass bound, and the continuous policy.
- [`xtc_batch(3)`]({{ '/reference/api/xtc_batch/' | relative_url }}) --
  the batch engine, callbacks, cancellation, and shutdown.
