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

## 8. Taking it to the next level: scalability levers alongside preemption

Preemption (above) closes the fairness gap.  Two further levers close
the SCALABILITY gaps measured against Tokio/Seastar, and one adds a
capability the COW-namespace idea points at -- all reusing substrate
libxtc already has, none requiring libgotcha.

### Lever S1 -- stack memory: madvise-on-park (the Tokio fan-in gap)

Measured (bench_mem_per_task): a parked stackful fiber commits ~4 KiB
(one page faulted in) but RESERVES its full stack (default 64 KiB).
Fiber stacks are mmap'd anonymous (coro_fctx.c: MAP_PRIVATE |
MAP_ANONYMOUS, with a guard page), so the tail above a parked fiber's
saved stack pointer can be returned to the OS with
madvise(MADV_DONTNEED) and faults back as zero-fill on resume.  This is
predictable (no relocation, no GC, no segmented-stack thrash -- the
technique Go abandoned), portable (Linux/BSD madvise; a no-op elsewhere),
and directly attacks the per-fiber RAM floor for the million-idle-
connection case.

  P-S1a: on park (xtc_yield / recv / latch wait), if the fiber's used
  stack depth (stack_top - current SP) is below a threshold, madvise
  the unused tail MADV_DONTNEED.  On resume it faults back.  Measure the
  new bytes/park with bench_mem_per_task.
  P-S1b: tune the threshold (only madvise when the reclaim exceeds a
  page or two, to avoid fault churn on shallow frequently-woken fibers).
  DELIVERABLE: bench_mem_per_task shows parked-fiber commit drop toward
  1 page; make check + a park/resume stress test green; no measurable
  hot-path regression for shallow fibers.  Bounded, low risk.

### Lever S2 -- the stackless model is already the answer for extreme
### fan-in

For workloads where even one page per fiber is too much (millions of
mostly-idle connections), the answer is NOT to shrink the stackful fiber
further but to use the STACKLESS tnt Isolate layer (now a supported
xtc_tnt_* API): Isolates are dense arena structs (hundreds of bytes, no
stack).  The framework thus offers BOTH models -- stackful, preemptible
fibers for general work; stackless Isolates for extreme fan-in -- and
the developer picks per workload.  This duality is a deliberate
next-level positioning, not a gap.  No new work; documented here so the
tradeoff is explicit.

### Lever S3 -- mctx-COW (the reframed libgotcha-COW idea)

The COW-namespace idea is most valuable applied NOT to TLS (libxtc has
almost no global TLS state by design -- confirmed by audit) but to the
per-proc MEMORY CONTEXT (mctx), which libxtc already has as hierarchical
arenas.  A COW snapshot of an mctx gives:
  - speculative execution with cheap rollback for the SSI/MVCC path
    (sqlxtc): snapshot, run speculatively, commit (keep) or abort (drop
    the COW pages) -- free rollback;
  - isolated cancellable allocations: a launch()ed unit (Phase 3) gets a
    COW mctx view; cancel-on-timeout drops the view and every allocation
    with it (no leak, no cleanup pass) -- composes with preemption.
It uses mprotect / write-fault COW (userfaultfd or a fault handler),
reusing the page-protection substrate the pkey tier already establishes.

  P-S3a: mctx snapshot/restore via COW page protection (design + a
  single-context prototype + a rollback microbench).
  P-S3b: wire an optional COW mctx view into xtc_launch's cancel path.
  DELIVERABLE (S3a): a test that snapshots an mctx, mutates it, restores,
  and confirms the pre-snapshot bytes are back; rollback cost measured.
  Higher effort, second-tier priority -- capability more than raw
  scalability.  Explicitly OPTIONAL / later.

### What we deliberately do NOT do (scalability)

- libgotcha per-library TLS namespaces: solves per-function global
  isolation, a non-problem for libxtc's message-passing procs; confirmed
  libxtc has almost no global TLS state.  The COW idea is redirected to
  mctx (S3) where it pays off.
- Segmented / split stacks (Go's own cautionary tale: hot/cold
  thrashing).  madvise-on-park (S1) gets the memory win without it.
- Seastar-style share-nothing: conflicts with the deliberate
  shared-buffer-pool threaded-PostgreSQL goal.

## 9. Landing preemption + DST soak + B-tree merge together

Three tracks, run independently and bounded, sequenced around one
release:

- TRACK A (preemption + S1): P0 timer seam + P0.5 crit_depth-coverage
  audit -> P1 cooperative-assisted preemption -> S1 madvise-on-park ->
  (next release) P2 signal-context preemption, P3 launch, S3 mctx-COW.
- TRACK B (B-tree merge): the BOUNDED 3-step plan (Update 3 in
  M_SQLXTC_BTREE_MERGE.md) -- Step 1 instrument-only (find the straddle,
  no fix), Step 2 fix that one sub-update, Step 3 harden + flip
  default-on.  Each a SHORT hard-capped task, never one open-ended run.
