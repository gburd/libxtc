# sqlxtc / libxtc performance ideas

Collected while building the MVCC engine and the benchmark harness.
Ordered roughly by expected impact.  None are implemented yet; this is
the discussion list the benchmarking was meant to produce.

## Already fixed (for the record)

  * **Cross-loop lost-wakeup race** (`__do_recv`).  Arming the receive
    waker after releasing the mailbox lock left a window in which a
    cross-thread sender saw no armed waker and the receiver stalled to
    its timeout.  Fixed (commit 691726f): arm under the lock that
    confirms the mailbox empty.  25x multi-core throughput, multi-second
    tail latency eliminated.  THE lesson: micro-benchmark the
    cross-loop path early; correctness tests hid it.

## High impact -- the per-op cross-loop RPC is the bottleneck

  1. **Co-locate a client with the shard that owns its data.**  Today a
     client on loop i issues an op against a key owned by loop j, so
     every op is a cross-loop send + park + wake + reply + park + wake
     (two `io_wakeup` syscalls).  Route a session to the loop that owns
     the keys it touches (or partition clients by shard) so the common
     case is a same-loop call -- no cross-loop hop, no syscall.  This is
     the Seastar discipline and should be the single biggest win for
     small ops.
  2. **Per-loop (sharded) transaction coordinators.**  All writes funnel
     through one 2PC coordinator proc, which both serializes writes and
     adds a cross-loop hop.  Give each loop a coordinator (or make the
     client itself drive 2PC) so write throughput scales with cores.
     Cross-shard commits still need a global ordering point, but
     single-shard commits (the common case) should never touch a
     central coordinator.
  3. **Batch operations per message.**  Amortize the per-message cost
     (alloc + framing + wakeup) over many ops: a client sends a batch
     request, the shard applies all and replies once.  Turns N
     round-trips into one.  Real SQL statements are already "batches"
     of storage ops, so this falls out naturally once the SQL engine is
     on the engine.

## Medium impact -- messaging mechanics

  4. **Coalesce / cheapen cross-loop wakeups.**  Each cross-loop waker
     does an `io_wakeup` (eventfd write) syscall.  Under load, many
     wakeups to the same loop could coalesce (one token already pending
     -> skip the write), and a futex- or atomic-flag fast path could
     avoid the syscall when the target loop is known to be spinning.
  5. **Avoid the per-reply malloc in `xtc_svr_call` / the reply path.**
     Each call allocates a reply buffer (and the request envelope).
     A small-reply inline path or a per-loop slab would cut allocator
     traffic on the hottest path.
  6. **`xtc_proc_wait_fd` arms its waker after releasing the lock** --
     the same shape as the bug just fixed.  Audit whether its
     fd-primary semantics are actually exposed to the race; fix or
     document.

## Engine-level

  7. **O(1) version-chain insert.**  The MVCC version array shifts all
     entries on insert (O(n)); a newest-pointer ring or a small
     linked stack makes insert and prune O(1).
  8. **Cache the HLC physical read.**  `hlc_tick` calls
     `clock_gettime(CLOCK_MONOTONIC)` per op (a vDSO call -- cheap but
     not free); a per-loop cached "recent now", refreshed on the loop's
     timer tick, would remove it from the op path.
  9. **Wait-free local reads via RCU.**  A read that lands on its own
     loop's shard could read a published snapshot structure (`xtc_rcu`)
     without a message at all -- the highest-value read optimization
     once clients are co-located (idea 1).
  10. **NUMA-aware shard placement** on many-core / multi-socket: pin
      shard i and its data to the NUMA node of loop i.

## Measurement / methodology

  11. **A Quack HammerDB Tcl driver** (or a TPC-C generator speaking
      both Quack and embedded SQLite) so the networked path is measured
      alongside the embedded one, and against SQLite under equal core /
      cache constraints.
  12. **Larger-than-RAM workloads** once the buffer-pool-backed storage
      carries the versioned data: working set several times the pool
      cap, SQLite held to an equal `cache_size`, to measure eviction +
      I/O behavior and p99 under paging.

## The structural point

For trivial ops the libxtc message-passing model is messaging-bound:
its strength is isolation, predictability, and bounded tail latency,
not raw small-op throughput versus a single-threaded in-process call.
The wins above (co-location, batching, bigger ops) move real workloads
off the messaging-bound regime; the SQL layer, whose statements are far
heavier than a hash lookup, is where the model should shine on
throughput while keeping the low, predictable p99 that is the actual
goal.
