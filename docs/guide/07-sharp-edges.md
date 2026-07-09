---
title: Sharp edges
parent: Guide
nav_order: 7
permalink: /guide/07-sharp-edges/
lede: >-
  Three well-known threaded-C footguns from the C library -- getenv/setenv
  races, the process-global rand(), and strncpy's missing NUL -- and the
  thin public helpers that file them down.
---

# Sharp edges

{: .no_toc }

1. TOC
{:toc}

C's standard library predates threads, and a few of its interfaces are
outright hazardous once more than one thread is running. libxtc exposes
three small, thread-safe replacements so a consumer never has to reach for
the raw, racy primitives. All of them are in `xtc.h`.

## 1. `getenv` / `setenv` are not thread-safe against each other

`setenv(3)` may reallocate the `environ` block that a concurrent
`getenv(3)` is walking. Two threads -- one reading, one writing -- can
therefore crash or read freed memory. Worse, the pointer `getenv` returns
can be invalidated by a later `setenv` from any thread.

`xtc_env_get` and `xtc_env_set` serialize every access on a process-wide
lock. `xtc_env_get` copies the value into a small per-thread buffer under
that lock, so the pointer it hands back cannot be pulled out from under you
by another thread's `xtc_env_set`. The pointer is valid until the next
`xtc_env_get` **on the same thread**; copy it if you need to keep it.

```c
if (xtc_env_set("XTC_DEMO", "on", /*overwrite=*/1) != XTC_OK)
        /* handle XTC_E_INVAL / XTC_E_NOMEM */;
const char *v = xtc_env_get("XTC_DEMO");   /* NULL if unset */
```

## 2. `rand()` / `random()` are process-global and not thread-safe

`rand(3)` and `random(3)` keep a single hidden state for the whole
process. Draw from two threads at once and you get a data race and a
correlated, useless stream.

`xtc_rand_u64` is a **per-thread** splitmix64: each thread has its own
state, so there is no shared-state race and no contention. `xtc_rand_seed`
makes the calling thread's stream reproducible -- the same seed replays the
same sequence, which is exactly what you want for tests and for
deterministic jitter/backoff. An un-seeded thread auto-seeds from the clock
so distinct threads get distinct default streams.

```c
xtc_rand_seed(42);
uint64_t x = xtc_rand_u64();   /* reproducible from seed 42 */
```

It is **not** a cryptographic generator -- do not use it for keys or
security tokens.

> A future release may hook this into the deterministic-simulation clock
> (DST) so a replayed run draws a reproducible sequence; today it is simply
> a clean, thread-safe, seedable source.

## 3. `strncpy` does not NUL-terminate

`strncpy(3)` omits the terminator when the source is at least as long as
the buffer, leaving an unterminated string that the next `strlen`/`printf`
walks off the end of. `strncat`'s count is the number of source bytes, not
the buffer size -- another classic overflow.

`xtc_strlcpy` and `xtc_strlcat` use the BSD contract: they **always**
NUL-terminate when `dstsize > 0`, and they return the total length they
*tried* to create. A return value `>= dstsize` is the truncation signal.

```c
char buf[8];
if (xtc_strlcpy(buf, "verylongvalue", sizeof buf) >= sizeof buf)
        /* truncated; buf is still a valid C string */;
```

## Putting it together

The following program is compiled and run by the documentation test gate,
so it is guaranteed to build against the current API:

{% include snippet.html file="05_sharp_edges.c" region="full" %}

See [`xtc_env(3)`](../../man/) for the full contract of each function.
