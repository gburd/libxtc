---
title: Resource limits
parent: Guide
nav_order: 6
permalink: /guide/06-resource-limits/
lede: >-
  How libxtc caps threads, cores, file descriptors, memory, and message backlog -- and why a bounded C program is worth so much.
---

1. TOC
{:toc}

---

Most C programs have no idea how much of anything they are using until
they run out. Descriptors leak until `accept` fails; a slow consumer's
backlog grows until the OOM killer arrives; a burst of blocking work
spawns threads until the scheduler thrashes. libxtc treats **bounded
resource use as a first-class feature**: it can cap the things that
otherwise grow without limit, reject work at the cap instead of dying,
and warn you before you get there.

## Why a library should impose limits

A limit is not a restriction; it is a *contract*. A program that
declares "I will use at most 1 GiB, 65,536 descriptors, and 64 pool
threads" is one you can size, deploy on a shared box, and reason about
under overload. Without limits, overload is undefined behavior -- the
program does whatever the OS does when it runs out, which is usually to
fall over at the worst moment. With them, overload is a *handled case*:
an acquire returns `XTC_E_RESOURCE`, your code sheds or queues the work,
and the service degrades instead of crashing. This is the difference
between a hobby program and an operable one, and libxtc bakes it in so
you do not have to hand-roll accounting for every resource.

## The resource accountant

[`xtc_res(3)`]({{ '/reference/api/xtc_res/' | relative_url }}) is a small
accountant with **hard caps** and **high-water alerts**. It tracks the
bounded resources a runtime actually consumes:

| Resource | Default cap | What it bounds |
|---|---|---|
| `XTC_RES_TASKS` | 100,000 | live fibers/tasks |
| `XTC_RES_CHANNELS` | 4,096 | live channels |
| `XTC_RES_CHAN_SLOTS` | 1,000,000 | in-flight channel messages |
| `XTC_RES_FDS` | 65,536 | open descriptors |
| `XTC_RES_MEM_BYTES` | 1 GiB | tracked bytes |
| `XTC_RES_INBOX_MSGS` | 65,536 | cross-loop messages in flight (per loop) |

Acquire is checked; release is unconditional:

```c
if (xtc_res_acquire(r, XTC_RES_FDS, 1) != XTC_OK) {
    /* at the descriptor cap -- reject this connection cleanly */
    return XTC_E_RESOURCE;
}
/* ... use the fd ... */
xtc_res_release(r, XTC_RES_FDS, 1);
```

Set your own caps at init (an `xtc_res_caps_t`) or at runtime
(`xtc_res_set_cap`), and arm an alert to learn you are *approaching* a
limit before you hit it:

```c
xtc_res_set_alert(r, XTC_RES_MEM_BYTES, 0.90);   /* fire at 90% */
xtc_res_set_alert_fn(r, on_pressure, user);      /* your callback */
```

Each resource also records a high-water mark and a reject count, so a
dashboard can show both current use and how often the cap bit.

## Threads and cores

libxtc's thread count is *structurally* bounded, not just budgeted:

- **Loop threads** -- one per core under the executor. You choose how
  many cores to use (`xtc_exec_init`); concurrency comes from fibers, not
  from spawning threads, so the thread count does not grow with load.
- **The blocking pool** -- `xtc_blocking_run` work runs on a pool that
  grows *on demand* only up to a hard cap (64 by default). A flood of
  blocking calls queues against the pool rather than spawning unbounded
  threads. See [Blocking work and I/O]({{ '/guide/05-blocking-and-io/' | relative_url }}).

So the total OS-thread footprint of a libxtc program is *(cores you
chose) + (up to 64 pool threads)* -- a number you can state up front, no
matter how many fibers, processes, or connections it serves.

## Memory and message backlog

The most common unbounded-growth bug in a message-passing system is a
mailbox that outruns its consumer (see the
[stuck-mailbox debugging recipe]({{ '/guide/debugging/' | relative_url }})).
libxtc bounds it two ways: a per-process `mailbox_cap` drops or rejects
past the cap (with a watermark callback so you can shed load early), and
`XTC_RES_INBOX_MSGS` / `XTC_RES_MEM_BYTES` bound the totals. The
[rexis example]({{ '/examples/rexis/' | relative_url }}) uses exactly
this to stay inside a fixed memory budget under a client flood rather
than OOM -- the property that separates a toy server from a real one.

## The full limit list

The exact structural limits (pid encoding, run-queue depth, lock
capacities, the Isolate handle layout) and all the tunable defaults are
catalogued on the [Limits reference page]({{ '/reference/limits/' | relative_url }}),
with the reasoning behind each number.

---

&larr; [Blocking work and I/O]({{ '/guide/05-blocking-and-io/' | relative_url }}) &middot;
Next: [Thinking in libxtc]({{ '/guide/transitioning/' | relative_url }}) &rarr;
