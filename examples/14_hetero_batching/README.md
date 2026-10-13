# Heterogeneous GPU + NPU concurrent batching (capstone)

A public-API consumer of the **async batch** interface
(`xtc_batch_create_async`, the submit/collect split, the FUTURE exec kind)
that drives **two unlike accelerators at the same time**: an Intel Arc GPU
via OpenCL and an Intel NPU ("AI Boost") via Level Zero. One executor, two
loops, one batch group per loop, each group bound to a different device, and
the continuous-batching policy keeps **both devices fed concurrently**.

This is the capstone of the accelerator track. Its sibling
[13_gpu_batching](../13_gpu_batching) proves the device-event -> FUTURE bridge
on a single device class (OpenCL, Mandelbrot). 14 adds a second, physically
different device (the NPU via Level Zero) and runs both at once.

Read [hetero.c](hetero.c) for the complete runnable example.

## The workload (pseudo-LLM decode)

Each batch MEMBER is a "request" -- a sequence that needs a **variable** number
of decode-like iterations before it emits end-of-sequence (DONE). Shallow
requests finish in one or two iterations and leave the batch; deep ones keep
iterating. New requests join the freed slots while deep ones continue, so the
device vector stays full and members leave/join mid-batch -- the vLLM
continuous-batching shape, here split across two accelerators running at once.
The per-iteration "compute" is a deterministic integer `decode_step`, so every
backend agrees on the result and the fake path is byte-identical across runs.

## Three device backends behind one vtable

The same batch submit/collect driver feeds all three; only `struct device`
and the per-group backend selection differ:

- **fake** (default): pure-CPU, deterministic, no GPU/NPU, CI- and sim-safe.
  Both groups use it unless `--hetero` is given. FUTURE (default) or SYNC.
  This is what `--self-test` uses.
- **opencl** (`XTC_WITH_OPENCL=1`): real GPU. `submit()` enqueues one decode
  NDRange per ready request and bridges the resulting `cl_event` to the
  engine's FUTURE exec kind through the group's one long-lived waiter
  thread, which `clWaitForEvents`, releases the event, then
  `xtc_promise_set`. (A thread per op costs ~50-100 us per op; a persistent
  waiter ~13-19 us. Use the FENCE kind where the runtime exports a fence
  fd.)
- **levelzero** (`XTC_WITH_LEVELZERO=1`): real NPU. `submit()` appends a small
  Level Zero workload to the NPU command list, executes it on the NPU command
  queue, and signals a `ze_event`; the group's waiter thread does
  `zeEventHostSynchronize` then `xtc_promise_set` -- the **same FUTURE bridge**
  as OpenCL. Each request stages through its own region of the host-visible
  buffer, and `collect()` copies that region back only after the event fired,
  so the data really made the round trip.

`--hetero` wires group 0 -> **opencl (GPU)** and group 1 -> **levelzero (NPU)**.

## What the NPU op actually is (honest label)

The NPU runs **Level Zero native**, not OpenCL C. A real model/graph is out of
scope (it needs a compiled L0 module or OpenVINO IR). The NPU op here is a
**device-memory copy chain** per decode iteration:

    host staging  --copy-->  NPU device buffer A
                  --barrier--
    NPU buffer A  --copy-->  NPU device buffer B
                  --barrier--
    NPU buffer B  --copy-->  host staging   (this copy signals the ze_event)

appended with `zeCommandListAppendMemoryCopy` / `...AppendBarrier`, executed on
the NPU command queue, and awaited through a `ze_event`. The decode arithmetic
(`decode_step`) runs on the host; the NPU work is a **command-queue
round-trip**, NOT a matmul. What this proves is exactly the scheduling claim:
the batch engine **fed the NPU command queue and awaited its completion** via
the FUTURE bridge, concurrently with the GPU group. It is labelled as a
round-trip precisely so no one reads it as NPU inference.

## Proving concurrency

Each group marks itself in-flight across a device op (submit..collect) with a
shared atomic. The run reports:

- `peak_overlap` -- the maximum number of groups in-flight at the same instant.
  `peak_overlap == 2` means the GPU op and the NPU op were simultaneously
  in-flight: **GPU busy AND NPU busy at the same time**.
- per-group `overlap_ops` -- how many of a group's ops ran while the other
  group was also mid-op.

## Build and run

Configure libxtc with `--enable-sched-policy` and build `libxtc.a`, then:

