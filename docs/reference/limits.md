---
title: Limits
parent: Reference
nav_order: 8
permalink: /reference/limits/
lede: >-
  The structural and default limits libxtc imposes, the exact numbers, and why each exists.
---

1. TOC
{:toc}

---

libxtc has two kinds of limit: a few **structural** limits fixed by an
on-wire or in-memory encoding (you cannot exceed them without a rebuild),
and many **default caps** that are tunable at runtime through the
[resource accountant]({{ '/guide/06-resource-limits/' | relative_url }})
([`xtc_res(3)`]({{ '/reference/api/xtc_res/' | relative_url }})). This page
lists both, with the reasoning. The philosophy of *why a library should
impose limits at all* is in the
[resource-limits guide chapter]({{ '/guide/06-resource-limits/' | relative_url }}).

## Structural limits (fixed by encoding)

These are baked into a type or an identifier layout. They are generous
by design, but exceeding one is a compile-time change, not a runtime
knob.

| Limit | Value | Why |
|---|---|---|
| Loops per process (`pid.loop_id`) | 65,536 (`uint16_t`) | A process id is `{loop_id, local_id, gen}`; 16 bits of loop id covers one loop per core on any machine that will exist for a long time. |
| Live processes per loop (`pid.local_id`) | 65,536 (`uint16_t`) | Per-loop slot index. With one loop per core this is 64K procs *per core*; the generation counter (below) makes reused slots safe. |
| Process-generation counter (`pid.gen`) | 4.29 billion (`uint32_t`) | Bumped each time a slot is reused, so a stale pid to an exited process is recognized as stale rather than aliasing a new one. Wraps only after 4 billion reuses of the same slot. |
| Per-loop run-queue depth (`XTC_DEQUE_CAP`) | 256 (power of two) | The work-stealing deque of *immediately runnable* fibers. Not a cap on total fibers -- parked fibers are not in it. 256 is generous; a backend-shaped workload typically has under 128 in flight. |
| lwlock backends (`XTC_LWLOCK_MAX_BACKENDS`) | 4,096 | The lightweight-lock shared/exclusive counter packs the shared count into the low 12 bits, so at most 4,096 concurrent holders -- matched to a large-but-bounded backend population. |
| lrlock reader slots | 64 per lock (default), 4,096 global | Each left-right lock has a fixed pool of wait-free reader slots; if more distinct reader threads than slots appear, `read_begin` returns NULL ("retry later"). Size `max_readers` to your concurrent-reader count. |
| Blocking-pool threads (`BLK_MAX_THREADS`) | 64 | The pool that runs `xtc_blocking_run` work grows on demand to this cap. It bounds how many OS threads offloaded blocking calls can consume, so a burst of blocking work cannot spawn unbounded threads. |
| Static amutex fast slots (`XTC_AMUTEX_STATIC_MAX`) | 32 | A small fixed set of statically-initializable async mutexes; beyond that use the dynamically created form. |

### Isolate layer (tnt) handle encoding

The optional [Isolate layer]({{ '/tnt/' | relative_url }}) packs its
handle into 64 bits:

| Field | Bits | Max |
|---|---|---|
| Shard (`XTC_TNT_SHARD_BITS`) | 8 | 256 shards (cores) |
| Type (`XTC_TNT_TYPE_BITS`) | 8 | 256 Isolate types |
| Slot (`XTC_TNT_SLOT_BITS`) | 20 | ~1,048,575 Isolates per (shard, type) |
| Generation (`XTC_TNT_GEN_BITS`) | 28 | ~268M reuses before wrap |

Plus a message payload cap of 96 bytes and an init-args cap of 64 bytes
(`XTC_TNT_MAX_PAYLOAD_SIZE` / `XTC_TNT_MAX_INIT_ARGS_SIZE`) -- Isolates
communicate with small, fixed messages by design.

## Default caps (tunable at runtime)

The resource accountant tracks bounded resources and rejects an acquire
past the cap with `XTC_E_RESOURCE`. These are the defaults
(`XTC_RES_CAPS_DEFAULT`); set your own with `xtc_res_set_cap` or an
`xtc_res_caps_t` at init.

| Resource (`xtc_res_kind_t`) | Default cap | Meaning |
|---|---|---|
| `XTC_RES_TASKS` | 100,000 | Live task/fiber allocations. |
| `XTC_RES_CHANNELS` | 4,096 | Live channel objects. |
| `XTC_RES_CHAN_SLOTS` | 1,000,000 | In-flight messages across all channels. |
| `XTC_RES_FDS` | 65,536 | Open file descriptors attributable to libxtc. |
| `XTC_RES_MEM_BYTES` | 1 GiB | Bytes in libxtc-tracked allocations. |
| `XTC_RES_INBOX_MSGS` | 65,536 (per loop) | Cross-loop inbox messages in flight. |

Each also tracks a high-water mark and a reject count, and can fire an
alert callback at a configurable percentage of the cap -- so you learn
you are *approaching* a limit before you hit it. See
[`xtc_res(3)`]({{ '/reference/api/xtc_res/' | relative_url }}) and the
[resource-limits guide chapter]({{ '/guide/06-resource-limits/' | relative_url }}).

## What is *not* limited

- **Total fibers/processes** has no single hard number -- it is bounded
  by the `XTC_RES_TASKS` cap and by memory, not by a structural limit
  below 64K-per-loop. The [Isolate layer]({{ '/tnt/' | relative_url }})
  exists precisely for populations larger than a fiber-per-entity model
  fits in memory.
- **Cores** -- libxtc runs one loop per core via the executor; the only
  ceiling is the 65,536 loop-id encoding, far beyond any real core count.

## Changing a structural limit

The structural limits live in `src/inc/*.h` as `#define`s
(`XTC_DEQUE_CAP`, `XTC_LWLOCK_MAX_BACKENDS`, `BLK_MAX_THREADS`, the tnt
bit widths). Raising one is a source edit plus a rebuild, and some (the
pid and tnt bit layouts) trade one field's range against another's --
they are sized deliberately. If you find a structural limit binding,
that is worth a conversation upstream, not a quiet local bump.
