# libxtc bounded model checking (CBMC)

This directory holds **bounded model-checking harnesses** for the
hardest-to-keep-correct code in libxtc, checked with
[CBMC](https://www.cprover.org/cbmc/) (the University of Oxford / Diffblue
bounded model checker; libxtc writes only the harnesses and `run.sh`).

There are two kinds, and the difference matters:

- **Real-code harnesses** (`real/`) compile the library source unchanged
  -- `goto-cc` links the `.c` file, or the harness `#include`s it -- and
  stub only the allocator and OS calls.  They cannot drift from the code
  that ships.  They also run CBMC's memory-safety checks
  (`--bounds-check`, `--pointer-check`, signed overflow).
- **Modelled harnesses** (this directory) are for concurrent algorithms
  whose source drags in the executor, slab, RCU or fiber waits and does
  not compile standalone.  They model the algorithm.  Where a model
  copies statements from the source, the copy sits between
  `/* BEGIN SOURCE <path> <label> */` and `/* END SOURCE */`, and
  `check_drift.sh` (in `make check`, no CBMC needed) fails the build if
  `<path>` no longer contains those statements.  Harnesses without
  markers are faithful-in-intent models whose agreement with the source
  is NOT mechanically checked; see the "source?" column.

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
| `hlc_harness.c`         | `src/ptc/proc.c` HLC         | hybrid logical clock is monotonic + causal (update > remote stamp) under concurrent tick/update | u4 | drift-checked __hlc_tick/_update |
| `fdclaim_harness.c`     | `src/evt/task.c` fd-readiness claim | a stale readiness event never orphans a newer park of the same fd (2.1.2) | u3 | claim statement drift-checked |
| `wakeclaim_harness.c`   | `src/evt/task.c` __task_wake_claim + `loop.c` PENDING verdict | two wakers racing a park: no lost wake, enqueued at most once (the 7th/9th surfaces) | u3 | drift-checked |
| `inbox_harness.c`       | `src/evt/loop.c` lock-free inbox + sleep/nudge | never asleep with work queued and no wakeup; every pushed task drained once; one wakeup per sleep | u4 | drift-checked push/sleep_begin; stack index-encoded |
| `parktimer_harness.c`   | `src/evt/loop.c` timer fire vs `task.c` cancel | a fired park timer never releases a migrated fiber's newer timer; no double release, no orphan (the 2.0 bug) | u3 | drift-checked slot claim |
| `timeout_harness.c`     | `src/ptc/future.c` with_timeout | source vs timer fire: promise set once, context freed once, each owner retires once (the 1.55.1 leak) | u3 | drift-checked callbacks |
| `combinator_harness.c`  | `src/ptc/future.c` when_all / when_any | registration (which may fail) racing an input's fire: set once, freed once, the right status (2.1.2: a failed registration was ignored) | u3 | drift-checked callbacks + registration |

Real-code harnesses (`real/`, library source compiled unchanged):

| harness | source | property | planted bugs (must FAIL) |
|---------|--------|----------|--------------------------|
| `down_decode_harness.c` | `src/ptc/proc.c` xtc_down_decode{,_ex} | memory-safe on every length 0..40 and byte content, for any caller struct size; kind is one of the five documented | a decoder that copies `sizeof d` past a short message |
| `net_frame_harness.c` | `src/io/io_net.c` xtc_net_recv_frame | adversarial peer (any chunking, EAGAIN/EINTR/close): memory-safe, oversize frame -> XTC_E_RANGE never allocated, OK returns exactly the declared bytes, error leaves *out NULL | no max_len guard; an off-by-one chunk |
| `res_harness.c` | `src/ptc/res.c` acquire/release | no over-admission, no signed overflow (cap INT64_MAX), used never negative, conservation | DST bug 6 RESOVER, DST bug 5 RESLEAK, the 1.59 headroom wrap |
| `lockmgr_harness.c` | `src/ptc/lock_mgr.c` get/upgrade/downgrade | two lockers, 3 steps, S/X/IX/IS: no two lockers' granted entries conflict; get/upgrade hold at least the asked mode; downgrade only to a dominated mode | DST bug 2 LOCKEXCL; the 1.50 own-mode-only re-request; the 1.50 enum-order "stronger" |

