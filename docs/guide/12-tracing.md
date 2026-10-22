---
title: Tracing with DTrace, perf and bpftrace
parent: Guide
nav_order: 12
permalink: /guide/12-tracing/
lede: >-
  One set of static probes in libxtc, read by DTrace on FreeBSD, illumos and
  macOS and by perf, bpftrace and SystemTap on Linux. Off by default and free
  when off; when on, attach to a running process you never prepared.
---

# Tracing with DTrace, perf and bpftrace

[`xtc_tail`]({{ '/guide/10-observability/' | relative_url }}) records the
runtime's events into a ring inside your process: you turn it on in code or
with an environment variable, and you read the trace afterwards. That is the
right tool when you can plan the capture. The probes on this page are for when
you cannot: the server is already running, misbehaving, and you want to ask it
a question now, from outside, without restarting it or changing a line.

libxtc carries a set of USDT (user-level statically defined tracing) probes:
named points in the scheduler, the mailbox, timers, file I/O, locks and
supervision. A tracer attaches to them in a live process. The same probe set
serves two families of tools:

| platform | tool | probe name as the tool sees it |
| --- | --- | --- |
| FreeBSD, illumos, macOS | `dtrace` | `xtc$target:::fiber-park` |
| Linux | `bpftrace`, `bcc` | `usdt:./prog:xtc:fiber__park` |
| Linux | `perf` | `sdt_xtc:msg_send` (single-site probes only; see below) |
| Linux | SystemTap | `process("./prog").mark("fiber__park")` |

`perf probe` collapses each double underscore in the event name, so the
probe `msg__send` is recorded as `sdt_xtc:msg_send`. perf 7.2 also cannot
enable a probe with more than one site in the binary (`fiber__park`,
`lock__wait__start` and others fire from several places): the second site
collides with the first's event name. bpftrace and SystemTap handle every
probe; use them for those.

The definitive list of probes, their arguments and their stability is the
[`xtc_usdt(3)`]({{ '/reference/man-pages/' | relative_url }}) man page; this
page is the tour.

## Building with probes

Probes are a build option, off by default:

```sh
../dist/configure --enable-dtrace        # needs a dtrace program
../dist/configure --enable-dtrace=sdt    # Linux: systemtap <sys/sdt.h> only
../dist/configure --enable-dtrace=auto   # whichever is available, else off
meson setup build -Ddtrace=auto          # the same for meson
```

`--enable-dtrace` (`=yes`) is the DTrace recipe PostgreSQL and Node.js use.
`dtrace -h` turns `src/xtc_probes.d` into a header of probe macros; on FreeBSD
and illumos `dtrace -G` then rewrites each probe call in the compiled objects
into no-ops and writes the probe table into `xtc_probes.o`, which goes into
`libxtc.a` and `libxtc.so`. macOS needs no `-G` step; its linker builds the
table. On Linux, `=sdt` is the recommended mode: it needs nothing but the
`<sys/sdt.h>` header from `systemtap-sdt-dev` (Debian) or
`systemtap-sdt-devel` (Fedora), and no build-time tool. `--enable-dtrace` on
Linux would use SystemTap's `dtrace` script instead, which adds is-enabled
semaphores; that combination follows the same recipe but has not yet been
run in libxtc's testing.

A static-library pitfall is handled for you: the probe table lives in an
object nothing calls, so a linker pulling members out of `libxtc.a` would
normally leave it behind and your statically linked program would have no
probes. Every object with a probe site references an anchor symbol in
`xtc_probes.o`, which forces it in.

Your own program needs nothing: no header, no flag, no tracing code. Link it
against a probe-enabled libxtc. This one is the target for every example
below; it bounces a message between two fibers that also contend on a lock:

{% include snippet.html file="16_usdt_target.c" region="full" %}

## First questions

Is anything happening? List the probes in a running program, then count what
fires:

```sh
dtrace -l -P 'xtc$target' -c ./usdt_target
dtrace -qn 'xtc$target:::msg-send { @["sent"] = count(); }
            xtc$target:::fiber-park { @park[arg1] = count(); }' -c ./usdt_target
```

Always scope a script with `xtc$target` (or `-p PID`). `xtc*` matches the
provider in every libxtc process on the machine, and a counting script
silently becomes a census of all of them.

On Linux the same question in bpftrace (the probes live in the program for a
static libxtc, in `libxtc.so` for a shared one):

```sh
sudo bpftrace -e 'usdt:./usdt_target:xtc:fiber__park { @park[arg1] = count(); }' \
    -c ./usdt_target
```

The first argument of `fiber-park` is the fiber (its task pointer) and the
second is why it parked: 1 mailbox, 2 sleep, 3 fd, 4 aio, 5 blocking pool,
6 sync primitive, 7 lwlock, 8 lock manager, 9 future.

## Key on the fiber, not the thread

A DTrace habit that does not carry over: keying per-request state on
`self->` (thread-local). A libxtc fiber is not a thread. A migratable fiber can
park on one OS thread and resume on another after work stealing, and one
thread runs thousands of fibers in turn. Every probe that concerns a fiber
passes its task pointer, so key on that:

```d
xtc$target:::fiber-park   { ts[arg0] = timestamp; why[arg0] = arg1; }
xtc$target:::fiber-resume /ts[arg0]/ {
	@lat[why[arg0]] = quantize(timestamp - ts[arg0]);
	ts[arg0] = 0;
}
```

That is `tools/dtrace/xtc_park_latency.d`.

## The scripts

`tools/dtrace/` and `tools/bpftrace/` carry the same five tools:

