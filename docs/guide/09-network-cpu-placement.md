---
title: Network CPU placement (RPS)
parent: Guide
nav_order: 9
permalink: /guide/09-network-cpu-placement/
lede: >-
  Steering NIC softirq off your loop cores is a kernel knob, not a libxtc
  feature -- here is what it measurably buys, and what it costs.
---

# Network CPU placement

On a busy network server, a meaningful share of each request's CPU is spent
in the kernel's receive path -- softirq work (`net_rx_action`,
`process_backlog`) that runs on whichever core the NIC interrupt landed on.
By default that is often *the same core running your fibers*, so the two
compete.

Seastar addresses this with a dedicated "networking core" backend. libxtc
does not, and deliberately will not: **the work is not ours to move.** This
page explains what we measured, and gives you the kernel knob that does move
it -- along with the trade it makes, which is not free.

## Why libxtc ships no code for this

libxtc's network path never hands work to io_uring's kernel worker pool
(io-wq). The only operations it submits are `poll_add` / `poll_multishot` /
`poll_remove`, a preempt timeout, and file `read`/`write`/`fsync`. Sockets
are *polled* for readiness and then read and written **inline, on the fiber's
own thread**.

Measured on a 32-vCPU box across 75 benchmark runs: `peak_iowq_threads = 0`
for every network run, and `io_wq_submit_work` never appears in a `perf`
profile while `tcp_sendmsg` accounts for ~46% of server CPU under
`__x64_sys_write`. Per-request kernel time was **flat at 7.8-8.6 us across
every loop count and compute level**.

So there is no concentration of libxtc-submitted kernel work to redistribute,
and an `io_uring_register_iowq_aff`-style knob would have nothing to bind.
That is why no such API exists here.

## The knob that does work: RPS

Receive Packet Steering is a kernel feature. It moves softirq receive
processing to a chosen set of cores, so your loop cores stop paying for it:

```sh
# Steer NIC receive softirq to CPUs 6 and 7 (mask 0xc0).
# One rx queue shown; repeat per queue on a multi-queue NIC.
for q in /sys/class/net/eth0/queues/rx-*; do
    echo c0 | sudo tee "$q/rps_cpus" > /dev/null
done

# Also move the hardware IRQs themselves off the loop cores.  Cloud
# defaults frequently do NOT do this: on the box measured here, two of
# eight ena vectors landed on CPUs 0 and 3, which were running fibers.
grep eth0 /proc/interrupts | awk '{print $1}' | tr -d ':' | \
while read irq; do echo 6,7 | sudo tee "/proc/irq/$irq/smp_affinity_list" > /dev/null; done
```

Then start your executor with fewer loops, leaving those cores free -- e.g.
6 loops pinned to CPUs 0-5 when steering softirq to 6-7.

`tuned`, `perftune.py` (from Seastar) and your distro's equivalents automate
this. Nothing above requires a libxtc change or rebuild.

## What it measurably buys, and costs

Two `c6i.8xlarge` instances over a real ena NIC (a driver box and a subject
box, four alternated repetitions per point), at **equal total cores** --
8 loops with inline softirq versus 6 loops plus 2 dedicated softirq cores:

| metric | change | read this as |
|---|---|---|
| p99 latency | **-18%** | the win |
| p50 latency | **+23%** | the cost |
| throughput | -1% to -5% | roughly neutral to slightly worse |
| machine-wide CPU per request | -5% | a small genuine saving |
| server *process* CPU per request | -20% | **mostly relocation, not reduction** |

**Read the last two rows together.** The 20% drop in the server process's
own CPU accounting is largely softirq leaving that process's books, not work
disappearing. Per-core attribution makes this concrete: application cores
burned 19.58 CPU-seconds without steering and 19.67 with it -- *identical* --
while the dedicated cores fell from 6.65 to 4.25 and the machine total
dropped about 5%. The application cores stay saturated either way, because
there are fewer of them doing the same work.

So the honest summary is: **you trade median latency for tail latency, and
reclaim a modest amount of machine CPU.** If your service is judged on p99
this is a good trade. If it is judged on p50, or on raw throughput, it is
not -- and a naive reading of "server CPU dropped 20%" will mislead you.

The effect is also workload-shaped, matching Seastar's published findings: it
helps when each request carries real compute, and on a pure-I/O workload with
no per-request work it is a straightforward regression.

## Measure it yourself

`bench/bench_net_compute.c` is the harness these numbers came from. It sweeps
loop count against per-request compute cost and reports throughput, p50, p99
and CPU-microseconds per request:

```sh
# subject
./bench_net_compute server 9998 <n_loops> <compute_us>
# driver, ideally on a second machine -- loopback distorts the result
./bench_net_compute client <addr> 9998 <n_conns> <msgs_each> <bytes> [threads]
```

Two cautions learned the hard way while producing the table above:

* **Compare equal hardware.** An early version of this comparison gave the
  RPS arm two extra cores and produced a spurious "+23% throughput, -31%
  CPU". Those numbers were wrong and are not in this page.
* **Use a real NIC.** On loopback the same configuration showed throughput
  *falling* 4-27%, because the "dedicated" cores were processing loopback
  softirq and the client shared the machine. Loopback cannot answer a
  deployment question.
