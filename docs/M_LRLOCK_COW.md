/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * docs/M_LRLOCK_COW.md — design note for next round
 */

# LRLock copy-on-write — design note

## Question
Can the Left-Right lock allocate the second copy only when first written,
and return that memory to the OS after the swap?

## Answer: Yes — design

Add `XTC_LRLOCK_COW` flag to `xtc_lrlock_opts_t`.  Behaviour:

1. **`xtc_lrlock_create(... | XTC_LRLOCK_COW)`** — allocates `data[0]`
   via `mmap(MAP_ANONYMOUS|MAP_PRIVATE)`; leaves `data[1] = NULL`.
2. **`xtc_lrlock_write_begin`** — if `data[1] == NULL`, mmaps a fresh
   region of `data_size` bytes, then `memcpy(data[1], data[0], data_size)`.
3. **`xtc_lrlock_publish`** — swap `side`, drain readers in old epoch,
   replay oplog onto the now-stale copy.  Then **munmap the stale copy**
   and set its slot to NULL.
4. Subsequent `xtc_lrlock_write_begin` re-mmaps + re-memcpy.

## Trade-offs

| Mode | Idle RAM | Write start latency | Write throughput |
|---|---|---|---|
| Default (current) | 2× data_size | ~0 | high |
| COW (proposed) | 1× data_size | mmap + memcpy | moderate |

For mostly-read workloads (the LRLock target use case) this halves
steady-state memory.  For write-heavy workloads, it adds two syscalls
per write.

## Alternative: madvise instead of munmap

Less aggressive: instead of `munmap` after publish, call
`posix_madvise(stale, sz, POSIX_MADV_DONTNEED)`.  The OS reclaims
pages opportunistically; the VA stays mapped so next write doesn't
need another mmap.  Best of both worlds for some workloads.

## Implementation plan (next round, ~50 LOC diff to lock_lr.c)

- Add `XTC_LRLOCK_COW` and `XTC_LRLOCK_MADVISE_FREE` to a new
  `flags` field in `xtc_lrlock_opts_t`.
- Switch `data[]` allocation from `__os_calloc` to `mmap`.
- Add explicit secondary-alloc/-free in write_begin/publish.
- Test: verify single-copy idle, both copies during write, and
  return to single-copy after publish.
- Bench: measure mmap overhead vs the baseline.

## Why not now

This round shipped `xtc_slab` (~600 lines new C + 9 tests) plus
tail-call infrastructure, which together absorbed the round's
budget.  COW LRLock is a clean ~50-line diff that deserves its
own focused round to bench properly.
