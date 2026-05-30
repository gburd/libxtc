# Cross-platform CI plan

This document outlines a strategy for automated cross-platform testing
in advance of submission to the PostgreSQL hackers list.  Manual
SSH-into-host runs (Linux [OK], FreeBSD [OK], illumos [OK], Windows [OK]) get us
through development; for a project that wants to land on the PG
build farm, we need something more systematic.

## Goal

Test xtc on every platform combination the PG build farm exercises,
on demand and on every commit, without requiring developers to own or
operate the underlying hardware.  Cache OS images cheaply enough that
running the matrix is "press a button" not "burn a weekend".

## What the PG build farm covers

The active animals at https://buildfarm.postgresql.org/ run roughly:

* **Linux**: Debian, Ubuntu, RHEL/CentOS/Rocky, Fedora, Alpine, Arch,
  SUSE -- across glibc + musl, gcc + clang, on x86_64, aarch64, ppc64le,
  s390x, riscv64, and a handful of more exotic arches.
* **BSD**: FreeBSD 13/14/15, OpenBSD 7.x, NetBSD 10.x, DragonFlyBSD --
  on amd64 + aarch64.
* **macOS**: 12, 13, 14, 15 on x86_64 and arm64.
* **illumos**: OmniOS, OpenIndiana, SmartOS -- amd64 and SPARC.
* **Windows**: 10, 11, Server 2019/2022 -- MSVC, MinGW, sometimes
  Cygwin.
* **AIX**: 7.x -- power7+, power8, power9.
* **Solaris**: 11.4 -- SPARC + amd64.

That's about 35-40 distinct build environments.  We won't match all
of them on day one, but we want a clear path.

## Approach: hierarchical caching

```
Tier 0 -- local dev:           Linux + the three remote SSH hosts I have
Tier 1 -- pre-commit hook:     Docker on the dev box, ~30 sec budget
Tier 2 -- pre-push gate:       GitHub-Actions free runners, ~5 min budget
Tier 3 -- full matrix:         self-hosted on EC2 / Hetzner with
                              S3-cached images, ~60 min budget
Tier 4 -- buildfarm submission: handed to PG buildfarm post-merge
```

Each tier subsumes the previous in coverage; the budget gates how often
each runs.

## Tier 1: Docker-on-dev

For Linux distros, Docker images of every supported flavour are tiny
to produce and run.  Maintain `dist/ci/docker/<distro>/Dockerfile`
covering: debian-bookworm, ubuntu-2404, fedora-40, alpine-edge,
archlinux, opensuse-tumbleweed.  Each image preinstalls gcc/clang +
autoconf + make + (optional) liburing + (optional) hegel.

Pre-commit hook runs:
```
for d in $(ls dist/ci/docker); do
   docker build -q -f dist/ci/docker/$d/Dockerfile -t xtc-$d .
   docker run --rm xtc-$d sh -c "cd /xtc && ./dist/configure && make -s check"
done
```

That covers ~6 Linux distros x ~30s each = ~3 min before the dev
notices, on a hot Docker cache.  Cold cache the first time is bigger
but those layers cache in `~/.docker`.

## Tier 2: GitHub Actions

Free GHA runners cover Linux (ubuntu-24.04, ubuntu-22.04), macOS
(macos-14, macos-15 on arm64; macos-13 on x86_64), and Windows
(windows-2022 with MSVC + MinGW).  That's 5-6 environments for free.

**Live as of 0.4.0** (`.github/workflows/ci.yml`): `build-and-test`
(gcc, clang), `sanitizers` (address, undefined), `coro-fctx`
(forced-fcontext / musl path), `examples`, **`macos`**
(`macos-latest`, Apple Silicon -- kqueue + ucontext + GCD dispatch
semaphores; runs the C munit suite), and **`windows-msvc`**
(`windows-latest` -- `dist/build_msvc.bat` builds xtc.lib via cl +
ml64 and runs the smoke test).  Adding the macOS job surfaced and
fixed six real portability bugs (the Darwin feature macro, the
rwlock storage size, unnamed-semaphore unsupport, `_SC_NPROCESSORS_ONLN`,
hardcoded `-lrt`, and a thread_local-vs-pthread_key teardown-order
bug in the lrlock slot reclaimer).

