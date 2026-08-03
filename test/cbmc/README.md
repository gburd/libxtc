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

| harness                | source under test            | invariant | bound | source? |
|------------------------|------------------------------|-----------|-------|---------|
| `deque_harness.c`      | `src/inc/deque.h` (Chase-Lev)| no task lost or duplicated across owner push/pop + concurrent steal | u4 | REAL header |
| `lwlock_harness.c`     | `src/ptc/lock_lw.c`          | mutual exclusion: never two writers; no shared holder while exclusive held | u4 | verbatim CAS core |
| `credit_harness.c`     | `src/orc/credit.c` window    | in-flight count never exceeds the window | u8 | faithful model |
| `seqlock_harness.c`    | bufmgr OLC version seqlock   | a reader never accepts a torn (mid-write) sample | u6 | REAL bm_read_begin/valid |
| `mpsc_harness.c`       | proc mailbox MPSC            | no message lost/duplicated/fabricated; per-sender FIFO | u5 | verbatim splice, index FIFO |
| `lrlock_harness.c`     | `src/ptc/lock_lr.c`          | writer mutates only the inactive side; reader sees a stable snapshot | u6 | faithful model |
| `rcu_harness.c`        | `src/ptc/rcu.c`              | a reader in a read-side never observes a reclaimed object | u6 | faithful model |
| `refcount_harness.c`   | proc teardown refcount       | resolver pins a live object OR sees gone -- never a UAF | u6 | faithful model |
| `wakepark_harness.c`   | `src/evt/loop.c` park latch  | the RUNNING->PARKED wake race never loses a wake (v1.8.0) | u6 | faithful model |
| `chash_resize_harness.c`| `src/ptc/chash.c` resize     | no key lost; table stays single-valued across a resize | u6 | essential model |
| `res_harness.c`         | `src/ptc/res.c` accountant   | `used` never exceeds `cap` under concurrent acquire (no over-admission) | u3 | verbatim CAS core |
| `hlc_harness.c`         | `src/ptc/proc.c` HLC         | hybrid logical clock is monotonic + causal (update > remote stamp) under concurrent tick/update | u4 | verbatim __hlc_tick/_update |

Every harness above VERIFIES SUCCESSFUL at the listed bound; each also has
a negative check (injecting the classic bug makes CBMC report the
counterexample) and a non-vacuity check (the asserted path is reachable).
Where a harness models the algorithm rather than including the real source,
the top-of-file comment says so precisely and explains why the real source
does not compile standalone under CBMC (it drags in the executor, RCU,
slab, pthread/fiber waits, etc. that do not bear on the property) -- the
model is a faithful transcription of the same steps/atomics/ordering, so
drift from the intended algorithm is still caught.

Full-suite wall time is about 3.5 minutes on an idle 16-core box; `mpsc`
(~2 min) and `credit`/`lwlock` (tens of seconds) dominate, the rest are
sub-second.

## Running

```sh
make -C build_unix cbmc            # or: cd test/cbmc && ./run.sh
```

Requires `cbmc` on PATH (`nix-shell -p cbmc`).  Not part of the default
`make check` (CBMC runs are minutes, not seconds); it is a separate
verification tier + a release gate, like `make check-dst`.

**Use a CBMC 5.x release** (e.g. 5.95.1).  CBMC 6.x aborts with
`pointer handling for concurrency is unsound` (exit 6) on the
concurrent-pointer harnesses; 5.x emits the same note as a warning and
completes to a verdict.  A static 5.x binary can be extracted from the
project's Ubuntu `.deb` release asset and run on any recent glibc.

## Reading a failure

CBMC prints a concrete trace: the exact interleaving + variable values
that violate the asserted invariant.  That trace is the bug -- either
the code drifted from the algorithm, or the algorithm was changed and
the harness's invariant must be updated deliberately (with a note on
why the change is sound).
