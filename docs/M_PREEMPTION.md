# M_PREEMPTION -- resumable, crit-depth-safe fiber preemption for libxtc

Bringing the capability of CMU's Lightweight Preemptible Functions
(libinger / libgotcha / libas-safe / libturquoise, CMU CSD 2022) into
libxtc: precise-timeout, resumable preemption of a running fiber, so a
runaway or untrusted unit cannot starve its event loop.

This document is a PLAN.  Nothing here is implemented yet.  It exists so
the approach is reviewed before any code, and so the work is broken into
bounded, independently-verifiable milestones (a prior open-ended attempt
at a hard concurrency task ran ~20h without converging; every phase here
has a concrete pass/fail deliverable and is a short, closed task).

## 1. Why (the foundational gap)

libxtc is cooperatively scheduled.  A fiber runs until it reaches a
yield point (xtc_yield / xtc_yield_if_due / a park).  The measured
consequence (bench/bench_fairness.c): one non-yielding CPU loop lets its
loop-mates complete ~8 cooperative iterations vs ~184 with the
cooperative yield-budget watchdog -- and a genuinely uncooperative or
untrusted loop (a PG extension, a bad UDF) starves the loop entirely.
The only current remedy is xtc_osproc (offload to an OS thread the
kernel preempts), which is heavyweight per unit.

For a foundation meant to run a preemptive, multi-tenant database this
is the deepest limitation.  BEAM preempts (reduction counting); Tokio
does not (libturquoise exists precisely to add preemption to a
Tokio-shaped pool); libxtc today is in Tokio's position.

## 2. What we take from the CMU work -- lessons, not code

DECISION: do NOT vendor libgotcha or libinger.  Take the correctness
DISCIPLINE; build a libxtc-native facility on machinery libxtc already
has.

- libgotcha (per-library-copy TLS namespaces via dynamic-linker symbol
  interception): NOT adopted.  It solves per-function global isolation
  for shared-global library code -- a problem libxtc does not have (its
  units are message-passing procs, not shared-global functions; memory
  isolation is already served by pkey/mctx).  Vendoring it would import
  a heavyweight, glibc-linker-specific hack to solve a non-problem.
- libas-safe (make async-signal-UNSAFE code safe to interrupt at an
  arbitrary instruction): the CENTRAL lesson, adopted as a SPEC not
  code.  You cannot safely interrupt code mid-malloc / mid-lock.
  libinger solves this the hard way (rewrites unsafe code to be
  interruptible).  libxtc solves it the PRAGMATIC way it already
  started: crit_depth (src/ptc/proc.c) already marks the unsafe regions
  (latch held, allocator in progress), and the fault handler already
  refuses to unwind when crit_depth > 0.  Preemption reuses this: a
  timer that fires inside a crit section DEFERS to the section's exit.
  This yields "cooperatively-assisted preemption" -- weaker than
  libinger's fully-arbitrary preemption, but dramatically simpler and
  sufficient, because the only unsafe regions are libxtc's own short,
  bounded latch/allocator windows.
- libinger (launch(f, timeout) -> pause/resume): the API model + the
  precise-timeout semantics, reimplemented natively.
- libturquoise (preemptive tokio-threadpool): the validation that this
  is the right fix for a work-stealing async pool -- and the benchmark
  shape to reproduce.

## 3. The key realization: a preemption IS an involuntary yield

libxtc's fiber pause primitive already exists.  xtc_yield does:

    jump_fcontext(&c->fctx, g_sched_fctx, NULL);   // save my resume
                                                    // point, jump to
                                                    // the scheduler

A preemption is the SAME operation triggered from a timer signal
instead of a cooperative call: save the running fiber's context into
c->fctx, jump back to g_sched_fctx, and let the scheduler re-queue the
fiber on the run queue.  The fiber is resumed later by the ordinary
run-queue path and never knows it was paused.  So "resumable
preemption" needs NO new pause/resume mechanism -- only a safe way to
INVOKE the existing yield from signal context.

What is genuinely new: (a) a per-worker timer signal source (none today
-- libxtc installs only the fault signals), and (b) the signal-context
"involuntary yield" that is safe w.r.t. crit_depth and the fctx
substrate.

## 4. Scope boundaries (what this is and is NOT)

IN: per-fiber time-slice preemption on a cooperatively-scheduled worker;
a precise-timeout launch (xtc_launch) for bounded-time / untrusted work;
fair time-slicing so a runaway CPU fiber yields the loop.

