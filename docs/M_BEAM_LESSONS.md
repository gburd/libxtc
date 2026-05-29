---
title: "BEAM / OTP recurring production issues -- and how libxtc relates"
---

# BEAM / OTP recurring production issues -- and how libxtc relates

xtc draws heavily from the BEAM (Erlang/Elixir) runtime and the OTP
behaviour patterns built on top of it.  The approach is sound; we
inherit twenty-five years of distributed systems lessons.  We also
risk inheriting the BEAM's actual production pain points if we do
not consciously avoid them.  This document catalogs the recurring
issues operators report when running BEAM systems at scale, then
asks which of them are baked into libxtc, which are not, and what
mitigations we can offer either way.

## Sources

The list below is composed from operator post-mortems, conference
talks, GitHub issues against `otp/erlang`, the `Erlang in
Anger` book, the Elixir community's `Discord` (the company)
blog series, the `WhatsApp at scale` writeups, and the Heroku /
Discord / Bleacher Report / Pinterest engineering blogs.  No single
issue is a deal-breaker for the BEAM; together they paint the
picture of "what stings in production after year two."

## The recurring issues

### 1.  Mailbox overflow / message build-up

**Symptom.**  A process is slow to handle messages (perhaps because
it does I/O inline, or it crashed and the supervisor takes a moment
to restart it, or another process is sending it work faster than it
drains).  The mailbox grows.  Memory grows.  Eventually the BEAM
either OOMs the host, or the affected process becomes so slow that
its supervisor's heartbeat times out and the whole supervisor tree
is restarted.

**BEAM root cause.**  Mailboxes are unbounded by default.  The
language has no first-class "this mailbox is full, don't send"
mechanism.  `Process.message_queue_len` is observable but reactive,
not preventative.  Folklore solutions: `:gen_server.cast` with a
manual rate gate; `gen_statem` with `postpone`; `:erlang.process_flag(:max_heap_size, ...)`
to kill an over-fat process before it kills the node.  None are
ergonomic.

**libxtc status.**  We have first-class bounded mailboxes via
`xtc_proc_opts_t.mbox_cap`: when the cap is reached, `xtc_send`
returns `XTC_E_AGAIN` instead of accepting and growing.  Sending
code can either treat the rejection as backpressure (block / queue
elsewhere / drop) or escalate.  Combined with `xtc_res` for global
RSS accounting, the operator has both per-process and process-global
budgets without writing the rate-limiting plumbing themselves.

**Improvements we should ship.**

* A "mailbox watermark" hook: a callback fired when mbox_n crosses
  a configurable percent of cap, so apps can shed load before the
  hard wall.  Cheap to add to `xtc_proc_opts_t`.
* `xtc_proc_mailbox_stats(pid, &out)` exposing current depth, peak
  depth, total received, total rejected.  Operators run a metrics
  scrape against this.

### 2.  Selective receive degenerates to O(N x M)

**Symptom.**  A process performs `receive {tag, X} -> ... end`.
The mailbox has 10000 unrelated messages ahead of the matching one.
Every iteration of the receive walks the entire queue.  Multiple
selective receives in sequence -- a common pattern in `gen_server`
state machines -- compound to O(N x M) where N is mailbox length
and M is the number of distinct receive patterns.  CPU goes to
100%, throughput tanks.

**BEAM root cause.**  Naive linear scan of the mailbox per receive.
The compiler tries to insert "receive marks" so subsequent receives
of the same pattern can resume scanning rather than restart, but
the optimization is fragile and unobservable.  Documentation
recommends "just don't do that" rather than offering a robust fix.

**libxtc status.**  We have selective receive (`xtc_recv_match`) with
the same naive O(N) walk.  We do not have the recv-mark optimization
yet.  Under heavy mailbox load with selective receive, we will hit
the same performance wall.

**Improvements we should ship.**

