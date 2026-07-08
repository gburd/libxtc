---
title: Processes and messages
parent: Guide
nav_order: 3
permalink: /guide/03-processes-and-messages
---

# Processes and messages
{: .no_toc }

1. TOC
{:toc}

---

A bare coroutine computes and returns. A **process** is a coroutine with
an *identity* (a `xtc_pid_t`) and a *mailbox*: other processes address it
by pid and communicate only by sending it messages. Nothing is shared;
there are no locks on a process's state because no one else can touch it.
This is the Erlang/BEAM model, in C.

## Spawn, send, receive

`xtc_proc_spawn(loop, fn, arg, opts, &pid)` starts `fn(arg)` as a process
and gives you back its pid. Inside a process, `xtc_self()` returns your
own pid, `xtc_send(pid, data, size)` copies `size` bytes into another
process's mailbox, and `xtc_recv(&buf, &size, timeout_ns)` blocks
(suspends the fiber) until a message arrives or the timeout elapses.

Here is a two-process ping/pong. `pong` waits for a number and replies
with one more; `ping` kicks it off and bounces the number back until it
reaches a limit.

{% include snippet.html file="03_ping_pong.c" region="full" %}

```
ping: got 1
ping: got 3
pong: reached 4, done
```

## Three things to notice

**Messages are copies.** `xtc_send` copies the bytes into the
recipient's mailbox. The sender and receiver never share the buffer, so
there is nothing to lock and no lifetime to coordinate across processes.

**A received buffer is yours to free.** `xtc_recv` hands you a
heap buffer that you own. Release it with
[`xtc_free`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_free.3)
-- not plain `free`. libxtc may be running under a custom allocator (an
embedder like PostgreSQL installs one), and freeing an
allocator-supplied buffer with the C library `free` is a mismatched-free
bug. Every libxtc call that returns a caller-owned buffer documents
`xtc_free`; that man page lists them.

**There is no sender field.** `xtc_recv` does not tell you who sent the
message. If you need to reply, put your own pid in the payload -- that
is what the `from` field in the example does. This keeps the mailbox a
plain byte queue and lets you design your own protocols on top.

{: .not_chosen }
> **Shared state behind a mutex.** The C default is a struct guarded by a
> `pthread_mutex`. It is faster for a single hot counter, but it does not
> compose: every new invariant adds another lock, lock order becomes a
> global proof obligation, and a thread that dies holding a lock wedges
> everyone. The process model trades a little copy cost for the property
> that state has exactly one owner and failure is contained to that
> owner. libxtc still ships mutexes, rwlocks, RCU, and a lock manager
> ([Locks and synchronization](../locks)) for the cases that genuinely
> want shared memory -- but the *default* unit of concurrency is the
> shared-nothing process.

## Selective receive

Sometimes a process wants the *next message that matches a predicate*,
leaving others in the mailbox for later. `xtc_recv_match(match_fn,
user_data, &buf, &size, timeout)` scans the mailbox and returns the
first message for which `match_fn` returns non-zero, preserving the
arrival order of the rest. This is how you implement a request/response
correlation (pull the reply with *your* request id) without draining
unrelated traffic. See
[`xtc_proc(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_proc.3).

## What you have learned

- A process is an addressable, mailbox-owning coroutine with private
  state.
- `xtc_send` copies; `xtc_recv` / `xtc_recv_match` receive; received
  buffers are freed with `xtc_free`.
- Replies carry the sender pid in the payload by convention.

Processes let things run independently. The next chapter is about what
happens when one of them *fails*, and how to build systems that recover:
[links, monitors, and supervisors](04-supervision).

---

&larr; [Fibers and the event loop](02-fibers-and-the-loop) &middot;
Next: [Links, monitors, and supervisors](04-supervision) &rarr;