- TRACK C (DST soak): a large seed sweep across the 8 sim tests, run in
  the background, feeding release qualification.

RELEASE GATE: preemption P1 + S1 + (B-tree Steps 1-2 if converged) + a
clean DST soak -> qualify -> cut.  P2 / launch / mctx-COW / B-tree
default-on land in the following release.

## 10. Implementation status (2026-07)

- Phase 0 (per-worker CPU-time timer seam): DONE.  xtc_preempt_arm/
  disarm/ticks/tick_pending; test/m14/test_preempt.c.
- Phase 0.5 (crit_depth / unsafe-depth audit): DONE.  Added a
  per-thread unsafe-depth counter (__xtc_unsafe_enter/leave/depth)
  bracketing the allocator (os_alloc.c); it also lets the fault handler
  avoid unwinding out of malloc.  test/m14/test_unsafe_depth.c.
- Phase 1 (cooperative-assisted preemption): DONE.  xtc_exec_set_preempt
  arms the per-worker timer; a tick makes xtc_yield_if_due callers yield
  (xtc_yield_check consults the tick).  test/m14/test_preempt_p1.c
  proves a compute fiber that yields-if-due is timer-sliced with no
  manual budget.  This is the SUPPORTED preemption today.
- Phase 2 (signal-context involuntary yield): INFRASTRUCTURE in place
  (xtc_preempt_set_involuntary, the handler's crit_depth+unsafe_depth
  safety gate, and the __xtc_coro_preempt substrate hook).  The portable
  whole-ucontext approach (copy the signal's ucontext_t into the fiber's
  resume slot and switch to the scheduler) is UNSOUND -- a
  signal-delivered ucontext is not interchangeable with a
  getcontext/swapcontext-captured one (FP/XSAVE/flags differ), and the
  scheduler's later swapcontext(&c->ctx) faults (verified: SIGSEGV in
  swapcontext).  test/m14/test_preempt_p2.c drives a PURE tight-loop
  runaway + peers.

  Phase 2b-arch (the real signal-context preemption): DONE for x86-64
  System V on the ucontext substrate (the default glibc build).  It is
  Go's async-preemption PC redirect, NOT the unsound ucontext copy: the
  timer handler, when safe (crit_depth == 0 && unsafe_depth == 0),
  rewrites the interrupted mcontext's RIP/RSP so that on sigreturn the
  fiber resumes -- on its own stack, with its own real registers -- at
  an on-stack trampoline (src/os/asm/preempt_trampoline_x86_64.S).  The
  trampoline saves the full GP file + RFLAGS + a 512-byte FXSAVE area,
  performs a NORMAL cooperative xtc_yield() (the known-good swapcontext
  path), and on resume restores everything and returns to the
  interrupted PC with RSP exactly restored -- invisible to the
  interrupted code.  test/m14/test_preempt_p2.c now ASSERTS that a pure
  tight loop with no yield points is sliced (peers advance ~1000-1500x)
  wherever __xtc_coro_preempt_effective() reports the redirect is active
  (x86-64 and aarch64 ucontext); on fctx/musl, winfiber, and the
  amalgamation (which cannot carry the .S) it DECLINES and falls back
  to Phase 1.  __xtc_coro_preempt_effective() reports at runtime which
  is active.

  Correctness: the redirect must never fire while a context is
  mid-switch or the trampoline is mid-save/restore, or a half-written
  register/machine context is corrupted.  Two guards ensure this: (1) a
  per-thread g_in_preempt flag set across every swapcontext (and the
  save/restore of per-fiber TLS around it), cleared receiver-side once
  control has fully landed; (2) an instruction-pointer range check that
  declines when the interrupted PC lies within the trampoline itself
  (covering its epilogue, where the flag is already back to 0).  With
  both, 800+ pathological-stress runs (8 pure-CPU runaways monopolizing
  one loop at 40-100us slices) are crash-free and ASan-clean.

  A THIRD hazard was found and fixed: a fiber preempted while holding a
  pthread_mutex deadlocked the loop, because the loop runs many fibers
  on one OS thread -- the preempted holder plus another same-loop fiber
  blocking on the same mutex wedges the thread (a holder can only
  release by being rescheduled, which needs the thread now blocked in
  pthread_mutex_lock).  This was the earlier "rare hang": captured on a
  96-vCPU EC2 host running the stress 90-way in parallel (a hang in
  __notify_links_and_monitors on tbl->lock).  Fixed by making the
  library's own locks preemption-safe -- __os_mutex_* and the new
  __xtc_mtx_lock/unlock bracket __xtc_unsafe_enter/leave, and every
  fiber-reachable internal lock (proc, sync, chan, lock_mgr, lock_lr,
  lock_lw, rcu, pdict, blocking, stats, alloc_audit, inject, cfg, mctx,
  sup, svr, reg, tnt) now goes through them so a tick landing inside a
  held lock defers to the cooperative path.  After the fix the same
  stress ran 62,000+ times crash- and hang-free on the 96-vCPU host.

  aarch64 (AAPCS64): the trampoline is preempt_trampoline_aarch64.S,
  the identical Go-style PC redirect adapted to arm64 (no red zone;
  orig_pc staged in a 16-aligned scratch slot below the interrupted
  sp).  It saves x0-x30, NZCV, and the full v0-v31 + FPSR/FPCR file,
  and restores all of them exactly except ONE register: aarch64 has no
  instruction that restores every GP register AND redirects PC+SP with
  zero free registers, so exactly one scratch is unavoidable.  We
  sacrifice only x16 (IP0), the ABI's intra-procedure-call temporary
  that the compiler treats as clobbered across any call -- the same
  minimal tradeoff Go's arm64 async preemption makes.  Validated on a
  64-vCPU Graviton3: peers advance during a pure tight loop (true
  preemption), 54,000+ pathological-stress runs crash- and hang-free
  (an x16+x17 first cut showed 1 crash in 19k; restoring x17 and
  sacrificing only x16 eliminated it), full make check green.

  Status: SUPPORTED but OPT-IN and EXPERIMENTAL on x86-64 and aarch64.
  Enabled only by an explicit xtc_preempt_set_involuntary(1) (off by
  default; Phase 1 cooperative is the default).  The earlier rare hang
  is fixed (see above); the remaining honest caveat is the aarch64 x16
  sacrifice, which has not produced an observed failure across 54k+
  stress runs but is documented as a theoretical residue.  Untrusted
  pure-CPU work can also use xtc_osproc (an OS thread the kernel
  preempts), and every cooperating fiber is covered by Phase 1.

  Remaining arches beyond x86-64 + aarch64 (ppc64le, riscv64, ...): the
  mechanism is identical, only the
  per-arch mcontext register names (.pc/.sp) and trampoline asm differ;
  a bounded follow-up.