* The recv-mark optimization: cache (predicate-fn, last-walked-pos)
  per process so subsequent calls with the same predicate skip
  already-rejected envelopes.  Low single-digit-percent regression
  in the trivial case in exchange for asymptotic improvement under
  load.  Tracked.
* A `xtc_recv_first_match` debug counter that increments every time
  a predicate walks N>1 envelopes.  Operators can alert on this to
  catch the anti-pattern early.

### 3.  Big GC pauses

**Symptom.**  A long-running process accumulates a large heap (large
binaries pinned by `<<>>` references, big lists kept "just in case",
ETS lookups cached), then suffers a multi-second stop-the-world GC
pause.  Latency p999 jumps a thousandfold.

**BEAM root cause.**  Each Erlang process has its own heap; GC is
per-process and stop-this-process.  Per-process is fast for small
heaps but gets expensive linearly with heap size.  Big binaries are
ref-counted in a separate area but still subject to copying when
processes share them.

**libxtc status.**  No GC.  C-managed memory; the operator owns
allocations.  `xtc_mctx` (memory contexts) provides
PostgreSQL-style hierarchical lifetime grouping with bulk free at
context destroy -- the closest we come to a GC-like batch reclaim.
`xtc_slab` provides cache-friendly fixed-size allocators with
optional shared-memory mode.

So we don't have GC pauses.  We have a different class of problem:
*manual reclaim discipline*.  Operators must remember to free, must
think about ownership transfer when sending data via `xtc_send`,
must use `xtc_mctx_destroy` on appropriate boundaries.

**Improvements we should ship.**

* Documentation: a "memory ownership" guide that walks an example
  app's lifetime: who owns the bytes after `xtc_send`?  When does
  a process's pdict get freed?  Etc.  This is the missing
  counterpart of "let it crash".
* `xtc_alloc_audit_t`: a debug-build mode that records every
  allocation along with the proc that did it, and asserts on
  exit that all of a proc's allocations were freed before the
  proc died.  Catches per-proc leaks.

### 4.  Schedulers stuck on busy-loop NIFs / dirty work

**Symptom.**  A user-defined NIF (Native Implemented Function) does
500 microseconds of computation and never explicitly yields.  The
scheduler thread is monopolised; sibling processes do not run; the
soft-real-time guarantee evaporates.  The "100% balanced scheduler
load" plot turns into "one core at 100%, others at 5%."

**BEAM root cause.**  Scheduler preemption is co-operative inside
NIFs.  Erlang has dirty schedulers, but they need to be configured
and the work explicitly routed there.  Many libraries don't.

**libxtc status.**  Worse, in some sense.  xtc fibers cooperatively
yield via `xtc_yield()`, by parking on I/O, or implicitly by
`xtc_recv`/`xtc_send`.  A long compute that does none of these will
monopolise its loop.  We don't have automatic preemption.

**Improvements we should ship.**

