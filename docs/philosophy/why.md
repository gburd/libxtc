---
title: Why libxtc exists
parent: Philosophy
nav_order: 1
permalink: /philosophy/why/
lede: >-
  The problem C never got a library for, and the shape of the answer.
---
C is where the systems that everything else runs on are written:
databases, kernels, network servers, language runtimes. Yet C has no
native answer to the problem every one of those systems faces --
**doing many things at once, on many cores, without the code collapsing
into a lock graph nobody can reason about.**

The industry solved this problem elsewhere. Go gave goroutines a
scheduler and channels. Rust's Tokio gave `async`/`await` over a
work-stealing runtime. Seastar gave C++ a shard-per-core,
future-based model. Erlang's BEAM gave a fleet of isolated processes
with supervision, and thirty years of production evidence that "let it
crash" builds systems that stay up. Each of these is, at bottom, the
same set of ideas: cheap user-space concurrency, an event loop instead
of a thread per task, and -- in the BEAM case -- isolation and
supervised recovery.

C never got the library. Every large C system reinvents a slice of it:
an ad-hoc event loop here, a callback framework there, a bespoke thread
pool and a hand-rolled lock hierarchy. The reinventions are partial,
incompatible, and each one re-learns the same lessons (lost wakeups,
priority inversion, the mutex you forgot to take) the hard way.

**libxtc is that missing library.** It brings the Tokio + Seastar + BEAM
model to C11, as a normal static or shared library (or a single-file
amalgamation), under an ISC license, portable across Linux, the BSDs,
macOS, illumos, and Windows. You get:

- **Fibers** -- suspendable stacks that switch in nanoseconds, so a
  blocking-looking call becomes a yield and one thread runs thousands of
  activities.
- **An event loop** over the best poller each OS offers (io_uring,
  epoll, kqueue, IOCP, event ports).
- **Processes** -- addressable, mailbox-owning units with private state,
  so concurrency does not mean shared memory and locks by default.
- **Links, monitors, and supervisors** -- the BEAM failure model, so
  recovery is a policy at the top rather than defensive code everywhere.
- **A full lock toolbox** for the cases that genuinely want shared
  memory -- RCU, left-right locks, lightweight locks, a deadlock-detecting
  lock manager.
- **Deterministic simulation testing** -- the FoundationDB/TigerBeetle
  discipline: replay the entire concurrent system, faults and all, from
  a seed, and find the race before it ships.

## The forcing function

libxtc is not built in the abstract. Its long-term target is a
**threaded PostgreSQL** -- taking a process-per-connection database that
has resisted threading for decades and giving it a runtime that makes
threading tractable. That goal drives the requirements: real
portability, an allocator hook an embedder controls, signal-mask
correctness, blocking-call escape hatches, and above all *testability*
under adversarial scheduling. The [example programs]({{ '/examples/' | relative_url }}) --
especially the from-scratch SQL engine -- are the proving ground: if a
real server cannot be written cleanly on libxtc, that is a bug in
libxtc, not in the server.

## The stance

Two commitments run through the whole project:

1. **Honesty over marketing.** Every claim in this manual is backed by a
   test or a runnable snippet; every gap is written down in
   [Known issues]({{ '/reference/known-issues/' | relative_url }}) rather than papered over. A
   documented limitation is worth more than an undocumented surprise.
2. **The default is safe; the escape hatch is explicit.** Shared-nothing
   processes, suspending I/O, and supervised failure are the defaults.
   Shared memory, blocking on a thread, and manual lock ordering are all
   available -- but you reach for them on purpose, and it shows in the
   code.

If you have written Go, Tokio, Seastar, or Erlang and wished you had it
in C, that is the gap libxtc fills. The next essay is about the
[choices and roads not taken]({{ '/philosophy/choices/' | relative_url }}) that gave it this shape.