```sh
# FAKE backend (default): links libxtc.a + pthread only, no OpenCL/Level Zero
make XTC_BUILD=/abs/path/to/configured-build
./hetero --self-test
```

The real GPU + NPU run on floki (Intel Lunar Lake). Both runtimes come from
nix; the NPU userspace driver must be on the loader path via
`ZE_ENABLE_ALT_DRIVERS`:

```sh
ZE_INC=$(nix eval --raw nixpkgs#level-zero.outPath)/include
ZE_LIB=$(nix eval --raw nixpkgs#level-zero.outPath)/lib
NPU=$(nix eval --raw nixpkgs#intel-npu-driver.outPath)

nix-shell -p opencl-headers ocl-icd intel-compute-runtime \
             level-zero intel-npu-driver gcc gnumake --run "
  make XTC_WITH_OPENCL=1 XTC_WITH_LEVELZERO=1 ZE_INC=$ZE_INC ZE_LIB=$ZE_LIB
  export ZE_ENABLE_ALT_DRIVERS=$NPU/lib/libze_intel_npu.so.1.28.0
  ./hetero --hetero
"
```

Expected tail (floki, Intel Arc GPU + Intel AI Boost NPU):

```
group 0 (GPU) -> cl device 0 (Intel(R) Arc(TM) Graphics)
L0: VPU device "Intel(R) AI Boost" (type=5)
group 1 (NPU) -> Level Zero VPU (op: device-memory copy chain per iteration)
mode=continuous hetero=1 reqs=48
completed=48 cancelled=0 failed=0 (exactly-once enforced)
reqs_done=48 device_ops=58 avg_occupancy=3.66 members/op
  group 0 [opencl]: reqs_done=24 device_ops=28 overlap_ops=28
  group 1 [levelzero]: reqs_done=24 device_ops=30 overlap_ops=0
CONCURRENT: peak_overlap=2 groups in-flight at once; GPU busy AND NPU busy at the same time: YES
```

libxtc and `make check` have NO OpenCL or Level Zero dependency; only the
opencl / levelzero backends link `-lOpenCL` / `-lze_loader`.

## CLI

- `--hetero` GPU (OpenCL) + NPU (Level Zero), both busy concurrently
  (needs both real backends built)
- `--mode=continuous|static` (default continuous)
- `--reqs=N` number of pseudo-decode requests, 2..256 (default 48)
- `--self-test` deterministic fake-backend checks, no GPU/NPU required
- `--help`

## Self-test (no GPU/NPU)

`./hetero --self-test` runs the fake backend deterministically over TWO groups
and asserts:

- exactly-once terminal completion for every request,
- checksum == an independent scalar reference,
- **both groups made progress** (reqs_done > 0 on each),
- variable-length requests coexist (shallow = 1 iteration, deep > 1),
- static mode (same queue depth of 8, cohort refill) is equally correct --
  it reports both modes' occupancy (fake, 48 requests: continuous 58 ops at
  3.66 members/op, static 71 ops at 2.99) but does not assert one beats the
  other: that is an op count, not a speed claim,
- SYNC and FUTURE exec kinds both correct.

It passes with no GPU/NPU present and is byte-identical across runs.

## Driver notes (floki, Intel Lunar Lake)

- **GPU readback.** On this compute-runtime (`intel-compute-runtime` 26.18 /
  Lunar-Lake Arc), both `clEnqueueReadBuffer` **and** the staging path inside
  `clEnqueueMapBuffer` crash in a built-in copy-kernel build
  (`NEO::Zebin::decodeZeInfo`) under the batch engine's queue-usage pattern.
  The opencl backend therefore uses `CL_MEM_USE_HOST_PTR` and reads the shared
  host pointer directly after `clFinish` (the Arc iGPU is UMA, so the host
  array is coherent) -- no read-buffer, no map, so no built-in copy kernel.
- **NPU event pool.** Each NPU op creates a `ze_event` at a rotating pool
  index; the waiter destroys it after the completion and only then marks the
  index free, so an index is never reused while its previous event lives (if
  it ever were, the op fails with `XTC_E_AGAIN` instead of a device error).

The OpenCL and Level Zero backends were changed after their last hardware
run (persistent waiter, per-request NPU staging regions, event-index
tracking, no polling feeder); they are compile-checked but have NOT been
re-run on the Lunar Lake GPU/NPU since. The expected output above is from
the earlier version.
