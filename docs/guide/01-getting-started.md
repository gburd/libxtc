---
title: Getting started
parent: Guide
nav_order: 1
permalink: /guide/01-getting-started/
lede: >-
  From an empty directory to a running libxtc program -- and every line of it explained.
---
1. TOC
{:toc}

---

This chapter takes you from an empty directory to a running libxtc
program, then explains every line of it. By the end you will understand
the three calls at the heart of the library -- `xtc_async`,
`xtc_loop_run`, and `xtc_await` -- and why a *fiber* lets you write
concurrent code that reads like ordinary straight-line C.

## Installing

libxtc has no required dependencies beyond a C11 compiler, libc, and
pthreads. `liburing` (Linux) and a TLS library (OpenSSL, GnuTLS,
wolfSSL, Mbed TLS, or BoringSSL) are optional and auto-detected.

```sh
git clone https://codeberg.org/gregburd/libxtc
cd libxtc
cd dist && autoreconf -i && cd ..
mkdir -p build && cd build
../dist/configure          # autodetects the I/O backend and TLS
make -j"$(nproc)"
make check                 # run the full test suite (optional but wise)
sudo make install          # libxtc.a, headers, and man pages
```

There is also a single-file **amalgamation** (`xtc.c` + `xtc.h`) for
dropping the whole library into another build with no configure step;
see [Architecture]({{ '/reference/architecture/' | relative_url }}) for when to prefer it.

{: .note }
> If `make check` reports a failure in `test_io_lifecycle` about
> `xtc_io_init` on a machine with a low `ulimit -l`, configure with
> `--with-io-backend=epoll` -- that is a memlock-limit quirk of
> io_uring on the host, not a library fault. See
> [Known issues]({{ '/reference/known-issues/' | relative_url }}).

## Your first coroutine

Here is a complete program. It spawns one coroutine that squares a
number, runs the event loop until the coroutine finishes, then reads the
result back.

{% include snippet.html file="01_hello_async.c" region="full" %}

Build and run it against the static library you just made:

```sh
cc -I libxtc/src/inc first.c libxtc/build/libxtc.a -lpthread -lm -o first
./first
```

```
coroutine: computing 7 * 7
result = 49
```

## What just happened

Three objects and four calls carry the whole program.

**The loop** ([`xtc_loop_init`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_loop.3),
`xtc_loop_run`, `xtc_loop_fini`). An `xtc_loop_t` is an event loop: a
run queue of ready fibers plus an OS I/O poller (io_uring, epoll,
kqueue, or IOCP, chosen at configure time). `xtc_loop_run` drives it,
resuming ready fibers one at a time on this thread until there is no
more work, then returns.

**The task** ([`xtc_async`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_async.3)).
`xtc_async(loop, fn, arg, &task)` allocates a fiber -- a small,
independently switchable call stack -- and queues `fn(arg)` to run on
it. It does *not* run `fn` yet; it returns immediately with a handle.

**The yield** ([`xtc_yield`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_async.3)).
Inside the coroutine, `xtc_yield()` suspends the fiber and hands control
back to the loop. The fiber's stack is left exactly as it was; when the
loop next schedules it, execution continues on the line after the yield.
This is the whole trick: a blocking-looking call becomes a suspension
point, and one thread can interleave many fibers.

**The await** ([`xtc_await`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_async.3)).
After the loop has run the task to completion, `xtc_await(task, &result)`
reads the `intptr_t` the coroutine returned.

{: .rationale }
> **Why a fiber and not a thread?** A thread costs a full kernel stack
> (often 8 MB of address space) and a context switch that traps into the
> kernel. A libxtc fiber is a user-space stack (kilobytes) and switching
> between fibers is a function call plus a register swap -- no syscall.
> That is what lets a single loop hold hundreds of thousands of
> concurrent activities. Chapter
> [Fibers and the event loop]({{ '/guide/02-fibers-and-the-loop/' | relative_url }}) measures the
> difference.

{: .not_chosen }
> **Callbacks / a state machine.** The classic C answer to "don't block
> the loop" is to split every operation into a callback chain or an
> explicit state machine (the libevent / libuv style). It works, but it
> shreds straight-line logic into fragments and turns local variables
> into heap-allocated context structs. Fibers keep the code linear and
> the variables on the stack; you pay a small per-fiber memory cost
> instead of a large readability cost. Where you genuinely want an
> explicit state machine, libxtc still offers one -- see the
> [tnt example]({{ '/examples/' | relative_url }}) -- but it is opt-in, not the default.

## The shape of every libxtc program

Nearly every program follows the same skeleton:

1. **Create** a loop (or, for the multi-core case, an *executor* of
   several loops -- see [Blocking work and I/O]({{ '/guide/05-blocking-and-io/' | relative_url }})).
2. **Spawn** the initial work with `xtc_async` (for a bare coroutine) or
   `xtc_proc_spawn` (for a supervised, message-passing process --
   [Processes and messages]({{ '/guide/03-processes-and-messages/' | relative_url }})).
3. **Run** the loop.
4. **Tear down** with `xtc_loop_fini`.

The next chapter opens up step 2 and 3: what a fiber really is, how the
loop decides what to run, and how to await I/O rather than just a
computed value.

---

Next: [Fibers and the event loop]({{ '/guide/02-fibers-and-the-loop/' | relative_url }}) &rarr;
