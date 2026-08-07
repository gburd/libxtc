---
title: Influences and lineage
parent: Philosophy
nav_order: 6
lede: >-
  The runtimes libxtc learned from, what it took from each, and the
  lines it deliberately did not cross.
---

# Influences and lineage

libxtc is not invented from nothing. It stands on a specific lineage of
concurrency runtimes, takes a deliberate lesson from each, and refuses
a specific thing from each. Naming them precisely is the honest way to
say what libxtc *is* -- and what it is not.

## The runtimes libxtc borrows from

- **Erlang / the BEAM** -- the failure model. Isolated processes,
  links, monitors, supervisors, "let it crash." libxtc's `xtc_proc`,
  `xtc_supervisor`, and the whole orc layer are the BEAM's lesson in C:
  you do not defend against every error at every call site; you let a
  fiber die and recover at a supervisor that owns a coherent piece of
  state.

- **Seastar** -- one thread per core, share-nothing by default. Each
  carrier owns its run queue, its timers, its slab caches; cross-loop
  traffic goes through channels and mailboxes, not shared mutexes.
  This is why libxtc scales without a lock on the hot path.

- **Rust's Tokio** -- `async`/`await` over a work-stealing runtime. The
  ergonomics of writing straight-line code that suspends, and a
  scheduler that keeps every core busy by stealing work.

- **Cats Effect** (the Scala effect system) -- two lessons, added in
  2026 after a close reading of Cats Effect 3.7 and Daniel Spiewak's
  account of building it:

  1. **The integrated runtime.** The single most important structural
     decision Cats Effect made was to pull the *syscall boundary into
     the scheduler* -- the fiber runtime itself owns the
     epoll/kqueue/io_uring wait, and tells the kernel how long to wait,
     so one carrier is one state machine that does both user work and
     the kernel conversation. Most runtimes bolt an I/O dispatch pool
     onto a separate compute pool and regret it for a decade. libxtc
     *starts* there: L1 `io` sits under L2 `evt` sits under the
     executor, integrated by construction. Cats Effect validated, from
     an entirely different language, that this is the right spine.

  2. **Resource safety must be a mechanism, not a manner.** Cats
     Effect's `Resource`/`bracket` guarantees a finalizer runs on every
     exit path -- success, error, *and* cancellation. Spiewak's candid
     observation is that for years the *encouragement* to use it was "a
     paper door": a convention the API carroted you toward, which
     humans respected and which a coding agent barges straight through,
     leaking a socket you find out about at 3am. The lesson libxtc
     takes: a resource guarantee you cannot enforce is a comment, not a
     contract. See [Resource scope](#resource-scope-the-mechanism-not-manner-principle)
     below.

- **Scala Native** -- the proof that a high-level runtime's ideas can
  be carried down to native code with no VM, instant startup, a small
  footprint, and zero-cost C interop. libxtc is the C-native end of
  that same spectrum: no VM to begin with, so the ideas the JVM world
  reached for late (an integrated runtime, structured cancellation,
  fiber dumps) are available from the first instruction.

## The lines libxtc will not cross

Borrowing discipline is not the same as borrowing a paradigm. libxtc
takes the *mechanisms* and the *engineering discipline* from these
systems, never the language model:

- **No monadic / type-class effect API.** Cats Effect expresses
  everything as values in a `MonadCancel`/`Spawn`/`Concurrent`
  hierarchy. That is the right answer *in Scala*. In C, the equivalent
  discipline is delivered by the layered `__os_*`/`__xtc_*`/`xtc_*`
  naming and the API-discipline merge gate -- which tell you which
  capabilities each layer may touch, enforced by a linter instead of a
  compiler. libxtc's API is plain, ergonomic C.

- **No garbage collector, no managed heap, no VM.** libxtc does manual,
  budgeted memory management with the `xtc_res` accountant. Scala
  Native's GC/safepoint machinery is instructive to *read* (its
  signal-to-safepoint trampoline informs how libxtc thinks about
  involuntary preemption) but it is not something libxtc adopts.

- **No stack-copying delimited continuations.** libxtc uses stackful
  fibers on hand-written `fcontext` assembly (with a `ucontext`
  bridge). This is a deliberate choice: it is fast, portable across the
  arches libxtc targets, and -- critically -- it is friendly to
  libxtc's deterministic simulation, which stack copying would
  complicate.

## Where libxtc is ahead

Two things libxtc has that neither Cats Effect nor Scala Native does,
and will not give up:

- **Deterministic simulation.** libxtc re-runs its *real* scheduler
  from a seed and gets byte-identical execution, with a guard that
  traps any nondeterministic primitive. Cats Effect tests hard but has
  no equivalent replay of the live runtime. This is libxtc's crown
  jewel; every other idea on this page is negotiable, that one is not.

- **A single native library, not a framework or a language.** You link
  `libxtc`. There is no runtime to install, no language to adopt, no VM
  to tune. That is the whole point.

## Resource scope: the "mechanism, not manner" principle

The sharpest lesson from the Cats Effect reading deserves its own
statement, because it shapes libxtc's resource and cancellation APIs:

> A resource-cleanup guarantee that lives only in documentation, naming
> conventions, and examples is a *paper door*. A human respects it; an
> automated caller (and a tired human at 3am) walks straight through it
> and leaks the resource. libxtc's answer is that acquire/release and
> structured cancellation are backed by a *runtime mechanism* -- a
> scope whose finalizers run on every exit path (normal return, error,
> `xtc_exit`, abort, and crash caught by the fault guard), with
> acquisition masked against cancellation so the release is always
> registered -- not merely by an API you are encouraged to use
> correctly.

This is why libxtc pairs its resource-scope and cancellation-masking
primitives with the deterministic-simulation tier: the guarantee is not
just asserted, it is *proven* -- a DST test injects an abort or a crash
in the middle of a scope and checks that every finalizer ran, on a
replayable seed.
