---
title: Thinking in libxtc
parent: Guide
nav_order: 6
lede: >-
  The mental shifts a C, C++, or Rust programmer must make -- and the anti-patterns that bite.
permalink: /guide/transitioning/
---
You are good at C, C++, or Rust.  That skill is necessary here and not
sufficient.  libxtc asks you to give up a few habits that are correct
everywhere else and that, kept, will produce deadlocks, stalls, and
heap corruption that look like libxtc bugs but are not.  This guide is
the short list of mental shifts and the anti-patterns that follow from
not making them.  Read it once before you write a libxtc program and
again the first time something hangs.

The model in one paragraph: a libxtc program is many lightweight
*processes* (fibers, "procs") running cooperatively on a small number
of OS threads ("loops").  A proc owns some state and talks to other
procs by sending messages; it never shares that state through memory.
A proc runs until it voluntarily yields -- at a `recv`, a sleep, an
awaited I/O -- at which point another proc on the same loop runs.  This
is Erlang's model and Seastar's model, expressed in C.

---

## Shift 1: the scheduler is cooperative, not preemptive

Nothing interrupts your proc.  It runs until it calls something that
yields.  On a preemptive system (threads) the OS slices time for you;
here you slice it yourself, by reaching a yield point.

  * **Anti-pattern: the CPU-bound loop with no yield.**  A `for` loop
    that crunches ten million rows without an `xtc_yield()` does not
    just run slowly -- it *freezes every other proc on that loop* for
    the whole duration, including the I/O completions they are waiting
    on.  The cure: call `xtc_yield()` periodically in long computations,
    or move the work off the loop with `xtc_blocking_run`, or use the
    yield watchdog (`xtc_yield_set_budget`) to catch yourself.
  * **Rust intuition that misleads:** `async` Rust is also cooperative,
    so this one transfers -- but Rust's `.await` points are marked by
    the compiler and you cannot forget them.  Here the yield points are
    specific function calls and there is no compiler to remind you.

## Shift 2: never block the OS thread

A loop is shared by thousands of procs.  Anything that blocks the
*thread* -- `sleep(3)`, a blocking `read(2)`, `getaddrinfo(3)`,
`pthread_mutex_lock` on a contended lock, a synchronous `fsync` --
blocks not just your proc but every proc on that loop.  The loop stops
serving I/O; timers stop firing; the system appears hung.

  * **Anti-pattern: calling a blocking libc/syscall directly on a
    loop.**  Wrap it in `xtc_blocking_run`, which runs it on a pool
    thread and parks your fiber until it completes.  The loop keeps
    serving everyone else meanwhile.
  * **Anti-pattern: `usleep`/`nanosleep` to "wait a bit."**  Use
    `xtc_proc_sleep`, which parks the fiber on a timer.  A thread sleep
    here is a global stall.
  * **Anti-pattern: a pthread mutex (or `xtc_lwlock`) held across a
    yield point.**  If you hold a thread-blocking lock and then `recv`
    or await I/O, another proc that wants the lock blocks the *thread*,
    and the holder -- a parked fiber on that same thread -- can never
    run to release it.  Instant deadlock, and it looks like a hang, not
    a crash.  Use the fiber-yielding locks: `xtc_amutex` (mutual
    exclusion) and `xtc_arwlock` (shared/exclusive) park the waiter
    instead of blocking the thread, so the holder can resume.

## Shift 3: share nothing; pass messages

The instinct from threaded C/C++ is "shared data structure + a lock."
The libxtc instinct is "give the data to one proc; everyone else asks
it."  A proc that solely owns its state needs no lock on that state at
all, because cooperative scheduling means no other proc can touch it
between your yield points.

  * **Anti-pattern: a global hash table guarded by a mutex, hit from
    many procs.**  Replace it with a proc that owns the table and a
    request/reply (`xtc_svr_call`) interface.  Now there is no lock,
    no contention, and the owner serializes access for free.  This is
    how the sqlxtc shards work and why they scale across cores without
    a single lock on shard-local state.
  * **When you do need shared, read-mostly data** (a config, a
    catalog), reach for `xtc_lrlock` (read-optimized, copy-on-write) or
    `xtc_rcu`, not a plain rwlock.

## Shift 4: messages are copied, and the mailbox is the pressure gauge

