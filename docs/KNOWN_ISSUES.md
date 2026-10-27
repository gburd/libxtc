---
title: Known issues
parent: Reference
nav_order: 7
lede: >-
  Honest caveats, workarounds, and the platform-verification status.
permalink: /reference/known-issues/
---
## CHANGED (2.0): every caller-allocated public struct is size-first

**What changed.**  Source and binary incompatible with 1.x, by design
(libxtc 2.0 is the first LTS candidate; nothing consumes 1.x structs in
the wild).  Each struct below now starts with `size_t size`; initialize
it with the named macro (or a designated initializer that sets `.size =
sizeof(T)`).  A zeroed struct (`= { 0 }`, `memset`) is now REJECTED with
`XTC_E_INVAL` -- set the size.  Sizes are x86-64, v1.59.0 -> 2.0:

| struct | initializer | bytes | also |
|---|---|---|---|
| `xtc_proc_opts_t` | `XTC_PROC_OPTS_INIT` | 56 -> 56 | `link_to` removed (never read); regrouped |
| `xtc_proc_info_t` | `XTC_PROC_INFO_INIT` | 80 -> 88 | |
| `xtc_mailbox_stats_t` | `XTC_MAILBOX_STATS_INIT` | 48 -> 56 | |
| `xtc_down_info_t` | `XTC_DOWN_INFO_INIT` | 32 -> 40 | now has a struct tag |
| `xtc_runtime_info_t` | `XTC_RUNTIME_INFO_INIT` | 40 -> 48 | |
| `xtc_loop_info_t` | (library-sized) | 32 -> 40 | read newer fields under `XTC_FIELD_OK` |
| `xtc_loop_stats_t` | `XTC_LOOP_STATS_INIT` | 16 -> 24 | |
| `xtc_sup_opts_t` | `XTC_SUP_OPTS_DEFAULT` | 32 -> 40 | |
| `xtc_child_spec_t` | `XTC_CHILD_SPEC_INIT` | 48 -> 56 | arrays stride by element 0's size; all elements equal |
| `xtc_app_opts_t` | `XTC_APP_OPTS_DEFAULT` | 64 -> 40 | `sup` is now `const xtc_sup_opts_t *` (NULL = defaults), not embedded |
| `xtc_svr_callbacks_t` | `XTC_SVR_CALLBACKS_INIT` | 48 -> 56 | `terminate` before `handle_continue` again |
| `xtc_svr_opts_t` | `XTC_SVR_OPTS_INIT` | 16 -> 24 | |
| `xtc_fsm_callbacks_t` | `XTC_FSM_CALLBACKS_INIT` | 24 -> 32 | copied at start (1.x borrowed the pointer) |
| `xtc_fsm_opts_t` | `XTC_FSM_OPTS_INIT` | 16 -> 24 | |
| `xtc_launch_opts_t` | `XTC_LAUNCH_OPTS_INIT` | 24 -> 24 | `loop` removed (never read) |
| `xtc_tls_opts_t` | `XTC_TLS_OPTS_INIT` | 120 -> 120 | regrouped |
| `xtc_tls_transport_t` | `XTC_TLS_TRANSPORT_INIT` | 24 -> 32 | |
| `xtc_tcp_opts_t` | `XTC_TCP_OPTS_DEFAULT` | 32 -> 40 | |
| `xtc_aio_t` | `XTC_AIO_INIT` | 64 -> 64 | size must cover the whole 2.0 layout; regrouped |
| `xtc_fs_stat_t` | `XTC_FS_STAT_INIT` | 24 -> 32 | file size renamed `size` -> `bytes` |
| `xtc_slab_opts_t` | `XTC_SLAB_OPTS_DEFAULT` | 104 -> 112 | size must cover `name`, `obj_size` |
| `xtc_slab_stats_t` | `XTC_SLAB_STATS_INIT` | 96 -> 104 | |
| `xtc_res_caps_t` | `XTC_RES_CAPS_DEFAULT` | 48 -> 56 | `xtc_res_t` embeds it: 280 -> 288 |
| `xtc_log_opts_t` | `XTC_LOG_OPTS_DEFAULT` | 32 -> 40 | |
| `xtc_cfg_spec_t` | `XTC_CFG_SPEC_INIT` | 104 -> 112 | size must cover `name`, `kind` |
| `xtc_osproc_opts_t` | `XTC_OSPROC_OPTS_INIT` | 40 -> 48 | |

Unchanged on purpose: the frozen lock-layer structs, array element types
the library strides at its own `sizeof` (`xtc_io_event_t`,
`xtc_accel_dev_t`, `xtc_msg_t`, batch items/results, scheduler lanes),
value types (`xtc_pid_t` -- which a separate 2.0 change widens --
`xtc_pt_t`, `xtc_waker_t`, the trace/tail records), and the experimental
batch / scheduler-policy structs, which already carry `size, version`.

**Also new in 2.0** (`docs/abi-stability.md`): `XTC_FIELD_OK(p, field)`
in `xtc.h`; the `XTC_2.0` symbol version on every export
(`dist/libxtc.map`, gated by `test/m0/test_symbols.sh [C9]`); a
`@stability stable|experimental` tier on every installed header (gated
by `test/dist/test_stability_tiers.sh`; 9 headers are experimental); the
`XTC_DEPRECATED(msg)` macro (`xtc_export.h`); the struct-layout gate
`test/dist/test_abi_layout.sh` with its table `test/abi/layout.txt`; and
`scripts/abi-check.sh BASE_TAG` for release-time abidiff.

**Migrating.**  Replace `T v = { 0 };` / `memset(&v, 0, sizeof v)` by
`T v = T_INIT;` (or add `v.size = sizeof v;` after the memset); add
`.size = sizeof(T),` to designated initializers and to each element of a
child-spec array; rewrite positional callback initializers as designated
ones; `opts.sup.X = ...` becomes a separate `xtc_sup_opts_t` and
`opts.sup = &it;`; `st.size` of an `xtc_fs_stat_t` becomes `st.bytes`.

---
## OPEN (libxtc): at most 65,536 live processes per loop

**The limit.**  `xtc_pid_t` names a process by its loop and a 16-bit
slot on that loop (`local_id`), so one loop can hold at most 65,536 live
processes.  `xtc_proc_spawn` on a full loop returns `XTC_E_RESOURCE` and
spawns nothing.  Every process counts, supervisors included.  A server
that runs one process per connection therefore stops accepting at about
65,500 clients per loop.  rexis 1.59 stopped at exactly 65,532 clients
(65,536 minus its supervisor, listener, expiry and metrics procs) and
closed each connection after that without a reply.  On the same host,
with the same client harness, Valkey held 70,002, so neither the client's
ports nor the file-descriptor limits were the cause.

**Workaround.**  Spread the processes over the loops of an `xtc_exec`
executor: each loop has its own 65,536 slots.  rexis does this with
`--loops=N`.  It puts each connection on the least-loaded loop with room
and refuses startup when `--max-clients` is more than its loops can hold.
A client over the limit gets `-ERR max number of clients reached` and a
close, as Redis sends.  Measured: `--loops=2` held 100,000 idle
connections on one 32-vCPU x86-64 host.

**Why it stays open.**  `xtc_pid_t` is a public struct passed by value
(see [ABI stability]({{ '/reference/abi-stability/' | relative_url }})),
so widening `local_id` has to wait for 2.0.  See
[xtc_proc(3)](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_proc.3).

## CHANGED (2.0): `xtc_pg_send_owned`, process-group fan-out with one copy fewer

`xtc_pg_send` copies a `const` buffer once per member, so rexis
PUBLISH, which built the frame in a heap buffer, copied it again even
for a channel with one subscriber.  `xtc_pg_send_owned(reg, group, buf,
size)` takes an `xtc_malloc`'d buffer and moves it to the last member
(`xtc_send_owned`): N - 1 copies for N members, none for one.  The
buffer is the library's on return (moved, or freed if nobody took it).
Measured on a 32-vCPU x86-64 host, one loop, 100,000 messages, 5 runs:
one subscriber at 64 KiB 338k -> 685k messages/s, at 4 KiB 1.74M ->
1.94M; four subscribers at 64 KiB 121k -> 147k (with glibc's trim
threshold pinned; with the defaults the copying publisher's
allocate-free cycle page-faults on every message and runs at 15k).
rexis PUBLISH uses it.

## CHANGED (2.0): a kernel-TLS `xtc_tls_write` is one system call, not one per record

**Before.**  With kernel TLS TX active, `xtc_tls_write` went through
OpenSSL, whose kernel-TLS record layer issues one `write(2)` per 16 KiB
record: 16 system calls for a 256 KiB write, and 0.71x the user-space
TLS rate for plain writes (1329 vs 1837 MiB/s, `bench_tls_sendfile`,
32-vCPU x86-64 host, Linux 6.12, OpenSSL 3.5.7).  The kernel's AES-GCM
was not the cost (it uses the same vector instructions OpenSSL does),
nor the SIGPIPE bracket (0.1% of the sender's samples).

**Now.**  When OpenSSL has nothing of its own to send (handshake done,
no close_notify sent, no KeyUpdate pending, no `SSL_write` waiting to
be retried), `xtc_tls_write` writes the plaintext to the socket itself
and the kernel frames the records: one `write(2)` for 256 KiB
(`test_tls_ktls` /roundtrip asserts at most 4; before: 16).  Measured,
5 runs interleaved: 1593 MiB/s (1523..1681), sender CPU 0.639 s/GiB
(0.769 before), 0.87x user-space TLS.  A `write(2)` of the same
plaintext to the kernel-TLS socket with neither libxtc nor OpenSSL in
the path measures the same (1525 MiB/s, 1503..1796), so the remaining
gap is the kernel's.  A dead peer is reported as before (`XTC_E_IO`).
`xtc_tls_sendfile` is unchanged (2106 MiB/s) and remains the reason to
enable kernel TLS.

## CHANGED (2.0): `xtc_rand_u64` replays under the simulator; sqlxtc `random()`, `randomblob()`

**Before.**  A thread that never called `xtc_rand_seed` seeded its
stream from the clock and the address of its state cell, which no seed
reproduces, so under the deterministic simulator the determinism guard
trapped every such draw and a sim-reachable path could not use the
RNG.  sqlxtc declined `randomblob()` and `random()` for that reason.

**Now.**  Under the simulator an un-seeded thread draws from the run's
own stream (`XTC_SIM_RNG_RAND`, reset by every `xtc_sim_activate`), so
its values are a function of the run's seed and replay, the way
`xtc_clock_mono` is virtualized.  A thread that called
`xtc_rand_seed` keeps the sequence it asked for.  Off the simulator
nothing changed.  The RNG is no longer a guarded class:
`test_sim_determinism_guard` now asserts that an un-seeded draw does
not trip the guard and replays, and `test_sim_rand` checks replay over
8 seeds x 4 fibers on 2 loops.

sqlxtc implements `random()` (a 64-bit value, never -2^63, as SQLite)
and `randomblob(N)` (N random bytes, at least 1; N must be a constant
of at most 1 MiB, as for `zeroblob`).  A `random()` in a WHERE clause is
evaluated per row, never used as an index seek bound.  Both replay
under the simulator (`test_sim_sql_now`).  They are not cryptographic.

## FIXED (2.0): one hot `xtc_amutex` shared by many plain threads

**The problem (1.59).**  With 16-128 OS threads (not fibers) contending
for one `xtc_amutex`, throughput was up to 52% below 1.58 (perf gate
cell `lock_amutex_w64`, accepted for 1.59).  1.59 woke a sleeper on
every release, so the lock never sat free while a woken thread left a
deep CPU idle state (that had cost the sqlxtc ledger 35-50%), but on a
hot mutex the woken thread lost it to a spinning thread and slept again:
3.2 context switches per acquire.

**The fix.**  A release that frees the mutex while another thread is
spinning on it no longer wakes a sleeper: the spinner takes the mutex
within its spin, so the mutex is not left free.  At most 8 releases in
a row skip the wake, so a sleeper still gets a turn on a mutex that
always has a spinner.  `xtc_arwlock` is unchanged.  Measured on a
64-vCPU x86-64 host, 3 reps x 3 s interleaved, median acquires/s
(`bench_sync_offloop amutex w N`, the perf gate's arguments):

| threads | 1.58 | 1.59 | 2.0 | 2.0, C1E+C6 on |
| --- | --- | --- | --- | --- |
| 4 | 794k | 947k | 989k | 1,018k |
| 16 | 484k | 252k | 519k | 512k |
| 64 | 486k | 247k | 523k | 411k |
| 128 | 485k | 243k | 538k | 427k |

The first three columns ran with the deep idle states C1E and C6
disabled, as the perf gate requires; the last with them enabled (1.58
there: 491k / 383k / 326k at 16 / 64 / 128, 1.59: 252k / 213k / 204k).
Context switches per 1,000 acquires at 64 threads: 1.58 1,814, 1.59
3,249, 2.0 678.  The arwlock cells did not move.

**What it costs.**  The sqlxtc ledger (transfer and blend, 8 and 64
clients) is unchanged with the deep idle states off (every cell within
1%, ranges overlapping).  With C1E and C6 on, transfer at 64 clients is
4.5% lower (45.0k vs 47.1k txn/s, median of 10 runs each, ranges
overlapping) and blend at 64 2% lower: a sleeper whose wake was skipped
later needs ~100 us to come out of C6.  Acquire p99 at 64 threads is
1.2 ms (1.59: 1.4 ms, 1.58: 0.75 ms).

**If many plain threads must share one mutex, fibers are still faster.**
Fibers on one loop never block an OS thread: a contended fiber parks and
the release hands the mutex to it in the same thread (1.59, same host:
2.45-2.49M acquires/s at 4-64 fibers, `bench_amutex_fibers`).  Fibers
spread over many loops bring the cross-thread hand-off back, so
co-locate the fibers that share one hot lock on one loop, or shard the
lock.

## FIXED (1.60): sqlxtc SELECT prepares serialized on one lock; write-tail stalls

**Prepares.**  Every SELECT prepare ran under one process-wide lock, because a prepare's
correlated-subquery state lived in four file-scope variables.  A plan that keeps a scalar
subquery's value (`WHERE seq > (SELECT max(seq) FROM t) - 5000`) is prepared at every execution
and runs the subquery inside the prepare, so one such query at a time could run.  The ledger
blend mix fell from 135k to 34k transactions/s at 64 connections and swung between 70k and 197k
at 32.  The state is now per prepare and the lock is gone (test_prepare_concurrent).

**Write tail.**  Two sources of sqlxtc's open-loop write tail are fixed.  The page provider
cooled and wrote pages while the free list held its target, so with a pool larger than the
store it rewrote the whole store through the page cache, and the kernel's 30 s dirty expiry
flushed about 550 MB in one burst that the log's synchronous writes queued behind
(test_pp_idle_writes).  The flush leader zero-filled the log 8 MiB at a time inside its flush, a
roughly 3 ms pause once per 8 MiB of log; it is now 1 MiB (test_wal_zero_step).  Open-loop transfer at
half load, 64-vCPU host: p99.9 13-18 ms -> 0.6-0.8 ms.

**Still open.**  A thread that naps inside the commit path (a row-intent or checkpoint-gate
wait, 20-200 us) and is woken late by the host (measured: about 27 ms timer wake-ups on 2 of 64
vCPUs of a virtualized host, reproduced by a plain clock_nanosleep loop with no engine) stalls
the commits queued behind it: a 25-35 ms spike in some 30 s runs.

## FIXED (1.60): sqlxtc served a damaged page as committed data; CHANGED: page format

**What was wrong.**  After a clean shutdown sqlxtc trusts its page file
and skips the log, and data pages carried no checksum.  One bad byte in
the page file -- a latent sector error, a torn or misdirected write --
was returned as committed data (`test_page_checksum`: `ROW00123-AAAAAAAB...`).
A damaged right link or fence could also send a read into a loop:
`test_page_cksum_more`'s misdirected-write case hung on 1.59 until it
was killed (40 minutes and 2 h 11 min, in `move_right` <- `cursor_descend`).

**The fix.**  Every page carries a CRC32C and its own page id, written
on every write-back and checked on every read from disk, before the
page is used.  A page that fails is refused: the statement gets
`page checksum mismatch at pid N`, never the data, and the log
fail-stops as for a failed log write (later writes are refused; reads of
other pages work), so the damaged page file is never trusted again.
Restart the server: it rebuilds the page file from the log, which after
a clean shutdown holds the whole live tree.  A write that got this error
may still have committed (its log record is written before it is
applied): after the restart, read it back before retrying it.  A damaged superblock is
rebuilt the same way at open.  See the
[sqlxtc durability notes]({{ '/examples/sqlxtc/' | relative_url }}).
Not caught: a lost write (a page that still holds an older, intact
image of itself).

**CHANGED: the page format.**  The B-tree node header grew from 40 to
48 bytes, and the superblock says `XBT2` (was `XBT1`).  The largest value
kept inline on a 4 KiB page is 2 bytes smaller (1008 bytes of key and
value).  **Upgrade:** the first start of 1.60 on a 1.59 store rebuilds
the page file from the log (as slow as crash recovery, once); the log
format is unchanged.  **Downgrade:** 1.59 on a 1.60 store does the same
in reverse.  Downgrades are not supported (see
[ABI stability]({{ '/reference/abi-stability/' | relative_url }}),
"Downgrades"), but nothing is misread either way.  `sx_size` refuses
a 1.59 page file.

## FIXED (1.60): a damaged sqlxtc B-tree made readers spin forever

A descent through the B-tree restarts from the root when it meets a node
that a concurrent merge changed, and follows right links while the key
is past a node's upper fence.  Neither loop had a bound.  Concurrent
splits and merges finish, so on a sound tree both loops end.  On a
damaged one (a right-link cycle, a leaf whose fences no longer match its
parent, a page with one corrupted byte) they never ended: every reader
and writer routed to that page spun at 100% CPU and reported nothing.
Both loops are now bounded.  Past the bound the statement fails with an
internal error, and the first failure in the process prints the key,
the root page, the reason for the last restart and the page it was on
(id, flags, fences) to stderr.  A scan that hits the damage also fails,
with an error rather than an early end, so the online checkpoint does not
write a log image that is missing the rest of the tree.  No measurable
cost.  Test: test_btree_bounded_descent.

Seen once in about 400 runs of a ledger benchmark in the 1.59 cycle
(lifecycle mix, 16 clients, on an experimental lock build that was never
released): 12 client threads spun inside the descent for 537 s.  It did
not reproduce in about 2,300 further runs of that build or in 249 runs of
1.59 (with the new bound) at 16, 32 and 64 clients, and the cause is NOT
known.  The stack
traces need a page that was damaged and stayed damaged.  A single
flipped byte in the page file produces the same livelock.  If you see
the stderr report ("sqlxtc btree: ... gave up"), keep the store and the
report and file an issue: the page id in the report is where to look.
New DST coverage of scans racing merges and page-id reuse:
test_sim_btree_scan.

## FIXED (2.0): sqlxtc ALTER TABLE RENAME could corrupt the table's schema

Since ALTER TABLE RENAME was added (1.x, 2026-06), the rename read the
old catalog value into a stack buffer that was not NUL-terminated and
took the column list with strlen, so stale stack bytes could be
appended to the renamed table's schema.  When that happened, every
INSERT into the renamed table failed with "unsupported SQL (native
engine)", and the damaged schema was persisted.  Whether it happened
depended on the stack's contents (`test_cat_reopen` failed about 1 run
in 200 under CPU contention).  Fixed; the test now poisons the stack
before the rename and fails deterministically without the fix.  The
fix does not repair a schema an affected build already persisted (the
row data itself was never touched); such a table must be recreated.

## FIXED (2.0): the clean-close version GC could bring back a deleted sqlxtc row

Never in a release: introduced by the 1.60 candidate's clean-close
version GC (31d1ef8) and fixed before 2.0.  The version GC (xs_gc)
collects dead version keys in batches of 512 and restarted its per-row
state at every batch, so a batch that ended right after a deleted row's
tombstone treated that row's older versions as a new row and kept one:
the deleted row came back, without its secondary-index entries.  The
GC had always had the flaw; a GC pass rarely held 512 dead keys until
the clean close ran it over a whole store.  `test_crash_oracle` caught
it under CPU contention ("rows missing from an index" after an open);
unloaded it passed.  Fixed by keeping the row state across batches;
`test_close_prune` and `test_sim_sqlxtc_gcidx` (now with small GC
batches) fail without the fix, planted bug 23 (GCBATCH) is caught by
DST, and `test_sim_sqlxtc_gcidx 2 1` is pinned in the seed corpus.

## FIXED (2.0): a fired park timer could release a newer timer of a migrated fiber

**What was wrong.**  When a park timer fired, the loop cleared the
waiting fiber's timer slot unconditionally and dropped the reference it
found there.  A migratable fiber woken and stolen by another loop in the
window between the fire and that clear had already cancelled the fired
timer and armed a new one on the thief loop, so the firing loop dropped
the NEW timer's reference: that timer lost its owner (it was never
cancelled and woke the fiber spuriously later), and the firing loop read
its reference count with nothing ordering the read after its allocation
on the other thread.  ThreadSanitizer reported it in
`/m5/exec/Blk4_cross_loop_state_timer` under load: 14 of 40 and 40 of
100 runs on a 32-vCPU host.

**The fix.**  The fire clears the slot only if it still holds the fired
timer (a compare-and-swap) and otherwise leaves it to the fiber, which
already released it.  0 of 100 TSan runs.  `/m3/timer/Tm16_fire_clears_own_slot`
reproduces the interleaving deterministically on one loop.  It is not
reachable under DST: the simulator runs no fiber inside a timer drain.

## CHANGED (1.60): involuntary preemption on musl/aarch64; platform checks

