# Critical review: libxtc against its claims

This document records a deep review of libxtc conducted against the
project's own claims, viewed through five lenses: a systems-kernel
maintainer's eye for correctness and simplicity; a database-engine
author's eye for durability and locking discipline; an actor-runtime
author's eye for the failure modes of message passing and
supervision; an async-runtime author's eye for scheduler fairness and
cancellation; and a shared-nothing-architecture author's eye for
per-core scaling and tail latency.

Every finding below is grounded in a specific file and line.  The
intent is not to diminish the work -- the foundation is strong and
the layering is disciplined -- but to find the gap between what the
README promises and what the code delivers, before a user does.

The findings are consolidated into a single ranked action list at the
end (section 7).

## 1. Correctness: where the code does not do what it claims

### 1.1 lrlock slot exhaustion is a use-after-free (critical)

`src/ptc/lock_lr.c` allocates a per-thread reader slot from a global
monotonic counter (`__global_slot_counter`, cap
`XTC_LRLOCK_MAX_GLOBAL_SLOTS = 4096`) cached in a `__thread` variable
(`__my_global_slot`).  The slot is never released on thread exit.

A long-lived process that creates and destroys more than 4096 distinct
threads over its lifetime -- a common pattern for servers that recycle
worker pools -- exhausts the counter.  After that, `__slot_for`
returns -1 and `xtc_lrlock_read_begin` takes its early-return path:
it announces nothing in the epoch bitmask.

The hazard is the split between `read_begin` and `read_data`.
`read_begin` is what announces the reader's epoch (the signal the
writer's drain-wait depends on).  `read_data` only loads `read_idx`
and returns the buffer.  When the slot is exhausted, `read_begin`
no-ops but a caller that still calls `read_data` (the pattern in
`examples/05_rexis/db.c:329`, where `db_read_begin` discards the
`read_begin` return and `read_data` is a separate call) receives a
live pointer into `data[idx]` with no epoch protecting it.  In COW
mode the writer calls `posix_madvise(MADV_FREE)` on the stale copy
after publish; the unprotected reader then reads zeroed or reused
pages.  This is a use-after-free reachable without any memory
corruption on the caller's part -- purely by thread churn.

### 1.2 The lrlock read API invites the same bug even without exhaustion

The two-call shape (`read_begin` to announce, `read_data` to fetch)
means any caller that calls `read_data` without a matching
`read_begin` on the same thread gets an unprotected pointer.  Nothing
in the type system or the function contract prevents it.  The safe
design returns the buffer pointer *from* `read_begin` and offers no
free-standing `read_data`, or makes `read_data` assert that the
calling thread holds an open epoch.

### 1.3 Integer overflow to heap overflow in message send (critical)

`xtc_send` (`src/ptc/proc.c:537`) computes `malloc(sizeof *e + size)`
with no check that `sizeof *e + size` does not wrap.  A `size` near
`SIZE_MAX` produces a small allocation, after which
`memcpy(e->data, data, size)` writes `size` bytes past it.
`xtc_svr_cast` (`src/orc/svr.c:323`) has the identical pattern with
`malloc(size + 1)`.  `xtc_mctx_calloc` (`src/ptc/mctx.c:216`) shows
the project already knows the correct idiom (`if (n != 0 && total / n
!= size) return NULL;`); the message paths simply omit it.

For a library whose consumers parse length fields off the wire and
hand them to `xtc_send`, this is a remotely reachable heap overflow.

## 2. Claims the README overstates

### 2.1 "No hidden allocations on the hot path"

`xtc_send` performs one `malloc` per message and the receiver performs
one `free`.  For an actor runtime, message passing *is* the hot path.
Links and monitors are slab-backed (`__link_slab`, `__mon_slab` in
`src/ptc/proc.c`), but the variable-size message envelope is not.  The
claim should either be softened to "no hidden allocations on the
scheduling hot path" or backed by a small-message envelope pool (a
slab for envelopes up to N bytes, falling back to malloc above that).

### 2.2 "Tier-1 platforms ... CI-tested every commit"

