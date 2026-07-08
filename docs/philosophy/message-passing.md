---
title: Message passing and the compromise
parent: Philosophy
nav_order: 4
permalink: /philosophy/message-passing/
lede: >-
  Pure shared-nothing message passing is the ideal; libxtc strikes a deliberate compromise -- and that compromise is exactly why xtc and tnt both exist.
---

# Message passing and the compromise

## The pure ideal

The purest concurrency model is **shared-nothing message passing**: every
unit of work owns its state, nothing is shared, and units communicate
only by sending copies of data to each other's mailboxes. This is
Erlang's model, and it is beautiful, because it makes whole categories of
bug impossible by construction. There are no data races, because there is
no shared data. There are no locks, so there is no lock ordering to prove
and no deadlock to chase. A process can be reasoned about entirely on its
own, because nothing outside it can touch its state.

libxtc's default unit -- the [process]({{ '/guide/03-processes-and-messages/' | relative_url }})
-- is exactly this. Spawn, send, receive; state has one owner; messages
are copies. For most code, this is the right model, and you should reach
for it first.

## Why a pure model is not always enough

The pure model has one cost, and for some workloads that cost is
decisive: **copying.** When two units need to cooperate on a large or
hot shared structure -- a buffer pool, a page cache, an index that
thousands of readers hit -- shipping copies of it through mailboxes is
too expensive. Pure message passing turns a shared read into a
serialized request to the one process that owns the data, and now that
process is a bottleneck that no amount of cores can relieve.

The honest systems engineer admits this. Beautiful as shared-nothing is,
a foundational runtime that *only* offered it would be unusable for the
data-plane heart of a high-performance system -- the very place people
reach for C.

## The compromise libxtc strikes

libxtc's answer is a deliberate, marked compromise:

- **Shared-nothing is the default, and the easy path.** Processes,
  mailboxes, links, supervisors -- the whole model above -- is what the
  [Guide]({{ '/guide/' | relative_url }}) teaches and what most code uses.
- **Shared memory is available, but explicit and disciplined.** For the
  cases that genuinely need it, libxtc ships real
  [synchronization]({{ '/reference/locks/' | relative_url }}): left-right
  locks and RCU for latch-free reads of hot shared structures, a
  deadlock-detecting lock manager for ordered multi-lock access. These
  are not the default; you reach for them on purpose, and it shows in the
  code -- the same "the escape hatch is explicit" principle as the
  blocking-pool.

You get the safety of message passing where it suffices and the
performance of shared memory where it is unavoidable, with a clear line
between them rather than shared mutable state leaking in everywhere by
habit.

## Why this is the reason xtc and tnt both exist

The compromise runs deeper than "processes plus optional locks." It is
the reason libxtc offers *two* concurrency substrates at all.

**`xtc` -- the fiber-and-process model** -- optimizes for the general
case: units with non-trivial, linear logic, a moderate-to-large
population, straight-line code that is easy to write and read. A fiber
owns a stack; a process owns state and a mailbox. This is where the
shared-nothing default and the explicit-shared-memory escape hatch live.

**`tnt` -- the [Isolate layer]({{ '/tnt/' | relative_url }})** -- optimizes
for the opposite extreme: enormous populations of tiny, uniform entities,
where the per-fiber stack itself is the binding cost. Isolates are
stackless state machines in dense arenas; a whole shard shares one fiber.

Both are message-passing at heart. The split exists because **there is no
single point on the spectrum that is right for every workload.** A pure
model would force one answer; libxtc instead names the trade-off
explicitly and gives you the substrate that fits -- fiber-per-unit when
clarity and generality matter, stackless-Isolate when sheer count and
per-entity footprint matter. The [decision table]({{ '/tnt/' | relative_url }})
on the Isolate page is exactly the "which compromise do I want here"
question made concrete.

That is the through-line of libxtc's philosophy: **do not pretend one
model wins everywhere.** Make the default safe, make the escape hatches
explicit, and give the engineer the honest choice.

---

&larr; [Let it crash]({{ '/philosophy/let-it-crash/' | relative_url }}) &middot;
Back to [Philosophy]({{ '/philosophy/' | relative_url }})