* **musl on aarch64 now has involuntary preemption.**  1.59 compiled
  the aarch64 signal-context redirect only for glibc, so on musl (where
  fcontext is the only substrate) `xtc_preempt_set_involuntary(1)`
  silently fell back to cooperative ticks: a fiber that never reached a
  yield point was not sliced.  Nothing in the redirect is glibc-specific
  (it rewrites the kernel's own signal frame, whose layout musl's
  `<ucontext.h>` matches, and the trampoline makes raw system calls), so
  the gate is gone.  On a 16-vCPU aarch64 host with SVE, Alpine (musl
  1.2.6, gcc 15.2), `test_preempt`, `_p1`, `_p2` and `_ring` ran 200
  times each with no failure, `/a64_sve_state` and `/a64_x16_preserved`
  included; they used to skip.  `test_preempt_p2` now fails, rather than
  skips, on an aarch64 Linux build without the redirect.  x86-64 musl
  still declines (not yet verified there).
* **DTrace, verified on FreeBSD 15.1.**  An `--enable-dtrace` build's
  `gmake check` passes, and `test_probes` there checks the real thing
  (as root, or through passwordless `sudo`): `dtrace -l` lists all 30
  probes in a statically linked program, a D script counts the
  workload's events exactly, and every `tools/dtrace/*.d` script runs.
  Also checked by hand: the probes in `libxtc.so` (`--enable-shared`),
  `dtrace -p` on a program already running, and the numbers in the
  tracing guide's two worked examples.
* **Apple Silicon runs fcontext in CI.**  The `macos` job's runner is
  arm64, so `fctx_aarch64_aapcs_macho.S` is assembled and linked and
  the full test suite runs on it on every push.  The job now fails if
  the build selects anything else.

## CHANGED (1.60): io_uring submission is deferred to the loop's poll

**Status:** behavior change, io_uring backend only; measured on a 32-vCPU
x86-64 host, local NVMe, XFS, Debian 13 (Linux 6.12, liburing 2.9).

* `xtc_io_reg_fd`, `_mod_fd`, `_del_fd` and `xtc_io_aio_submit` used to
  enter the kernel once per submission queue entry.  They now leave the
  entry queued, and the next `xtc_io_poll` submits everything in the
  same `io_uring_enter` that waits (liburing 2.4 or later; with older
  liburing the poll submits, then waits).  A file AIO submit with none
  of its ring's AIO in flight still goes in at once, and a loop submits
  what is queued after a fiber yields and every 8 runs.  A zero-timeout
  poll with nothing queued and no file AIO in flight no longer enters
  the kernel.  See `xtc_io(3)` SUBMISSION.
* Who must change: only a caller that drives an `xtc_io_t` itself and
  waits for `xtc_aio_t.done` without calling `xtc_io_poll`.  That never
  worked portably (only a poll delivers the completion), and it can
  now hang on io_uring when other AIO of the same ring is in flight,
  because the operation is not submitted until the poll.
  `xtc_aio_*` and every library path poll; nothing in the tree did
  this.
* Measured against 1.59.0 (5 interleaved runs each, medians): the
  fiber switch (`bench_micro fiber_switch`) 156 -> 83 ns, because the
  loop's every-64-runs poll no longer enters the kernel when nothing is
  pending; spawn throughput (`bench_exec_scale 8`) +8%; 64 fibers of
  page-cache reads on 16 loops +12% IOPS at 10% less CPU per op;
  64 O_DIRECT readers on one loop with 5 us of work per read +11% IOPS,
  p99 -7%; 16 loops each doing an fdatasync, p99 79 -> 57 us.  SQEs
  per `io_uring_enter` (`scripts/uring-gate.sh`): aio_read 1.00 -> 1.75,
  rexis 0.50 -> 0.97, kaka 0.97 -> 2.42.
* Unchanged: WAL group commit (`bench_wal`), the sqlxtc I/O soak, the
  kaka soak and single-loop rexis throughput are within run-to-run
  spread: they are bound by the device and fsync, not by system calls.
* Cost: a fiber whose AIO is queued behind other AIO in flight waits up
  to 8 runs of other fibers to be submitted.  One loop with 32-64
  readers that compute 10-20 us between reads: p99 +2.5% to +4%.

## CHANGED (1.59): fcontext is the default on Linux aarch64 (glibc) too; aarch64 fixes

**What changed.**  Linux aarch64 with glibc now defaults to the fcontext
substrate, as Linux x86-64 does from 1.59 (`--with-coro=ucontext` keeps
ucontext).  glibc's aarch64 `swapcontext` costs more than on x86-64 (the
`rt_sigprocmask` call plus the FP/SIMD file): on a 16-vCPU aarch64 host
(Neoverse-V1) one fiber switch is 1091 ns on ucontext and 136 ns on fcontext,
and 16 loops of cross-loop request/reply run 2.08M round trips/s on ucontext
(down from 2.45M at 8 loops: the same process-wide signal-lock wall) against
8.91M on fcontext.

This is the first release qualified on aarch64.  Fixed on the way, each with a
test that fails before the fix:

* *FPCR / FPSR per fiber* (fcontext): `jump_fcontext` did not save them, so a
  fiber's `fesetround` or flush-to-zero leaked to the next fiber on the thread,
  as MXCSR did on x86-64 (`test_async /C10_fp_env_per_fiber`).
  Also on musl/aarch64 and Apple Silicon, where fcontext was already the
  default.
* *Migrating fibers* (fcontext): a migratable fiber that finished on another
  OS thread jumped to the first thread's scheduler, because the compiler kept
  the thread pointer in a register across the fiber body (`test_exec /Blk*`,
  `test_steal /M9` crashed).  Also live on musl/aarch64.
* *Involuntary preemption* (both substrates): the preemption trampoline lost
  x16 and the upper SVE register state (z, p, FFR) of a preempted fiber; glibc
  runs SVE `memcpy` / `memset` on SVE cores.  A preempted fiber now resumes
  through the kernel's own signal frame (`test_preempt_p2 /a64_x16_preserved`,
  `/a64_sve_state`).  aarch64 fcontext gained the preemption redirect too.
* *Off-loop spin* (`xtc_amutex`, `xtc_arwlock`, `xtc_notify`): the spin budget
  counted hint instructions, and the aarch64 `yield` hint is about 0.4 ns
  against x86 `pause` at 11-20 ns, so a waiter spun for a fraction of a
  microsecond and parked.  The budget is now time-calibrated.
* *Backtraces in a fiber* (fcontext): a fiber's outermost frame claimed its
  own entry as the return address, and the DWARF unwinder walked off the stack
  (`xtc_dump` crashed on clang builds).  A CFI-terminated start trampoline
  ends the unwind (`test_fctx /fctx_unwind_ends`).

Not verified: big-endian or 32-bit Arm.  (Apple Silicon: see 1.60 above.)

## CHANGED (1.59): io_uring ring modes and one-shot polls

**Status:** behavior change, measured on a 64-vCPU x86-64 host, Linux 6.12.

* A loop's ring is created with `IORING_SETUP_COOP_TASKRUN` (Linux 5.19+;
  a plain ring on older kernels), and when a thread starts RUNNING the loop
  it is rebuilt on that thread with `DEFER_TASKRUN` + `SINGLE_ISSUER`
  (6.1+).  A later run on a different thread (a second `xtc_exec_run`, a
  loop created on one thread and run on another) rebuilds it again; file
  AIO still in flight when a run ends is reaped by the leaving thread and
  delivered to the next run.  Single-loop rexis, requests/s: COOP +10.4%
  (pipeline 1) / +7.0% (pipeline 16) over a plain ring, DEFER a further
  +4.3% / +2.5%.
* Cost to know about: the kernel frees a `DEFER_TASKRUN` ring only after
  an RCU grace period.  Creating and destroying RUNNING loops at more than
  ~1,000 rings/s grows unreclaimable kernel memory (4,000/s: +152 MB;
  unpaced: +3 GB in 8 s, still +2.4 GB a minute later).  A loop that is
  created and destroyed without running stays on `COOP_TASKRUN` and does
  not pay this.  Keep executors alive across operations rather than
  creating one per request.
* Readiness polls are one-shot, and a timed-out socket's poll is kept
  armed for the waiter's next park.  The idle-connection collapse is gone
  (each re-park used to cancel a poll, and a cancel walks a 16-bucket
  kernel hash: at 30k idle connections rexis served 199 requests/s, now
  173k).  A registration that is not deleted is now re-reported while the
  fd stays ready (level-triggered, as on epoll); before, a multishot poll
  reported it once.

## CHANGED (1.59): fcontext is the default coroutine substrate on Linux x86-64 (glibc)

**What changed.**  The hand-written fcontext substrate (`src/evt/coro_fctx.c`,
already the default on musl and Apple Silicon) is now the default on Linux
x86-64 with glibc; `--with-coro=ucontext` (or `-DXTC_CORO_FORCE_UCONTEXT`)
keeps ucontext, and CI tests both.  ucontext's `swapcontext` makes a
`rt_sigprocmask` system call on every switch, and on Linux that call takes the
process-wide signal-handler lock, so with many loops every fiber switch in the
process serializes on one kernel spinlock.  Measured on a 64-vCPU x86-64 host:
16 loops of cross-loop request/reply 2.2M round trips/s on ucontext (56% of
cycles spinning on that lock) against 12.1M on fcontext; one fiber switch 550
-> 145 ns.

**Behaviour differences, and what was done about them:**

* *Signal mask.*  ucontext restored each fiber's saved mask on every switch;
  fcontext does not touch it, so a fiber runs with its thread's mask.  An
  `xtc_exec` worker blocks everything except the synchronous faults, SIGABRT
  and the preemption signal -- the same set a ucontext fiber unblocked.  A fiber
  on a loop the application runs on its own thread runs with that thread's
  mask.  See `xtc_preempt(3)`.
* *Fault containment on executor workers* (a latent fcontext bug, also on musl
  and Apple Silicon before this release): workers start with every signal
  blocked, and a SIGSEGV raised while blocked is delivered with the default
  action, so a contained fault (`xtc_fault_guard_install`) in a proc on an
  `xtc_exec` worker killed the process.  The fcontext substrate now unblocks
  the synchronous fault signals on each scheduler thread.
* *Floating-point control state.*  `jump_fcontext` now saves and restores MXCSR
  and the x87 control word (callee-saved in the System V ABI), as
  `swapcontext` does; before, a fiber's `fesetround` leaked to the next fiber on
  the thread.
* *Involuntary preemption* works on fcontext there too (the same signal-context
  redirect and trampoline; `test_preempt_p2` passes on both substrates).  It
  remains opt-in, with known residual hangs on BOTH substrates (about 1 run in
  2000 of the `test_wal_kill` preempt sweep: a woken task lost from a loop's
  run queue, a fiber parked in a notification wait with no wake source, and an
  example's `pthread_once` called from a fiber).

## FIXED (1.59): sqlxtc durability and snapshot bugs found by the 1.59 review

* **A large transaction, and every commit after it, was lost in crash
  recovery.**  See the next section.
* **An autocommit statement could lose an update or skip a row.**  Its
  snapshot was not registered with the version GC, so once a newer
  version committed, a prune could reclaim the version the statement was
  about to read.  The UPDATE then changed nothing and reported success,
  and a SELECT missed the row.  Pruning was off by default, so this needed
  `xstore_autovacuum(1)` or a concurrent `xstore_gc()`.  Every statement
  now pins its snapshot for its duration (test_snap_registry).
* **A transaction's spilled rows were never deleted.**  A transaction
  that buffers more than 256 KiB of rows spills the older ones into the
  tree; they stayed after COMMIT and ROLLBACK, so a bulk load kept a
  second copy of its rows in the page file until the next crash rebuild
  (test_steal_reclaim).
* **A direct-I/O store (`--storage-direct`) never reopened trusted.**  Its
  superblock write was unaligned, which Linux refuses under O_DIRECT, so
  every open rebuilt from the log (test_dio_restart).

## FIXED (1.59): sqlxtc lost a large transaction, and every commit after it, in crash recovery

Through 1.58, a transaction whose log record was larger than 16 MiB
committed and was acknowledged, but crash recovery treated the record as a
torn tail.  Recovery lost that transaction and every commit logged after
it, and the reopen truncated the log at that point.  A transaction is one
log record, so a bulk load in one transaction (TPC-C's loader commits
100k Stock rows at once, about 33 MB) produced such a record.  A clean
shutdown hid the bug, because the shutdown compacts the log into small
records; only a crash before the next log checkpoint lost data.  1.59
verifies each frame against the file size and a streamed checksum instead
of a length cap, and writes a record bigger than the log buffer directly
(test_wal_bigtxn).

## FIXED (1.59): sqlxtc silent wrong results found during the 1.59 work

Each of these returned a wrong answer with no error.  All were present
in 1.58.0.

* **Secondary-index entries were lost at every storage open.**  The open
  rebuilt every index, and inserting an index entry next to a range that
  deletes had emptied could fail in the leaf split on prefix-compressed
  leaves; the failure was discarded, so the entry was silently dropped.
  Base rows were never lost, but a query through the index returned too
  few rows and a UNIQUE check could miss a duplicate, until the next open
  rebuilt -- and lost again.  On a 10-warehouse TPROC-C store one open
  dropped 4,567 index entries; TPROC-C's Delivery, which picks the oldest
  new order through such an index, then skipped the lost orders forever.
  1.59 fixes the split, never trusts an index build that saw a failed
  insert, and clears every index before building any
  (test_btree_limits, test_crash_oracle).
* **A range scan could skip rows or return them out of order** under
  concurrent merges.  A forward scan released the leaf it had finished
  before it fixed the page id in that leaf's right link; a delete could
  merge the sibling away in the gap and the page id be reissued to
  another key range, and the scan carried on from the first slot of that
  page.  Every forward scan used this path: SELECT range and full scans,
  the row selection of UPDATE and DELETE, version garbage collection, the
  unique-constraint check and the online checkpoint's dump.  A scan from
  the first key had a related hole through a stale root page id.  The
  scan now accepts the next page only if it is the live leaf whose lower
  fence equals the upper fence of the one it left, and a scan from the
  first key only on the leftmost leaf; otherwise it re-descends
  (test_btree_cursor_hop, test_btree_smo_stress).
* **`ORDER BY ... LIMIT` kept the last of tied rows, not the first.**
  SQLite returns the first K rows of the unlimited query, ties in scan
  order; sqlxtc differed on 9 of 19 such query shapes.  The comparison
  oracle sorted both results before comparing, so it could not see this;
  it now compares ORDER BY results row by row (test_order_limit).
* **Fifteen SELECT shapes returned wrong answers**: a bound parameter
  inside a derived table or CTE read NULL; GROUP BY output columns were
  mapped to the wrong key; HAVING over a computed output never matched;
  derived-table, HAVING, join and CASE comparisons applied the wrong type
  affinity against a string literal; `GROUP BY 1` grouped by the constant
  1; a correlated subquery inside an aggregate query read past the end of
  a buffer; and a two-table join ordered by a column it did not output
  wrote past the end of one.  1.59 runs every aggregate through one
  operator over any row source (test_sql_results).
* **Two servers could open one store.**  Nothing stopped a second
  `sqlxtc-server` (or one with `SO_REUSEPORT` on the same port) from
  opening a store a running server held, and the two then overwrote each
  other's pages.  The store is now locked for the life of the open
  (test_sqlxtc_exclusive.sh).

## FIXED (1.59): involuntary preemption could hang, corrupt or lose work

Involuntary preemption is opt-in (`xtc_preempt_set_involuntary(1)`).
With it on, a preemption tick hands the OS thread to the scheduler in
the middle of whatever the fiber was doing, so every per-loop structure
a fiber updates must be bracketed against it.  1.58 missed several:

* **io_uring SQE reservation.**  A tick between reserving a submission
  slot and filling it let the next submitter publish the half-filled
  slot: the preempted fiber's own operation never reached the kernel (a
  hang), and the kernel could re-execute a stale earlier write at an old
  file offset into a buffer that no longer belonged to it.
* **The run queue and the timer heap.**  A tick inside a same-loop wake's
  two-pointer enqueue lost the woken task; one inside a timer-heap sift
  lost a timer another fiber armed.
* **The slab magazine.**  A tick inside the per-thread magazine's pop or
  push gave one object two owners.
* **aarch64 registers.**  The preemption trampoline used x16 for its final
  branch and saved only the low 128 bits of the vector registers, so a
  preempted fiber could lose x16 and its SVE state (glibc's memcpy uses
  SVE on current Arm cores).  It now resumes through the kernel's own
  signal frame.
* **The sqlxtc WAL's CRC table init**, built with `pthread_once` from
  whichever fiber checksummed first, wedged the whole loop if that fiber
  was preempted inside it.