Found by this tier and fixed: the fd-readiness claim ABA
(`fdclaim_harness.c`, 2.1.2) and the combinators' ignored registration
failure (`combinator_harness.c`, 2.1.2: when_all never resolved,
when_any leaked its context, with_timeout reported a completed source
as a timeout).

`real/res_harness.c` supersedes `res_harness.c`, which transcribes the
pre-1.59 release (fetch_sub then clamp); res.c has floored the
subtraction in one CAS since 8f85d0d2.  The old harness is kept until it
is retired deliberately, but it no longer describes the shipped code.

Every harness above VERIFIES SUCCESSFUL at the listed bound; each also has
a negative check (injecting the classic bug makes CBMC report the
counterexample) and a non-vacuity check (the asserted path is reachable).

## What this tier does NOT check

- **Memory ordering.**  CBMC interleaves threads under sequential
  consistency.  A store-buffering litmus test with `memory_order_relaxed`
  atomics verifies under `--mm sc`, `tso` and `pso` alike (measured on
  5.91.0), so a relaxed load or store that should have been acquire or
  release is invisible here.  The `memory_order_*` arguments the harnesses
  carry are there for the drift check, not because they are verified.
  The memory-order class is covered by TSan (`make check` under
  `-fsanitize=thread`, the CI TSan job), not by this tier.
- **Pointers shared between threads.**  CBMC 5.x refuses
  `--pointer-check` together with threads and refuses `_Atomic` pointers
  between threads ("pointer handling for concurrency is unsound").  The
  concurrent harnesses therefore index-encode pointer-valued words (a task
  or timer is an array index) and run `--bounds-check`,
  `--div-by-zero-check`, `--signed-overflow-check` and
  `--pointer-overflow-check` only.  The sequential real-code harnesses run
  `--pointer-check` too.

## Writing a harness that proves something

Three ways a harness here verified while proving nothing, each caught by
planting the bug it guards against:

- A file-scope variable is zero-initialised, not nondeterministic.
  `static int64_t CAP;` made every cap 0 (unlimited) and the cap assertion
  vacuous; use `nondet_*()` for inputs.