* Documentation: every public xtc primitive should be tagged async-
  vs sync-vs-may-block; the `dist/s_async` lint enforces this in our
  source but the tagging needs to extend to caller-provided
  functions (e.g., a comment on `xtc_proc_fn` saying "must yield
  every N microseconds").
* A `xtc_yield_check` macro callers can sprinkle in compute loops:
  reads a per-loop "pls yield" flag set by the scheduler when other
  work is queued.  Equivalent of BEAM's `BUMP_REDUCTIONS`.
* For genuinely-blocking work: a `xtc_blocking_pool_run(fn, arg)`
  primitive (already considered for the M19 roadmap) that ships
  work to a side thread and resumes the calling proc when done.
  Avoids the dirty-scheduler ergonomics issue.

### 5.  Distributed Erlang's all-pairs TCP topology

**Symptom.**  Five Erlang nodes form a cluster.  Net split happens.
Nodes get confused about "alive" vs "split-brain".  The `epmd`
daemon caches stale routes.  Connections retry endlessly.  Memory
fills with un-deliverable RPCs.

**BEAM root cause.**  Distributed Erlang's distributed-process
abstraction is leaky.  It assumes all nodes can reach all others
directly, which doesn't survive cloud network reality.  The
canonical advice is "don't use distributed Erlang for anything
mission-critical; use HTTP/gRPC between groups of nodes."

**libxtc status.**  N/A.  We don't have distributed processes.  When
we add them (out of the current scope), we should learn from this
and:

**Improvements we should ship (when we do).**

* If we add distributed processes, do not assume all-pairs.  Embrace
  hub-and-spoke and route-aware topologies.  Make the failure model
  explicit (deliveries can fail; locality matters).
* Don't try to make `xtc_send(remote_pid, ...)` syntactically the
  same as local send.  The semantics differ; the syntax should too.

### 6.  Hot-code-loading fragility

**Symptom.**  A live system loads a new module version with
`code:load_file/1`.  Old processes are still running old code.
There's a window where new and old run simultaneously.  Subtle
state-shape mismatches manifest as crashes hours later.

**BEAM root cause.**  Hot reload is fundamental to OTP, but its
correctness depends on disciplined application of `code_change`
callbacks.  Many real codebases don't ship `code_change` rigorously,
because it's tedious and the tooling is weak.

**libxtc status.**  We don't support hot code reload.  Operators
restart the binary.  This avoids the fragility but removes a real
convenience for live deployments.

**Improvements we should ship (perhaps).**

* Document explicitly that we don't do hot reload and that
  blue/green or rolling restarts are the supported pattern.
* If we ever add it, ship a `code_change` analog as a first-class
  proc callback (`xtc_proc_code_change_fn`) and lint that warns
  if a long-lived proc opts into hot reload without registering
  one.

### 7.  Supervisor restart-loop storms

**Symptom.**  A leaf worker crashes.  Its supervisor restarts it.
It crashes again because the underlying problem (DB unavailable,
disk full) hasn't gone away.  Supervisor's intensity threshold is
hit; it dies, escalating to the parent supervisor.  The parent
restarts the whole subtree.  All children die again.  Loop.  CPU
spikes; logs explode; if the system supports recovery it can
take minutes to settle.

**BEAM root cause.**  Default `max_restarts` and `period` are
generous (1 in 5 seconds is the OTP default).  For "this dependency
is down for 10 minutes" the right answer is *back off and wait*,
not *retry every 200ms*.  Erlang has no built-in exponential
backoff; you write it yourself.

**libxtc status.**  Same.  Our `xtc_supervisor` ports the OTP
restart-intensity model verbatim.  Restart on transient + max_restarts
threshold; no automatic backoff.

**Improvements we should ship.**

* Add `xtc_sup_opts_t.backoff_initial_ns` and `.backoff_max_ns`:
  when a child crashes, wait the backoff duration before the next
  restart, doubling on subsequent crashes.  Resets to initial on a
  clean run for >= `period_ns`.  Ports the resilience pattern from
  `:libring` / Akka / Fault-tolerance literature.
* A "circuit-breaker" wrapper for gen_server-style `xtc_svr`: when
  backend calls fail at >= some threshold, fast-fail subsequent
  ones for a cooldown rather than retry.

### 8.  Heisenbugs from cross-thread scheduler load balancing

**Symptom.**  A process appears to behave differently when running
on scheduler 3 vs scheduler 7.  Test sometimes flakes; production
sometimes locks up, but only on the EU region.  Eventually
identified as a memory-ordering bug exposed by NUMA migration.

**BEAM root cause.**  The scheduler migrates processes between
threads aggressively to balance load.  This is correct in theory
but exposes any latent assumptions about thread-locality.

**libxtc status.**  We have the same migration risk in
`xtc_exec`'s work-stealing executor.  A task can be created on loop
0, stolen by loop 3, do work on a different CPU, then migrate
again.

**Improvements we should ship.**

* Loop-pinning for tasks that need it: `xtc_task_pin(t, loop_idx)`
  to forbid stealing.  Existing primitive; document it.
* `xtc_proc_pin(pid)` to keep a proc on its initial loop.  Useful
  for procs that own thread-locality (e.g., a SQLite handle).
* The xtc_lockmgr's deadlock detector already works across loops;
  validate that with property tests.

### 9.  ETS table contention

**Symptom.**  An ETS (in-process key/value store) with `read_concurrency`
flag works great for read-mostly.  Workload turns write-heavy.  Lock
contention crushes throughput.

**BEAM root cause.**  ETS is a reader-writer lock around a hash table
with optional sharding.  Sharded mode is opt-in and not the default.
Most code using ETS was written when traffic was light and never
revisited.

**libxtc status.**  We don't have an ETS analog yet.  If we add
`xtc_dict` (one of the gaps surfaced by the rexis example), we
should ship sharded read-mostly mode by default, not as a flag.

**Improvements we should ship.**

* When we ship `xtc_dict`: sharded by key-hash, each shard owns its
  own `xtc_lwlock`.  Read-mostly access is the default; multi-writer
  concurrency comes from the sharding.
* Provide stat readouts so operators see contention before it bites.

### 10.  Observability surface erodes over time

**Symptom.**  The BEAM has phenomenal observability tools
(`observer`, `recon`, `etop`, `process_info` introspection).
Engineers write feature code, ship it, ignore the observability
hooks.  Three years later, when production has a bad day, the
metrics and traces don't cover the new code.

**BEAM root cause.**  Observability is opt-in.  The defaults are
generous, but custom metrics for specific business logic require
explicit hooks.  Easy to skip during development.

**libxtc status.**  We're starting with less observability than the
BEAM ships with.  We have `xtc_log` and the `xtc_inject` framework;
we're adding `xtc_stats` (counters/gauges/tdigests).  The risk is
that adoption for this is voluntary too.

**Improvements we should ship.**

* Make the obvious metrics free: every `xtc_proc_spawn` increments
  a counter; every `xtc_send` increments another.  No opt-in.
  Reads via `xtc_metrics_iterate`.  The cost should be ~1-2 cycles
  per call (per-CPU sharded counter).
* A standard "scrape this fd to get all live counters in
  Prometheus exposition" entry point.  Apps wire it to whatever
  HTTP/scrape endpoint they expose.

### 11.  Schedulers stuck waiting on each other

**Symptom.**  Two schedulers each hold one ETS lock.  Each tries to
take the other's.  Deadlock.  In Erlang this is rare because ETS
locks are short-lived, but it does happen, and OTP gives you no
deadlock detector to find it.

**BEAM root cause.**  Erlang assumes operations on shared state are
short.  No deadlock detector.

**libxtc status.**  We *do* have a deadlock detector
(`xtc_lockmgr`).  But it's separate from the synchronisation
primitives (`xtc_lwlock`, `xtc_amutex`, etc.) used in non-database
contexts.  Cycles among the latter are not caught.

**Improvements we should ship.**

* Optional integration: every `xtc_lwlock_acquire` registers with
  `xtc_lockmgr` if a global "lock-graph tracking" mode is enabled.
  Cycles get reported, not silently waited on.  Off by default
  (it has overhead); enabled in test/staging.

### 12.  Hard to reason about timeout interactions

**Symptom.**  A `gen_server:call` has a 5-second timeout.  The
called server is itself making a `gen_server:call` to another
server with a 30-second timeout.  The outer call times out; the
inner call is still in flight.  The result, when it finally
arrives, is delivered as an unsolicited message and the outer
caller (long since gone) doesn't know what to do with it.

**BEAM root cause.**  Cancellation is cooperative.  A timed-out
`gen_server:call` doesn't cancel the work; it just stops waiting.

**libxtc status.**  Same situation.  `xtc_svr_call` with a timeout
returns `XTC_E_AGAIN`; the server keeps processing and may later
emit a reply.  We don't have cancellation propagation.

**Improvements we should ship.**

* A `xtc_abort_source` (already in xtc_sync.h) that callers can
  attach to a request.  Callees check the abort flag and exit
  early.  Already designed; needs first-class wiring through
  `xtc_svr_call` and `xtc_proc_*` request-shaped APIs.
* Document the cancellation pattern explicitly, with an example.

## Aggregate response

Of the twelve recurring BEAM/OTP issues:

| # | issue | already-handled in libxtc | gap | mitigation tracked |
|---|---|---|---|---|
| 1 | Mailbox overflow | partial -- bounded mboxes exist | watermark + stats hooks | yes |
| 2 | Selective receive O(N x M) | no -- naive walk | recv-mark | yes |
| 3 | GC pauses | yes -- no GC | manual reclaim discipline doc | yes |
| 4 | Schedulers stuck on long compute | no | xtc_yield_check + blocking-pool | yes |
| 5 | Distributed all-pairs | n/a -- no distrib yet | learn before adding | n/a |
| 6 | Hot reload fragility | n/a -- not supported | document; consider | n/a |
| 7 | Restart-loop storms | partial -- intensity yes, no backoff | exponential backoff | yes |
| 8 | Cross-thread heisenbugs | partial -- pin primitives exist | document + PBT | yes |
| 9 | ETS contention | n/a -- no dict yet | shard-by-default when added | n/a |
| 10 | Observability erosion | partial -- xtc_log exists, stats coming | free counters everywhere | yes |
| 11 | Lock cycles outside lockmgr | partial -- lockmgr stands alone | optional integration | yes |
| 12 | Cooperative cancellation | partial -- abort_source exists | wire through call APIs | yes |

The clear pattern: libxtc inherits roughly half of the BEAM's
production issues and is well-positioned to address them because
we're earlier in our lifecycle.  Specifically:

* Issues 1, 2, 7, 9, 10, 11, 12 each correspond to a concrete
  improvement we can implement.  None requires changing the public
  API surface in incompatible ways.
* Issues 3 and 4 require operator education and thoughtful default
  behaviour rather than runtime changes.
* Issues 5 and 6 are out-of-scope until we add the relevant
  features (distributed processes, hot reload).
* Issue 8 is a class of bug the BEAM has and we have; the
  primitives to mitigate it exist in xtc, but the discipline
  needs to be documented.

## Roadmap impact

The improvements above sketch a "production hardening" milestone that
libxtc should reach before claiming v1.0.  Roughly eight items, each
sized between a half-day and a week.  Total effort: about eight
weeks at the current pace.  None individually risky.

Listing them as concrete agenda items:

1. `xtc_proc_opts_t.mailbox_watermark_pct` + watermark callback (issue 1)
2. `xtc_proc_mailbox_stats(pid, &out)` (issue 1)
3. Selective receive: recv-mark optimization (issue 2)
4. Memory-ownership guide doc (issue 3)
5. `xtc_alloc_audit_t` debug allocation tracker (issue 3)
6. `XTC_YIELD_CHECK` macro + scheduler-side flag (issue 4)
7. `xtc_blocking_pool_run` worker pool (issue 4)
8. `xtc_sup_opts_t.backoff_initial_ns` + circuit breaker option (issue 7)
9. `xtc_lockmgr` integration toggle for `xtc_lwlock` (issue 11)
10. `xtc_abort_source` first-class wiring through `xtc_svr_call` etc. (issue 12)
11. Always-on per-call counters (issue 10) -- depends on `xtc_stats` landing
12. `xtc_proc_pin(pid)` hardening + PBT (issue 8)

These are added to the post-v0.2.0 backlog in `PLAN.md` (when
that's revised; it currently doesn't list them explicitly).