Each has a test that raises the real preemption signal inside the
window (test_preempt_p2, test_slab, test_wal_crc_once).  The
test_wal_kill preemption sweep went from 8 hangs in 4,000 runs to none
in 28,000.  The opt-in item below ("involuntary preemption corrupts a busy
same-loop pair") no longer reproduces, but these fixes are not proven to
be its cause, so it stays open.

## OPEN (1.59): known costs of the 1.59 performance work

* **The store is larger than SQLite's on disk.**  Measured on stores
  each engine built from the same load (allocated blocks, XFS):

  | store | SQLite | sqlxtc page file | sqlxtc page file + log |
  | --- | --- | --- | --- |
  | ledger, 100k accounts, 2M transfers | 267 MB | 459 MB (1.72x) | 732 MB (2.74x) |
  | TPROC-C, 10 warehouses | 899 MB | 1,219 MB (1.36x) | 2,306 MB (2.56x) |
  | TPROC-C, 50 warehouses | 4,498 MB | 6,040 MB (1.34x) | 11,413 MB (2.54x) |

  (1.58: 2.56x and 3.61x for the page file; 1.59: 488 MB, 1.83x, for the
  ledger.  The TPROC-C rows are 1.59's, not re-measured.)  Where the
  rest goes:
  * **The log.**  SQLite in WAL mode checkpoints its log back into the
    database and truncates it; sqlxtc keeps a compacted log of the live
    rows next to the page file (crash recovery replays it).  It is
    rewritten, not grown, once it passes `--checkpoint-mb` (default
    64 MiB) and twice its size after the last checkpoint, so the
    steady-state log is about the size of the live data.
  * **Per-entry overhead.**  Every table row carries a 20-byte version
    key (table id, rowid, inverted commit timestamp; about 9 bytes
    stored after prefix compression) and every entry a 10-byte slot,
    against about 4-6 bytes of cell overhead in SQLite.  A narrower
    slot (6-8 bytes for leaves whose values are empty, as index leaves'
    are) would save 16-32 MB on the ledger, but its 4-byte head is the
    search accelerator of every node lookup, and it is a page-format
    change; it is left for a later format bump.
  * **Index fill.**  Index leaves split at 70% full (SQLite's
    redistribute-before-split keeps about 90%).  Moving entries into a
    sibling before splitting raises it (modelled on random inserts: 70%
    -> 74-76% shifting into the right sibling only, 81-86% into either
    sibling, for 2-8x the structure changes), about 10 MB and 60 MB on
    the ledger.  Not done: a move into the LEFT sibling moves keys left,
    and readers and parked cursors only chase right, so every hop and
    resume would need a lower-fence check; that is a later release.
  * **Old versions.**  Versions no snapshot can see are reclaimed by
    autovacuum while the server runs, and the rest at a clean close (it
    leaves one version per live row): a load used to leave about 5 per
    updated row (the ledger's accounts table: 512k versions for 100k
    rows, 31 MB).  A store that crashes keeps them until the rows are
    written again or the next clean close.  The pages freed are holes
    on the saved freelist: allocated size drops, the file's apparent
    size does not until they are reused.
  * **Growth steps.**  An open store's page file is grown ahead of its
    pages, an eighth of its size at a time (1 to 8 MiB), so a page
    write-back never extends the file (an io_uring write that extends
    a file runs on an io-wq worker).  A clean close trims the unused
    tail; a store that crashes keeps it (at most one step) until its
    next clean close.
* **The WAL frame checksum is now CRC32C.**  1.59 reads logs written by
  1.58 (FNV-1a frames) and mixed logs, but 1.58 cannot read a 1.59 log.
  That is expected: downgrades are supported only between LTS releases,
  to the previous LTS, and there are no LTS releases yet (see
  [ABI stability]({{ '/reference/abi-stability/' | relative_url }}),
  "Long-term support").

## OPEN (1.58): sqlxtc buffer-pool sizing and the hot-row hold

* **The default buffer pool is 256 pages (1 MiB); size it.**  Pass
  `--storage-frames`.  For TPROC-C at 10 warehouses (about 7 GB) on a
  128-vCPU host, 262144 pages (1 GiB) measured best (49.8k tpmC at 8
  connections).  Smaller pools spend most of their CPU in the CLOCK
  sweep (65536 pages: `evict_one` 78% of the server at 8 connections,
  8.4k tpmC).  Since 1.59 the trickler visits at most 16384 frames a
  pass, so a large pool no longer costs a full-pool walk every 5 ms; in
  process, TPROC-C at 10 warehouses runs best with a pool that holds the
  store (1048576 pages, 4 GiB).
* **A log checkpoint costs CPU and write bandwidth while it runs.**
  When the log passes `--checkpoint-mb` (default 64 MiB, then twice its
  last post-checkpoint size) the live rows are dumped into a new log.
  Since 1.59 commits keep running during the dump (they used to wait
  for all of it: about 16 s on a 10-warehouse TPROC-C store); they are
  held only to take its start point and to swap the files.  The dump
  itself runs on a blocking-pool thread and reads every live row.
* **Restart after an unclean stop replays the whole log.**  A 10-warehouse
  TPROC-C store killed after 150 s of load at 8 connections takes 47 s to
  reopen (1.58: 71 s; 1.59 scans the log once instead of twice and builds
  the indexes in parallel); after a clean SIGTERM drain it reopens in
  0.12 s (1.58: 24.7 s), because the page file and its indexes are
  trusted.
* **TPROC-C scaling is bounded by hot rows.**  A Payment holds its
  warehouse row from its first UPDATE to COMMIT (about 9 ms at 32
  connections), so throughput per warehouse levels off; connections
  beyond a few per warehouse wait on row intents.
* **Conflicts are reported, not hidden.**  SNAPSHOT and stronger fail a
  conflicting write at the statement with "could not serialize"; the
  transaction is then aborted (later statements fail until ROLLBACK).
  READ COMMITTED, autocommit, and a transaction's first statement re-run
  on a fresh snapshot (at most 8 times), waiting for the row's holder
  first.
* **Rows over about 1,000 bytes go out of line.**  Since 1.59 an inline
  entry on 4 KiB pages holds up to 1,010 bytes (1.58: 506), so TPROC-C's
  662-byte customer row stays inline; a larger value goes out of line on
  its own pages.
* **A crash rebuilds the indexes.**  After an unclean stop the store
  replays its (compacted) log and rebuilds every secondary index (see
  above).  Since 1.59 the engine keeps no double-write ring (it protected
  nothing in this mode and cost about 1,000 fdatasyncs a second under
  TPROC-C).

## RESOLVED (1.58): sqlxtc SQL compatibility and TPROC-C, and a bare-coroutine UAF

An audit of SQLite 3.53.4 against sqlxtc, run as differential tests
against Python's sqlite3, found (and 1.58 fixes):

* **libxtc: a bare `xtc_async` coroutine ran as the last proc on its
  thread.**  The proc TLS was not cleared on fiber entry, so the
  coroutine's `xtc_self()` named that proc, its `xtc_recv` took that
  proc's mail, and once the proc exited every later yield read freed
  memory (and `xtc_svr_call` from such a coroutine hung).  Fixed on
  ucontext, fcontext and Windows fibers.
* **libxtc: `xtc_async.h`, `xtc_exec.h` and `xtc_sim.h` did not compile
  as a consumer's first include** (`size_t` without `<stddef.h>`).  The
  install gate now compiles every installed header on its own.
* **sqlxtc returned wrong results** for: `LIMIT a, b` (offset and limit
  swapped); `''` inside a string literal; hex literals (`SELECT 0x10`
  gave 0); REAL to text (`%g`, six digits, so 1.23456789 was stored as
  "1.23457"); REAL/INTEGER column affinity; ORDER BY / LIMIT on a
  compound SELECT (applied to its last arm only); composite PRIMARY KEY
  and UNIQUE (parsed and ignored, so duplicates were accepted); an index
  seek or a join inside a transaction (missed the transaction's own
  writes, or read the latest commit instead of its snapshot); a row
  inserted into a rowid gap inside a transaction (invisible to that
  transaction).
* **sqlxtc rejected** bound parameters in UPDATE SET expressions, joins
  and subqueries; multi-column indexes; quoted identifiers, keywords as
  names, `?NNN` / `:name` parameters, mixed-case names; NOT LIKE, NOT
  BETWEEN, IS / IS NOT / IS DISTINCT FROM, LIKE ... ESCAPE, NULLS
  FIRST/LAST; the date/time functions, printf/format and the common
  scalar and math functions.  The SQLite compatibility corpus grew from
  82 to 163 cases, plus 4 expected-unsupported (all pass; 1.57.0 passes
  90 of the first 159).
* **sqlxtc could not run TPROC-C** under a standard sqlite3-API driver
  (Turso's perf/tpc-c), and every lookup was a full scan.  It now runs
  all five transactions with one access-path chooser (primary-key and
  index seeks for SELECT, aggregates, ORDER BY, UPDATE and DELETE;
  MIN/MAX from the first or last key; index nested-loop joins), a plan
  cache, O(1) in-transaction bulk INSERT, row write intents (a
  write-write conflict fails or waits at the statement, with deadlock
  detection, instead of failing COMMIT), and no global lock on the
  per-statement catalog lookups.

## RESOLVED (1.57): sqlxtc large rows, B-tree limits, and the 1.56 regressions

The 1.56.0 re-review and a study of RethinkDB's B-tree found, and 1.57
fixes:

* **sqlxtc lost acknowledged rows of about 800 bytes or more** after a
  crash, in tables that mixed small and large rows, and reported "commit
  failed; transaction rolled back" for a transaction that was partly
  durable (rows of about 2 KB).  The B-tree had no entry-size bound: a
  count-based split could fail after it was linked, a one-entry leaf could
  not split, and recovery ignored the failed insert.  Every entry is now
  bounded by the page, leaves split by bytes, and larger values are stored
  out of line (up to 65535 bytes); the mixed-size crash harness keeps every
  acked row, and the transfer commits whole.
* **B-tree limits:** an optimistic lookup returned values over 1 KiB
  truncated; keys over 1 KiB overflowed a split buffer; pages over 8 KiB
  could not split internal nodes.  Keys above `bt_max_key()` are now
  refused cleanly.
* **Every INSERT scanned the whole table** for the next rowid (9.6 ms per
  INSERT at 100k rows); it is now one descent (0.2 ms at any size).
* **A delete held the B-tree root exclusive** across its descent, which
  serialized concurrent deletes and readers (47k delete+insert/s on four
  loops, now 390k).
* **1.56 performance regressions**, each bisected and fixed: cross-loop
  spawn (TLB-shootdown storm from freeing stacks on the target thread;
  now faster than 1.55), two-loop io_uring ping-pong (self-feeding
  wakeup re-arm), idle CPU (a 1 ms steal tick on every loop even with
  nothing stealable), off-proc `xtc_svr_call` (one global slot lock),
  rexis SET with large values, kaka default-mode produce (helper-thread
  cap and group commit).
* **kaka `--fsync-interval-ms` lost acknowledged records on a process
  crash** (they were acked from a userspace buffer); it now writes before
  every ack, as documented.
* **kqueue (FreeBSD, macOS, BSDs):** deleting a write-only registration
  left the write filter armed (spurious wakeups, `XTC_E_INVAL` with
  migratable procs), and fd lookup was linear (82% throughput loss at
  30k idle connections); both fixed.
* **Batch:** producers retrying `xtc_batch_submit_mt` after close kept
  the driver from finishing.

## RESOLVED (1.56): the 1.55.0 production-readiness review

An adversarial review of 1.55.0 (five areas, every BLOCKER/MAJOR
reproduced on a 32-vCPU host) found 11 BLOCKERs and ~37 MAJORs; 1.56
fixes them, each with a regression test shown to fail on 1.55.0.  The
ones a consumer is most likely to have hit:

* `xtc_sem_acquire` / `xtc_notify_wait` stranded a second fiber waiter
  forever; `xtc_recv_correlate` / `xtc_recv_match` skipped replies already
  in the save queue when the predicate's value changed behind the same
  pointer.
* A killed proc parked in a sync, future, lwlock or lockmgr wait left its
  stack waiter linked (use-after-return on the next post) or kept a lock
  grant; `xtc_exit_pid` could not interrupt `xtc_proc_sleep` or those
  waits at all.
* Finished procs were never reclaimed (see below); cancelled timers and
  every `xtc_timer_set` / `xtc_future_with_timeout` timer were retained
  until `xtc_loop_fini`.
* A yielding migratable fiber starved every other migratable fiber on its
  loop, and a loop with any pending timer never stole work.
* OpenSSL: one client's TLS error made the next TLS call on that thread
  fail for every healthy connection.
* io_uring: on completion-queue overflow the kernel ends multishot polls;
  the backend did not notice, so registrations leaked and readiness was
  lost.  fd lookup was a linear scan on io_uring, poll and select.
* `xtc_blocking_run` used a pipe per call and, at EMFILE, silently ran the
  work on the loop thread.
* `xtc_batch_submit_mt` returned `XTC_OK` and a handle for requests it
  later dropped; `xtc_accel_wait_fence` spun at 100% CPU with an unread
  message pending.
* `xtc_svr_call` from a plain thread let a late reply write into the
  timed-out caller's freed stack frame.

The 1.56.0 platform gates (Linux GPU/NPU, Windows ARM64/x64 MSVC,
FreeBSD, real riscv64) then found, and 1.56.0 fixes:

* Windows IOCP: a migratable fiber's fd cleanup running on another loop
  corrupted the heap (`/m5/exec/Blk3_cross_loop_del_fd`, also in
  1.55.0).  The MSVC test gate counted crashed tests as passes, which is
  how it stayed hidden; it now checks exact exit codes.
* `xtc_ncpus` counted CPUs outside the process's affinity mask (riscv64
  SoCs with fenced-off cores; also 1.55.0).
* FreeBSD: the native kqueue file-AIO path was compiled out (1.55.0
  too); every `xtc_aio_*` went to the blocking pool.
* io_uring: back-to-back ring create/destroy could fail with a transient
  `ENOMEM`; a persistent one is now `XTC_E_NOMEM`.

The full list, with before/after measurements, is in the 1.56.0 release
notes (`debian/changelog`).  Behaviour changes a consumer must act on:

* **Links have BEAM semantics.**  A linked proc's NON-normal exit kills
  every linked peer that does not trap exits; a normal exit is silent to
  it.  Call `xtc_proc_trap_exit(1)` to receive an `E` message instead
  (`XTC_HAVE_PROC_TRAP_EXIT` is defined when available).
* DOWN and EXIT messages bypass `mailbox_cap`.
* `xtc_notify_signal` hands each signal to exactly one parked fiber,
  oldest first; `xtc_sem_post(n)` wakes only the waiters the count can
  satisfy; `xtc_gate_drain` on an open gate returns when the count
  reaches 0.
* Migratable work migrates more by default: an executor loop with an
  empty run queue steals even while it owns pending timers.
* TLS peer EOF without close_notify, a reset, or a transport error is
  `XTC_E_IO` (was `XTC_E_INTERNAL`).
* `xtc_blocking_run` may return `XTC_E_AGAIN` (queue full) or
  `XTC_E_RESOURCE` (no pool thread) without running `fn`; it never runs
  `fn` on the loop thread.  `xtc_aio_*` report that as `-EAGAIN` /
  `-ENOMEM`.
* `xtc_svr_call` to a server that dies returns `XTC_E_NOTFOUND`.
* `xtc_send` returns the wake error (the message is still queued) instead
  of `XTC_OK` if waking the receiver failed.
* `xtc_lock_get` / `xtc_lock_upgrade` return `XTC_E_ABORTED` when the
  waiter is killed under an enclosing mask.
* The proc registry supports 1024 live loops (was 64); a full registry is
  `XTC_E_RESOURCE`.
* `xtc_chan_mpsc_try_recv` can return `XTC_E_AGAIN` while
  `xtc_chan_mpsc_len()` is above zero (a sender is mid-write); retry.
* `xtc_rcu_current_epoch()` returns 1 on a fresh rcu (was 0).
* New functions: `xtc_proc_trap_exit`, `xtc_timer_release`,
  `xtc_net_listen_backlog`.  No existing public struct changed.

## NEW (1.55): asynchronous (device-offload) continuous batching

**What it adds.** `xtc_batch_create_async` turns a batch group's step
into a submit/collect pair: `submit` launches one device (GPU/NPU/
thread-pool) operation over the ready vector and returns a completion
descriptor; the driver awaits it without stalling the loop; `collect`
reads the device outputs and decides per-member CONTINUE/DONE at the
next boundary.  Four exec kinds -- FENCE (an fd), FUTURE (an
`xtc_future_t`), BLOCKING (a closure on the blocking pool), and SYNC --
cover the common device-completion mechanisms.  `xtc_batch_submit_mt` /
`_cancel_mt` / `_resume_mt` admit, cancel and resume requests from any
loop or a plain OS thread through a bounded lock-light ring, and
`xtc_batch_loop_for_device` / `xtc_batch_router_*` are advisory
placement helpers (NUMA-nearest loop, keyed sharding).  All of this is
behind `--enable-sched-policy`; a default build returns `XTC_E_NOSYS`
and no existing public struct changed size.  See `xtc_batch(3)`, the
scheduling guide, and `examples/13_gpu_batching` (OpenCL Mandelbrot) and
`examples/14_hetero_batching` (GPU + NPU capstone).

**Limitation: the FENCE exec kind is native-only.**  The simulation
backend has no hook to make an arbitrary registered fd become ready at a
virtual time, so a FENCE group (which parks on a device fd) is not
sim-reachable and is tested natively, like `xtc_accel_run_blocking`.
This is a deliberate scope decision, not a defect: sim coverage of async
offload uses the FUTURE kind, which has no fd and no clock and replays
byte-identically.  A real-fd fence under DST would need a new sim
fd-readiness primitive and is explicitly deferred.

**Limitation (NOT a libxtc bug): Intel compute-runtime 26.18 GPU
readback segfault.**  On a Lunar-Lake Arc iGPU with Intel
compute-runtime 26.18, both `clEnqueueReadBuffer` and the staging path
inside `clEnqueueMapBuffer` crash inside a built-in copy-kernel build
(`NEO::Zebin::ZeInfo::decodeZeInfo`, reached under `call_once`) when
driven by the example's queue-usage pattern.  The same crash hits any
OpenCL program that reads back a buffer this way on that silicon +
runtime, independent of libxtc.  `examples/14_hetero_batching` works
around it by allocating the GPU output with `CL_MEM_USE_HOST_PTR` over a
page-aligned host array and reading that pointer after `clFinish` (the
iGPU is UMA, so it is coherent) -- no read-buffer, no map, so the
crashing built-in copy kernel is never built.  This is a driver fault on
that specific silicon/runtime, documented here so an example user on
that hardware knows the symptom and the workaround.

## RESOLVED (1.50): lock-manager conversions broke mutual exclusion

Present since 1.0; found and fixed in 1.50 (87219d3).  When a locker
asked for a lock on an object it already held (a conversion):

- it was checked only against its OWN held mode, so it could be granted
  beside a conflicting holder (IS holder asks for S while another holds
  IX: granted);
- "stronger" meant a larger enum value, which is not a strength order, so
  an S holder asking for IS was moved to IS and a conflicting IX then
  admitted -- and `xtc_lock_downgrade` refused IX -> IS but allowed IX -> S;
- a conversion queued behind a waiter that was waiting for the same
  locker, an undetectable deadlock;
- `xtc_lock_upgrade` ignored the locker deadline and waited forever, and a
  blocking upgrade left the old lock held beside the new one.

Strength now comes from the conflict matrix, conversions are checked
against every other holder and are never queued behind waiters, and
upgrade is bounded by `xtc_lockmgr_id_set_timeout`.  The DST lock-manager
test gained a mutual-exclusion oracle that catches the old behavior on
its first seed (pinned in the corpus).

## RESOLVED (1.50): TLS host-name checks, secure client default, SIGPIPE

Fixed in 1.50 (PLAN.md 19.27.6-19.27.8):

- `xtc_tls_set_hostname` returned `XTC_E_NOSYS` on GnuTLS, wolfSSL and
  mbedTLS, so a CLIENT on those backends accepted a valid certificate for
  ANY host.  All four non-Windows backends now check the name (cdc3dab),
  and m18 asserts a wrong-name certificate is REJECTED on each.
- A zeroed `xtc_tls_opts_t` meant `XTC_TLS_VERIFY_NONE` for a CLIENT.  A
  client now verifies by default, the verify modes mean the same on every
  backend, and a NULL `ca_file` loads the platform trust store (mbedTLS,
  which has none, refuses a verifying client without `ca_file`) (517f9d5).
  **Behavior change:** a client that relied on the insecure default and
  talks to a self-signed server must now set
  `verify_peer_mode = XTC_TLS_VERIFY_NONE` explicitly.
- A TLS write to a peer that had closed raised `SIGPIPE` and killed the
  host process on all four backends unless the embedder ignored the
  signal.  It now returns an error (5653513); the same for `xtc_net`
  sends (2711c82).

SChannel (Windows) applies the same client verify default since 1.51
(in 1.50 a zeroed opts there still verified nothing).  Still open:
SChannel does not implement `xtc_tls_set_hostname` (`XTC_E_NOSYS`).
wolfSSL refuses to create a SERVER connection from a context with no
certificate (`xtc_tls_create` returns `XTC_E_NOMEM`); the other backends
defer that error to the handshake.

## RESOLVED (1.50): xtc_xproc lifetime, reaping and death classification

Fixed in 1.50 (PLAN.md 19.27.1-19.27.3, 19.27.9):

- `xtc_xproc_destroy` freed the child's state under a monitor's shadow
  fiber (a heap-use-after-free, visible only under ASan) and neither
  signalled nor reaped a running child, which was orphaned and later
  became a zombie.  Destroy now sends SIGTERM, waits up to 2 s, sends
  SIGKILL, and reaps; the shadow holds its own reference (c1841bc).
- A child killed by a signal was reported as `XTC_DOWN_KIND_EXIT` with the
  signal number as its exit code, and every short-lived child's status
  arrived as 96 (a truncated internal NOPROC code, from a spawn-then-
  monitor race).  A monitor now sees CLEAN, EXIT or SIGNAL exactly
  (0f007ca).
- `xtc_xspawn_entry` forked straight into a new runtime, and the exec path
  of `xtc_osproc_spawn` called `setenv` after `fork`: either could leave
  the child of a multithreaded parent blocked forever on a lock another
  parent thread held.  The exec path now does only `execve` after fork,
  and `xtc_xspawn_entry` re-execs the binary when `main()` calls
  `xtc_xproc_win_child_maybe` (b031dfb).  `xtc_xspawn`, which passes a raw
  function pointer, still forks without exec; prefer `xtc_xspawn_entry`
  from a multithreaded parent.

## RESOLVED (1.50): supervisor restart and join lifetime

- Under `XTC_SUP_ONE_FOR_ALL` / `XTC_SUP_REST_FOR_ONE` the replacement was
  spawned while its killed predecessor could still be inside an exit hook
  holding a shared resource.  The supervisor now waits (up to 5 s) until
  each old child has left the process table before respawning (8680179,
  PLAN.md 19.27.4).
- `xtc_sup_join` with a timeout that expired freed the still-running
  supervisor anyway -- a use-after-free, or a silent wedge of the
  supervisor on its freed lock -- and returned `XTC_OK`.  It now returns
  `XTC_E_AGAIN` and frees nothing (f6637ef).

## RESOLVED (1.50): xtc_lock_vec is all-or-none for its GETs

`xtc_lock_vec` left the earlier GETs held when a later one failed, while
the man page promised rollback.  It now releases the GETs issued since the
last non-GET request and reports the retained prefix in `*out_executed`
(340574c, PLAN.md 19.27.5).  The frozen lock ABI is byte-identical to
1.0.0.

## RESOLVED (1.52.x): select backend -- migratable fibers in timed xtc_proc_wait_fd could strand

**Status:** RESOLVED.  Present since the 1.50 backend sweep; fixed in
`src/io/io_select.c`.  The select backend is a portability fallback;
epoll, io_uring, kqueue and poll were unaffected.

**What it was.**  `test/m5/test_exec.c`
`/m5/exec/Blk6_migratable_waitfd_resume` hung on
`--with-io-backend=select`: 16 migratable fibers spawned on one of 8
loops, each parking in `xtc_proc_wait_fd` with a finite timeout on a pipe
that is never written, while peers steal them.  After a few iterations
the executor stopped making progress with every loop spinning in
`select(2)`.

**Root cause.**  The select backend converted a positive `timeout_ns`
to a `struct timeval` with a plain truncating divide
(`tv_usec = (timeout_ns % 1e9) / 1000`).  When a migratable fiber
re-parks with a nearly-elapsed deadline, the loop passes the small
positive remainder (`next_deadline_ns - now_ns`) as `timeout_ns`; any
value under 1000 ns truncated to `{0, 0}`, which makes `select(2)` a
non-blocking poll.  The loop then re-drained its timers (none due yet,
the real deadline being a few hundred ns away that the clock had not
reached), recomputed the same sub-microsecond timeout, and spun -- under
the executor, every loop busy-spun in `select` making no progress while
the lock-holder fiber never got scheduled.  The poll backend never hit
this because it rounds a positive timeout UP (`(timeout_ns + 999999) /
1000000`), so a positive wait is always at least one tick.

**Fix.**  The select backend now rounds a positive timeout UP to the
next whole microsecond and keeps an explicit `timeout_ns == 0 -> {0,0}`
branch, so a positive deadline can no longer collapse to a non-blocking
poll.  `make check` on `--with-io-backend=select` passes, Blk6 passes
repeatedly, and the Blk6 CPU time drops to a small fraction of its wall
time (genuinely blocking in `select`, not spinning).

**Caveat on the evidence.**  The truncation-to-zero spin is a real
defect and the fix is verified non-regressing on the select backend.
The original report was a Linux-only hang that did not reproduce on the
macOS/arm64 host this fix was developed and tested on, so while the
truncation bug fully explains the "every loop stuck in `select`, no
progress" signature, it has not been independently confirmed to be the
sole cause of the specific Linux failure.  The fix is safe regardless:
rounding a sub-microsecond wait up to 1 us cannot strand a fiber or
over-sleep a deadline meaningfully.

## RESOLVED (1.51): FreeBSD -- intermittent test_exec hang in Blk4

**What it was.**  The GitHub FreeBSD 15.1 VM job (`vmactions/freebsd-vm`)
timed out inside `test/m5/test_exec.c` in 1.48 and in two of the eight
CI runs of the 1.50 candidate; the one log that named a test showed
`/m5/exec/Blk4_cross_loop_state_timer` passing and the run stopping
after it.  Blk4 runs migratable fibers in a 1 ms timed `xtc_recv` while
a foreign thread wakes them and peers steal them.

**Cause.**  `xtc_recv` took the calling proc from `__current_proc`, a
thread-local that fiber context save/restore carries across a park, and
that a migration can leave naming a different proc than the fiber
running.  The receive then popped from, armed the waker of, and parked
on another proc's mailbox, and could block forever on that proc's
mailbox lock, taken on the other fiber's behalf by a thread that went
idle.  Linux never hit it in testing.  1.51 (`d4df0bd3`) re-anchors the
proc from the running fiber at receive entry and after every resume, as
`xtc_proc_sleep` and `xtc_proc_wait_fd` already did.

**Evidence.**  On a native FreeBSD 15.1 host (8 CPUs, kqueue, clang),
full `/m5/exec` runs, each bounded at 3 minutes: v1.50.0 hung in Blk4 in
1 of 100 runs on 2 CPUs (`cpuset -l 4-5`) and 1 of 123 on all 8.  The
captured stack: seven executor threads idle in `kevent`, the eighth
blocked for good in `blk4_proc -> xtc_recv -> __do_recv ->
__xtc_mtx_lock`.  v1.51.0: 0 of 100 on 2 CPUs.  Blk4 alone on v1.50.0
did not hang in 1,000 runs (`cpuset -l 0-1`), nor Blk5 in 300; the full
suite's earlier tests are part of the shape.  CI's FreeBSD job has passed
every run since (57 of 57, September 24 to October 26).

The Blk4 timer race fixed in 2.0 ("a fired park timer could release a
newer timer of a migrated fiber", above) is a different defect: it
cannot block a thread on a lock, and this hang did not recur while it
was still present (1.51 to 1.59).

## RESOLVED (1.51, 1.55): xspawn_entry children failed to start on small hosts under ring churn

**What it was.**  A re-exec'd `xtc_xspawn_entry` child that could not
create its own io_uring ring exited at once with 246 (240 minus
`XTC_E_INTERNAL`, see `xtc_xproc_child_main`).  On 4-vCPU CI runners with
8 threads continuously creating and destroying rings, up to 73 of 400
children did.  The cause is the kernel tearing a ring down
asynchronously: the next setup can fail until the earlier ring's memory
is released.  It needs a kernel that charges ring memory to the locked-
memory budget: on a 4-CPU Linux 7.2 host, v1.50.0 loses 300 to 345 of the
400 children of `/m10.10/xproc/entry_mt_parent`; Linux 6.12 never
reproduced it.

**Now.**  The child retries `xtc_loop_init` for about 2 s with a doubling
back-off (1.51), and ring setup itself retries a transient `ENOMEM`
(1.55); only a failure that persists exits 240 minus `XTC_E_*`.  Same
host and test: 0 of 400 in each of 15 runs (pinned to 4 CPUs, and with
only 4 CPUs online).  The test fails if more than 4 of 400 cannot start.
A child cannot fall back to the epoll backend instead: the I/O backend is
chosen at configure time.  See xtc_xproc(3).

## RESOLVED (1.51): timed waits reclaim their timer when it fires or cancels

**Status:** RESOLVED in 1.51 (`5cf69ad`) for the timer-node half; the
fiber-stack retention below is a separate item still open.

Every timed park -- `xtc_proc_sleep`, `xtc_recv` with a timeout,
`xtc_proc_wait_fd` with a timeout -- used to allocate a timer node
(~80 B) that stayed on the loop's `all_timers` list after it fired or was
cancelled, until `xtc_loop_fini`, so a long-lived loop that slept in a
hot path grew without bound.  As of 1.51 a park timer is refcounted (its
two owners are the loop's timer heap and the task's `park_timer` slot)
and freed by the last owner to release it, so a fired or cancelled park
timer is reclaimed during the run instead of at teardown.  The soak
(`test/soak/soak_mem_bound.c`, `make soak-mem`) "tasks" arm now plateaus
at +0 B/gen (was ~80 B/park), and the fix is ASan+LSan+UBSan clean.  (A
separate bug that LEAKED these nodes at `xtc_loop_fini` once
`xtc_timer_set` had been used was fixed earlier in 1.50, 42fe0e2.)

The "procs" arm of that soak still grows: that is the completed-coro
fiber-stack retention documented next, which is unchanged.

## RESOLVED (1.51): xtc_res FDS meters inbound connections

`xtc_net_accept` (new in 1.51) charges each accepted connection against
`XTC_RES_FDS` before accepting it, so at the cap the connection stays in
the kernel backlog (`XTC_E_RESOURCE`) instead of being accepted and
dropped; `xtc_net_close` refunds it.  Descriptors from a raw `accept(2)`
are still not charged -- use `xtc_net_accept`.

## RESOLVED (1.54): clang builds misidentified the running proc after migration

**Symptom.** On clang builds, a migratable fiber that parked in
`xtc_proc_wait_fd`, `xtc_proc_sleep`, `xtc_recv`, or `xtc_blocking_run`
and resumed on another carrier thread could overwrite the previous
thread's current-proc slot. The proc running on that thread then
saved and restored the wrong identity. Under clang ASan, `test_exec`
reported a heap-use-after-free in the resume kill check (6 of 16 runs on
the `Blk2` workload). gcc builds, including all CI jobs, were not
affected.

**Cause.** clang computes a thread-local variable's address once per
function and keeps it in a register or spill slot across the call that
switches stacks. When the fiber returns on another OS thread, the
cached address still names the old thread's slot. gcc re-reads `%fs`
for each access in the TLS models libxtc uses. This is the same hazard
as MSVC caching `__declspec(thread)` across `SwitchToFiber` (below).

**Fix.** Thread-locals accessed after a stack switch on a fiber are
reached through a non-inlinable accessor that passes the address through
a `volatile` local, so each access recomputes it on the current thread.
`test/m5/test_exec.c` `Blk8_migratable_self_identity` checks that
`xtc_self()` still names the caller between parks; on clang -O2 it
fails 10 of 10 runs without the accessor and passes 10 of 10 with it.

## RESOLVED (1.54): loop-relative PIDs misrouted across standalone loops

**Symptom.** Every standalone loop encodes `loop_id` 0 in its PIDs, so two
procs on two standalone loops can carry equal PIDs. Lookups that used the
PID alone could pick the wrong loop:

- `xtc_arena_group_discard` killed an unrelated proc with an equal PID and
  could report a reaped member as still present; `xtc_arena_group_add`
  could drop a second member as a duplicate.
- `xtc_app_shutdown` from a plain thread force-killed an unrelated proc.
- A `one_for_all` supervisor waited out its cleanup bound and stopped
  instead of restarting; `xtc_fsm_join` never returned `XTC_OK`;
  `xtc_sup_add_child` from another standalone loop timed out; stop kicks
  reached the wrong proc.

**Fix.** Each subsystem records the owner loop with the PID (the spawn
loop, which migration does not change) and resolves presence, kills, and
control messages in that loop's table only. Public `xtc_send`,
`xtc_exit_pid`, and `xtc_proc_info` behave as before. Regressions in
`test/m11/test_mctx.c` and `test/m10/test_{sup,fsm,app}.c` fail on 1.53.

**Contract.** Owner loops must outlive the structure that names their
procs: an arena group until discard reports `all_gone` or the group is
destroyed, an fsm until `xtc_fsm_join` returns `XTC_OK`.

## FIXED (2.0): caller-allocated structs grew during 1.x -- do not mix minors

**Status:** FIXED in 2.0 by an API change (see CHANGED (2.0) below).
What follows is the 1.x defect, kept so a packager of a 1.x release
knows the rule for it.

**The 1.x defect.**  `docs/abi-stability.md` said a MINOR bump was
additive-only.  For function signatures that held; for caller-allocated
option and info structs it did not: the caller allocates the struct at
its OWN header's size, and the 1.x library read (or wrote) its own,
larger size.  Measured between v1.0.0 and v1.59.0 (x86-64, gcc):
`xtc_proc_opts_t` 40 -> 56, `xtc_sup_opts_t` 24 -> 32, `xtc_app_opts_t`
48 -> 64, `xtc_tls_opts_t` 48 -> 120, `xtc_proc_info_t` 72 -> 80 with
`mask_depth`/`mask_deferred` INSERTED at offset 24 (`__fill_proc_info`
then wrote 80 bytes into an old caller's 72-byte, typically stack,
object), `xtc_svr_callbacks_t` 40 -> 48 with `handle_continue` INSERTED
before `terminate` (an old caller's `terminate` was called through the
`handle_continue` prototype), `xtc_aio_t` 48 -> 64.

**1.x consumer rule (still true for every 1.x release).**  Recompile
consumers against the headers of the exact minor whose library they
link; the SONAME `libxtc.so.1` is not licence to swap 1.x minors.

**The 2.0 fix.**  Every such struct now starts with `size_t size`, set
by its initializer to the caller's `sizeof`; the library reads an input
and writes an output only up to that size (a field past it takes its
default; size 0 is `XTC_E_INVAL`).  Fields are only ever appended in a
minor.  Three gates hold it: `test/abi/test_abi_sized.c` (an N-1
consumer under ASan: before the fix, `xtc_proc_info` "WRITE of size 88"
into a 24-byte prefix, `xtc_sup_start` "READ of size 56" from a 40-byte
child-spec stride, `xtc_svr_start` and `xtc_slab_create` reads past the
caller's object; all pass after), `test/dist/test_abi_layout.sh` (the
committed sizeof/offsetof table, so no layout change is silent), and
`scripts/abi-check.sh` (abidiff against the previous tag, release-time).
Symbol versions (`XTC_2.0`) and stability tiers complete the contract;
see `docs/abi-stability.md`.

---
## OPEN (1.56): sqlxtc recovery, kaka scope, and other example limits

The example programs are teaching code built on libxtc's public API.
After the 1.55.0 production-readiness review their durability and
isolation defects were fixed (1.56); what remains is recorded here.

* **sqlxtc row and key limits.**  A row (all its columns, encoded) is at
  most 4096 bytes in sqlxtc's native SQL engine (`XS_VMAX` in xstore.c,
  `XS_REC_MAX` in vexec.c).  A longer INSERT or UPDATE is refused with
  "row too large: the native engine stores at most 4096 bytes per row";
  before 1.59 it was refused as "unsupported SQL (native engine)", and
  until 1.59 this entry wrongly gave 65535.  65535 bytes is the B-tree
  layer's limit (its 16-bit value length; values over a quarter page go
  out of line), which the SQL layer does not reach yet.  A key is at most
  `bt_max_key()` bytes (498 on the default 4 KiB page), so an indexed
  TEXT/BLOB value longer than about 470 bytes cannot be indexed; the
  index insert is refused.
* **sqlxtc crash recovery rebuilds from the log.**  Recovery replays the
  write-ahead log onto a fresh page file; it does not yet trust the
  on-disk pages, their double-write buffer, or page LSNs
  (`xstore_recover_inplace` is exercised by tests only).  The log is
  compacted periodically (`--checkpoint-mb`, default 64 MiB, and at 2x
  the last post-checkpoint size), so restart time is proportional to the
  live data plus the writes since the last checkpoint.
* **sqlxtc fail-stops on a log I/O error.**  Any write-ahead-log write or
  fsync error (ENOSPC, EIO, EFBIG) fails the transactions in that batch
  and every later write with an I/O error until the server restarts;
  acknowledged commits are durable.  The superblock (clean flag) is a
  single pwrite at offset 0, not double-written.  Mid-log corruption is
  treated as end-of-log and truncated on reopen without a diagnostic.
* **sqlxtc serialization failures.**  SNAPSHOT, REPEATABLE READ and
  SERIALIZABLE fail a write-write conflict at the conflicting statement
  (1.58: row write intents; before 1.58, at COMMIT) with "could not
  serialize access due to concurrent update".  Clients must retry.
* **kaka is not Kafka.**  It spawns one proc per connection, has no
  consumer-group membership/rebalancing, no time-based retention, and no
  METADATA request.  Produces are acknowledged only after a group-commit
  fdatasync (`--fsync-interval-ms` relaxes this).
* **rexis `--cores` pins; it does not add threads.**  The server runs one
  event loop unless `--loops=N` is given; with N > 1 the connections
  spread over N loops but every command still takes one keyspace latch,
  so writes do not run in parallel.  `--max-memory` bounds the keyspace
  and every client's query/output buffers; a client over its output
  limit is disconnected.  A value of 4 KiB or more that a GET reply is
  still sending stays allocated (and charged to the client's budget)
  after a SET or DEL replaces it, until the reply has left.
* **lorb `test_lob` leaks under LeakSanitizer** (pre-existing):
  `lob_book_destroy` does not free its `xtc_chash` nodes.
* **sqlxtc restart after kill -9.**  For about 50 ms after the killed
  process is reaped, the kernel can still accept a connection on its
  listening socket and then reset it.  Nothing is acknowledged on such a
  connection; a client retries.  A graceful SIGTERM closes the listener
  first and is unaffected.

## OPEN (1.56): late or deferred kills, and poll/select scaling

* `xtc_exit_pid` wakes a proc parked anywhere, and the sync, future,
  lwlock and lockmgr waits deliver the kill at once.  A proc parked in
  `xtc_await`, an `xtc_aio_*` operation, or an `xtc_blocking_run`
  offload receives it only when that operation completes (memory-safe,
  but late; the offload is MASKED by design so the pool worker never
  writes into a dead frame).
* `xtc_timer_set` with a non-NULL `out_timer`: a handle that is never
  passed to `xtc_timer_release` stays allocated (~128 B) until
  `xtc_loop_fini`.  Pass NULL when no handle is needed.
* The poll and select backends do O(registered fds) work per wait,
  inherent to poll(2)/select(2): measured 216k round-trips/s with no idle
  connections, 57k at 10k idle and 7.5k at 30k idle.  Use epoll or
  io_uring for many mostly-idle connections.
* There is no built-in TLS handshake or idle timeout.  Bound them with
  `xtc_proc_wait_fd` timeouts; `xtc_tls(3)` DEADLINES shows the pattern.

## OPEN: involuntary preemption corrupts a busy same-loop pair (x86-64 ucontext)

**Status:** OPEN, pre-existing (1.55.0 identical).  Involuntary
preemption is opt-in (`xtc_preempt_set_involuntary(1)`); the default,
cooperative preemption, is not affected.

**What happens.**  A producer and consumer on ONE loop, both preempted by
the signal timer (`xtc_preempt_arm(20000)`) while they spin on an mpsc
channel, either stall (the loop thread runs, the channel is empty, the
consumer never sees the last message) or abort with "stack smashing
detected" inside `xtc_yield` (`coro_uctx.c`).  On floki (x86-64 glibc
ucontext) a 1-loop reproducer stalled 9/12 and aborted 3/12 on the 1.56
candidate and 10/12 and 2/12 on 1.55.0; `-mno-red-zone` does not help.
GitHub CI's sched-policy job hit it in `/m7/chan/mpsc_involuntary_preempt`.

**What to do.**  Do not enable involuntary preemption in production.
The test is opt-in (`XTC_TEST_INVOLUNTARY=1`) until the signal-context
redirect path is fixed.

## RESOLVED (2.0): valgrind memcheck reported libxtc fiber stacks as invalid

**What it was.**  libxtc allocates fiber stacks with `mmap` and did not
register them with valgrind, so once a fiber resumed on another loop's
thread after a cross-loop `xtc_amutex` hand-off, memcheck reported its
stack accesses as errors: 1,629 (ucontext) and 1,678 (fcontext) in
`/m9/sync/kill_after_park_while_running`, with "client switching
stacks?" warnings.  It was not a memory error in libxtc (ASan, which
libxtc annotates for fiber switches, reported nothing), but CI's memcheck
job had to skip that case.