The xtc workflow runs the same `./dist/configure && make check` on
each runner.  Total wall time: 4-7 minutes per push.  This is the
"PR can't merge if red" gate.

A second GHA workflow on `schedule:` runs the deeper matrix once a
day on cron -- Tier 3 (below) but sourced from S3 caches, so nightly
cost is bounded.

## Tier 3: self-hosted EC2 + S3-cached images

This is what you asked about -- caching OS images in S3 to amortize
acquisition cost across the project lifetime.

### Why S3 caching matters

* OS images are big (1-30 GB each).
* A FreeBSD 14 amd64 cloud image from FreeBSD's own release server
  takes ~20 minutes to download from a US-East EC2 region; once
  cached in S3 (same region) it's ~2 minutes.
* Some images we build ourselves (illumos with autoconf preinstalled,
  AIX with gcc-aix); rebuilding from scratch every CI run is
  prohibitive.
* Buildfarm-aligned images (Debian-bookworm-amd64, RHEL-9-aarch64,
  FreeBSD-14-amd64, etc.) are the same artefacts every CI run; cache
  them once.

### Layout

```
s3://xtc-ci/
   images/
      debian-12-amd64.qcow2          (~2 GB)
      debian-12-aarch64.qcow2
      fedora-40-amd64.qcow2
      alpine-edge-amd64.qcow2
      archlinux-amd64.qcow2
      freebsd-14-amd64.qcow2         (~3 GB)
      openbsd-7.5-amd64.qcow2
      netbsd-10-amd64.qcow2
      dragonflybsd-6.4-amd64.qcow2
      omnios-r151052-amd64.qcow2
      openindiana-2024.04-amd64.qcow2
      windows-server-2022-amd64.vhdx (~30 GB; pre-licensed eval)
      aix-7.3-power9.qcow2           (built once, kept indefinitely)
   tools/
      qemu-system-x86_64.tar.gz
      qemu-system-aarch64.tar.gz
   builds/<git-sha>/
      <platform>/results.json        (test outcomes, gcov lcov)
```

### Driver shape

```
dist/ci/run-matrix.sh:
  for platform in $MATRIX; do
     1. aws s3 cp s3://xtc-ci/images/$platform.qcow2 /local/cache/
        (if checksum doesn't match cached copy)
     2. boot qemu/firecracker/cloud-hypervisor with that image
     3. ssh into the booted VM
     4. rsync the source tree in
     5. run autoreconf -i; configure; make check
     6. capture results, scp them back out
     7. shut down the VM
  done
```

QEMU + S3 is a good fit because:
* QEMU works across all the host arches we care about (intel, ARM).
* The qcow2 format is single-file-snapshottable; S3 stores the base
  image once, and per-CI-run we just stack a copy-on-write overlay
  locally.
* Total runtime: image-fetch (~2 min) + boot (~30 sec) + tests
  (~3 min) + teardown (~10 sec) ~= 6 min per platform.  10 platforms
  in parallel on a beefy host = 6-8 min wall time for the whole
  matrix.

### EC2 cost model (illustrative)

Run the matrix once daily as a scheduled GHA -> self-hosted runner job:

* Spin up `c7i.4xlarge` spot (16 vCPU, 32 GB RAM): ~$0.30/hr spot.
* Run the matrix in 8 minutes, shut down.
* 30 days x 8 minutes = 4 hours/month x $0.30 = $1.20/month compute.
* S3 storage: ~50 GB images x $0.023/GB-month = $1.15/month.
* S3 transfers: same-region EC2 -> S3 is free; egress is ~$0.09/GB
  (we don't egress in normal CI, only when seeding a new image).
* Total: under $5/month for daily full-matrix CI.

Pre-merge runs (every PR) go through Tier 2 GHA which is free, plus
spot-trigger of Tier 3 on opt-in (`/test-matrix` PR comment) so we
spend the $0.05 only when changes warrant deeper coverage.

### Image acquisition

For each platform:

1. Use the OS vendor's official cloud image when available (FreeBSD,
   Debian, Fedora, Ubuntu, Alpine all publish qcow2 directly).
