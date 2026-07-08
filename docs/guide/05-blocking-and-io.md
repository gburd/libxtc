---
title: Blocking work and I/O
parent: Guide
nav_order: 5
permalink: /guide/05-blocking-and-io
---

# Blocking work and I/O
{: .no_toc }

1. TOC
{:toc}

---

The event loop's one rule is: **never block the thread that runs it.**
A fiber that calls a slow synchronous function -- `read()` on a cold
file, a CPU-bound compression pass, a legacy C library that does its own
blocking I/O -- stalls *every other fiber* on that loop until it
returns. This chapter is about the three ways libxtc lets you do slow
work without breaking that rule.

## 1. Suspending I/O the loop already understands

For sockets and timers, libxtc suspends the fiber and lets the OS poller
wake it. You do not manage readiness yourself:

- **Timers:** `xtc_proc_sleep(ns)` parks the fiber; the loop wakes it
  when the timer fires (see [chapter 2](02-fibers-and-the-loop)).
- **Sockets:** the `xtc_net_*` API
  ([`xtc_net(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_net.3))
  does non-blocking connect / accept / read / write and suspends the
  fiber until the socket is ready, driven by io_uring / epoll / kqueue /
  IOCP under the hood.
- **Arbitrary fds:** `xtc_proc_wait_fd(fd, events, timeout, &revents)`
  parks the fiber until `fd` is readable/writable -- the building block
  for wrapping any fd-based protocol.

## 2. Async file I/O

File reads and writes do not have a clean readiness model on every OS,
so libxtc presents one portable async file API, `xtc_aio_*`
([`xtc_aio(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_aio.3)):
`xtc_aio_pread`, `xtc_aio_pwrite`, `xtc_aio_fsync`, `xtc_aio_fdatasync`.
Where the platform has a native completion mechanism (io_uring on Linux,
IOCP on Windows, kqueue AIO on the BSDs, event-port AIO on illumos) the
operation completes with **zero extra threads**. Everywhere else it
transparently falls back to the blocking pool (below) -- so you write
storage code once, as if async file I/O is always available, and it is
always correct.

## 3. The blocking pool: escaping to a thread, on purpose

When you must call something that will block and has no async form -- a
third-party library, a CPU-bound kernel -- hand it to the **blocking
pool** with `xtc_blocking_run(fn, arg, &result)`
([`xtc_blocking(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_blocking.3)).
libxtc runs `fn(arg)` on a dedicated worker thread, **suspends the
calling fiber** (so the loop keeps serving everyone else), and resumes
the fiber with the result when the worker finishes. The blocking is real
-- but it happens on a pool thread, not the loop thread.

```c
/* inside a fiber: compute a slow hash without stalling the loop */
int result;
xtc_blocking_run(hash_a_big_buffer, buf, &result);
/* the fiber resumes here once the pool thread is done */
```

{: .rationale }
> **Why an explicit pool call instead of auto-detecting blocking?** A
> library cannot know which of your function calls will block. Making
> the escape hatch *explicit* means the one place a thread hop happens
> is visible in the code and in a profile -- you can see, and budget,
> every departure from the single-threaded model. It is the same
> philosophy as `unsafe` in Rust: not forbidden, but marked.

## Scaling across cores: the executor

One loop uses one core. To use all of them, run an **executor** -- N
loops, one per core, with work-stealing between them
([`xtc_exec(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_exec.3)).
A process spawned on a busy loop can be stolen and run on an idle one;
messages and completions cross loops safely. Your process code does not
change -- it is still spawn / send / recv -- it just runs on more cores.

{: .not_chosen }
> **A thread pool per subsystem.** The mainstream C server design gives
> each subsystem its own thread pool and synchronizes with locks and
> queues between them. It works, but the thread count grows with the
> number of subsystems, context switches dominate under load, and the
> lock graph becomes the system's hardest correctness problem. The
> executor model uses one thread per core regardless of how many
> subsystems you have; concurrency comes from fibers, not threads, and
> the only threads are the N loop threads plus the blocking pool. Fewer
> threads, no cross-subsystem lock graph.

## Determinism: testing the whole thing

Because all concurrency flows through the loop, libxtc can replace the
real scheduler and I/O with a **deterministic simulation** driven by a
seed -- the FoundationDB / TigerBeetle approach. The same process code
runs, but scheduling, timers, message ordering, and injected faults
(partitions, latency, disk errors, crashes) are reproducible from the
seed. This is how libxtc finds concurrency bugs before they ship; see
the `test/sim/` suite and the deterministic-testing notes.

## What you have learned

- Never block the loop thread; suspend instead.
- Sockets/timers/fds suspend via the OS poller; files via `xtc_aio_*`;
  everything else via the explicit `xtc_blocking_run` pool.
- The executor scales the same process code across cores with
  work-stealing.
- Deterministic simulation replays the whole concurrent system from a
  seed.

That completes the guide. For the mental-model shift from
threads-and-locks thinking, read
[Thinking in libxtc](transitioning); to see it all assembled into real
programs, read the [Examples](../examples/).

---

&larr; [Links, monitors, and supervisors](04-supervision) &middot;
Next: [Thinking in libxtc](transitioning) &rarr;
