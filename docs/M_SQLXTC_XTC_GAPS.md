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

### GAP (FIXED): xtc_svr had no deferred reply (gen_server:reply/2)

`xtc_svr`'s `handle_call(state, req, size, xtc_svr_call_t *call)` gives
a `call` handle that is **stack-scoped to that one dispatch** -- in
`src/orc/svr.c` it is `struct xtc_svr_call call = {0};` on the
dispatch loop's stack.  `xtc_svr_reply(call, ...)` therefore only
worked synchronously, from inside the callback.  There was no
`XTC_SVR_NOREPLY` result and no way to stash the requester and reply
later.

Group commit is exactly the pattern that needs this: receive many
commit calls, defer all their replies, do ONE fsync, then reply to the
whole batch.  In Erlang this is `{noreply, State}` + `gen_server:reply(From, R)`.

  * Fixed in the library: added `xtc_svr_call_save()` (returns a
    heap-allocated copy of the call handle that outlives the
    callback) and the `XTC_SVR_NOREPLY` result code.  `handle_call`
    saves the handle, stashes it, returns `XTC_SVR_NOREPLY`, and a
    later callback calls `xtc_svr_reply` on the saved handle (which
    frees it).  Additive to the frozen ABI (the call struct is opaque;
    a new field + a new function + a new constant).  Tested:
    `test/m10/test_svr.c` /deferred_reply -- two in-proc callers'
    replies are deferred past `handle_call` and answered together from
    a later cast.  Full suite + ASan clean.
  * Still worked around in wal.c: the WAL writer stays on raw
    `xtc_proc` send/recv anyway, because group commit also needs a
    sub-millisecond batch WINDOW and `xtc_svr`'s dispatch loop uses a
    fixed 100ms recv poll (see the next entry) -- so even with
    deferred reply, xtc_svr is not the right host for the tight WAL
    batch.  Deferred reply IS the right tool for the 2PC coordinator
    and other fan-out subsystems, which is why it was worth fixing.

### GAP: xtc_svr dispatch loop has a fixed 100ms recv poll

`__svr_entry` does `xtc_recv(&msg, &size, 100ms)` per iteration, so a
server cannot implement a sub-millisecond timed batch window (group
commit wants ~0.5ms).  A raw proc can (`xtc_recv` with an arbitrary
computed timeout), which is why the WAL writer is a raw proc.

  * Possible fix (library): let the server specify its idle poll
    interval in `xtc_svr_opts_t`, or expose a timer callback.  Low
    priority -- raw procs cover the tight-timer case today.  TODO.

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
