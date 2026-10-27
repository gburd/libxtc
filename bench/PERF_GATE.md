# Performance regression gate

libxtc guards its performance release over release with three tiers.
Each one catches what the tier below cannot.

| tier | what | where | when | decides by |
| --- | --- | --- | --- | --- |
| 1. work counts | context switches per lock acquire, page fixes and descents per lookup, fsyncs per commit, allocations per message | `make check`, sqlxtc `make test` | every commit, CI | an exact or bounded count (deterministic) |
| 2. micro + 3. macro timings | `scripts/perf-gate.sh`: the hot paths below, built from source for the previous release and the candidate | one dedicated, measurement-grade host | before every release tag | a live A/B, see "The rule" |

## Why work counts gate CI and timings do not

CI runners are small shared virtual machines, where timings vary by more
than any regression worth catching.  A count does not vary.  The 1.59
lock work shows the pairing: `test_sync /offloop_no_herd` asserts the
waiters' summed voluntary context switches (5,811-11,048 before the fix,
207-209 after, bound 800), so a change that brings back the thundering
herd fails `make check` on any machine.  Its throughput consequence
(1.4-12x on a 16-vCPU host) can only be measured on dedicated hardware.
When a change makes something cheaper, add a count test next to its
timing claim, so the saving cannot quietly disappear later.

## Running the release gate

```sh
scripts/perf-gate.sh v1.58.0 HEAD /var/tmp/perf-1.59
```

It builds both refs from `git archive` into the output directory, runs
every cell `REPS` times (default 5), alternating which side goes first
on each repetition, and prints a verdict table.  Exit codes: 0 means no
regression, 1 a regression, 2 a build or setup error, and 3 that the
host is not measurement-grade.  A full run takes about 45 minutes on a
64-vCPU host.  `CELLS="hop_cross_16x64 lock_amutex_w4"` runs a subset.

Results go to the output directory, never into the repository:
`results.csv` (one row per repetition), `summary.txt` (the verdict) and
`host.txt` (CPU model, cores, kernel, compiler, idle states, governor,
the two revisions).  Quote `summary.txt` and `host.txt` in the release
notes' qualification section.

## Why there is no scheduled CI timing job

A weekly "perf-gate-lite" on a CI runner (the micro and hop cells, which
need no idle-state control) was considered and not added.  A runner is
a fresh shared virtual machine each run, so a weekly number compares
two different machines; the same cells moved by up to 5x between hosts
during 1.59's qualification, and a +70% measured on 16 vCPUs was -45%
on 64.  An A/B inside one run (both refs built and interleaved, as
perf-gate.sh does) cancels the host, but on a shared runner its spread
is wider than the 5-10% thresholds, so the job could only ever report,
never decide -- and a number nobody can act on is noise.  The timing
gate stays a release-time manual tier on one measurement-grade host;
CI keeps the work-count tier (exact counts, host-independent).

## The rule

A cell regresses when both of these hold:

1. the candidate's median is worse than the base's by more than the
   cell's threshold (5% for most cells, 10% for the bench_micro cells,
   whose single-threaded nanosecond timings are noisier); and
2. the ranges do not overlap: every candidate repetition is worse than
   every base repetition.

Rule 2 stops one-sided noise (an interrupt, a page fault storm) from
failing the gate.  Rule 1 stops a real but negligible shift from failing
it.  With 5 repetitions an unchanged build passes rule 2 for a given cell
by chance with probability 2/252, about 0.8%, so a regression verdict is
worth investigating every time; rerun the cell (`CELLS=...`) before
calling it.

A regression that is understood and decided on (a trade-off the release
makes on purpose) is passed by name: `PERF_GATE_ACCEPT="lock_amutex_w64"`.
The verdict still prints it as `ACCEPTED regression`, and the release
notes must name it with its cause.  There is no way to accept a cell
silently.

The gate compares against the previous release live, never against a
stored number.  A stored baseline goes stale when the host, kernel,
compiler or idle-state settings change, and during the 1.59
qualification those changes moved the same binary's results by up to
5x on one host.  Building and running both sides on the same host
cancels them.

## A measurement-grade host

The gate refuses to run (exit 3) unless:

- every CPU idle state deeper than C1 is disabled.  A wake that lands
  on a CPU in C6 costs 80-130 us instead of 2-3 us.  On one 64-vCPU host
  this made a lock change look 35-50% slower when it was 24-46% faster:

  ```sh
  for s in /sys/devices/system/cpu/cpu*/cpuidle/state[2-9]/disable; do
      echo 1 | sudo tee "$s" > /dev/null
  done
  ```

- the cpufreq governor, where the host has one, is `performance`;
- the 1-minute load average is below 1.

`PERF_GATE_FORCE=1` runs anyway and marks every result `unqualified`.
Use that only to debug the gate itself.

## The guarded cells

| cell | path | why it is guarded |
| --- | --- | --- |
| `micro_fiber_switch`, `micro_proc_send_recv` | fiber switch, same-loop message | the scheduler's cost floor; glibc ucontext versus fcontext changed it 3.7x |
| `micro_timer_set_fire`, `micro_slab_alloc_free`, `micro_amutex_lock`, `micro_chan_mpsc`, `micro_deque_steal`, `micro_chash_get` | the bench_micro hot paths | each a primitive every program uses |
| `hop_same_2x64`, `hop_cross_2x64`, `hop_cross_16x64` | `bench_hop`: request/reply between loops | cross-loop messaging; per-slab statistics on one cache line capped it at 3.3M hops/s, and a fix moved the cost onto a kernel lock instead |
| `lock_amutex_w4`, `lock_amutex_w64`, `lock_arwlock_mix16`, `lock_arwlock_r64` | `bench_sync_offloop`: plain threads on one lock | off-loop locks at low and high thread counts; one 1.59 change won at 64 threads and lost 4-9% at 4 |
| `timer_scale_8` | `bench_timer_scale`: 8 loops arming and cancelling | the timer heap under load |
| `exec_spawn_8` | `bench_exec_scale`: spawn throughput, 8 loops | task creation and work stealing |
| `wal_fiber_c8`, `wal_thread_c1` | sqlxtc `bench_wal`: group commit | the durable-commit path |

A bench that the base release predates (for example `bench_hop` before
1.59) is reported as "only one side measured this cell: not judged", and so
is a cell whose bench reads zero on one side (for example the WAL
cells against 1.58, whose WAL has no thread-mode commit path).

To guard a new path, add one line to `cells()` in `scripts/perf-gate.sh`:
the name, 1 if higher is better (throughput) or 0 if lower is better
(latency), its threshold, the command, and an awk program that prints the
one number to compare.
