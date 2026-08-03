# libxtc bounded model checking (CBMC)

This directory holds **bounded model-checking harnesses** that verify
the hardest-to-keep-correct concurrent algorithms in libxtc against
their *actual C source* using [CBMC](https://www.cprover.org/cbmc/).

## Why

Some algorithms are correct by a subtle argument that a future edit can
silently break -- a lock-free deque, a left-right lock, epoch
reclamation, a wake/park latch.  Unit tests and even DST exercise
*schedules that happened to run*; a bounded model checker exhaustively
explores *every* interleaving up to a bound and proves a safety
property holds on all of them (or hands back a concrete counterexample
trace).  This is the same tier Berkeley DB added in
`test/cbmc` -- model the invariant, check the real code, and any drift
from the intended algorithm is caught deliberately.

Each harness states, in a comment at the top, the exact invariant it
proves and the bound it proves it to.  A green run means: *within the
bound, no interleaving violates the invariant.*  It is not a proof for
unbounded thread counts, but for these algorithms the interesting races
are exposed at 2-3 concurrent actors, which is well within reach.

## What is modelled (each maps to a real source algorithm)

| harness                | source under test            | invariant |
|------------------------|------------------------------|-----------|
| `deque_harness.c`      | `src/inc/deque.h` (Chase-Lev)| no task lost or duplicated across owner push/pop + concurrent steal |
| `lwlock_harness.c`     | `src/ptc/lock_lw.c` model    | mutual exclusion: never two writers, no deadlock |
| `lrlock_harness.c`     | `src/ptc/lock_lr.c` model    | left-right: a reader never blocks; writer sees a quiescent side |
| `rcu_harness.c`        | `src/ptc/rcu.c` model        | a reader in a read-side never observes a reclaimed object |
| `refcount_harness.c`   | proc teardown refcount       | resolver pins a live object OR sees gone -- never a UAF |
| `wakepark_harness.c`   | loop.c prepare/park latch    | the RUNNING->PARKED wake race never loses a wake |
| `mpsc_harness.c`       | proc mailbox MPSC            | no message lost/duplicated; per-sender FIFO |
| `credit_harness.c`     | `src/orc/credit.c` window    | in-flight count never exceeds the window |
| `seqlock_harness.c`    | bufmgr OLC version seqlock   | a reader never accepts a torn (mid-write) sample |
| `chash_resize_harness.c`| chash grow/shrink           | no key lost; table stays single-valued across a resize |

## Running

```sh
make -C build_unix cbmc            # or: cd test/cbmc && ./run.sh
```

Requires `cbmc` on PATH (`nix-shell -p cbmc`).  Not part of the default
`make check` (CBMC runs are minutes, not seconds); it is a separate
verification tier + a release gate, like `make check-dst`.

## Reading a failure

CBMC prints a concrete trace: the exact interleaving + variable values
that violate the asserted invariant.  That trace is the bug -- either
the code drifted from the algorithm, or the algorithm was changed and
the harness's invariant must be updated deliberately (with a note on
why the change is sound).