There is no continuous-integration workflow that builds and runs the
test suite.  `.github/workflows/` and `.forgejo/workflows/` contain
only `pages.yml`.  Nothing runs `make check`.  The claim is currently
false.  The lime sibling project ships a full `ci.yml`; xtc needs the
equivalent before the claim can stand.

### 2.3 "Run on Linux, FreeBSD, illumos, macOS, and Windows.  All five are supported with the same source."

Verified: Linux (glibc), FreeBSD, Windows (MinGW + Clang64).
Not verified: macOS (no host yet), AIX (no host yet).  Partial:
illumos (broke on the OpenSSL link in the last attempt), Windows MSVC
(blocked on the GAS-to-MASM asm port).  The honest statement is
"Linux, FreeBSD, and Windows are verified; macOS and illumos are
in progress."  PLAN.md section 21 already tracks this accurately; the
README should match it.

## 3. Database-author lens (durability, locking discipline)

The lock manager (`xtc_lockmgr`) is the strongest part of the tree:
the conflict matrix, locker/object model, and waits-for-graph deadlock
detector are faithful to the Berkeley DB design and the victim-policy
set is complete.  Two observations:

  * The Bitcask example (`examples/05_rexis/bitcask.c`) has no merge
    or compaction.  Dead bytes accumulate until the operator restarts
    with a fresh directory.  This is documented in the header, but a
    storage engine without compaction is a demo, not a component; if
    Bitcask is meant to be reused, compaction is table stakes.

  * `xtc_lrlock` writes are serialized by a single writer mutex.  That
    is correct and documented, but the COW path's `MADV_FREE` plus the
    1.1 finding means the durability story has a sharp edge: a reader
    can observe a freed page.  Fix 1.1 before promoting lrlock for
    anything resembling a buffer pool.

## 4. Actor-runtime lens (message passing, supervision)

  * The bounded mailbox (default 4096, rejects with `XTC_E_AGAIN` when
    full -- `src/ptc/proc.c:367`) is the right call and avoids BEAM's
    classic unbounded-mailbox OOM.  This is a genuine improvement over
    the system it emulates.  It must be paired with documentation that
    senders MUST check the return; a dropped `XTC_E_AGAIN` is a silent
    message loss.

  * The supervisor strategies are present, but the review found no
    test that exercises restart-intensity limits (the BEAM
    `max_restarts` / `max_seconds` shutdown that prevents a crash loop
    from pinning a core).  If it exists it is under-tested; if it does
    not, a crash-looping child can spin a supervisor indefinitely.

  * There is no backpressure between a fast sender and a slow receiver
    other than the mailbox cap.  Once the cap is hit, messages are
    dropped, not throttled.  For a Kafka-shaped workload (the proposed
    `kaka` example) this is the difference between a bounded queue and
    data loss; the example will need explicit credit-based flow
    control on top of the mailbox.

## 5. Async-runtime lens (scheduler, cancellation)

  * The idle-CPU busy-loop bug fixed earlier this cycle
    (`coro_uctx.c` / `coro_winfiber.c` returning RESCHED instead of
    PENDING when parked on a timer or fd) was exactly the class of
    scheduler bug that erodes trust in a runtime.  Its existence
    argues for a standing "idle process consumes ~0 CPU" regression
    test in `make check`, not just an ad-hoc verification.

  * `xtc_abort_source` exists but the review did not find it threaded
    through the call-style server APIs (`xtc_svr_call`).  A call that
    cannot be cancelled when its caller is torn down leaks a waiter.
    Tokio learned this lesson the hard way; cancellation must reach
    every await point.

  * Work-stealing (`src/evt/exec.c`, the deque) exists but fairness
    under a pathological all-on-one-core workload is untested.  A
    fairness regression here shows up as p99 cliffs, which is exactly
    the metric the README sells.