**Now.**  When configure finds `<valgrind/valgrind.h>` (header-only; the
`valgrind-devel` / `valgrind` package), every fiber stack is registered
with `VALGRIND_STACK_REGISTER` when its coroutine is created and
deregistered before the stack is pooled or unmapped.  Outside valgrind
the requests are skipped after one `RUNNING_ON_VALGRIND` check per process:
spawn+exit throughput is within 0.4% of a build without the header
(medians of 15 interleaved runs, ranges overlapping).  The same test runs
with 0 errors on both substrates, and the skip is gone; CI's memcheck
list (test_sync, test_chan, test_mctx, test_timer, test_loop, test_svr,
test_lockmgr) is 0 errors with no stack-switch warnings.

**Still not memcheck targets.**  `test_proc`'s fault-containment cases
write to address 0x10 on purpose; its `pid_local_id_ceiling` case holds
65,536 fiber stacks, more mappings than valgrind tracks (`VG_N_SEGMENTS
is too low`).  `test_exec`'s `Blk2_concurrent_commit` spins on a shared
lock across a blocking offload and does not finish in 15 minutes under
valgrind.  The timing tests in `test_sync` still skip under
`XTC_UNDER_VALGRIND` (valgrind serializes threads; those tests measure
scheduling, not memory).

## RESOLVED (1.56): a finished proc fiber is reclaimed at once

**Status:** RESOLVED in 1.56 for processes (`xtc_proc_spawn` and its
variants -- every per-connection unit in the shipped servers).

**What it was.**  A coroutine task stayed on its loop's `all_tasks` list,
with its fiber stack (two mappings: stack + guard page) and coro struct,
until `xtc_loop_fini`.  Memory tracked the number of procs EVER spawned on
the loop, not the number alive: +617 MB RSS and +200,000 mappings after
100,000 short-lived procs with at most 256 live.  On a stock Linux kernel
(`vm.max_map_count` 65530) every spawn failed after about 32,000 lifetime
spawns.

**Now.**  When a proc's fiber finishes (it returns, calls `xtc_exit_self`,
is killed, or faults and is contained) the loop releases it on the
scheduler stack, after switching off the dead fiber.  The stack goes to a
per-thread cache of at most 64 (or is unmapped) and the coro is freed.
The task struct is recycled on its home loop; a proc that finished on a
work-stealing thief is handed back through a lock-free list.  Same test
after the fix: +1.4 MB RSS, +131 mappings.  A 4-loop migratable run with
~480,000 steals stays at +878 mappings at peak and +8 after
`xtc_exec_fini`.

**Still retained until `xtc_loop_fini` (by contract, not a leak):**

* A task created with `xtc_task_spawn` or `xtc_async`.  Its handle is
  documented as valid for the loop's lifetime and `xtc_await` reads the
  finished coroutine's result through it.  A long-lived service should run
  short-lived work as procs, not as bare `xtc_async` coroutines.
* Task STRUCTS (a few hundred bytes each, no stacks).  Recycled ones are
  kept on the loop's free list up to the high-water count of
  simultaneously live tasks and freed at `xtc_loop_fini`.  They are never
  returned to the allocator mid-run because a waker is a raw
  `{loop, task}` pair: a late wake must land on a valid task, and at worst
  it is one spurious wake, which every wait tolerates.

**Sharp edge for library code.**  State keyed on the running task's
identity must be released when the fiber exits, because the next proc on
that loop usually reuses the struct.  Two in-tree cases were found and
fixed in the same release: a `xtc_cfg` session binding (now unbound when
the proc exits) and the owner of a recursive `xtc_amutex` (now the task
plus the proc's pid, so a later proc cannot re-enter a lock a dead proc
still holds).

**`xtc_loop_fini` with live procs.**  Procs still parked when their loop
is finalized (an executor shut down with idle connections) have their
queued and saved messages, link and monitor entries, and memory context
released.  Their at-exit callbacks are NOT run and no DOWN or exit signal
is sent, because their fibers never resume.  Make procs that hold external
resources exit before shutdown.

---
## RESOLVED (1.49.x): allocator, accounting, cancellation and lifetime defects

**Status:** RESOLVED.  Recorded here because several were previously
open-ended "suspected" items, and a consumer needs to know which release
changed the behaviour it may have worked around.

**Registration lifetime, cfg scoping, arena discard** (`d18481b`):

- io_uring buffer/fd registration had a use-after-free across
  re-registration; the registration lifetime is now correct.
- `xtc_cfg` sessions are **FIBER-scoped**, not per-OS-thread.  A session
  binding now rides with the fiber across a yield and across a
  work-stealing migration; two fibers on one loop can each hold their
  own.  Consumers that assumed per-thread scoping should re-read
  `xtc_cfg(3)`.
- Registry entries are refcounted, so unregistering a variable a live
  session still overrides is safe in EITHER order.
- Integer cfg values from a file are parsed wide then range-checked, so
  an out-of-range value is rejected rather than silently truncated.
- `xtc_arena_group_discard` now waits for a killed member's pid to leave
  the proc table entirely (not merely for its alive flag to clear)
  before resetting the arena, because at-exit callbacks run after the
  flag clears and can still touch arena memory.
- `xtc_mctx_reset` no longer holds the context lock across cleanup
  callbacks, so a callback may introspect or allocate in its own
  context (it used to self-deadlock).

**Cancellation safety in the park paths** (`9f196cb`), five defects:

- An explicit wake is no longer reported as a timeout.
  `xtc_proc_wait_fd` returns `XTC_E_AGAIN` only when the timeout
  actually fired, with `XTC_WAIT_TIMEOUT` in `*out_revents`; a bit-less
  wake returns `XTC_OK`.
- A killed waiter releases its fd registration and timeout timer before
  the unwind completes, so the next waiter on that fd no longer gets
  `XTC_E_INTERNAL` from a duplicate-registration rejection.
- An at-exit hook may now park after a kill without restarting the
  at-exit list.
- The check/arm gap in `wait_fd` is closed: the mailbox observation and
  the waker arming happen under one lock hold, so a send or kill landing
  in that window can no longer be lost (it could park the proc forever
  with `timeout_ns < 0`).