- Lever S1 (madvise-on-park, stack-memory reclamation): DONE (mechanism),
  OFF by default, opt-in via xtc_stack_reclaim_enable().  On park
  (xtc_yield, which the recv / latch / timer park paths all route
  through), a fiber returns the unused tail of its stack --
  [stack_base + guard, current_SP - keep_margin), page-aligned inward --
  to the OS with madvise(MADV_DONTNEED); it faults back zero-fill on
  resume.  Implemented in all three coro substrates: coro_uctx.c and
  coro_fctx.c do the real reclaim (gated on MADV_DONTNEED availability),
  coro_winfiber.c is a no-op (OS-owned fiber stacks).  The reclaim
  region is bounded by the guard page below and a live margin (default
  one page) below the SP, and only fires when the reclaimable span
  exceeds a page (no fault churn on shallow fibers).

  Correctness: test/m14/test_stack_reclaim.c stamps a deep live frame,
  parks (tail reclaimed), resumes, and verifies the live frame survived
  and the refaulted tail is usable again -- across many park/resume
  cycles, for several fibers, with reclaim off (count stays 0) and on
  (count rises, all sentinels intact).  make check + ASan + UBSan clean.
  Under AddressSanitizer the reclaim safely DECLINES (ASan relocates
  fiber frames to a heap fake-stack, so the running SP is outside the
  fiber's mmap'd stack and the geometry check finds nothing to reclaim);
  the test asserts correctness there but not a fire count.

  HONEST SCOPE: the reclaim is exactly [stack_base, park_SP) -- the
  region BELOW the parked fiber's stack pointer (stacks grow down, so
  that is the unused-so-far region).  The win is therefore realized for
  a fiber whose committed stack HIGH-WATER lies below where it parks
  (e.g. a handler that recurses deep during parse/plan, returns, then
  parks shallow awaiting I/O -- the deep pages are reclaimed).  A fiber
  that parks AT its deepest point has little below its SP to reclaim.
  This is a narrower guarantee than "shrink every parked fiber to one
  page": the structural per-parked-fiber floor (the pages between the
  park SP and stack_top, plus the coro struct) is NOT reclaimable while
  parked, because those pages are live from the stack's perspective.
  For extreme fan-in where even that floor is too much, the stackless
  tnt Isolate layer (Lever S2) remains the right tool -- the duality is
  deliberate.  A clean synthetic RSS-drop microbenchmark proved elusive
  (compiler frame-collapse under -O2 kept transient deep stack from
  staying committed long enough to sample), so no headline bytes/task
  number is claimed here; the mechanism's reclaim region and safety are
  what is verified.
