# xtc gaps found by dogfooding sqlxtc

The purpose of building sqlxtc on libxtc is not only to produce a SQL
server -- it is to push libxtc hard enough, against a real database
workload, to find where the core library has bugs or missing
primitives.  This is the running ledger of what that has surfaced:
what was missing, whether it was fixed in the library or worked around
in the example, and what is confirmed to work as intended.

Entries are newest-first.  "Confirmed OK" entries matter as much as
gaps -- they record a primitive we were worried about and proved
sufficient.

---

## Stage 1 -- group-commit WAL writer (wal.c)

### GAP: xtc_svr has no deferred reply (gen_server:reply/2)

`xtc_svr`'s `handle_call(state, req, size, xtc_svr_call_t *call)` gives
a `call` handle that is **stack-scoped to that one dispatch** -- in
`src/orc/svr.c` it is `struct xtc_svr_call call = {0};` on the
dispatch loop's stack.  `xtc_svr_reply(call, ...)` therefore only works
synchronously, from inside the callback.  There is no `XTC_SVR_NOREPLY`
result and no way to stash the requester and reply later.

Group commit is exactly the pattern that needs this: receive many
commit calls, defer all their replies, do ONE fsync, then reply to the
whole batch.  In Erlang this is `{noreply, State}` + `gen_server:reply(From, R)`.

  * Impact: any request/reply subsystem that batches, fans out, or
    otherwise replies out-of-band cannot use `xtc_svr`.  That is a
    large class for the scale-out plan (the WAL writer, the 2PC
    coordinator, a query-result aggregator).
  * Worked around: wal.c is built on raw `xtc_proc` send/recv -- the
    committer `xtc_send`s its record and parks on an ack message; the
    writer replies with `xtc_send` to each stored `reply_to` pid after
    the batch fsync.  Robust, but it re-implements the call/reply
    correlation that `xtc_svr` exists to provide.
  * Proposed fix (library): make `xtc_svr_call_t` survive past
    `handle_call` (heap-allocate the slot, or ref-count it), add an
    `XTC_SVR_NOREPLY` return so the callback can defer, and allow
    `xtc_svr_reply` to be called later from any context.  Additive to
    the frozen ABI (a new constant + a lifetime change).  TODO.

### GAP: no XTC_E_IO error code

The core `xtc_err` enum has no I/O-error code; a storage layer that
opens/reads/writes files has nowhere to map `errno` from a failed
`open`/`pwrite`/`fsync`.  wal.c currently returns `XTC_E_INTERNAL` for
a file-open failure, which conflates "environment failure" with
"library invariant violated (a bug)".

  * Proposed fix (library): add `XTC_E_IO = -12` (or similar) and a
    `xtc_strerror` entry.  Additive.  TODO.

### CONFIRMED OK: "wait for N commits or T microseconds" needs no new primitive

The group-commit coordination -- block for the first committer, then
gather more until a batch cap OR a time window closes, then release
all of them -- was the place we most expected to find a missing
primitive (a countdown latch with timeout, a sequence gate).  It is
not: a blocking `xtc_recv(timeout = -1)` for the first record followed
by `xtc_recv(timeout = deadline - now)` in a loop expresses the
window exactly, and the mailbox naturally buffers commits that arrive
while the writer is parked on the offloaded fsync.  `xtc_blocking_run`
offloads the write+fsync so the loop keeps serving committers.  The
raw proc model is sufficient and clean here.

### CONFIRMED OK: a parked consumer's mailbox keeps accepting sends

While the writer is parked in an offloaded `fdatasync`, committers on
the same loop keep `xtc_send`-ing new records into its mailbox; the
next batch drains them.  No lost sends, no wedge, on a single loop and
on a 4-loop executor.  This is the property the whole cooperative
model depends on, and it holds.

---

## Earlier findings (pre-ledger, recorded for completeness)

  * **Built `xtc_arwlock`** -- a fiber-yielding shared/exclusive latch.
    The parallel-writer B-tree needed latch coupling on a cooperative
    loop; `xtc_lwlock` blocks the OS thread and `xtc_amutex` is
    exclusive-only, so neither could back it.  Fixed in the library.
  * **Built `xtc_proc_sleep`** -- a fiber timer-park.  The SQLite busy
    handler could not thread-sleep (it would wedge the loop whose
    parked fiber holds the lock).  Fixed in the library.
  * **`xtc_svr_call_abortable`** -- cooperative cancellation of a
    pending call (statement-timeout shape).  Added to the library.
  * **Cooperative-yield watchdog** -- `xtc_yield_*`: a non-yielding
    fiber starves its loop; the over-budget signal is the bridge to
    fire a cancellation.  Added to the library.