- `xtc_blocking_run` no longer lets a worker outlive the caller's frame.

**Allocator and accounting** (`464f876`):

- `xtc_aligned_alloc` / `__os_aligned_alloc` reject a size whose
  round-up to the alignment would not fit in a `size_t`, with
  `XTC_E_RANGE`.  Previously it WRAPPED: `xtc_aligned_alloc(64,
  SIZE_MAX)` returned a non-NULL 64-byte block, so the first write
  corrupted the heap.
- A slab destructor runs once, not twice.
- `xtc_res_acquire` tests headroom before adding, so a huge request is
  rejected with `XTC_E_RESOURCE` instead of overflowing `int64_t` (which
  was signed-overflow UB and in practice wrapped NEGATIVE, sailing past
  the cap check and landing `used` at `INT64_MIN` -- breaking the
  hard-cap and non-negative invariants at once).
- The allocation redzone honors the requested alignment.
- The allocation auditor keeps its record when a downstream `realloc`
  FAILS (it used to drop the record first, so the original still-live
  block vanished from the auditor and the leak checker reported clean
  exactly when an allocation had failed).
- A slot-size floor fix in the slab.

---
## RESOLVED: fiber strand under migration + blocking offload (kqueue)

**Status:** RESOLVED in 1.48.0.  Root cause found and fixed; the FreeBSD
CI job is gating again.

**What it was.** `xtc_proc_wait_fd` acted on the WRONG PROC.  `self` came
from `__current_proc`, a thread-local that the coroutine layer saves and
restores across every yield -- and under the multi-loop executor the
restored value routinely named a *different* proc than the fiber actually
running.  Measured with a control that involves no second thread-local
(compare `__current_proc`'s task against the task the loop rebinds per
step): **99.2% wrong** (12,120,000 against 99,592).