OUT (for now): preempting arbitrary async-signal-unsafe code at any
instruction (libas-safe's full generality) -- we defer to crit_depth
instead; preempting across the WAL/latch hot path (those set crit_depth
and are intentionally non-preemptible); Windows (phase-gated -- VEH has
no periodic-timer analog as clean as POSIX timers; a later phase).

## 5. Bounded phases (each a closed task with a pass/fail deliverable)

### Phase 0 -- the per-worker timer seam (no preemption yet)
Add a per-worker POSIX interval timer (timer_create + SIGVTALRM, or
setitimer(ITIMER_VIRTUAL)) armed by the executor worker, delivering to
the worker thread.  The handler does NOTHING yet but set an atomic
"tick" flag.  Wire it OFF by default (a config/opt-in).
DELIVERABLE: a test that arms the timer, runs a busy loop, and observes
the tick flag set N times; make check green; no behavior change when
off.  Bounded, ~1 day.

### Phase 1 -- cooperative-assisted preemption (safe, no signal unwind)
The timer handler sets the running fiber's park_requested + a
"preempt_pending" flag (like the yield-budget watchdog, but timer-driven
not call-driven).  The fiber is preempted at its NEXT crit_depth==0
safe point -- which the scheduler/latch-exit paths already reach
frequently.  This is preemption WITHOUT a signal-context stack switch:
strictly safe, reuses the yield path, and already covers the "long loop
that occasionally touches a yield-friendly point" case.
DELIVERABLE: bench_fairness shows a non-yielding-BUT-crit-safe loop now
yields on the timer (coop_iters rises without any xtc_yield_if_due
call); replayable under DST with a seeded timer.  Bounded.

### Phase 2 -- true signal-context involuntary yield (the hard core)
The timer handler, when crit_depth == 0, performs the involuntary yield
IN THE SIGNAL HANDLER: capture the interrupted context and
jump_fcontext back to g_sched_fctx.  This preempts a fiber that never
reaches a cooperative point at all (a pure tight loop).  The
async-signal-safety argument is libinger's, scoped by crit_depth: the
handler only unwinds when crit_depth == 0, i.e. not inside a latch or
the allocator, so no async-signal-unsafe state is straddled.  Requires
care that jump_fcontext from a signal handler + sigaltstack interact
correctly (the fault handler already jumps from a signal via
siglongjmp; this jumps via fcontext instead -- verify the signal mask
and altstack handoff).
DELIVERABLE: bench_fairness with a PURE tight loop (no yield points at
all) -- loop-mates still make progress; the runaway is time-sliced.
ASan + TSan clean.  This is the phase that closes the BEAM gap.  Bounded
but the highest-risk phase; if signal-context fcontext proves unsafe,
fall back to Phase 1 + document (still a real improvement).

### Phase 3 -- xtc_launch(fn, arg, timeout_ns) (the libinger API)
On top of Phase 2: run fn on a fiber with a one-shot timeout; on
timeout, either PAUSE (resumable -- reschedule) or CANCEL (siglongjmp to
cleanup, reusing the fault-recovery path).  Precise-timeout semantics
like libinger's launch().  Composable (a launched fn can launch).
DELIVERABLE: a test that launches a fn exceeding its timeout and
observes it paused/cancelled at the deadline within a bounded slop;
statement-timeout use case demonstrated.

### Phase 4 -- DST + benchmark + docs
Model the preemption timer as a seeded DST event (XTC_SIM_RNG has room
for a PREEMPT stream) so preemptive schedules replay.  Reproduce
libturquoise's fairness benchmark (bench/ vs a cooperative baseline).
Document the crit_depth-scoped safety contract and the "what libxtc
does NOT preempt" boundary honestly, next to the libinger comparison.

## 6. Honest risk assessment

- Phase 2 (signal-context fcontext jump) is the crux.  jumping stacks
  from a signal handler is done in the wild (Go's async preemption does
  essentially this) but is delicate: signal mask restoration, the
  altstack, and re-entrancy if a second timer fires mid-jump.  Go's
  implementation is the closest real-world reference; libinger's is the
  academic one.  Phase 1 is the safe fallback that still delivers most
  of the value.
- crit_depth coverage MUST be complete: any async-signal-unsafe region
  not wrapped in crit_depth is a preemption hazard.  A prerequisite
  audit (a Phase 0.5) should confirm every malloc/lock hot path bumps
  crit_depth.  This is itself a bounded, valuable audit.
- Cost: an armed interval timer per worker + a branch on the tick flag
  at safe points.  Measure it; the timer is only armed when preemption
  is enabled (off by default keeps the cooperative fast path unchanged).
- Portability: POSIX timers first; Windows (a timer-queue or a watchdog
  thread that suspends the worker) is a later, separate phase.

## 7. Why this is the best method (vs the alternatives)

- vs vendoring libinger/libgotcha: avoids a glibc-linker-specific
  dependency and per-function-namespace machinery libxtc does not need;
  reuses libxtc's existing signal + crit_depth + fctx substrate.
- vs cancel-on-timeout only: that gives statement timeouts but NOT fair
  time-slicing of a runaway (can only kill, not pause) -- half a fix.
- vs "stay cooperative + xtc_osproc for untrusted work": keeps the
  heavyweight OS-thread-per-unit cost and cannot time-slice within a
  loop.
The native, crit_depth-scoped, phased approach delivers libinger's
capability at libxtc's weight, on the machinery already present, with
each step verifiable.
