---
title: Choices and roads not taken
parent: Philosophy
nav_order: 2
permalink: /philosophy/choices
---

# Choices and roads not taken

Every design is defined as much by what it rejects as by what it adopts.
This page collects the significant alternatives libxtc considered and
deliberately did *not* choose, with the reasoning. The guide chapters
carry these as inline "alternative not chosen" callouts; here they are
in one place.

## Fibers, not callbacks or explicit state machines

**Not chosen: the libevent/libuv callback model.** Splitting every
operation into a callback chain keeps the stack shallow but shatters
straight-line logic and forces local variables into heap context
structs. libxtc uses fibers so the code stays linear and the variables
stay on the stack; the cost is a small per-fiber stack instead of a
large readability tax. Where an explicit state machine is genuinely
wanted -- vast numbers of tiny uniform entities -- libxtc offers the
stackless [Isolate layer](../examples/) as an opt-in, not as the
default.

## Shared-nothing processes, not shared state behind a mutex

**Not chosen: a mutex-guarded shared struct as the default unit of
concurrency.** It is faster for one hot counter, but it does not
compose: every invariant adds a lock, lock order becomes a global proof
obligation, and a holder that dies wedges everyone. libxtc's default is
the shared-nothing process, where state has exactly one owner and
failure is contained. The lock primitives still ship
([Locks](../locks)) for the cases that truly want shared memory -- they
are the exception, chosen deliberately.

## Let it crash, not defensive error handling everywhere

**Not chosen: local recover-from-every-error code.** That approach turns
programs into mostly-error-handling, and still has no clean answer for
the corruption you did not foresee. libxtc adopts BEAM supervision:
write the happy path clearly, contain failure to one process, and put
recovery in one place -- the supervisor -- where it restarts from a
known-good state. It is less code and it handles the unforeseen.

## One thread per core, not a pool per subsystem

**Not chosen: a dedicated thread pool for each subsystem.** That design
grows threads with subsystems, lets context switches dominate under
load, and makes the cross-subsystem lock graph the hardest correctness
problem in the system. libxtc runs one loop thread per core (plus a
blocking pool); concurrency comes from fibers, not threads. Fewer
threads, no lock graph between subsystems.

## An explicit blocking escape hatch, not auto-detected blocking

**Not chosen: transparently detecting and offloading blocking calls.** A
library cannot know which calls will block, and hidden thread hops are
invisible in a profile. `xtc_blocking_run` makes the one place a thread
hop happens explicit -- visible in the code and the profile, like
Rust's `unsafe`. Not forbidden; marked.

## Hand-written context-switch assembly, not `ucontext` everywhere

**Not chosen: `ucontext` as the sole coroutine substrate.** `ucontext`
saves the signal mask with a syscall on every switch -- an order of
magnitude slower than a register swap. Since the switch is the hottest
path in the library, libxtc ships hand-written `fcontext` assembly for
the common architectures (x86-64 ~7.6 ns/switch) and falls back to
`ucontext` (and Win32 fibers) only where it must.

## Deterministic simulation, not just stress tests

**Not chosen: relying on stress tests and sanitizers alone to find
concurrency bugs.** Stress tests find bugs that happen to occur on the
machine you ran them on; they are not reproducible and they do not
explore the adversarial interleavings. libxtc adds FoundationDB-style
deterministic simulation: the whole concurrent system replays from a
seed under a pessimal scheduler with injected partitions, latency, disk
faults, and crashes. A failing seed reproduces exactly, every time.
Sanitizers and stress tests still run -- as a complement, not the
primary defense.

## A native library, not a framework or a language

**Not chosen: a new language, or a framework that owns `main()`.** The
systems libxtc targets are decades of existing C. It is a library you
link, not a runtime you submit to: you keep `main`, you keep your build,
you call `xtc_*`. The single-file amalgamation exists precisely so it
can drop into a foreign build with no configure step. libxtc adapts to
your program, not the other way around.

---

Back to [Why libxtc exists](why) &middot; on to the
[Guide](../guide/) or the [Examples](../examples/).