| script | the question it answers |
| --- | --- |
| `xtc_park_latency` | how long fibers stay parked, by reason |
| `xtc_lock_contention` | which locks make callers wait, for how long, from which stacks |
| `xtc_timer_lateness` | how late timers fire, and how many are armed only to be cancelled |
| `xtc_aio_latency` | native file-AIO latency per op; blocking-pool queue and run time |
| `xtc_lost_wake` | which fibers parked and never resumed, and whether a wake was ever generated |

```sh
dtrace -qs tools/dtrace/xtc_park_latency.d -c ./server
dtrace -qs tools/dtrace/xtc_lock_contention.d -p "$(pgrep server)"
sudo bpftrace tools/bpftrace/xtc_timer_lateness.bt -p "$(pgrep server)"
```

## Worked example: a lock convoy

`examples/15_usdt/convoy.c` has eight fibers that each take one `xtc_amutex`
and then sleep 200 microseconds, standing in for I/O, while holding it. Nothing
is wrong in the sense of a crash; it is just slow. Run the contention script
against it:

```sh
dtrace -qs tools/dtrace/xtc_lock_contention.d -c './usdt_convoy convoy'
```

On a FreeBSD 15.1 host the report named one amutex with 159 contended waits
out of 160 acquisitions and 246 ms spent waiting in a 36 ms run (eight fibers
waiting in parallel). Of the 159 waits, 155 fell in the 1 to 2 ms bucket:
seven fibers queued ahead, each holding the lock for 200 microseconds. The
stack aggregation pointed at `worker` in `convoy.c`:

```
              convoy`xtc_amutex_lock+0x3fe
              convoy`worker+0x27
              convoy`__proc_entry+0x17b
              159
```

The fix is the obvious one once you can see it: do the I/O outside the lock.
`./usdt_convoy fixed` under the same script reports no contended waits, and
the run takes 4.5 ms instead of 36.

For a flame graph of where the waits come from, keep more frames and feed the
stacks to Brendan Gregg's FlameGraph scripts:

```sh
dtrace -x ustackframes=32 -qs tools/dtrace/xtc_lock_contention.d \
    -c './usdt_convoy convoy' > stacks.out
stackcollapse.pl stacks.out | flamegraph.pl > convoy.svg
```

## Worked example: a reply that never comes

`./usdt_convoy lostwake` has a server that forgets to answer one request in
ten; the client's `xtc_recv` waits out its 20 millisecond timeout each time.
The park latency for reason 1 (mailbox) is bimodal. In the same FreeBSD run,
73 of 80 mailbox parks ended within 128 microseconds and 7 ended between 16
and 32 ms, the longest at 21.3 ms: the 20 ms timeout. Keying the long parks
on `arg0` splits them 4 and 3: four are the client waiting for replies that
never came, three are the server waiting for the next request while the
client sat out its timeout.

`xtc_lost_wake` asks the sharper question. It records each fiber's park, marks
it when a waker claims it (`fiber-wake`), and reports every fiber still parked
at the end with its state: 1 means nothing ever tried to wake it (the wake
source never fired: a reply never sent, a completion never posted), 2 means a
waker did claim it but it never ran (the wake was lost after it was
generated, in a run queue or an inbox). Those are different bugs in different
parts of the code. The same split is what `xtc-tail.py --strands` makes from an
`xtc_tail` trace; the probes give it to you live, on a process nobody set up
for tracing.

## Cost, and why the probes sit where they do

With probes off (the default), the probe macros expand to nothing and their
arguments are not evaluated. The compiled library is the same machine code as
one with no probe sites at all: the `.text` of every object that has a probe
site is byte-identical to the same object built from the source before the
probes were added.

With probes on and no tracer attached, a DTrace probe site is a short run of
no-ops, plus whatever argument setup the compiler emitted for the call that
`dtrace -G` replaced; an SDT probe is one `nop`, with its arguments left in
registers. Turning a probe on traps to the kernel at that site, so what an
enabled probe costs depends on how often it fires. That is why the probes sit
on the slow paths: a park, a wake, a contended lock wait, a timer firing, a
failed allocation, a supervisor restart. The fast paths have none: an
uncontended lock, a slab hit, a send to a fiber that is already running. For
those, use function-boundary tracing (`pid$target::xtc_amutex_lock:entry` in
DTrace, `uprobe:./prog:xtc_amutex_lock` in bpftrace), which costs nothing
until you ask.

No probe reads a clock. Times come from the tracer (`timestamp` in D, `nsecs`
in bpftrace). That keeps the probes outside the deterministic simulator's
determinism guard, and the DST tier passes with probes compiled in.

## Alternatives we did not take

- **Separate tracepoints per tool.** LTTng-UST tracepoints, a perf-specific
  format, a bcc-only helper. USDT notes are already read by perf, bpftrace,
  bcc, SystemTap and, on Linux, LTTng; one probe set covers them all, and the
  same `.d` file gives DTrace platforms the same names.
- **Uprobes on function symbols only.** They need no build option, and you
  should use them for anything not probed. But a function boundary is not an
  interface: libxtc's internal function names change between releases, and a
  park is not a function, it is a point inside one. The probes are named
  events with documented arguments that stay put.
- **Always-on probes in the default build.** On FreeBSD and illumos, `-G`
  costs a build step, and on Linux the header adds a dependency. Opt-in keeps
  the default build free of both, and the off build is provably the same code.
- **ETW on Windows.** Out of scope; on Windows the probe macros are empty.

## See also

- [`xtc_usdt(3)`]({{ '/reference/man-pages/' | relative_url }}) -- every probe,
  argument and code
- [Observing a running application]({{ '/guide/10-observability/' | relative_url }})
  -- `xtc_tail`, the in-process ring
- [Debugging and observing]({{ '/guide/debugging/' | relative_url }}) -- gdb and
  lldb helpers for a stopped or core-dumped process