- CBMC ships models of some libc calls (`recv`, `poll`) that win over a
  definition in the harness.  Name the stub differently and swap the calls
  after linking (`goto-instrument --replace-calls recv:peer_recv`;
  `run.sh`'s `@RECV`).
- An unstubbed allocator returns success without setting the pointer.
  Stub every allocator the code under test calls.

So every harness carries planted bugs (`-DPLANT_*`, or the DST tier's
`-DXTC_DST_INJECT_BUG=<id>` for the real-code ones, so one switch breaks
both tiers) that must make CBMC FAIL, and `--cover location` is the
quick check that the asserted code is reached at all.
Where a harness models the algorithm rather than including the real source,
the top-of-file comment says so precisely and explains why the real source
does not compile standalone under CBMC (it drags in the executor, RCU,
slab, pthread/fiber waits, etc. that do not bear on the property) -- the
model is a faithful transcription of the same steps/atomics/ordering, so
drift from the intended algorithm is still caught.

Full-suite wall time is about 7 minutes on an idle 48-core box (cbmc
5.91.0): `credit`, `mpsc`, `lwlock`, `real/net_frame` and `real/lockmgr`
take 1-2.5 minutes each, the rest seconds.

## Deliberately NOT modelled

The L1 proportional-share scheduler and the L3 over-budget stall
watchdog (both in `src/evt/loop.c`, INSPIRED BY Glommio) have **no CBMC
harness on purpose**: they have no concurrent invariant to check.  The
run classes, their virtual runtimes and ready FIFOs, the min-vruntime
pick, the run-end accrual, and the stall check are all touched ONLY by
the loop's own owner thread -- the run queue is single-threaded by
design (see the `XTC_NOALLOC_BEGIN` owner-only contract).  A stolen task
carries its class TAG (an int) across loops, but the class STATE it
names is per-loop and read only by that loop's owner, so there is no
cross-thread race to bound-check.  The correctness that matters --
weighting is proportional to shares, vruntime is monotonic, the default
lane is never starved, replay is byte-identical -- is a single-threaded
ordering property, proved by the DST test (`test/sim/test_sim_sched_shares.c`),
the PBT (`test/pbt/pbt_sched_shares.c`), and the unit test
(`test/m14/test_stall.c`), not by interleaving exploration.

## Running

```sh
make -C build_unix check-cbmc      # or: make -C build_unix cbmc
# or directly:                       cd test/cbmc && ./run.sh
# one harness:                        ./run.sh mask_harness
# per-harness wall budget (seconds):  CBMC_TIMEOUT=1800 ./run.sh
```

Not part of the default `make check` (CBMC runs are minutes, not
seconds); it is a separate verification tier, like `make check-dst`.
The drift gate (`check_drift.sh`) is in `make check`.  CI runs the
tier in the `cbmc-nightly` job (nightly + `workflow_dispatch`), where
every harness must verify.

**CBMC 5.x is required** (validated: 5.91.0 and 5.95.1).  CBMC **6.x
cannot verify these harnesses at all**: it aborts with `pointer handling
for concurrency is unsound` *before* reaching a verdict on every
concurrent harness, so every harness would report a failure for a tool
reason indistinguishable from a real invariant violation.  `run.sh`
detects a 6.x on PATH and SKIPs with an explanation rather than emitting
14 false failures.

Note that `nix develop` (this repo's dev shell) provides whatever nixpkgs
ships, currently **6.x** -- so a plain `make check-cbmc` in the dev shell
SKIPs.  To actually verify, put a 5.x ahead on PATH; the simplest way:

```sh
nix shell github:NixOS/nixpkgs/nixos-23.11#cbmc \
    --command sh -c 'CBMC_TIMEOUT=1800 sh test/cbmc/run.sh'
```

(That pin ships 5.91.0.  A static 5.x binary can also be extracted from
the project's Ubuntu `.deb` release asset and run on any recent glibc.)

## Reading a failure

CBMC prints a concrete trace: the exact interleaving + variable values
that violate the asserted invariant.  That trace is the bug -- either
the code drifted from the algorithm, or the algorithm was changed and
the harness's invariant must be updated deliberately (with a note on
why the change is sound).

## Known harness issue: mask_harness (pre-existing, not a runtime bug)

`mask_harness` (cancellation masking vs a remote kill) reports a
spurious counterexample under cbmc 5.95.1: its `__CPROVER_ASYNC_1`
remote killer can commit its release-CAS AFTER `owner()` has run every
delivery point, so the model shows `fired == 1 && acted == 0` -- a kill
with no subsequent park to observe it.  That is a MODELLING artifact of
`__CPROVER_ASYNC_1` (which may schedule the async thread past the end of
the fiber's window), NOT a defect in proc.c: the real runtime always
reaches a next park (or its at-exit) that acquire-loads `kill_pending`,
so a masked kill is delayed, never dropped.  Sequencing the killer
before the owner's delivery points proves the masking LOGIC verifies
SUCCESSFUL (acted == 1 exactly), but that ordering also loses the
interleaving coverage that discriminates "defer while masked" from a
bug that acts while masked, so it is not a satisfactory fix.  A faithful
async model (bounding the killer to fire within an observed window)
is TODO; until then treat this one harness as advisory (it still
verifies at the bound in run.sh, which is why it stays in the list).
