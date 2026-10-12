# GPU-accelerated continuous batching (Mandelbrot)

A public-API consumer of the **async batch** interface
(`xtc_batch_create_async`, the submit/collect split, the FUTURE exec kind).
Each batch MEMBER is a TILE of a Mandelbrot image; a GPU (or a deterministic
CPU fallback) computes tiles, and continuous batching keeps the device fed as
shallow tiles finish and new tiles join while deep tiles keep iterating -- the
vLLM continuous-batching shape, driven by a GPU kernel instead of an LLM decode
step.

Read [mandel.c](mandel.c) for the complete runnable example. This is the
sibling of [12_continuous_batching](../12_continuous_batching) (an admission-
order proof with no device dependency); 13 adds a real device backend.

## Three device backends behind one vtable

The same batch submit/collect driver feeds all three; only `struct device`
differs:

- **fake** (default): pure-CPU Mandelbrot, deterministic, no GPU, CI- and
  sim-safe. Hands the engine a FUTURE token (immediately resolved) by default,
  or SYNC. This is what `--self-test` uses.
- **opencl** (`XTC_WITH_OPENCL=1`): real GPU. `submit()` sets per-tile kernel
  args, enqueues one NDRange per tile, and bridges the resulting `cl_event` to
  the engine's FUTURE exec kind via a detached worker thread that
  `clWaitForEvents` then `xtc_promise_set`. `collect()` reads back each tile's
  buffer region and decides DONE (converged) vs CONTINUE (needs more passes).
- FENCE kind is not used: OpenCL does not portably export a sync_file fd, so
  FUTURE is the portable path (the hardware note prescribes this).

`request` (the `struct tile*`) is the request-as-carrier: it holds the tile's
device-output location and a mock KV-slot id. `complete()` firing is the cue
the slot is free -- the contract a real vLLM-style server fills.

## Build and run

Configure libxtc with `--enable-sched-policy` and build `libxtc.a`, then:

```sh
# FAKE backend (default): links libxtc.a + pthread only, no OpenCL
make XTC_BUILD=/abs/path/to/configured-build
./mandel --self-test

# OPENCL backend: links -lOpenCL
make XTC_WITH_OPENCL=1 XTC_BUILD=/abs/path/to/configured-build
./mandel --backend=opencl --devices=2 --mode=continuous
```

`XTC_BUILD` defaults to `../../.agent/final-native`. libxtc and `make check`
have NO OpenCL dependency; only the opencl backend links `-lOpenCL`.

On a multi-GPU box (e.g. two AMD R9 280X via rusticl):

```sh
RUSTICL_ENABLE=radeonsi ./mandel --backend=opencl --devices=2 --mode=continuous
```

creates two batch groups, one per cl device, each on its own executor loop, and
feeds both concurrently -- both GPUs busy at once.

## CLI

- `--backend=fake|opencl` (default fake)
- `--devices=N` (groups/GPUs, 1..2; default 1)
- `--mode=continuous|static` (default continuous)
- `--width=N --height=N` image size (default 256x256)
- `--tiles=N` tiles per axis, NxN total (default 4)
- `--maxiter=N` escape-iteration cap (default 256)
- `--self-test` deterministic fake-backend checks, no GPU required
- `--help`

## What to look for

- **Variable-length tiles:** shallow tiles (outside the set) converge in one
  pass and DONE early; deep tiles (near the boundary) need many passes and stay
  in the batch. The self-test asserts shallow=1 pass, deepest > 1.
- **Continuous vs static occupancy:** continuous refills a freed slot
  immediately, so the device vector stays fuller (higher members-per-op) and
  needs fewer device ops for the same tiles. The self-test asserts
  continuous occupancy > static.
- **Exactly-once completion:** every accepted tile completes exactly once; the
  `complete` callback asserts it.
- **Checksum:** an exact integer sum of per-pixel escape-iteration counts. The
  fake-CPU path equals an independent scalar reference exactly; the GPU float
  path matched bit-for-bit on the tested AMD GPUs (small per-vendor boundary
  differences are possible and reported with a note, not asserted exact).

## Self-test (no GPU)

`./mandel --self-test` runs the fake backend deterministically and asserts all
of the above (exactly-once, checksum == reference, variable passes, continuous
> static occupancy, SYNC and FUTURE both correct). It passes with no GPU
present and is byte-identical across runs.