## 6. Shared-nothing lens (per-core scaling, tail latency)

  * The cache-line padding and per-CPU sharded counters (`xtc_stats`)
    are the right primitives.  But the lrlock global slot counter
    (1.1) is a single shared atomic incremented by every new thread;
    in a Seastar-style one-thread-per-core design with short-lived
    task threads, that counter is both a contention point and the
    source of the exhaustion bug.  A per-core slot table indexed by a
    shard id (the way Seastar indexes everything by `this_shard_id()`)
    removes both problems.

  * The conformance benchmarks (M17) compare against tokio and BEAM
    but, as noted in the bench RESULTS, the comparisons have been
    repeatedly mis-framed (async mutex vs raw mutex, escript VM
    startup).  A benchmark you cannot trust is worse than none.  The
    W4 parking_lot fix this cycle is the right direction; the rest of
    the suite needs the same scrutiny before any number is published.

## 7. Proposed actions (ranked)

Tier 0 -- correctness and security, before any release:

  A1. Fix the lrlock slot exhaustion use-after-free (1.1).  Move to a
      per-core / per-registered-reader slot table; release the slot on
      thread exit via a `pthread_key` destructor; make `read_begin`
      return the buffer pointer and either remove free-standing
      `read_data` or have it assert an open epoch (1.2).

  A2. Add overflow guards to every `malloc(fixed + n)` /
      `malloc(n + k)` on a caller-supplied size: `xtc_send`,
      `xtc_svr_cast`, and a grep-driven sweep for the rest (1.3).
      Return `XTC_E_INVAL` on wrap.

  A3. Add a standing security sweep to the test tree: a script that
      greps for unchecked additive/multiplicative allocation sizes,
      `strcpy`/`sprintf`/`strcat`, and format-string misuse, and fails
      `make check` on a new occurrence.

Tier 1 -- make the claims true:

  A4. Add a real CI workflow (build + `make check` + PBT) for the
      Tier-1 platforms, modelled on lime's `ci.yml`.  Until it exists,
      soften the README's "CI-tested every commit" (2.2).

  A5. Reconcile the README platform claim with PLAN.md section 21
      (2.3): state what is verified, what is in progress.

  A6. Either back the "no hot-path allocations" claim with a
      small-message envelope pool, or soften it to name the scheduling
      hot path specifically (2.1).

  A7. Add the regression tests the bugs imply: idle-process-CPU-near-
      zero (5), supervisor restart-intensity shutdown (4), lrlock
      reader/writer under thread churn (1.1), work-steal fairness (5).

Tier 2 -- durability and depth:

  A8. Thread `xtc_abort_source` through `xtc_svr_call` and every
      await point; add a test that a torn-down caller cancels its
      outstanding call without leaking a waiter (5).

  A9. DONE -- Bitcask has a compaction/merge pass (bitcask_compact);
      reusable, or relabel it explicitly as a demo (3).

  A10. Re-audit the entire M17 conformance suite for fair comparisons
       before publishing any number (6).

Tier 3 -- API ergonomics:

  A11. Document the mailbox-full contract prominently: senders MUST
       handle `XTC_E_AGAIN` (4).

  A12. Consider a per-shard API (`xtc_shard_id()`) so shared-nothing
       consumers can index per-core state the way Seastar does (6).

## 8. Overall assessment

The architecture is sound and the layering is honest: L0 through L5
each do one job, the OS layer is genuinely portable, and the lock
manager is a faithful, complete port of a design that has held up for
decades.  The bounded mailbox is a deliberate improvement over BEAM.
The wait-free lrlock read path is real (when slots are available).

The gaps are of two kinds.  The first is a small number of concrete
defects -- the lrlock slot use-after-free and the send-path integer
overflows -- that are squarely in day-zero-CVE territory and must be
fixed before anyone runs this in production.  The second is a
credibility gap: the README claims CI, full platform support, and
zero hot-path allocation that the code does not yet back.  None of the
second kind is hard to close; they are mostly a matter of doing the
work the claims describe, or trimming the claims to the work done.

This is a strong foundation on the right trajectory.  Closing the
Tier-0 items makes it safe; closing Tier-1 makes it honest; closing
Tier-2 and Tier-3 makes it the library the README describes.