`xtc_send` copies the bytes you give it into the recipient's mailbox.
The recipient owns its copy and frees it.

  * **Anti-pattern: sending a pointer to your stack, or to memory you
    free.**  The recipient may run much later; the pointer is dangling.
    Send the data, not a pointer to it (or transfer ownership of a heap
    block explicitly and never touch it again).
  * **Anti-pattern: casting the received buffer to a struct with an
    8-byte-aligned member and reading the field.**  The payload sits at
    an arbitrary offset in the envelope; reading a `uint64_t` straight
    out of it is undefined behaviour (and UBSan will say so).  `memcpy`
    into a local struct first.
  * **Anti-pattern: an unbounded producer to a slow consumer.**  If A
    sends faster than B drains, B's mailbox grows without bound until
    the process is killed by the OOM killer.  This is the single most
    common pathology in message systems.  Bound mailboxes
    (`mailbox_cap`), watch the depth (it is the first thing
    `xtc-procs` shows), apply backpressure (a request/reply call
    naturally rate-limits), and shed load at a watermark.

## Shift 5: let it crash, and design for restart

The reflex to wrap every operation in error handling and limp onward
produces fragile, half-recovered state.  The libxtc/OTP reflex is to
let a proc that hits a broken state die, and let its supervisor restart
it from a known-good init.

  * **Anti-pattern: catching and "handling" an unrecoverable error to
    avoid a crash.**  Prefer a clean death and a supervised restart.
    Put recoverable resources (locks, fds, memory contexts) on the
    proc's at-exit hooks so they are released on the way down.
  * **Design init to be idempotent and fast**, because it runs again on
    every restart.

## Shift 6: ordering across loops is not what your watch says

Two procs on two cores do not have a shared clock.  A message you sent
"first" can arrive after one sent "later" by another proc.

  * **Anti-pattern: assuming arrival order equals causal order.**  If
    you need causality, carry it explicitly (a logical clock; see
    the per-shard hybrid logical clock).  The sqlxtc sharded test makes this visible: a
    global timestamp source still produced dozens of cross-core
    "reorderings" per run, because logical order and arrival order are
    different things.

## Language-specific notes

### For C programmers

You already think in pointers and ownership; the new discipline is
*yield-awareness*.  Every call can be a yield point; ask "if this parks,
what state am I holding, and is it safe for another proc to run?"  A
content latch or a raw pointer into a buffer manager page held across a
park is the classic trap.

### For C++ programmers

  * RAII does not span yields the way you expect.  A guard object whose
    destructor releases a lock is fine *within* a non-yielding region,
    but if you hold it across a `recv` you have re-created the
    held-lock-across-yield deadlock (Shift 2) with nicer syntax.
  * Exceptions do not cross the C ABI or the fiber boundary.  Do not
    let an exception propagate out of a proc function into libxtc.
  * Large objects on the fiber stack blow the (bounded) fiber stack;
    heap-allocate them.

### For Rust programmers

  * The model is `async`/`await` with a work-stealing executor -- this
    transfers well -- but it is *unsafe by construction*: there is no
    borrow checker, no `Send`/`Sync`, no `Pin`.  Ownership of proc
    state and of message buffers is by *convention*, enforced by you,
    not by the compiler.
  * "Just share an `Arc<Mutex<T>>`" is the habit to drop.  The libxtc
    equivalent is a proc that owns `T`; others message it.  You will
    reach for shared-mutable-with-a-lock and you should stop and ask
    whether an owning proc is cleaner -- it almost always is.
  * There is no `?`-operator unwinding; errors are return codes
    (`XTC_E_*`).  Check them.

---

## A development-workflow trap that is not about concurrency

libxtc's headers carry struct layouts that several `.o` files bake in.
After you change a struct in an internal header (`loop_int.h`,
`io_int.h`, the proc struct), a partial rebuild that recompiles only
some translation units links mismatched layouts and crashes in ways
that make no sense.  When you change an internal struct, `rm -f *.o
*.a` and rebuild.  This has cost more than one debugging session.

---

## The one-line summary

Own your state and pass messages; never block the thread or hold a
blocking lock across a yield; copy data into messages and watch the
mailbox depth; let procs crash and supervise them.  Do that and libxtc
behaves; fight it with shared-memory-and-locks habits and it will hang
in ways your threaded instincts will misread.

See `docs/guide/debugging.md` for how to find these problems when they
happen, and `docs/getting-started.md` for a first program.