2. Use `packer` to build images we can't get directly (illumos, AIX,
   custom PG-buildfarm-style images with specific tool versions).
3. Cache base images in S3; refresh on a quarterly cadence (CI's
   `dist/ci/refresh-images.sh` cron job).
4. Layer customizations as small diff-overlays so the CI run does
   `qcow2-overlay base.qcow2 + xtc-tools.qcow2` rather than rebuilding
   a fat image.

### Images we host vs vendor-host

* Vendor-host (we just `aws s3 cp` from our cache):
  Linux distros, FreeBSD, OpenBSD, NetBSD, DragonFlyBSD.
* Build-and-host (one-time setup, then keep forever):
  illumos OpenIndiana, illumos OmniOS, AIX 7.3 (gcc-aix preinstalled),
  Solaris 11.4 (if licensing permits -- Oracle is fussy here).
* GHA-native (don't put in S3):
  macOS 13/14/15, Windows Server 2022 -- GHA already provides these
  for free, so we use Tier-2 GHA for them rather than paying for our
  own VMs.

## Buildfarm submission checklist

Before submitting xtc to the PG hackers list:

* [ ] Tier 1 + Tier 2 + Tier 3 all green for 7 consecutive days.
* [ ] Coverage > 80% on every src/ file (currently 80.6% per audit).
* [ ] All M*_CLAIMS.md verified by tests.
* [ ] No `TODO` / `XXX` / `FIXME` in src/ (currently 0).
* [ ] Documentation matches reality (verify via the audit agent).
* [ ] Performance benchmarks captured on at least one host of each
  arch family (x86_64, aarch64, ppc64le, sparcv9).
* [ ] xtc test suite passes under sanitizers (asan, tsan, ubsan) on
  Linux + FreeBSD.

## Open questions

1. **AIX licensing**: IBM's PowerVM emulator (`qemu-system-ppc64`
  with PowerNV machine + AIX install image) requires a vendor
  agreement.  Workaround: rent an IBM Cloud Power instance for the
  matrix run (~$0.50/hr) instead of hosting our own image.

2. **Solaris licensing**: same problem.  Oracle Solaris is not
  redistributable; OpenIndiana / OmniOS are, and they share the
  illumos kernel + libc, which is what we actually care about.  Skip
  Oracle Solaris for now.

3. **macOS in CI**: GHA's macOS runners are limited.  For deep
  testing we'd need a Mac mini farm at MacStadium/Scaleway (~$30/mo
  per Mac).  Defer until xtc has a real macOS user base.

4. **Caching invalidation**: when do we refresh S3-cached images?
  Quarterly base + on-demand rebuild via `refresh-images.sh`.
  Vendor releases (e.g. FreeBSD 15.0) trigger a manual refresh.

5. **Reproducibility**: every CI run logs the exact image SHA + tool
  versions used.  Stored in `s3://xtc-ci/builds/<git-sha>/manifest.json`
  for forensics.

## Phasing

**Phase 1 (this round, deferred)**: GitHub Actions workflow covering
ubuntu-22.04, ubuntu-24.04, macos-14, windows-2022.  Free.  Lands as
`.github/workflows/ci.yml` once we want pre-merge gates.

**Phase 2**: Docker images for the 6 main Linux distros.  Costs
~$0/mo (run on dev boxes) and ~$2/mo (run nightly via GHA + self-
hosted runner).

**Phase 3**: Full S3-cached matrix with QEMU.  Real cost begins
($5-15/mo depending on test frequency).  Triggered by deferred
question: do we want this before or after the M16 PG adapter?

## TL;DR for the hackers list

When we submit to PostgreSQL, we should be able to say:

> "xtc passes its 150+ test suite on Linux x86_64 (epoll + io_uring),
>  FreeBSD 15 amd64 (kqueue), illumos OpenIndiana SPARC + amd64
>  (event ports), and Windows 11 x86_64 (IOCP).  Continuous CI runs
>  the matrix on every commit via GitHub Actions + a self-hosted
>  EC2 runner that boots cached QEMU images from S3.  AIX support is
>  code-complete but awaits a host."

That's a concrete, defensible answer -- and the cost is roughly the
price of two cups of coffee per month.
