# M_DPDK -- optional DPDK userspace-networking backend

## Status

DETECTION + BUILD WIRING ONLY (as of 2026-06).  The flake provides
DPDK as an optional Linux-only dev dependency, and `configure
--with-dpdk` probes libdpdk via pkg-config and defines `XTC_HAVE_DPDK`.
The actual DPDK I/O backend (a poll-mode reactor + a userspace TCP/IP
path) is NOT yet implemented -- this document scopes that work so it
can be built deliberately rather than half-baked.

The default build neither needs nor links DPDK.  `--with-dpdk=no` is
the default; `--with-dpdk=auto|yes|PATH` enables detection.

## Why DPDK at all

The scalability review (vs Seastar) identified that libxtc uses the
kernel network stack, whereas Seastar's throughput ceiling comes from
DPDK userspace networking (poll-mode drivers, no per-packet syscall, no
kernel/user copy, hugepage-backed mbuf pools).  A DPDK backend would
let libxtc close that specific gap for a dedicated networking
deployment -- at the cost of binding cores to NICs, requiring
hugepages, and bypassing the kernel firewall/routing.  It is a
deployment-specific opt-in, never the default.

## What DPDK actually requires (why this is a large feature)

DPDK is an L2 framework, not a sockets replacement.  A working backend
must supply, above raw DPDK:

1. EAL initialization (rte_eal_init): hugepage setup, core mask, PCI
   probing.  This is process-global and must run before any loop
   starts; it does not fit the per-loop xtc_io_init model cleanly --
   it needs an executor-level init hook.
2. Per-core poll-mode RX/TX: each xtc_exec loop owns one or more NIC
   queues and polls them (rte_eth_rx_burst) in its step -- replacing
   the epoll/kqueue readiness model with busy-poll.  This changes the
   loop's idle behavior (a DPDK loop never truly sleeps; it polls),
   which interacts with the executor's idle-detection / auto-stop.
3. A userspace TCP/IP stack: DPDK delivers raw Ethernet frames.  To
   present xtc_net's stream/datagram API, the backend needs ARP, IP,
   and TCP (or a vendored stack like lwIP / F-Stack / Seastar's native
   stack, or TLDK).  This is the bulk of the work and the main risk.
4. mbuf <-> xtc buffer bridging: zero-copy where possible, matching the
   xtc_aio / xtc_net buffer ownership contract.

## Proposed shape (when implemented)

- A new I/O backend `src/io/io_dpdk.c` selected by
  `--with-io-backend=dpdk` (gated on XTC_HAVE_DPDK), implementing the
  same `xtc_io_*` vtable as io_epoll.c -- but readiness becomes
  RX-burst polling and "wakeup" becomes a per-core ring enqueue.
- An executor-level `xtc_exec_dpdk_init(eal_args)` that runs
  rte_eal_init once and assigns NIC queues to loops by exec_id.
- A userspace TCP/IP module under `src/io/dpdk/` (vendor TLDK or a
  minimal native stack); xtc_net's listen/dial/recv/send route through
  it when the DPDK backend is active.
- Hugepage + core-pinning requirements documented in
  docs/M_PORT.md and the getting-started guide; clearly flagged as a
  bare-metal / SR-IOV deployment feature, not a laptop default.

## Build + dependency

- flake.nix: `dpdk` is an optional Linux-only buildInput + devShell
  package (alongside liburing).
- configure: `--with-dpdk=auto|yes|no|PATH`; pkg-config `libdpdk`;
  sets XTC_HAVE_DPDK + XTC_DPDK_CFLAGS/XTC_DPDK_LIBS.
- The DPDK backend, once written, links `$(XTC_DPDK_LIBS)` and is the
  only TU that includes DPDK headers, so a non-DPDK build is byte-for-
  byte unchanged.

## Honest assessment

This is a multi-month feature dominated by the userspace TCP/IP stack,
not the DPDK plumbing.  The detection/wiring landed now is the cheap,
correct first step: the dependency is reproducible (flake) and
discoverable (configure), so the backend can be built without first
re-litigating the build integration.  Do NOT claim a DPDK backend
exists until io_dpdk.c + a TCP/IP path pass the xtc_net test suite over
a real (or SR-IOV / virtio-user) NIC.
