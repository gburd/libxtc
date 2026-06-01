# Causality in sqlxtc: version vectors, dotted variants, and what we actually need

The question raised: rather than timestamps for causality, why not dotted
version vector sets (DVVSet)?  Is there an improvement worth adopting?

This note answers it, and -- importantly -- separates two problems that
"causality" conflates, because sqlxtc needs different tools for each.

## Two different problems wearing one word

1. **Ordering for isolation (MVCC).**  A transaction must read a
   consistent snapshot and commit at a point that respects what it
   read.  On ONE machine this is a *total order* problem: pick commit
   timestamps so that the serialization order is well defined.

2. **Conflict detection across replicas.**  Two clients update the
   same key without having seen each other.  You must detect that they
   are *concurrent* (neither happened-before the other) and either
   keep both versions or merge them.  This is the problem version
   vectors and DVVSets solve.

sqlxtc today is single-machine and sharded.  Problem 1 is real now;
problem 2 only appears when sqlxtc replicates across nodes.  The right
answer differs by stage.

## Why not plain version vectors

A version vector (VV) maps actor -> counter and gives a partial order
(happens-before / concurrent).  Two well-known failures:

  * In a client/server KV store the VV is keyed by *client*, so it
    grows without bound as clients come and go.  Riak hit this.
  * A server that handles a write on behalf of a client cannot cleanly
    attribute the new event to itself without conflating it with the
    client's causal past -- the "sibling explosion" problem.

## Dotted version vectors and DVVSet (the fix, and its limits)

Dotted Version Vectors (Preguica, Baquero, Almeida, Fonte, Goncalves,
2010) add a *dot*: a single (actor, counter) pair that names THIS
update, kept separate from the causal-context VV.  A **DVVSet** packs
all concurrent versions of one key into one compact container whose
size is bounded by the number of **replicas (servers)**, not clients.
This is the correct, modern tool for **per-key concurrent-version
tracking in a multi-replica store**, and it is what Riak adopted.

Its scope, precisely: DVVSet shines when (a) you have multiple
replicas of the same key, (b) writes can happen at different replicas
without coordination, and (c) you must reconcile siblings later.  That
is a Dynamo-shaped, eventually-consistent system.

## sqlxtc is not (yet) Dynamo-shaped

The sharded design in `M_SQLXTC_SCALEOUT.md` gives every key a *single
owner shard*.  Within a shard there is one writer order.  Across
shards, a coordinator serializes a transaction.  There is no
multi-master replication of a key, so there are no siblings to
reconcile -- which is exactly the situation DVVSet exists for.  Adding
DVVSet now would be machinery for a conflict that the architecture
prevents by construction.

What sqlxtc actually needs at each stage:

  * **Now (single node, sharded):** a monotonic commit-order source so
    MVCC snapshots are well defined.  The demo's central allocator is
    the crudest form; it is a scaling chokepoint and -- as the sharded
    test made visible -- a *logical* order that does not survive as an
    *arrival* order across cores.  The shard must order by the stamp,
    not by arrival.

  * **The improvement to adopt now: a Hybrid Logical Clock (HLC).**
    (Kulkarni, Demirbas, Madappa, Avva, Leone, 2014.)  An HLC is a
    64-bit stamp combining a physical-time high part with a logical
    low part.  It is monotonic, close to wall-clock (so it is
    human-readable and usable for TTLs and debugging), and it captures
    happens-before across messages: a proc bumps its HLC past any
    stamp it receives.  Each shard keeps its own HLC and advances it on
    every message -- *no central allocator*, so the chokepoint is gone
    -- yet two events with HLC a < b are guaranteed not to be a
    happens-after-b-then-a inversion.  This is what CockroachDB,
    YugabyteDB, and MongoDB use for exactly this single-cluster
    MVCC-ordering job.  It is strictly better than the central
    sequencer for sqlxtc: decentralized, causal, and timestamp-shaped.

  * **Later (multi-node replication):** keep HLC for cross-node commit
    ordering AND add **DVVSet per replicated key** for sibling
    detection on the eventually-consistent path (e.g. an async read
    replica that takes local writes).  HLC and DVVSet compose: HLC
    orders, DVVSet detects true concurrency.  This is the Riak-2.x +
    Cockroach-HLC combination, and it is the honest end state if
    sqlxtc ever replicates.

## The improvement beyond DVVSet worth knowing: Interval Tree Clocks

There is a genuinely newer idea than DVVSet for the actor case:
**Interval Tree Clocks** (Almeida, Baquero, Fonte, 2008).  An ITC is a
causality stamp with three operations -- fork, join, event -- designed
for systems where the set of participants is *dynamic*: a stamp can be
forked when a process spawns and joined when it dies, so the causal
identity space is allocated and reclaimed automatically.  A
fixed-width version vector cannot do this; it leaks an entry per actor
that ever existed.

ITC is the right shape for **an actor runtime where procs are created
and destroyed constantly** -- which is precisely libxtc.  It is almost
certainly overkill for sqlxtc's *data* path (HLC wins there), but it is
the elegant tool for the *control* path: **causal tracing of messages
across a dynamic set of procs** (see below), where you want a compact,
fork/join-aware happens-before stamp rather than an ever-growing
vector.

## The connection the question half-implies: causality is also a debugging tool

There is a second reason to care about causality, and it ties directly
to the observability work (`M_OBSERVABILITY.md`).  Erlang's
`seq_trace` propagates a small trace token along message sends so an
operator can reconstruct the *causal chain* of one request as it fans
out across processes -- the BEAM's built-in distributed trace.  The
same machinery that orders events for MVCC can order events for
*tracing*:

  * Ride an HLC (or an ITC stamp) in every xtc message envelope.
  * On receive, a proc advances its clock past the message's stamp.
  * A trace records (stamp, from, to, kind); sorting by the causal
    stamp reconstructs the true happens-before of a request across
    procs and cores, even when wall-clock arrival order lies (as the
    sharded test's 59-75 cross-core reorderings showed).

So the causality mechanism is dual-use: HLC for data-path MVCC
ordering, and a causal token in the envelope for control-path tracing.

## Recommendation

  1. Drop the central timestamp allocator for MVCC; adopt a **Hybrid
     Logical Clock per shard**.  Decentralized, causal, timestamp-shaped,
     no chokepoint.  This is the concrete next step on the data path.
  2. Add a **causal trace token** (start with the HLC; consider an ITC
     stamp if proc fork/join causality becomes interesting) to the
     message envelope, feeding the seq_trace-style causal tracer in the
     observability plan.
  3. Defer **DVVSet** until sqlxtc actually replicates a key across
     nodes; at that point adopt HLC-for-order + DVVSet-for-siblings,
     the proven combination.  Building it earlier is solving a conflict
     the sharded design does not have.

In one line: version vectors and DVVSet answer "did these two updates
conflict?", which a single-owner sharded store never asks; HLC answers
"in what order, respecting causality?", which it asks on every commit
-- so HLC now, DVVSet only if we replicate, and ITC as the elegant
option for fork/join-aware causal *tracing*.
