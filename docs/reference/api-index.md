---
title: API reference
nav_order: 4
has_children: true
permalink: /reference/api/
lede: >-
  Every public function, formatted from its manual page and grouped by layer.
---

1. TOC
{:toc}

---

This is the complete public `xtc_*` surface, rendered from the shipped
manual pages and grouped by the layer each function belongs to. Every
function here also installs as a system man page (`man 3 xtc_loop`) and
its coverage is a [release gate]({{ '/testing/' | relative_url }}) -- a
public symbol without a manual page fails the build.

Looking for the generated, cross-referenced **C API** (types, structs,
call graphs)? That is the
[Doxygen reference]({{ '/api/' | relative_url }}), built from the
headers. This section is the prose contract for each function.

## Categories

- [Foundational basics]({{ '/reference/api/basics/' | relative_url }}) --
  allocation, clocks, sleep, atomics, stats, logging, error strings,
  config, version: the pieces that smooth POSIX/libc's rough edges for
  concurrent code.
- [Event runtime (L2)]({{ '/reference/api/event/' | relative_url }}) --
  the loop, the executor, async/await.
- [Processes and messaging (L3)]({{ '/reference/api/proc/' | relative_url }}) --
  spawn, send, receive, gen_server, OS-process bridge.
- [Synchronization and locks (L3)]({{ '/reference/api/sync/' | relative_url }}) --
  sync primitives, lwlock, lrlock, the lock manager, RCU.
- [Orchestration (L4)]({{ '/reference/api/orc/' | relative_url }}) --
  supervisors, app bring-up, registry, launch, the Isolate layer.
- [I/O, files, and network (L1)]({{ '/reference/api/io/' | relative_url }}) --
  the reactor, async file I/O, sockets, TLS, block devices, schedulers.
- [Memory and resources]({{ '/reference/api/mem/' | relative_url }}) --
  slabs, memory contexts, resource caps, the blocking-pool contract.
- [Observability and debugging]({{ '/reference/api/obs/' | relative_url }}) --
  live inspection, causal trace, dumps, proc dictionary, alloc audit.
- [Testing and fault injection]({{ '/reference/api/test/' | relative_url }}) --
  fault points, preemption, stack reclaim.

## Environment variables

libxtc reads a small set of environment variables.  A read while the
simulator runs trips the determinism guard and fails the run (see
[`xtc_runtime(3)`]({{ '/reference/api/xtc_runtime/' | relative_url }})).
The I/O backend and the simulation seed are not among them: the backend
is chosen at configure time, and a simulation's seed is an argument to
the run.

| Variable | Effect |
|---|---|
| `XTC_TAIL_ENABLE` | Read by `xtc_tail_from_env()` when the application calls it: turns the tail ring on (`all` for every source). |
| `XTC_CTRL_FD` | Set by the parent in a child started by `xtc_osproc` or `xtc_xproc`: the inherited control socket. |
| `XTC_AIO_FORCE_OFFLOAD` | `1` sends every `xtc_aio` operation to the offload pool; a testing aid. |
| `PATH` | Searched by `xtc_osproc` for a program named without a slash. |
| `TMPDIR`, `TMP`, `TEMP` | The directory for temporary files, in that order. |

Sanitizer and debugging knobs (`ASAN_OPTIONS`, `UBSAN_OPTIONS`,
`TSAN_OPTIONS`) are the standard ones; the project's CI runs with
`detect_leaks=1:abort_on_error=1` under ASan and
`print_stacktrace=1:halt_on_error=1` under UBSan (see
[Testing]({{ '/testing/' | relative_url }})).

## Error codes

Every fallible function returns `XTC_OK` (0) or a negative `XTC_E_*`
code; [`xtc_strerror(3)`]({{ '/reference/api/xtc_strerror/' | relative_url }})
maps a code to a string. The allocator contract is that
`XTC_OK` implies a non-NULL pointer, so callers check the return code,
never the pointer as well -- see
[`xtc_free(3)`]({{ '/reference/api/xtc_free/' | relative_url }}).