So `wait_fd` registered another fiber's task as the readiness tag and
then stored into that task's `park_fd`, **displacing its still-live fd
registration**.  The displaced-registration count matched the leaked
count one for one, around 700 per run.  On a backend with a userspace fd
registry (kqueue's `reg_fds`) each orphan leaves one more loop watching
that open file, so several loops raced to dispatch its readiness to their
own tag -- waking a task that was not the parker, while the real parker
stayed blocked on a completion whose result had already been published.
At the wedge, 107 of 117 live fds were registered in more than one of 12
kqueues, one of them in all twelve.

That also explains why the cleanup looked innocent: it was almost never
*skipped*.  The registration was overwritten out of existence before any
cleanup could name it.

**The fix.**  `struct xtc_coro` gained a `proc` back-pointer, published
once by `__proc_entry` from the fiber itself while its identity is
unambiguous; `xtc_proc_wait_fd` re-anchors `self` from the running fiber
when the thread-local disagrees with the running task; and `__proc_free`
clears that back-pointer, because the coro outlives the proc (the loop
steps it once more to observe DONE) and leaving it dangling was a
use-after-free -- caught by ThreadSanitizer, which is why the fix carries
all three parts and not just the first two.

Alongside it, the dispatcher no longer *guesses* which registration
fired: `xtc_io_event_t` now carries the `fd` the event is for, populated
by all nine backends, and `task->park_fd` became atomic so the
unregister is claimed exactly once instead of a torn test-then-use.

**Verified.**  The reproducer went from 0/6 passing to 8/8;
`/m5/exec/Blk2_concurrent_commit` from a 6-hour hang on every run to
`[ OK ]` 6/6; the multi-holder audit from 107-of-117 to **zero**; and
FreeBSD `make check` to rc=0 with 571 tests and no failures for the first
time.  Linux is unaffected (0/12 hangs, `make check` green,
TSan/ASan/UBSan clean).

**If you are on 1.47.0 or earlier** and cannot upgrade: spawning the
offload-heavy fibers with `migratable = 0` avoids it completely, at the
cost of work-stealing for those fibers.  Linux (`epoll`, `io_uring`) was
never affected -- neither keeps a userspace fd registry.

---
## RESOLVED: runtime-thread signal mask (process-directed signal on a scheduler thread)

**Status:** RESOLVED.  The carrier reported a process-directed SIGCHLD
delivered to a libxtc scheduler thread (where MyProcPid == 0) instead of
the thread the embedder designated.

Three layers:

1. Every runtime thread is created with all signals blocked
   (__os_pthread_create_masked).

2. The real residual: the ucontext coroutine substrate restores
   uc_sigmask on every swapcontext, and getcontext captured the CREATING
   thread's mask -- so a fiber created from a thread with a signal
   unblocked (a proc spawned from the embedder's main thread) unblocked
   that signal on whatever runtime loop/worker thread later ran the
   fiber.  (The hand-written fcontext substrate does not touch the
   signal mask, so it was immune -- which is why the forced-fcontext CI
   never caught this.)  Fixed in src/evt/coro_uctx.c: the fiber's
   uc_sigmask blocks process-directed signals but EXEMPTS SIGVTALRM
   (preemption), the synchronous fault signals SIGSEGV/SIGBUS/SIGFPE/
   SIGILL (which the R1 fault guard must catch -- blocking a
   hardware-generated fault is undefined behavior anyway), and SIGABRT
   (assert/panic).

3. A proc fiber's uc_sigmask is inherited across fork(); xtc_osproc's
   child path now resets its own mask to empty immediately after fork so
   the child (and any exec'd image) starts clean, not with the runtime
   mask.

**Evidence:** test/m1/test_thread_sigmask.c (main is the only
SIGUSR1-unblocked thread; multi-loop executor + blocking pool + 200
process-directed SIGUSR1s) reports 0 deliveries to any libxtc thread,
20/20 under CPU load and in the gated parallel make check; R1 fault
containment (test_proc fault_contain/fault_early_contain/
recovery_registry) passes; fork/exec (test_osproc) passes; the three
preemption tests pass; ASan make check is clean.  test_thread_sigmask is
now in the gated make check set.

## RESOLVED: xtc_exec_fini cross-thread-spawn teardown leak (LSan)

**Status:** RESOLVED.  Surfaced 2026-07 via
test/concurrency/repro_idle_uring_wake.c.

**Symptom:** procs spawned CROSS-THREAD (xtc_proc_spawn from an OS thread
that is not on the target loop) onto a service-mode executor that is
stopped mid-flight were not fully reclaimed by xtc_exec_fini --
LeakSanitizer reported the task+coro (xtc_async -> __os_calloc,
coro_uctx.c) as leaked, run-to-run variable (~3-7%).

**Fix, in two parts:**

1. __xtc_inbox_fini now, for each undrained XTC_INB_PUBLISH message,
   runs the carried task's cleanup (releasing the fiber stack + coro
   struct) and frees the task -- exactly as xtc_loop_fini's all_tasks
   walk does for drained tasks.  XTC_INB_WAKE references an
   already-tracked task and is left untouched.  This reclaimed procs
   left sitting in a loop's inbox at fini (the bulk).

2. THE RESIDUAL (the run-queue case) root cause: xtc_async stores
   t->cleanup only AFTER __xtc_task_spawn_ex makes the task visible (it
   pushes an XTC_INB_PUBLISH for a foreign spawn).  A short-lived
   cross-thread-spawned coro can be drained, run, and reach DONE on the
   target loop's thread while the spawning thread has not yet stored
   t->cleanup -- so the DONE path read cleanup==NULL, recycled the task
   as a PLAIN task (freeing the task struct without releasing its
   coro/fiber-stack -> the coro leaked), and left the spawner's pending
   t->cleanup store to land on freed memory (a latent UAF).  Fixed in
   __xtc_loop_step: the recycle-as-plain path now also requires
   t->fn != __xtc_coro_step.  t->fn is set before the task is published
   and never changes, so it is a race-free discriminator: any
   coro-backed task is left on all_tasks for the fini walk (which runs
   its by-then-stored cleanup exactly once); only genuinely plain
   pinned tasks recycle.

**Evidence:** repro_idle_uring_wake under ASan leak-detection goes to
0 leaks across 70+ consecutive runs on both epoll and io_uring (was
~3-7%); the full proc/sup/svr/tnt suites pass under ASan with no
use-after-free or double-free; a drain-to-idle shutdown was already
leak-clean and stays so.

**Why it was not caught earlier:** the leak is only visible with a
WORKING LeakSanitizer -- GitHub's containerized CI runner restricts the
ptrace LSan needs, so LSan silently no-ops there.  The lost-wakeup
guarantee the reproducer checks is covered leak-clean under DST by
test/sim/test_sim_wake_park.c.

## Platform runtime-verification status (1.x readiness, B2)

For an honest 1.x stance, here is exactly which platforms run at
RUNTIME (the full test suite executes) versus which only COMPILE:

- Linux (epoll + io_uring), macOS (kqueue): runtime-verified on every
  commit in CI.
- FreeBSD 15 (clang, kqueue): NOW in per-commit CI via a FreeBSD VM
  on the Linux runner (vmactions/freebsd-vm) -- full gmake check.  Also
  re-verified by hand on the FreeBSD host (nuc, 2026-07, gmake check
  passes).
- RISC-V 64 (Linux, gcc): NOW in per-commit CI via QEMU user-mode
  emulation (riscv64-qemu job: cross-arch build + C unit + property
  suites) -- catches width/endian/atomics bugs.  Also re-verified on a
  native RISC-V host (rv, 2026-07, full make check passes).
- illumos (SunOS 5.11, UltraSPARC v9 / sparcv9 big-endian, gcc, event
  ports): re-verified against the tree (2026-07 host was unreachable
  this round; last full gmake check passed 2026-07, including the
  property suites on big-endian SPARC, OpenSSL 3, and the native
  event-port file-AIO path (SIGEV_PORT -> PORT_SOURCE_AIO) incl.
  /aio/roundtrip).  NOT in per-commit CI -- deliberately: there is no
  GitHub illumos runner, and running OmniOS/OpenIndiana in a nested
  QEMU VM on the Linux runner is slow and flaky enough that it is not
  worth the per-commit cost (unlike FreeBSD's mature vmactions VM or
  RISC-V's fast qemu-user).  illumos stays a periodic manual
  re-verification (an x86_64 illumos-in-CI experiment is a possible
  future item, but not "easy").
- Windows: the IOCP runtime (AFD socket poll, cross-thread wakeup,
  file AIO) was RUNTIME-verified on a Windows host with MinGW (2026-06
  -- see "IOCP backend status" below; three bugs found and fixed).
  Per-commit Windows CI remains an MSVC xtc.lib + smoke build.
- AIX (pollset): compiles, code-reviewed, no test host.

## RESOLVED (partial): native stack backtrace beyond execinfo

**Status:** real backends added; one (Windows) remains compiled-but-not-
runtime-verified for lack of a Windows host.

**Previous issue:** `src/os/os_backtrace.c` had only the execinfo backend
(glibc/macOS/BSD) and a no-op stub everywhere else, so musl and Windows
got an empty C backtrace in `xtc_dump` / the panic+crash handler.

**What changed this round:**
  - **libunwind backend** (`XTC_HAVE_LIBUNWIND`) for libc's lacking
    `<execinfo.h>` (notably musl): `__os_backtrace` walks the calling
    thread with `unw_step`; `__os_backtrace_emit` symbolizes best-effort
    with `dladdr` (`XTC_HAVE_DLADDR`) or prints raw addresses otherwise.
    Detected by `dist/configure.ac` (`--with-libunwind=auto|yes|no|PATH`,
    OPTIONAL); execinfo wins when both are present.
  - **DbgHelp backend** (`_WIN32`): `CaptureStackBackTrace` +
    `SymFromAddr`, linked with `-ldbghelp`.
  - New test `test/m12/test_backtrace.c` drives a 3-deep call chain,
    asserts >= 2 captured frames, and (on a symbolizing platform)
    requires at least one of its own frames to be named.

**Verified on this round's Linux glibc x86_64 host:**
  - execinfo path: full `make check` for m1/m12 green, `test_backtrace`
    green, the existing `test_dump` panic/crash path green.
  - libunwind+dladdr path: built by forcing `XTC_HAVE_LIBUNWIND` and
    linking the system libunwind; produced a REAL symbolized trace and
    passed `test_backtrace`.
  - libunwind addresses-only path (dladdr disabled): `test_backtrace`
    passes on frame count alone.
  - All three Unix variants compile `-Wall -Wextra -Wpedantic` clean.

**NOT runtime-verified (honest gaps):**
  - The libunwind path has not run on an actual musl host yet, only by
    forcing the backend on glibc.
  - The Windows DbgHelp path is COMPILED-NOT-RUNTIME-VERIFIED: it
    cross-compiles clean with mingw-w64 and links with `-ldbghelp`, but
    no Windows machine in the CI matrix exercises it.  Reviewed against
    the Win32 DbgHelp API docs; same status as `src/io/io_aix.c`.
  - Platforms with neither execinfo nor libunwind still get the honest
    no-op stub (`__os_backtrace_supported()` returns 0).

See `docs/guide/debugging.md` for the per-platform symbolization matrix.

## RESOLVED: xtc_slab SHARED_MEMORY mode cross-process support

**Status:** FIXED in this round.

**Previous issue:** The SHARED_MEMORY mode tests used MAP_PRIVATE | MAP_ANONYMOUS,
which is single-process memory, not actual shared memory.  Additionally, the
slab's shm_cursor was stored in per-process private memory, so two processes
with their own xtc_slab_t structs would each start carving chunks at offset 0,
causing collisions.

**Fix:** The cursor now lives in a 64-byte header at the start of the shared
region, using atomic CAS for coordination.  First attacher initializes the
header (magic=0x5854435F534C4142 "XTC_SLAB", version=1, cursor=64); subsequent
attachers verify magic and use the existing cursor.

**Verification:** New test file test/m11/test_slab_shm.c exercises real
cross-process sharing via fork(2) + POSIX shm_open:
  - test_shm_basic_fork: parent allocs, child reads+modifies via offset
  - test_shm_alloc_in_child: child allocs, parent reads via offset
  - test_shm_concurrent_alloc: 50 concurrent allocs from each process, no overlap
  - test_shm_size_too_small: XTC_E_RESOURCE when region < header+chunk
  - test_shm_resolve_invalid_offset: NULL on junk offsets

The previous misleading tests have been renamed to clarify their scope:
  - test_slab.c: test_shm_offset_resolve -> test_shm_offset_resolve_single_process
  - pbt_slab.c: prop_shm_offset_roundtrip -> prop_shm_offset_roundtrip_single_process

## RESOLVED: Windows fault containment (SEH) + fiber teardown double-free

**Status:** resolved.  `xtc_fault_guard_install` was a no-op on Windows;
it now installs a Vectored Exception Handler that contains a
fiber-attributable hardware fault by restoring the CONTEXT captured at
`xtc_proc_recovery_arm()` (via `EXCEPTION_CONTINUE_EXECUTION` -- no
stack unwinding, which is what makes it safe on a fiber stack; a
`longjmp` driven from a VEH reliably corrupted the CRT heap).
Runtime-verified on a Windows host (santorini, MinGW gcc) by a
dedicated driver: a proc arms recovery, registers resources, and takes
a real access violation -- the VEH contains it (exit 0), the recovery
resource registry releases the proc's fd + callback automatically, and
the monitor observes DOWN(reason).  A companion driver confirms the
ESCALATION half of the contract: a fault inside a critical section is
NOT contained -- the VEH returns EXCEPTION_CONTINUE_SEARCH and the
process dies with 0xC0000005 (EXCEPTION_ACCESS_VIOLATION), preserving
the PG critical-section PANIC semantics.
While wiring this up, a pre-existing Windows double-free was found and
fixed in `coro_winfiber.c` (the done branch destroyed the coro
eagerly *and* via the task cleanup at `loop_fini`), which had made any
loop+process tear down with heap corruption on Windows.

## Windows: `test_proc` (incl. `selective_receive`) -- RESOLVED, now GATED

**Status: RESOLVED (2026-09-09).**  The full munit `m8/test_proc` now
BUILDS AND RUNS under MSVC and is part of the `dist/build_msvc.bat`
hard gate: **18 of 18 cases pass, 1 skips** (`/fault_escalate` only,
which needs `fork()`).  `/selective_receive` -- the historically
suspect case -- passes.  Verified interactively on a cloud Windows
Server 2022 x86_64 host (16 vCPUs, MSVC 19.44.35228), not inferred
from CI.

The earlier note said "the full munit `test_proc` cannot build under
cl.exe (munit uses GCC-isms)" and treated the smoke test as the only
possible Windows guard.  That was WRONG on the cause: munit itself
builds fine (the `MUNIT_ARRAY_PARAM` VLA fix landed long ago).  What
actually blocked it was FOUR test-side portability defects, all fixed:

1. `#include <sys/wait.h>` at file scope, needed only by the one
   `fork()`-based case.  Now `_WIN32`-guarded, and that single case
   returns `MUNIT_SKIP` on Windows (its escalation half is covered by
   the SEH driver -- see "Windows fault containment (SEH)" above).
2. THREE hand-rolled `__attribute__((packed))` mirror structs for the
   DOWN / EXIT signal layouts.  The tree already has the portable
   `XTC_PACK_PUSH` / `XTC_PACKED` / `XTC_PACK_POP` trio for exactly
   this (MSVC needs `#pragma pack`, which an attribute cannot express);
   the test now uses it.
3. `clock_gettime(CLOCK_PROCESS_CPUTIME_ID)` in `/recv_inf_parks` (the
   "a parked proc burns no CPU" proof).  Windows DOES have a clean
   equivalent -- `GetProcessTimes` (kernel+user) -- so the case now
   runs on Windows instead of being written off as unportable.  It is
   worth more there than elsewhere: the IOCP backend's 8 ms AFD repoll
   sweep is exactly the kind of thing a regression could turn into a
   spin, and this case would catch it.
4. An LLP64 truncation: `8L * 1024 * 1024 * 1024` overflows a 32-bit
   MSVC `long` (`C4307`).  Now `8LL`.  Same class as the documented
   `test_blocking` `_Atomic long` bug.

Two REAL bugs were found by getting the test to run -- one in the
library, one in the test harness.  Both are documented next.

## Windows: contained-fault DOWN reason was a raw `EXCEPTION_*` code (LIBRARY BUG, FIXED)

**Status: FIXED (2026-09-09).**  Found by running `m8/test_proc` on a
real Windows host for the first time: `/fault_early_contain` asserted
`s.reason == 11` against an actual value of `-1073741819`.

**Root cause:** the SEH vectored handler (`__xtc_veh` in
`src/ptc/proc.c`) stored the raw Win32 exception code into
`p->fault_sig`:

```c
p->fault_sig = (int)code;       /* EXCEPTION_ACCESS_VIOLATION */
```

`EXCEPTION_ACCESS_VIOLATION` is `0xC0000005`, i.e. `-1073741819` as a
signed `int`.  That value became the proc's `exit_reason` and was
delivered to every monitor as the DOWN `reason`.  It broke three
documented promises at once (`src/inc/xtc_proc.h`): the reason is
specified to be a POSITIVE signal number ("e.g. 11 for SIGSEGV"), to
sit inside 1..255, and to be distinguishable from `XTC_DOWN_NOPROC`
(`-100000`) precisely so a supervisor can tell a contained fault from
"target already gone".  A negative, out-of-range, platform-specific
value satisfies none of them -- so a supervisor written to the
documented contract misclassified every contained Windows fault.

**Fix:** a `__xtc_veh_code_to_signo()` mapping applied where the code
is recorded, so Windows reports the same signal numbers POSIX does:
access violation / misalignment / in-page error -> `SIGSEGV`, the two
divide-by-zero codes -> `SIGFPE`, illegal / privileged instruction ->
`SIGILL`.  (`SIGBUS` does not exist on Windows, hence misalignment
folding into `SIGSEGV`, which is what a POSIX kernel reports for the
same fault on the architectures libxtc targets.)

**Evidence:** `/m8/proc/fault_early_contain` went from FAIL
(`-1073741819`) to OK (reason 11) on the host; `/fault_contain` and
`/recovery_registry` (whose reasons flow through the same field) pass;
the POSIX path is untouched (19 of 19 on Linux; the change is inside
`#if defined(_WIN32)`).

## Windows: `close(fd) == -1` as a "was it closed?" probe is FATAL (TEST-HARNESS BUG, FIXED)

**Status: FIXED (2026-09-09).**  `/m8/proc/recovery_registry` did not
merely fail on Windows -- it killed the whole test binary with
`0xC0000409` (`STATUS_STACK_BUFFER_OVERRUN`), so no later case ran.

**Root cause: the TEST, not the library.**  The library closed the
tracked fd correctly; the test proved it had, with the POSIX idiom

```c
munit_assert_int(close(g_rec.fd), ==, -1);   /* expect EBADF */
```

On Windows the MSVC CRT's `_close()` on an unused descriptor invokes
the *invalid-parameter handler*, whose default action is
`__fastfail(FAST_FAIL_INVALID_ARG)`: the process dies immediately and
no assertion is ever evaluated.  Isolated with a 15-line standalone
program (no libxtc linked): a bare double `close()` aborts with
`0xC0000409` every time.  `_get_osfhandle()` on a closed fd was
measured to be JUST AS FATAL, so it is not a safe alternative.

**Fix:** `test/include/fd_probe_compat.h` provides
`xtc_test_fd_is_closed(fd)` -- on POSIX a plain `close(fd) == -1`, on
Windows the same `_close()` bracketed by a no-op thread-local
invalid-parameter handler (`_set_thread_local_invalid_parameter_handler`
with a REAL no-op function; passing `NULL` restores the fatal default),
which was measured to make the CRT return `-1`/`EBADF` exactly as POSIX
does.  A general Windows test hazard, so it lives in `test/include/`
rather than inside one test.

## Windows: `tnt/test_tnt` all-zero counters -- NOT A BUG; the module is compiled out

**Status: RESOLVED as a DOCUMENTATION ERROR (2026-09-09).**  The
previous entry called this "a real cross-shard-wake bug still under
investigation... likely the self-wake pipe's interaction with the IOCP
readiness model."  That diagnosis was wrong in the strongest possible
way: **there is no tnt code on Windows to have a bug in.**

`src/orc/tnt.c` is wrapped in `#if !defined(_WIN32)`; the Windows half
of the file (from the `#else`) is nothing but `XTC_E_NOSYS` stubs, with
the in-source comment "tnt is a POSIX feature (raw socket I/O in the
couriers)".  There is no self-wake pipe, no shard scheduler, and no
cross-shard sender in a Windows build.

**Verified, not reasoned:** a standalone probe on the Windows host
prints `xtc_tnt_start rc=-3` (`XTC_E_NOSYS`).  The all-zero counters
were the arithmetic consequence of a runtime that was never compiled
in -- exactly what a NOSYS stub is supposed to produce.

Two further corrections to the record:

* `test_tnt` had ALREADY grown a `rc == XTC_E_NOSYS -> return 77`
  (SKIP) branch in commit `e41f128` (2026-08-03), so the "all-zero
  counters" symptom the entry described was already gone before this
  investigation; the entry had simply gone stale.
* But that branch was UNREACHABLE on Windows, because a later commit
  (`53e6ea1`, 2026-09-05) added `#include <sys/socket.h>` at file
  scope for the socketpair-staged echo scenario -- so `test_tnt` did
  not even COMPILE under MSVC (`C1083: Cannot open include file:
  'sys/socket.h'`) and the SKIP could never be reported.

**Fix:** the POSIX-only body of `test_tnt.c` is now `_WIN32`-guarded as
a whole, with a small Windows `main()` that prints the SKIP and returns
77.  Deciding it at COMPILE time is what lets `test_tnt` join the MSVC
gate at all: the Windows translation unit never references
`<sys/socket.h>`, so it builds, and it reports its status honestly
instead of failing to compile.  `tnt/test_tnt` is now IN
`dist/build_msvc.bat` (as a standalone, non-munit driver -- it has its
own `main()` and no `munit.c`) and reports SKIP.  On Linux it is
unchanged and still passes the full scenario.

## IOCP backend status (Windows)

**Status:** round-2 native rewrite; the smoke test now runtime-verifies
more of it on santorini (VS18, ARM64).  As of 2026-07 the MSVC smoke
gate passes: version, strerror, clocks, slab, lwlock, SEH fault
containment, xtc_fs, selective_receive (IOCP wakeup ordering),
single-op AND multi-op native file AIO (overlapped pwrite/pread reaped
from the port), and cross-thread wakeup coalescing (256 foreign
xtc_send via PostQueuedCompletionStatus) all pass on the real host.

ONE scenario was a SKIP through v1.20.1 and is now FIXED (with a
documented workaround, not a root-cause fix in the AFD driver itself --
see item 4 below): the loopback-socket connect/accept/echo over the AFD
poll path (smoke_sock_server / smoke_sock_client) now passes on a cloud
Windows Server 2022 host, verified across 5+ consecutive smoke runs, a
ASan build (7/8 clean; the 1/8 failure is a pre-existing, unrelated
flake documented separately below), and a dedicated CPU-scale test at
1,000/2,000/5,000/10,000 idle pending sockets (0.00% measured CPU at
every size).

**Status note (2026-07):** substantial progress on an MSVC Windows host
(Windows Server 2022, cloud VM); SEVEN real bugs found and fixed across two
work sessions (three in the IOCP round below, plus four more --
poll-timeout, pthread retval, thread affinity, slab mmap zero-fill --
in the full test-surface sweep; see the sweep update further down and
docs/M_WINDOWS_MATRIX.md); the one then-remaining item (tnt/test_tnt)
turned out not to be a bug at all -- see the tnt entry above -- and a
EIGHTH real bug (the raw EXCEPTION_* DOWN reason) was found in 2026-09
by getting m8/test_proc to run on the host:
1. FIXED -- the AFD poll IOCTL code was wrong.  It was built as
   CTL_CODE(0x12, 9, METHOD_BUFFERED, FILE_ANY_ACCESS) = 0x00120024;
   the AFD driver rejected every poll with STATUS_INVALID_DEVICE_REQUEST
   (0xC0000010).  The value the kernel accepts is the wepoll/libuv
   literal 0x00012024.  After the fix NtDeviceIoControlFile returns
   STATUS_SUCCESS/PENDING.
2. FIXED -- a stale-GetLastError loop-killer.  On an empty 0 ms poll,
   GetQueuedCompletionStatusEx returns FALSE with n_done == 0 but
   GetLastError can carry a STALE code (e.g. ERROR_ALREADY_EXISTS 183)
   rather than WAIT_TIMEOUT, so xtc_io_poll wrongly returned
   XTC_E_INTERNAL and killed the loop.  Now treats n_done == 0 as the
   benign timeout it is.
3. FIXED -- synchronous AFD completions.  When the fd is already ready
   the poll IOCTL returns STATUS_SUCCESS and does NOT queue a port
   completion, so accept-ready / connect-complete were never reaped.
   The arm now posts a self-completion for the synchronous case; a
   reg->pending guard makes a duplicate a no-op.  Accept and connect
   readiness now flow (verified: the server accepts, the client
   connects + sends).
4. WORKED AROUND (2026-07-13) -- the ASYNC (STATUS_PENDING) AFD poll
   does not complete when the socket LATER becomes ready.  Root cause
   confirmed with a standalone reproducer that has NO libxtc code in
   it: a ~150-line program opens \Device\Afd directly, arms one async
   IOCTL_AFD_POLL for AFD_POLL_RECEIVE on a not-yet-readable accepted
   socket, sends from the client, and waits on the port -- the poll
   returns STATUS_PENDING and NEVER completes, even though the socket
   is genuinely readable afterward.  A follow-up experiment proved AFD
   CAN always answer with CURRENT readiness synchronously (arming a
   poll on a socket that already has data waiting returns
   STATUS_SUCCESS with the correct event bits, reliably, every time)
   but never notifies of a FUTURE readiness change through this
   driver/version's \Device\Afd handle.  Cancelling a stuck pending
   poll does not help either: NtCancelIoFileEx finalizes the IRP's
   IOSB in-process (STATUS_CANCELLED) but, like every other
   synchronous AFD outcome on this driver, does not post a port
   completion.  This rules out libxtc's IOCP reap loop, registration
   lifetime, base-handle resolution (SIO_BSP_HANDLE_POLL resolves
   correctly and identically every time), and the IOSB/OVERLAPPED
   aliasing as causes -- the defect is in the AFD driver's future-edge
   tracking for this handle, upstream of anything libxtc controls.

   The workaround (`__xtc_iocp_repoll_sweep` in src/io/io_iocp.c):
   xtc_io_poll periodically re-checks every PENDING registration older
   than XTC_IOCP_REPOLL_NS (8 ms) with a single BATCHED, zero-timeout,
   throwaway AFD poll covering up to 64 overdue sockets per syscall
   (confirmed by direct experiment: a second, independent poll on a
   socket that already has its own long-pending poll outstanding does
   not disturb it, and Timeout=0 always resolves synchronously with
   the true current state).  Only the sockets the probe actually
   flags ready are canceled and re-armed on their real registration
   OVERLAPPED, which self-posts through the existing synchronous-arm
   path.  A first per-socket (not batched) version of this workaround
   measured 50%, 90%, and 98% CPU with 1,000, 2,000, and 5,000 idle
   pending sockets respectively (one cancel+rearm syscall pair per
   socket per interval, regardless of readiness); the batched redesign
   measured 0.00% CPU at 1,000/2,000/5,000/10,000 idle pending sockets
   (one cheap syscall per up-to-64 sockets when nothing is ready).

   Latency caveat (measured, not theoretical): GetQueuedCompletionStatusEx's
   millisecond timeout is quantized to the process's current Windows
   timer resolution, which defaults to ~15.6 ms (the classic 64 Hz
   system tick) unless something has raised it.  On the cloud host used
   for this work the observed worst-case echo latency was ~15.6 ms
   regardless of the 8 ms constant; calling timeBeginPeriod(1) (from
   ANY process on the system, not necessarily this one) measured
   ~3-5 ms instead.  libxtc does not call timeBeginPeriod itself -- it
   would raise the SYSTEM's timer resolution for as long as this
   process runs, a global side effect this project does not impose on
   an embedder's behalf (the same reasoning as not pinning threads to
   cores).  An application that wants tighter Windows socket-readiness
   latency than the default tick can call timeBeginPeriod(1) itself.

   This is a workaround, not a fix to the AFD driver's own behavior --
   if a future Windows version or driver starts posting the completion
   correctly, the sweep still works (it only ever refreshes sockets
   that are ALREADY overdue; a socket whose real completion arrives
   promptly never reaches the sweep).  Full repro harness, the batching
   experiments, and the SSH recipe are recorded in
   .agent/AFD_ASYNC_COMPLETION_2026-07.md.

**Update (2026-07, full MSVC test-surface sweep, cloud Windows Server
2022):** every `TESTS_C` test was built + run individually under
cl.exe 2022 and the Windows gate grew from 16 to a 100-test HARD GATE
(`build_msvc.bat` step 5, all-pass, 0 warnings under `/WX`; per-test
matrix in docs/M_WINDOWS_MATRIX.md).  Four more real bugs were found
and fixed on the host, plus the poll-timeout bug below:

5. FIXED -- `xtc_io_poll` ignored its caller timeout across AFD repoll
   slices.  It capped the wait to the 8 ms sweep interval and returned
   after a SINGLE GetQueuedCompletionStatusEx slice, so a poll armed
   while the socket was not yet readable (bug 4 above) returned XTC_OK
   with zero events even though the caller asked to block up to
   timeout_ns and data had since arrived.  `test/m2/test_io_events`
   E1/E3/E5 (readable/HUP/many-ready) caught it: writable fired (socket
   immediately writable) but readable never did.  Fix: a
   deadline-bounded loop that sweeps + re-arms between 8 ms slices
   until an event/wakeup is emitted or the caller's real deadline
   elapses (src/io/io_iocp.c).
6. FIXED -- the pthread compat shim discarded the thread return value
   (`m1/test_thread` T1/T2); see docs/M_WINDOWS_MATRIX.md.
7. FIXED -- `__os_thread_set_affinity` was XTC_E_NOSYS on Windows
   (`m1/test_cpu`); now SetThreadAffinityMask.
8. FIXED -- test_slab's Windows mmap shim used malloc, not calloc, so
   the shm slab version-check tripped on a reused heap block
   (`m11/test_slab` shm_reclaim); plus test_blocking stored a 64-bit
   clock in `_Atomic long` (LLP64 truncation).

## MSVC ASan: pre-existing, rare thread-startup flake (not the AFD workaround)

**Status:** KNOWN FLAKE, pre-existing, unrelated to any libxtc code.
While validating the AFD workaround above under an MSVC
`/fsanitize=address` build (2026-07-13, cloud Windows Server 2022), the
smoke test's overall pass rate was 7/8 clean runs; the 1/8 failure
reproduces identically with EVERY libxtc change reverted (confirmed by
building and running the unmodified pre-workaround baseline the same
number of times, which flaked at the same ~1-in-6-to-8 rate with the
identical signature):

```
AddressSanitizer: stack-buffer-overflow ... WRITE of size 360
    #0 _asan_wrap_memset (clang_rt.asan_dynamic-x86_64.dll)
    #1 RtlInitializeResource+0x759 (ntdll.dll)
    #2 BaseThreadInitThunk+0xf (KERNEL32.DLL)
    #3 RtlUserThreadStart+0x2a (ntdll.dll)
```

The fault is inside `RtlInitializeResource`, called from a brand-new
OS thread's startup (`BaseThreadInitThunk` -> `RtlUserThreadStart`),
before any libxtc code on that thread runs -- ASan's own
`__asan_handle_no_return`/stack-switch interposition on Windows
fibers/threads is a documented rough edge
(https://github.com/google/sanitizers/issues/189, which the smoke
test's own "ASan is ignoring requested __asan_handle_no_return"
warning already references).  Always occurs, when it occurs, AFTER the
loopback socket echo check has already printed `ok` -- it is a
teardown/thread-creation-path artifact, not a data-path bug.  Non-ASan
MSVC builds (the `/WX` zero-warning gate) and the plain smoke run never
exhibit it.  Tracked as a documented flake, not a release blocker;
re-running the smoke binary passes.

### Round 2 (current source): native completion port + AFD poll

`src/io/io_iocp.c` was rewritten from the round-1 readiness emulation
to a native completion-port design (details in
`docs/M_WINDOWS_MATRIX.md`):

  - `CreateIoCompletionPort` + `GetQueuedCompletionStatusEx` is the
    only wait primitive; the round-1 64-handle
    `WaitForMultipleObjects` cap is GONE.
  - Socket readiness uses the AFD poll fast path (`\Device\Afd` +
    `IOCTL_AFD_POLL` via `NtDeviceIoControlFile`), re-armed per
    completion (level-triggered).  This is the wepoll/libuv design.
  - Wakeup is `PostQueuedCompletionStatus`; file AIO is overlapped
    `ReadFile`/`WriteFile` reaped from the same port (no hEvent).
  - OVERLAPPED-ownership rule enforced: the kernel owns an OVERLAPPED
    from request-accept to completion-dequeue; deregister cancels with
    `NtCancelIoFileEx` and defers the free to the reap; registration
    nodes have stable heap addresses so the kernel-held back-pointer
    never dangles.  `dist/configure.ac` adds `-lntdll`;
    `dist/build_msvc.bat` links `ntdll.lib`.

**What is verified (Linux dev host this round):** cross-compiles clean
with mingw-w64 gcc 14.3.0 `-std=c11 -Wall -Wextra`, links into a
PE32+ binary against `-lntdll -lws2_32`, and leaves the Linux build
untouched (`io_iocp.c` is `XTC_IO_BACKEND_IOCP`-only -- an empty TU on
Linux; the full C munit suite stays green on epoll/uring).

**What is NOT verified:** the backend has NOT executed on Windows.
The AFD poll correctness, the level-triggered re-arm (no busy-loop, no
dropped edges), the cancel/lifetime under churn (no double-free, no
freeing a kernel-owned OVERLAPPED), the wakeup coalescing/ordering,
and the file-AIO port round-trip must all be validated on santorini
before this is production quality.  Same reviewed-but-untested status
as `src/io/io_aix.c`.  This rewrite is the suspected fix territory for
the `test_proc::selective_receive` flake (the wakeup ordering changed),
but that cannot be confirmed without the host.

**RUNTIME-VERIFIED (2026-08, cloud Windows Server 2022 VM, 8 vCPUs, MSVC
2022 Build Tools):** the four IOCP-runtime smoke scenarios below ALL
PASS on real Windows -- native IOCP file AIO (8 overlapped pwrite/pread
at distinct offsets), cross-thread wakeup (256 foreign xtc_send via
PostQueuedCompletionStatus coalescing), loopback socket echo via AFD
poll (level-triggered re-arm), and the AFD repoll-sweep scale (256 idle
pending sockets, batched probe, not O(n)/socket).  Additionally the
MSVC munit subset (16 tests across m0/m1/m10/m11/m14) builds and passes
16/16 (after fixing the per-milestone munit git-symlink checkout on
Windows -- see dist/build_msvc.bat), and the DbgHelp backtrace backend
is runtime-verified (test/m12/test_backtrace_win.c: 5 frames captured,
SymFromAddr resolved a known function name).  The historical
"awaiting a santorini run" text is kept below for provenance.

**(historical) Smoke coverage WRITTEN, awaiting a santorini run (NOT yet verified):**
`test/msvc/smoke.c` now drives four IOCP-runtime scenarios beyond the
already-verified strerror/clocks/slab/lwlock/fault-containment set.
They COMPILE (cross-checked with mingw-w64 gcc 14.3.0 `-std=c11 -Wall
-Wextra -fsyntax-only` against the real Windows headers, with the MSVC
pthread shim forced into its `_MSC_VER` path) but have NOT been built
or run under cl.exe on a Windows host:

  - `smoke_aio_proc` (pre-existing): a single overlapped pwrite+pread
    round-trip at offset 0 -- the file-AIO port round-trip.
  - `smoke_aio_multi_proc` (new): several positioned overlapped
    pwrite/pread ops at distinct offsets in one loop run, so more than
    one AIO completion is reaped from the port per run (the reap loop,
    not just a single completion).
  - `smoke_xt_worker` + `smoke_xt_sender` (new): a foreign OS thread
    bursts `xtc_send` at a batch of parked worker procs, exercising the
    cross-thread `PostQueuedCompletionStatus` wakeup and its coalescing
    (`__xtc_io_iocp_wakeup_post`); the loop must reap every delivery and
    return once all workers exit.
  - `smoke_sock_server` + `smoke_sock_client` (new): a 127.0.0.1
    connect/accept/echo driven entirely by `xtc_proc_wait_fd` on the
    raw Winsock socket fds -- the AFD poll fast path and its
    level-triggered re-arm (WRITABLE for connect completion, READABLE
    for accept, then READABLE/WRITABLE for the echo).  Send/recv is raw
    Winsock in the test so its error handling does not depend on any
    errno mapping in the library net helpers; if a listen port cannot
    be bound on the runner the scenario SKIPS rather than fails.

These are the runtime scenarios the "What is NOT verified" list above
names; the smoke test is the intended Windows regression guard for
them once santorini runs `dist/build_msvc.bat` and reports the `ok`
lines.  Until that run they remain COMPILED-NOT-RUNTIME-VERIFIED.

### Round 1 (historical): WSAEventSelect emulation -- SUPERSEDED

The round-1 backend (WSAEventSelect + WaitForMultipleObjects,
hard-capped at 64 handles, ~60% of native IOCP throughput) passed the
full suite on the reference Windows toolchain (MinGW64: 233/233).
Those results describe the SUPERSEDED source, not the round-2 rewrite
above; they are retained here only as the baseline the round-2 code
must re-establish on santorini.  Round-1 notes that still stand:

  - **Clang64 POSIX-only test ports.**  `test_net_udp` used a bare
    `nanosleep` (absent in the Clang64/MinGW runtime) -- now portable
    (a `test_msleep` shim).  `test_proc_wait_fd` still uses `pipe(2)` +
    `clock_gettime(CLOCK_PROCESS_CPUTIME_ID)` + pthreads; porting it to
    the socket-pipe compat is straightforward but must be verified on
    the host before landing (an unverifiable Windows edit risks a
    silent Linux regression), so it is deferred to a santorini pass.
  - **`test_proc::selective_receive` flake** (above): the cooperative
    equivalent (`test/otp/test_otp_proc_lib.c`) passes on every
    platform, so selective receive itself is correct; the flake is in
    test_proc's exact IOCP-wakeup timing and needs the host to chase.

**Native file AIO (mechanism):** `xtc_io_aio_submit` issues overlapped
`ReadFile`/`WriteFile`; `fsync`/`fdatasync` have no async form on
Windows (FlushFileBuffers is synchronous) and are offloaded.  Round 1
joined the OVERLAPPED.hEvent to the WaitForMultipleObjects set and was
verified on santorini under VS2022 (17) and VS2026 (18) by the smoke
test (an overlapped pwrite+pread round-trip).  Round 2 instead reaps
the AIO completion from the port (no hEvent); that variant is
COMPILED-NOT-RUNTIME-VERIFIED and the smoke test must be re-run.

Driving the santorini host non-interactively from CI/automation is not
yet wired (it is configured for an interactive PowerShell session); the
Windows matrix is run by hand via `dist/santorini-matrix.sh`.

## Async file I/O: backend coverage

`xtc_aio_pread/pwrite/fsync/fdatasync` present one portable API; the
native completion mechanism is per backend, with a blocking-pool
offload everywhere else (always correct, just thread-backed).  Native
completion is implemented and validated on:

  - **io_uring** (Linux): `IORING_OP_READ/WRITE/FSYNC`, reaped from the
    CQE ring.  CI `build-and-test` + ASan/UBSan.
  - **IOCP** (Windows): overlapped `ReadFile`/`WriteFile` reaped from
    the completion port; fsync offloaded.  santorini smoke test.
  - **kqueue** (FreeBSD, macOS): POSIX AIO (`aio_read`/`aio_write`/
    `aio_fsync`) with `SIGEV_KEVENT` -> `EVFILT_AIO`.  CI `macos` job
    compiles and runs `test_aio` against it (green).

  - **illumos / Solaris event ports** (SunOS 5.11): POSIX AIO
    (`aio_read`/`aio_write`/`aio_fsync`) with `SIGEV_PORT` posting the
    completion as a `PORT_SOURCE_AIO` event on the loop's existing event
    port, reaped in the `port_getn` drain (`aio_return` -> publish
    `a->res` -> wake `a->tag`).  Implemented in `src/io/io_solaris.c`
    (`xtc_io_aio_submit` + `struct sol_aio`); RUNTIME-verified on the
    illumos/OpenIndiana host (sun) -- full `gmake check` passes,
    including the native `/aio/roundtrip` case.

Still offloaded to the blocking pool (native AIO is a follow-up with
platform-specific completion plumbing -- they are DIFFERENT mechanisms,
not one shared path):

  - **epoll / poll / select** (Linux without io_uring, generic POSIX):
    would need POSIX AIO with `SIGEV_SIGNAL` delivered to a self-pipe /
    signalfd the loop already watches (epoll has no AIO filter).  A new
    shared `io_posixaio.c` submodule.
  - **AIX**: AIX `aio_*` (or the legacy LIO interface).

The offload fallback makes "write storage code once as if AIO is always
available" hold on all of these today; the follow-ups only remove the
thread hop.

**Decision (updated 2026-07): illumos native AIO is now DONE and
verified; epoll/poll/select and AIX stay offload-backed deliberately.**
Assessment of the remaining targets:
- Linux without io_uring (epoll/poll/select): the only ZERO-THREAD
  clean form is libaio (io_submit + an eventfd via IOCB_FLAG_RESFD),
  which adds a dependency; POSIX AIO with SIGEV_SIGNAL is
  signal-safety-hazardous and SIGEV_THREAD reintroduces the very thread
  hop this would remove.  On Linux the zero-thread answer already exists
  and is native: io_uring.  So this target has near-zero value.
- illumos event ports (SIGEV_PORT -> PORT_SOURCE_AIO on the existing
  port): DONE.  It was implemented WITH the illumos host in the loop
  (the bring-up did surface an EINVAL from a stack-local port_notify_t;
  fixed by heap-allocating the notify+aiocb in struct sol_aio so they
  outlive the async call) and now passes the full suite on sun.
- AIX aio_*: no test host at all; remains offload-backed.
The offload path is CORRECT and COMPLETE on the still-offloaded targets
(native AIO is a pure performance optimisation that removes a thread
hop, not a correctness gap).

## Portable block-device I/O layer -- DONE (v1.4.0)

Implemented as xtc_bdev (src/io/io_bdev.c, xtc_bdev.h): open a raw
device / partition (or a regular file), query logical+physical sector
size and capacity via the native per-OS ioctl (BLKSSZGET/BLKGETSIZE64,
DIOCGSECTORSIZE/DIOCGMEDIASIZE, DKIOCGMEDIAINFO, the Windows drive
geometry IOCTL) with a fstat fallback, aligned pread/pwrite through the
xtc_aio path, and flush via xtc_aio_fsync.  See xtc_bdev(3).

## test_alloc M7 -- NOW ENABLED on Windows (the skip was stale)

**Status: RESOLVED (2026-09-09); the documented reason was STALE.**  The
previous entry read: "intentional -- `_aligned_malloc` returns memory
that requires `_aligned_free`, not plain `free`.  The hook surface uses
a single free path.  Keeping the M7 case Windows-skipped is correct."

That premise stopped being true when commit `d10c257` ("os/alloc: fix
UBSan misalignment on cache-line-aligned allocations") gave the
allocator vtable a MATCHED `aligned()` / `aligned_free()` PAIR.  There
is no longer a single free path: `__os_aligned_free()` exists, routes to
`_aligned_free` on Windows and `free` elsewhere, and `src/inc/os_alloc.h`
documents the two as a pair that MUST be used together.  The AGENTS.md
over-aligned-allocation rule depends on that pair existing.

What actually kept M7 unrunnable on Windows was that the TEST released
with the WRONG half -- `__os_free(q)` on memory from
`__os_aligned_alloc()`, which is exactly the mismatched-free bug the
pair exists to prevent and which would corrupt the heap on Windows.  So
the skip was masking a defect IN THE TEST, and the case as written could
never have been simply un-skipped.

**Fix:** M7 releases with `__os_aligned_free()` (the documented pair)
and runs on every platform.  It also gained the case the AGENTS.md rule
actually cares about -- an `XTC_CACHE_LINE` alignment stricter than
`max_align_t` -- plus an `aligned_free(NULL)` no-op check.

**Evidence:** `/m1/alloc/M7_aligned` OK on the Windows host (test_alloc
8 of 8, 0 skipped -- was 7 + 1 skip) and still OK on Linux (8 of 8).

## Windows multi-core scalability: datapoint RE-MEASURED, curve reproduces

**Re-measurement (2026-09-09):** re-run on the same instance shape as
the original datapoint (cloud x86_64 Windows Server 2022, 16
vCPU, MSVC 2022, IOCP backend) with the same `per_loop=30000`, so it is
a like-for-like comparison -- on a newer source tree (v1.43.0):

| loops | 2026-07 (below) | 2026-09 run 1 | 2026-09 run 2 |
|------:|----------------:|--------------:|--------------:|
|     1 |            77 K |          55 K |             - |
|     2 |           102 K |          81 K |             - |
|     4 |     **127 K**   |   **121 K**   |   **124 K**   |
|     8 |            91 K |          97 K |         100 K |
|    16 |            82 K |          80 K |          90 K |

Every run: `spawn_ok == done`, `fail = 0`.  **The documented shape
holds** -- spawn throughput peaks at 4 loops and regresses past it --
and the peak reproduces within ~5%.  The 1- and 2-loop numbers came in
lower than 2026-07 and the 8/16-loop numbers slightly higher, which
narrows the peak-to-tail ratio without changing the finding; repeated
runs at the top three points show a several-percent run-to-run spread,
so the small deltas are not worth attributing to any source change.
The 2026-07 conclusion (the ceiling is cross-loop work-steal cache
thrash + OS thread placement, NOT the slab allocator) stands unmodified.

**Status:** MEASURED on cloud x86_64 Windows Server 2022 (16
vCPU, MSVC 2022, IOCP backend) via bench/bench_win_scale.c -- a
portable spawn-throughput probe buildable under cl.exe (public xtc_*
API + xtc_clock_mono only, no POSIX headers).  The Win32-fiber
substrate corruption blocker is fixed (above), so these numbers are on
a sound runtime.

Spawn throughput (spawn + immediate-exit child, per-loop drivers,
per_loop=30000, all runs done==total, fail=0):

    loops= 1   77 K spawns/s
    loops= 2  102 K spawns/s
    loops= 4  127 K spawns/s   <- peak
    loops= 8   91 K spawns/s
    loops=16   82 K spawns/s

Two honest findings:

1. **Spawn throughput peaks at 4 loops and regresses past it** (as
   measured).  This is NOT the slab allocator: the FlsAlloc fiber-local
   magazine (below) is validated engaging (a slab alloc/free microbench
   on the MSVC host shows fast=2,000,000 slow=256 -- every op hits the
   fast path -- at 26.3 M ops/s), and re-measuring the spawn curve WITH
   the magazine active gives the SAME shape (74K->129K peak at 4 loops
   ->83K at 16).  So the spawn bottleneck past 4 loops is cross-loop
   work-steal cache thrash + OS thread placement (the same NUMA/steal
   placement characteristic documented for the Linux reuse benchmark),
   not allocation.  All runs correct (spawn_ok==done, fail=0).
   The slab magazine is now FlsAlloc-backed on Windows and VALIDATED on
   an MSVC host (Windows Server 2022, 16 vCPUs): FlsAlloc/
   FlsGetValue/FlsSetValue give each fiber its own tls_mag array that
   correctly follows the fiber across SwitchToFiber, unlike the
   __declspec(thread) static TLS (the original corruption cause); the
   FlsAlloc callback frees it on fiber exit.  The MSVC smoke
   (slab round-trip + the xtc_xproc DOWN-reason=42 path, which drives
   __mon_alloc through the slab on a fiber) passes with no crash/hang.

2. **A high-volume ceiling was found AND fixed.**  At per_loop=50000 x
   16 loops (800 K short-lived procs) only ~422 K children ran
   (deterministic), because completed coros are not freed until
   loop_fini (see __xtc_coro_step / loop.c -- true on all platforms)
   and each accumulated Win32 fiber COMMITTED its full 64 KiB working
   stack, exhausting the process commit charge (~27 GiB on a 30 GiB
   box).  Fixed in coro_winfiber.c: CreateFiberEx now commits only a
   small initial 16 KiB and lets the OS grow the stack up to a >= 1 MiB
   reserve, so the commit charge is proportional to actual stack use;
   800 K x 16 loops now completes done==total.  The deeper item --
   eagerly freeing completed coros instead of accumulating them until
   loop_fini -- remains a cross-platform improvement (Windows just hit
   the wall first because of the fiber commit).

A Seastar/Tokio comparison on Windows is now UNBLOCKED but not yet run:
it wants (a) the FlsAlloc magazine so the fast path is measured, and
(b) message-throughput + tasks/sec workloads added to
bench_win_scale.c.  POSIX scalability was measured at scale (cloud Phase
B, 192 vCPU); the Windows curve above is the first cross-platform
datapoint.

## xtc_xproc end-to-end spawn+monitor on Windows -- RESOLVED

**Status:** RESOLVED (2026-07, cloud x86_64 Windows Server 2022 + MSVC
2022).  The full cross-process path now works end to end on Windows:
CreateProcess re-exec + `--xtc-xproc-child` sentinel + loopback-TCP
control connect (nonce) + dedicated reader thread + shadow-proc monitor
+ exit-latch loop wakeup deliver a normal DOWN with the child's exit
reason.  The MSVC smoke test `xtc_xproc: re-exec child
spawn+send+monitor, DOWN reason=42` is now a HARD GATE (was a SKIP);
40/40 non-ASan runs pass and 3/3 MSVC-ASan runs are clean.

Two substrate bugs were the blocker (both fixed), found via MSVC
AddressSanitizer on the box:

1. **coro_winfiber.c fiber stack was undersized.**  `CreateFiberEx` was
   called with `dwStackCommitSize == dwStackReserveSize == 64 KiB`,
   leaving zero growth headroom.  A proc's exit teardown
   (`__proc_entry -> __notify_links_and_monitors -> xtc_send -> slab`)
   plus a Win32 API callback (a ~360-byte ntdll memset seen under ASan)
   overflowed the fiber stack and corrupted adjacent memory (the
   near-NULL slab-pointer AV).  Fix: reserve generously (>= 1 MiB, or
   16x the committed working size) so the OS grows the stack normally,
   committing only the requested size.

2. **The slab per-thread magazine used `__declspec(thread)` (static
   TLS), which is unsafe under Win32 fibers.**  The compiler may cache
   the static-TLS base across a `SwitchToFiber`, so a magazine pointer
   handed to `xtc_slab_alloc` dereferenced a stale TLS block on a
   resumed fiber -- traced to a hang/corruption at
   `mag->slots[--mag->n]` inside `__mon_alloc` (the monitor entry
   allocation), which is exactly the path xtc_xmonitor drives.  Fix:
   `__tls_mag_for` returns NULL on Windows, so every alloc/free takes
   the locked slow path (touches no `__declspec(thread)` state, correct
   on a fiber).  Performance-only tradeoff -- a fiber-local-storage
   (FlsAlloc) magazine is a later optimization; Windows is not the
   throughput target yet.

POSIX xtc_xproc (fork/waitpid) and the fcontext/ucontext substrates are
unchanged.  The reader-thread control channel and the io_wakeup
exit-callback (which fixed the earlier proc-table-SRWLock deadlock) are
kept.

## Fiber-switch sanitizer annotations enable detect_stack_use_after_return=1

**Status:** SHIPPED (v1.13.0 for ASan; TSan fiber-identity added
v1.17.0).  The fcontext and ucontext coro substrates annotate every
user-space stack switch for the sanitizers, using the CORRECT,
mutually-exclusive API for each:
  - **ASan/LSan** (`XTC_FIBER_SWITCH_ANNOTATE`, `__SANITIZE_ADDRESS__` /
    `__has_feature(address_sanitizer)`): the stack-switch API
    `__sanitizer_start_switch_fiber` / `__sanitizer_finish_switch_fiber`
    around every switch, so ASan tracks the fiber stacks instead of
    mis-attributing stack memory.  The full fiber runtime
    (test_fctx/proc/async/svr/fsm) passes ASan with
    `detect_stack_use_after_return=1` on BOTH substrates.
  - **TSan** (`XTC_TSAN_FIBERS`, clang `__has_feature(thread_sanitizer)`
    only): the fiber-IDENTITY API -- `__tsan_create_fiber` at coro
    create, `__tsan_switch_to_fiber` at every switch (into a coro, and
    back to the once-captured scheduler fiber), `__tsan_destroy_fiber`
    at teardown -- so TSan carries per-fiber happens-before across
    cooperative switches instead of seeing one confused thread.  TSan
    does NOT provide `__sanitizer_*_switch_fiber`, so the two guards are
    mutually exclusive (TSan decided first; a TSan build emits ONLY the
    identity calls, an ASan build ONLY the stack-switch calls).  A full
    `clang -fsanitize=thread` libxtc build runs test_fctx/async/proc/
    svr/chan with ZERO TSan warnings.  clang-only by design: gcc's
    libtsan has no fiber support, so a gcc TSan build emits no fiber
    annotations (documented; not a gcc-buildable TSan-with-fibers
    configuration).  Requested by the PG-integration team and delivered.

Compiled to nothing in a non-sanitized build (0 sanitizer references,
verified: `nm libxtc.a | grep __tsan_` is empty).

**CI:** the shared ASan job runs with `detect_stack_use_after_return=1`
on every commit.

## __notify_links_and_monitors DOWN-send vs proc-teardown race (RESOLVED)

**Status:** RESOLVED (v1.13.0) by a teardown refcount on the proc struct.
Surfaced by the fiber-switch annotations under
`detect_stack_use_after_return=1` (which shifts scheduling enough to
widen the window): when a proc exited, `__notify_links_and_monitors`
sent a DOWN to each monitoring proc via `xtc_send`; a monitor proc
concurrently torn down on another thread could be freed between
`__resolve` returning its pointer and `__mbox_deliver_locked` touching
its mailbox -- a heap-use-after-free.  This was the SAME root cause as
the blocking-pool wake UAF and (suspected) the macOS sqlxtc MT-load
flake: `__resolve` handed out a `struct xtc_proc *` after releasing the
table lock, with no lifetime guarantee.

**Fix:** an atomic `refs` on `struct xtc_proc`.  `__table_lookup` takes a
reference WHILE HOLDING the owning table lock -- atomic with the detach
in the teardown path -- so a resolver either pins a live proc or sees
NULL, never a freed pointer.  The owner (spawn) holds one ref; teardown
detaches from the table then drops the owner ref, and the struct's
mailbox-drain / lock-destroy / free happen in `__proc_free` only when the
last ref (owner + any in-flight resolver) is released.  Every `__resolve`
caller (`xtc_send`, `xtc_exit_pid`, `xtc_proc_wake`, the deferred-delivery
callback, link/monitor push, `xtc_proc_mailbox_stats`) releases its ref.
CI now runs the ASan job with `detect_stack_use_after_return=1`;
validated by 5 clean full-`make-check` runs under ASan+SUAR (0 UAF, 0
leaks).  A DST test (test_sim_proc_teardown) models the resolve-vs-exit
race deterministically.

## svr.c branch coverage

**Status:** improved this round with NULL/invalid-argument guard tests
(test/coverage/test_fault_inject.c `/svr/null_guards`), which exercise
the early-return XTC_E_INVAL edges across xtc_svr_start/stop/join/reply/
call/cast that the happy-path server tests skipped, PLUS the reply-path
OOM edge (`/svr/reply_oom`): a new `svr.reply.oom` injection point at
the top of xtc_svr_reply forces the non-empty-reply allocation to fail,
covering the XTC_E_NOMEM return on BOTH reply branches (the synchronous
slot path and the in-proc reply_pid tag+payload path) -- ASan-clean, no
leak on the failure path.

**Targets remaining:** the deeper call-after-stop timing edge (a call
that races xtc_svr_stop) still needs a scheduler-ordering harness
rather than a plain injection point; it is error handling, not mainline
behavior.

## io_common.c coverage

**Status:** improved this round.  The four `io.init.*` fault-injection
points (calloc/pipe/fcntl/backend fail) were already tested; added
`/io/null_guards` (xtc_io_init/fini/wakeup reject NULL with
XTC_E_INVAL) and `/io/wakeup_roundtrip` (the live xtc_io_wakeup post +
coalesced second post) in test/coverage/test_fault_inject.c.  The
remaining uncovered branches are backend-specific cleanup edges in
`xtc_io_fini` and the ENOMEM/EAGAIN corners only reachable on a
particular backend; a per-backend fault-injection sweep is the way to
close them further.

## RESOLVED: macOS/arm64 xtc_dump() SIGBUS when called from a live fiber

**Status:** RESOLVED (fiber-stack-aware backtrace walker, `8034f93`;
fallback hardened so it can never reach the unbounded system
`backtrace()` from a fiber).

`xtc_dump()` captures a C backtrace of the calling thread.  On
macOS/arm64, when called from inside a running fiber (a live `xtc_proc`
body, not the top-of-thread panic/abort path), the system `backtrace()`
walks the frame-pointer chain of the small guard-paged fiber stack and
could run past the stack top into unmapped memory, raising SIGBUS --
intermittently (it depended on what lay just past the fiber stack and
whether that address was mapped), which is why it surfaced only
occasionally in CI's `test_dump/basic`.

**Fix:** `src/os/os_backtrace.c` now uses a fiber-stack-aware frame
walker on Apple targets instead of the system `backtrace()`.  It
discovers the active stack's VM region at call time from its own SP
(`mach_vm_region` -- a query of the task's VM map, which cannot itself
fault) and walks the FP chain manually, stopping the instant the next
frame record would leave the mapped region, is misaligned, or does not
ascend -- so an over-walk past a guard-paged fiber stack top can never
touch an unmapped page.  No coroutine-layer coupling: the bound is
derived from the running SP, so it works whether the caller is on a
fiber mmap or an OS-thread stack.  If the region cannot be resolved (a
rare `mach_vm_region` failure), the walker returns NO frames rather
than falling back to the unbounded system `backtrace()` -- the dump
then prints "backtrace unavailable", which is strictly better than a
SIGBUS.  The symbolization step (`backtrace_symbols_fd`) only resolves
the already-captured, in-bounds return addresses (a dladdr-style
lookup, not a stack walk), so it does not fault.

Other platforms were never affected: Linux uses the EH-ABI unwinder
(`_Unwind_Backtrace`), which terminates cleanly on a fiber stack.
`test_dump/basic` exercises the from-a-fiber path (its `dump_driver`
runs as an `xtc_proc` and calls `xtc_dump`), so the macOS CI runner
covers the fixed path per commit.

## AIX: not a supported target (off the roadmap)

**Status:** UNSUPPORTED / UNMAINTAINED.  An AIX/ppc64 OS-layer port
(os_thread/os_mutex/os_tls + an io_aix.c pollset backend + POSIX AIO
offload) exists in-tree and compiles, but AIX is deliberately NOT a
supported platform: it is never built or run in CI, has no runtime
verification, and is off the roadmap.  AIX is IBM-proprietary
(ppc64-only; PostgreSQL itself dropped AIX in v16), the install media
is licensed, and emulating it under QEMU/TCG is impractical for a
reliable CI gate.  The in-tree port is left in place (it is harmless
and compiles cleanly) but carries no support commitment -- treat any
AIX behavior as unverified.  Do not file AIX as a gap; it is a
decision, not a TODO.

## RESOLVED: macOS now in CI

The `macos` GitHub Actions job (macos-latest, Apple Silicon) builds and
runs the full C munit suite every commit -- kqueue backend, fcontext
substrate (the runner is arm64), GCD dispatch semaphores.  Standing it
up fixed six real portability bugs (Darwin feature macro, rwlock
storage size, unnamed semaphores, _SC_NPROCESSORS_ONLN, hardcoded -lrt,
lrlock slot reclamation teardown order).


## Apple Silicon (macOS arm64): now defaults to the fcontext substrate (macOS x86-64 still ucontext)

**Status:** RESOLVED for Apple Silicon as of 1.23.x -- macOS/arm64 now
defaults to the hand-written fcontext coroutine substrate, closing the
per-switch performance gap.  macOS *x86-64* still uses ucontext (there
is no Mach-O x86-64 fcontext assembly), and involuntary preemption
remains Linux-only everywhere on macOS (see below).

`src/os/asm/fctx_aarch64_aapcs_macho.S` provides the AAPCS64
make/jump_fcontext in the Mach-O assembler dialect (leading-underscore
symbols, `.p2align`, no ELF-only directives), byte-identical in register
discipline to the ELF variant.  `src/evt/coro_fctx.c` and
`src/evt/coro_uctx.c` select it as the default on `__APPLE__ &&
__aarch64__` (equivalent to `-DXTC_CORO_FORCE_FCTX`); build with
`-DXTC_CORO_FORCE_UCONTEXT` to fall back to `swapcontext`.  This removes
the per-switch `sigprocmask` syscall (measured on Apple M-series:
~34 us/task-switch on ucontext under `bench_million_tasks.c` at 1M
fibers -> ~23 us there, with a larger per-switch win masked by memory
pressure at that scale; a 2-fiber micro-bench shows the true delta).
The fcontext path on macOS declines involuntary preemption, so it falls
back to Phase-1 cooperative preemption there.

Involuntary preemption (`src/ptc/preempt.c`, Phase 2) is STILL
Linux-only: it needs a per-thread `SIGVTALRM`-class timer
(`timer_create`/`CLOCK_THREAD_CPUTIME_ID`), which macOS does not offer
(`ITIMER_VIRTUAL` is process-wide, not per-thread).  On macOS,
`xtc_preempt` degrades to cooperative-only: a fiber that never calls a
yield point will not be involuntarily interrupted.  See
`man/man3/xtc_preempt.3` for the per-platform statement.

**Remaining:** a macOS x86-64 Mach-O fcontext variant (low priority --
Apple has moved to arm64), and a GCD dispatch-timer or kqueue
`EVFILT_TIMER` per-worker tick to give Phase-2 preemption a real signal
source on macOS (tracked in PLAN.md).  Neither changes correctness.


## sqlxtc multi-thread load test flakes on the macOS CI runner

**Status:** KNOWN FLAKE (transient, re-run passes).  The
`sqlxtc multi-thread load` CI step (`test/sqlxtc/test_sqlxtc_mt.sh` ->
`test_sqlxtc_concurrent.py`, N clients * M queries against a shared
server on a plain `CREATE TABLE`) intermittently fails on the macOS
runner with all clients timing out and `server CRASHED under multi-loop
load (cores=3)`.  Observed once on 5d78c00 and passed on re-run with no
code change; the 5 preceding commits' macOS jobs were green.

The failing path is the VDBE/connection handling under concurrent load
(the test table is NOT xstore-backed, so it never reaches the vexec
fast path) -- so it is independent of the vexec/Track-B work.  Likely
the same multi-loop cross-thread timing sensitivity seen elsewhere on
the macOS/kqueue runner.  Treat a lone macOS MT-load failure as flaky:
re-run the job (`gh run rerun <id> --failed`) and judge by the other 12
jobs.  A persistent failure across re-runs would indicate a real
concurrency regression and must be chased.

## `pbt_proc::send_recv_roundtrip` and `pbt_proc::fifo_order` flake under `make check`

**Status:** RESOLVED in M11.5b.  The proc registry's `__lt[]` table was
leaking entries on `xtc_loop_fini`; consecutive PBT loops were aliasing
stale entries.  Fix: added `__xtc_proc_loop_unregister(loop)` called from
`xtc_loop_fini`.  Both properties are re-enabled.

## xtc_cfg: feature status (was "missing features")

**Status:** All of it DONE, including per-session scoping -- which this
entry previously declared "out of scope for the runtime."  That is no
longer true and the old text is corrected here rather than left to
mislead.

- Configuration-file parsing (postgresql.conf reader): DONE --
  `xtc_cfg_load_file()` reads `name = value` lines (comments, quotes,
  per-kind parsing, bounds/validators), skipping unknown/bad lines.  As
  of `d18481b` an integer value is parsed WIDE and then range-checked
  against the target C type, so `4294967297` for an `XTC_CFG_INT` knob is
  rejected as invalid rather than truncated to 1.
- SIGHUP-driven reload: DONE as a mechanism -- `xtc_cfg_reload()`
  re-reads the last loaded file.  The app wires SIGHUP to it from the
  event loop (the function is not async-signal-safe, by documentation).
- Hot-path read handles: DONE -- `xtc_cfg_ref()` resolves a name to an
  opaque, pointer-stable `xtc_cfg_ref_t` once; `xtc_cfg_ref_get_*` then
  read with no name lookup and no scan, still observing live
  `xtc_cfg_set_*` updates.  A registry entry is never relocated, so the
  handle can be cached for the process lifetime like a compiled-in
  pointer.
- Per-session / per-database scoping, the transactional override stack,
  and source-precedence tracking: **DONE** (`47443de`, hardened in
  `d18481b`), not out of scope.  `xtc_cfg_session_create/_destroy/_bind/
  _current` manage a session; `_push`/`_commit`/`_abort` give the
  nestable level stack that backs `SET LOCAL`, `SET` inside a
  rolled-back transaction, function-local `SET`, and subtransactions;
  `xtc_cfg_ssn_set_*` set a per-session override carrying a source rank
  (`XTC_CFG_SRC_DEFAULT`..`_OVERRIDE`) that a lower-ranked source cannot
  displace; `_source` reports the winner and `_reset` drops the current
  level's override.  The binding is per-FIBER (it rides a yield and a
  work-stealing migration), and unregistering a variable a live session
  still overrides is safe in either order because entry storage is
  refcounted.  Unscoped `xtc_cfg_get_*` resolve through the bound
  session when there is one and fall straight through to the global value
  when there is not, so bare-name reads work as in PostgreSQL.  See
  `xtc_cfg(3)`.

Nothing in `xtc_cfg` is known missing today.  If something is wanted, it
is a new feature request, not an unfinished one.

## xtc_slab_pressure_stop API incomplete

**Status:** DONE.

Resolved: `xtc_slab_pressure_listen_ex()` returns an opaque
`xtc_slab_pressure_t` handle and `xtc_slab_pressure_stop(handle)` joins
the listener thread, closes its fds, and frees it.  Plain
`xtc_slab_pressure_listen()` delegates to `_ex` and discards the handle
(unchanged fire-and-forget behaviour).


## epoll backend: rare lost blocking-I/O-completion wakeup under heavy churn

**Status:** RESOLVED (pending continued monitoring).  The primary causes
were fixed earlier (see (A)/(A-residual)/(B) below); the last remaining
epoll-only residual in the buffer-manager stress test appears CLOSED by
the v1.8.0 cross-thread prepare/park wake fix.

**The closing fix (v1.8.0):** the residual was the same lost-wake class
as the PostgreSQL carrier's cross-thread fd/latch wake miss -- a
cross-thread wake (here, the blocking pool completing an offloaded fsync
and waking the parked evictor) arriving while the target task was still
RUNNING, between arming its waker and the loop transitioning it to
PARKED on yield, was DROPPED by the XTC_INB_WAKE drain (which only
enqueued PARKED tasks).  The fix latches task->wake_pending when the
task is not yet PARKED and the RUNNING->PARKED transition consumes it
and re-schedules instead of parking (see src/evt/loop.c).

**Evidence:** test_bufmgr_mt, which historically hung ~3-7% of runs on
epoll, now passes 130/130 consecutive runs on the epoll backend with
zero hangs, and the reproducer test/concurrency/repro_blocking_epoll.c
(which hung 8/8 at its worst) passes 10/10.  test_bufmgr_mt is no longer
gated off epoll: it runs on the Codeberg (epoll) CI as well as the
io_uring CI.  Kept in this document (rather than deleted) so the
long investigation and the several rejected approaches below stay on
record; if any epoll wake-miss recurs, this is the history to read
first.

<details><summary>Historical investigation (the fixes that led here)</summary>

### (A) FIXED -- xtc_yield / xtc_await did not preserve __current_proc

The primary lost-wakeup was not in the epoll backend at all.  It was a
fiber-context bug: the per-thread "current proc" pointer
(`__current_proc`, an L3 process-layer TLS) was not preserved across a
yield.  When a proc calls `xtc_yield()` (or `xtc_await`) the scheduler
runs OTHER procs in between -- each setting `__current_proc` to itself --
and on resume the public yield primitives did NOT restore it.  Internal
parks (`xtc_proc_wait_fd`, `xtc_proc_sleep`, `xtc_recv`, the amutex)
each restored it by hand, but a plain `xtc_yield()` did not.  A proc
that yielded therefore resumed running as whatever proc ran last; its
next `xtc_blocking_run` -> `wait_fd` then registered the completion fd
under the WRONG task and parked the WRONG task, so the real proc never
woke.

Minimal reproducer: ONE loop, TWO procs, three `xtc_blocking_run` calls
each -- hangs 10/10 on epoll (a trace showed proc P1, after its
worker's bare `xtc_yield()`, registering its pipe fd under proc P2's
task tag and "P2" parking twice without waking).  io_uring masked it:
its completion path re-samples readiness, so the misattributed park
self-healed; epoll's `epoll_wait(-1)` blocked forever.

Fix: preserve the process context across every coro yield via a hook
(`__xtc_fiber_ctx_save` / `__xtc_fiber_ctx_restore`, installed by
`xtc_proc_spawn`, no-op when no process layer is in use, so the L2 coro
layer keeps no hard dependency on L3).  Applied to `xtc_yield` and
`xtc_await` in all three coro substrates (fctx, ucontext, Win32 fiber).
Verified: the pure reproducer goes 0 -> 20/20 on epoll (and the
N-loop/N-proc one 0 -> 20/20); io_uring `make check` + ASan + UBSan +
epoll `make check` all clean; io_uring `bufmgr_mt` 50/50.

### (A-residual) FIXED -- epoll-only hang in test_bufmgr_mt was a pin underflow

After the (A) fix, `test_bufmgr_mt` still hung about 3-7% on the epoll
backend (io_uring far less, and confounded by host load).  The earlier
read of this as a benign "thrash livelock" was WRONG: it was a real
pin-accounting bug in the example's from-scratch buffer manager
(examples/06_sqlxtc/bufmgr.c), traced to ground with armed abort probes
+ post-mortem cores and now fixed at root.

Root cause (swip mode only): a fixer can hold a STALE swip word `w`
pointing at a frame that has since been evicted, freed, and recycled by
the demand-load path for a DIFFERENT slot.  The fixer does
`try_pin(sw_frame(w))` and transiently bumps that recycled frame's pin
(it fails its slot recheck a moment later and unpins -- net zero).  But
the demand-load path claimed the frame with an UNCONDITIONAL
`store(pin, 1)`, which clobbered the fixer's transient increment; the
fixer's matching `unpin` then drove pin to -1.  A frame wedged at
pin == -1 is indistinguishable from an eviction reservation, so every
later fixer spins `try_pin` on it forever -- the hang.  (epoll merely
lost the timing lottery more often; io_uring re-samples readiness and
mostly masked it.)

Fixes, all in bufmgr.c, verified epoll 80/80 + io_uring 30/30 + ASan
12/12 + UBSan 8/8 clean, with the btree/xstore (pid-mode) tests green:

  - The swip demand-load and swip alloc paths claim a recycled frame
    with a CAS loop (`claim_frame`: spin CAS pin 0 -> 1, yielding),
    NOT a blind store, so a fixer's transient stale pin is never
    clobbered.  Scoped to swip mode only: pid-mode (`bm_fix_pid`, the
    B-tree path) has no swip references, so no stale fixer exists; it
    keeps the plain store (a CAS-wait there would deadlock against a
    latch-coupling pin that cannot drain while the claimer spins).
  - Eviction releases a reservation with CAS(-1 -> 0), never a blind
    store, so it cannot clobber a concurrent loader's fresh pin; and it
    re-validates `state == BM_COOL` after reserving (a stale COOL read
    could otherwise reserve a frame already on the free list).
  - `free_push` sets `state = FREE` before clearing pin, closing the
    COOL+pin==0 window an eviction sweep could otherwise reserve in.

Two general robustness improvements landed alongside (they reduce churn
and were part of the original investigation, kept because they are
correct on their own): `bm_fix` yields on a contended `try_pin` retry
so the loop regains control, and a CLOCK second-chance reference bit on
frames spares a recently touched COOL page one eviction sweep.

test_bufmgr_mt is no longer gated: it runs on BOTH the io_uring CI
(GitHub) and the epoll CI (Codeberg).

### (B) FIXED -- buffer-manager load/publish pin-ordering race

While chasing the epoll hang, a genuine buffer-manager race was found
and fixed (examples/06_sqlxtc/bufmgr.c).  In bm_fix's demand-load path
the frame was published as BM_COOL and only THEN pinned
(`state = COOL; pin = 1`).  In that window a concurrent evict_one --
which acts on COOL frames -- could win `try_reserve` (CAS pin 0 -> -1),
and the load's unconditional `pin = 1` store then clobbered the
reservation, leaving the frame both published (in use) and pushed back
onto the free list: a double-owned frame that corrupts the free list
(`free_n` seen at 762 for a 32-frame pool) and livelocks get_free_frame.
Fix: pin the frame BEFORE it is ever visible as COOL/HOT (own it from
get_free_frame on), so eviction's try_reserve always fails on it.  Same
reorder applied to bm_fix_pid.  Verified: io_uring make check + ASan +
UBSan clean, bufmgr_mt 12/12.  This race is normally hidden when the
offloaded load completes fast; it surfaces when completion is slow.

The buffer-manager multi-threaded stress test (`examples/06_sqlxtc`,
`test_bufmgr_mt`) hangs intermittently when libxtc is built with the
epoll I/O backend (`--with-io-backend=epoll`); the same test passes
reliably under io_uring (130+ runs).  It is therefore run only on the
io_uring CI (GitHub) and skipped on the epoll CI (Codeberg containers,
whose seccomp profile blocks io_uring).

Diagnosis (post-mortem core, non-instrumented timing):

  - All executor loop threads are idle in `epoll_wait`, the blocking
    thread-pool workers are idle on their condvar (so every submitted
    disk I/O has COMPLETED and its wakeup byte was written), free
    frames are available (`free_n == 8`), and there are zero data
    mismatches -- yet `xtc_exec_run` never returns because one worker
    proc (`g_workers_done == N_WORKERS - 1`) never finishes.

  - The stuck worker is the buffer-manager EVICTOR: it reserved a
    frame for eviction (`pin == -1`), parked in `xtc_proc_wait_fd` on
    the offloaded flush write, and its completion wakeup was lost, so
    the frame stays `EVICTING` forever.  The loss is rare (~1 in many
    thousands of `xtc_blocking_run` calls) and timing-dependent, which
    is why the heavy-churn stress test triggers it while the lighter
    loop tests (test-vfs-loop, test-xstore, ...) do not.

  - io_uring masks it: its completion path samples current readiness
    rather than blocking indefinitely on a level/edge fd transition,
    so a missed edge self-heals; epoll's `epoll_wait(timeout = -1)`
    blocks forever.

The lost wakeup is in `xtc_proc_wait_fd`'s epoll arm/park/dispatch
path, hit through `xtc_blocking_run`.  A later session built the
minimal isolated reproducer this asked for -- N procs issuing
concurrent `xtc_blocking_run` on an N-loop epoll executor, no buffer
manager (test/concurrency/repro_blocking_epoll.c) -- and it hangs 8/8,
so the bug is purely in the library primitive.  Findings:

  - **It is NOT fd reuse.**  A persistent per-proc wakeup pipe (never
    reused across procs while a registration is live) still hangs, and
    a bounded 50ms re-poll of every loop does not recover the parked
    proc.  Ground truth from a core: stuck procs are PARKED on fds
    that are READABLE (the wake byte was written) yet are NOT in any
    epoll instance -- the fd was unregistered while the proc was still
    parked on it.  The only `del_fd` that can do this is dispatch's
    `del_fd(t->park_fd)`.

  - **Approaches tried, and why each was rejected (none shipped):**

    1. *Cross-thread waker* (pool wakes the proc via xtc_waker_wake):
       fixes the hang but ASan reports a heap-use-after-free -- the
       proc can return from xtc_blocking_run and exit the instant its
       result is read, racing a wake that holds its task pointer.
       NOTE (v1.13.0): this is the SAME root cause as the now-fixed
       resolve-then-deliver UAF (see "proc-teardown race RESOLVED"
       above).  The proc-teardown refcount makes it safe for a peer to
       hold a proc reference across the wake; revisiting this approach
       with the refcount taken around the pool wake is the likely clean
       fix for the lost-wakeup, and should be tried in the dedicated
       session.

    2. *Timer-poll* (proc polls a done flag via xtc_proc_sleep):
       memory-safe and fixes the pure reproducer, but its added
       latency makes test_bufmgr_mt deadlock on BOTH backends (it
       slows completion enough to provoke buffer-manager frame
       exhaustion), i.e. it REGRESSES io_uring, which passes today.

    3. *Persistent per-proc wakeup pipe*: still hangs (ruling out fd
       reuse, as above).

    4. *Remove del_fd from dispatch* (rely on the parker's cleanup +
       run-before-poll): made the reproducer hang 12/12 -- the
       still-readable fd is mishandled, so the dispatch del_fd is
       load-bearing in a way not yet understood.

    5. *Event carries the fired fd* (epoll stores the fd in
       `data.u64`, a per-loop fd->tag map recovers the registrant, and
       dispatch dels exactly `ev->fd` -- the fd that actually fired --
       instead of a possibly-stale `t->park_fd`): implemented across
       all backends, verified to NOT regress io_uring (make check +
       reproducer + bufmgr_mt all green), but the epoll reproducer
       still hangs 15/15.  So the wrong-fd-del was not the root cause;
       reverted rather than ship inert complexity (a per-loop map).

    6. *Non-blocking completion read + re-wait* (set the pipe read end
       O_NONBLOCK and loop wait_fd until the byte is actually read, so
       a wakeup with no byte yet cannot block the loop thread in
       read()): contained to blocking.c, but the epoll reproducer
       still hangs 15/15.  Reverted.

Sharper diagnosis (instrumented, this is where it stands):

  - A parked proc resumes from its wait_fd `xtc_yield()` with
    `wake_revents == 0` AND `park_fd` still set -- i.e. it was
    re-scheduled WITHOUT the fd dispatcher running (dispatch sets
    wake_revents and clears park_fd before waking).  Such "spurious"
    resumes are not rare: tens of thousands per run.  The committed
    blocking_run then does a blocking read() on the not-yet-readable
    pipe, wedging the loop thread so it stops polling -- a plausible
    cascade into the hang -- but making that read non-blocking
    (approach 6) did not fix it, so the spurious resume is not the
    whole story.

  - The spurious resumes are NOT cross-thread: an instrumented
    `__xtc_inbox_push` counted ZERO `XTC_INB_WAKE` pushes for the
    whole run, so no `xtc_waker_wake` cross-thread path fired.  The
    only same-thread enqueue of a PARKED proc is the fd dispatcher,
    which clears park_fd and sets wake_revents -- so a resume with
    neither updated is self-contradictory under the current model and
    indicates the park/dispatch bookkeeping is desynchronised from the
    actual scheduler wakeup in a way the probes perturb rather than
    pin.  Per-fd counters also show stuck fds with `del == reg` and
    `deliv == 0/1` (the fd is unregistered, by the parker's own
    cleanup on a spurious resume, before epoll ever delivers its
    event), consistent with the "readable but not in any epoll"
    post-mortem above.

The bug needs a dedicated redesign rather than a point fix -- most
likely a per-proc wakeup eventfd registered ONCE at spawn with
EPOLLONESHOT re-arm (no per-wait registration churn, memory-safe, and
no cross-thread proc reference), or moving blocking-completion off the
fd-park mechanism entirely.  Until then test_bufmgr_mt runs only on the
io_uring CI and is skipped on the epoll CI, and xtc_blocking_run keeps
its committed pipe + wait_fd path (fast on io_uring).  The reproducer
and this write-up set up that focused effort.

### FIXED -- test_server_storage flaky hang (lost cross-thread wakeup)

`examples/06_sqlxtc/test_server_storage` hangs intermittently (~1 in 8
locally, io_uring backend) in the connection-per-proc + storage
workload.  A SIGABRT core of a hung run shows the scheduler loops
blocked in `io_uring_wait_cqes` while two blocking-pool workers sit in
`fdatasync` -- the WAL group-commit writer (wal.c `flush_io_fn`) and the
double-write buffer (bufmgr.c `dw_io_fn`).  The signature is a lost
cross-thread completion wakeup: a fiber offloaded an fsync via
`xtc_blocking_run`, the worker is (or just finished) running it, but the
loop never observes the completion and idle-waits forever.

Present at the shipped state (e36a68d), so it is NOT caused by the
xtc_aio page-I/O wiring; that work (do_io / bm_sync on xtc_aio) is
orthogonal.  It was surfaced while attempting to convert the WAL writer
and double-write buffer onto xtc_aio: that conversion must wait until
this lost-wakeup is understood, since adding more concurrent
completion traffic to the same hot path would only worsen it.

Likely the same lost-wakeup class as the (A) investigation; needs a
dedicated session with a clean core walk of the parked fiber's waker
state and the blocking-pool -> loop wake path.  Until then it is a
known flake on the io_uring examples CI.

FIXED (commit after b7a6558): the root cause was a cross-thread io_uring
submission.  xtc_proc_wait_fd -- which xtc_blocking_run parks on --
registered its wait fd on self->task->loop->io (the proc's HOME loop),
but under the multi-loop executor a proc runs stolen on another loop, so
the POLL_ADD went to a ring this thread does not own and was silently
dropped.  Fixed by registering/timing/cleaning up on __xtc_current_loop
(the running loop).  test_server_storage 30/30; the WAL/double-write
xtc_aio conversion is now unblocked.

</details>

## RESOLVED: macOS build break: sigev_notify_kqueue (io_kqueue.c)

**Status:** FIXED.  A 2026-06 macOS SDK image dropped the BSD
`struct sigevent` member `sigev_notify_kqueue`, so the native-file-AIO
path added in ce0cacc failed to compile on macOS with "no member named
'sigev_notify_kqueue'".  The `#if` guard
(`defined(EVFILT_AIO) && defined(SIGEV_KEVENT)`) passed, but the member
was absent under the current SDK.

**Fix:** the native kqueue file-AIO path is now restricted to the
platforms that actually provide the member AND honor SIGEV_KEVENT
completion for regular files -- FreeBSD and DragonFly
(`#if ... && (defined(__FreeBSD__) || defined(__DragonFly__))` in
src/io/io_kqueue.c).  macOS is deliberately excluded and falls through
to the blocking-pool offload (the XTC_E_NOSYS path), which is correct
and never blocks the loop -- Darwin's POSIX AIO does not reliably
support SIGEV_KEVENT completion on regular files anyway.  The macOS CI
job (macos-latest, Apple Silicon) builds and runs the full C suite
green on every commit.

## RESOLVED: pbt-hegel CI job failed on ubuntu-24.04 with no output

**Status:** RESOLVED (2.0); the job is a merge gate again (no longer
`continue-on-error`).

The job once exited 2 printing nothing, which could not be reproduced
locally.  The cause was the job itself: `out=$(make ...)` under `set -e`
aborted the step before the captured output was printed.  Once the step
captured the status explicitly and always printed, the last failing runs
(2026-09-15) showed a real property failure (`[PBT] alloc/hook_balanced
FAIL`), fixed that day by a1c57151 (the allocator hook vtable is copied
and all six callbacks are required).  From then to 2026-10-26 the job
passed 54 of 54 GitHub runs (one more was cancelled together with its
whole run), all 36 properties across 17 suites, so `continue-on-error`
was removed: a property failure blocks a merge again.

