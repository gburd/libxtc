# Performance Audit Summary

## Pass 1: False Sharing

### Changes Made

1. **XTC_CACHE_LINE moved to xtc_int.h** (default 64, configurable via -DXTC_CACHE_LINE=128)

2. **struct xtc_proc mailbox fields** (proc.c):
   - Consumer-side (mbox_head, mbox_n) separated from producer-side (mbox_lock, mbox_tail)
   - 64-byte padding between consumer and producer fields
   - `_Static_assert` verifies separation >= 64 bytes

3. **struct xtc_chan_mpsc** (chan.c):
   - head and tail atomics placed on separate cache lines using `_Alignas(XTC_CACHE_LINE)`
   - `_Static_assert` verifies separation >= 64 bytes

4. **epoch_entry_t** (lock_lr.c):
   - Already padded to XTC_CACHE_LINE
   - Added `_Static_assert(sizeof(epoch_entry_t) == XTC_CACHE_LINE)` to verify

### Not Changed (Analysis)

- **struct xtc_loop inbox**: The inbox uses a mutex-protected FIFO, not a lock-free structure. Cross-thread writes are serialized by the mutex, so false sharing is not the primary concern.

- **struct xtc_slab counters**: The slab stats are either protected by the per-cache mutex (slow path) or updated from per-thread magazines (fast path). The TLS magazine architecture already avoids most contention.

- **__tls_mags in slab.c**: Verified to be `__thread`, so per-thread by definition.

## Pass 2: Branch Prediction Hints

### XTC_LIKELY / XTC_UNLIKELY Macros Added to xtc_int.h

```c
#if defined(__GNUC__) || defined(__clang__)
#  define XTC_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define XTC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define XTC_LIKELY(x)   (x)
#  define XTC_UNLIKELY(x) (x)
#endif
```

### Annotated Sites (33 total)

| File | Count | Description |
|------|-------|-------------|
| proc.c | 10 | Error returns, NULL checks, resolve failures |
| chan.c | 9 | MPSC try_send/recv error paths, channel full/empty |
| slab.c | 11 | Magazine hit/miss, OOM paths |
| lock_lr.c | 3 | Wait-for-readers spin loop |

### Key Hot-Path Annotations

- **slab.c**: Magazine fast path marked LIKELY; slow-path/OOM marked UNLIKELY
- **chan.c**: CAS success marked LIKELY; channel full/empty marked UNLIKELY
- **lock_lr.c**: All-done condition marked LIKELY; reader still active marked UNLIKELY

## Pass 3: Struct Packing

### Analysis

- **struct envelope** (proc.c): Already well-packed (pointer + size_t + flex array)
- **struct lock_entry** (lock_mgr.c): Uses slab allocator, size less critical
- **struct xtc_proc**: Mailbox padding added for correctness, not packing

No fields reordered; the existing layouts are reasonable.

## Pass 4: Tail-Call Opportunities

### Annotated Sites (4 total, up from 2)

| Function | File | Result |
|----------|------|--------|
| xtc_recv | proc.c | OK (jmp) |
| xtc_recv_match | proc.c | OK (jmp) - NEW |

Both delegate to `__do_recv` as a tail call, verified by `scripts/check-tailcalls.sh`.

## Pass 5: Const-Correctness

### Analysis

The codebase already has good const-correctness:
- All `*_len()` functions take `const` pointers
- All `*_stat()` functions take `const` pointers
- Supervisor getters use `const xtc_supervisor_t *`

No changes needed.

## Pass 6: Scoping

### Analysis

- All `__os_*` symbols are intentionally exported (OS abstraction layer API)
- All `__xtc_*` symbols are cross-TU internal helpers (needed for linking)
- No inadvertently exported internal helpers found

## Verification

### Build

```
cd build_unix && make -j4  # No errors or warnings
```

### Tests

All key tests pass:
- test_proc: 4/4 (100%)
- test_chan: 5/5 (100%)
- test_slab: 10/10 (100%)
- test_lrlock: 8/8 (100%)
- test_loop: 4/4 (100%)

### Tail-Call Verification

```
scripts/check-tailcalls.sh
# 4 ok, 0 warn, 0 fail, 0 unverified
```

## Micro-Benchmarks

Created `bench/audit/` with:

1. **false_sharing_mpsc.c**: MPSC channel throughput under 4-producer contention
   - Result: ~5.5M msgs/sec

2. **false_sharing_proc.c**: Proc mailbox throughput under 4-sender contention
   - Result: ~1.4M msgs/sec

## Bug Fixes (Found During Audit)

1. **XTC_E_VERSION**: Added missing error code for slab.c shm version mismatch
