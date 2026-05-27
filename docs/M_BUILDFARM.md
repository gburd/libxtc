# Buildfarm readiness checklist

This document captures the concrete steps required to ensure that
when xtc lands as a dependency or component of PostgreSQL, every
animal on the PG buildfarm continues to pass green.  Updated when we
discover new gaps via the cross-platform CI matrix (see
`docs/M_CI.md`).

## What "the buildfarm" actually tests

A representative slice of active animals (~75-100 distinct
configurations at any given time):

### Linux x86_64 -- most common
- Debian stable + unstable, Ubuntu LTS, RHEL/Rocky/CentOS Stream,
  Fedora, Alpine (musl), Arch, openSUSE Tumbleweed.
- Compilers: gcc 9-15, clang 14-19, occasionally icc.
- Builds with: `--enable-cassert --enable-debug --enable-tap-tests
  --with-openssl --with-ldap --with-icu --with-llvm`.

### Linux non-x86_64
- aarch64 (Debian, Ubuntu, RHEL on AWS Graviton).
- ppc64le (RHEL/Rocky on IBM POWER).
- s390x (Debian, RHEL on IBM Z).
- riscv64 (Debian/sid).
- mips64 (Debian, gcc).

### BSD family
- FreeBSD 13/14/15 amd64 + aarch64; clang 16-19.
- OpenBSD 7.4/7.5 amd64; clang 16.
- NetBSD 10 amd64 + sparc64; gcc 12.
- DragonFlyBSD 6.4 amd64; gcc 13.

### macOS
- macOS 13/14/15 on x86_64 + arm64; Apple clang.

### illumos / Solaris
- OmniOS r151044, r151052; gcc 13.
- Solaris 11.4 SPARC (Oracle's machine).

### Windows
- Windows 10/11 amd64 + arm64.
- MSVC 2019/2022 (cl.exe), MinGW-W64 gcc, occasionally clang-cl.

### AIX
- AIX 7.2/7.3 power9; xlc + gcc-aix (rare but present).

## Where xtc currently stands

| Tier | Platform | xtc status |
|---|---|---|
| [OK] verified | Linux x86_64 (epoll + io_uring), gcc 14 | 151/151 |
| [OK] verified | FreeBSD 15 amd64 (kqueue), clang 19 | 151/151 |
| [OK] verified | OpenIndiana SPARC (port_*), gcc 13 | full pass (test_fctx skipped on SPARC) |
| [H] partial  | Windows 11 / MinGW x64 (IOCP), gcc | 31-35/36 (round 3 fixes pending Windows verify) |
| (O) untested | macOS, OpenBSD, NetBSD, DragonFlyBSD, AIX, ppc64le, s390x, riscv64, aarch64-Linux | code structurally ready; need hosts |

## What we need to ensure no animal fails on merge

### 1. Sanitiser + warning hygiene
Every Linux + FreeBSD CI run must pass under:
- `-Wall -Wextra -Wpedantic -Werror` (already enforced).
- `-fsanitize=address,undefined` (currently NOT enforced; add to CI).
- `-fsanitize=thread` for multi-thread tests (currently NOT enforced).
- `-fanalyzer` (gcc 12+) on a once-a-week scheduled run.

### 2. Strict feature-test compliance
Some animals build with `-std=c89 -Wpedantic` or extreme defines.  We
should ensure:
- xtc compiles with `-std=c11` and `-std=gnu11`.
- xtc builds when `_POSIX_C_SOURCE`, `_XOPEN_SOURCE`, `_DEFAULT_SOURCE`,
  `__BSD_VISIBLE`, `__EXTENSIONS__` are all defined and all undefined.
  (Currently we set them in `xtc_int.h`; verify none leak out of
  internal headers into public ones.)
- Public headers (`src/inc/xtc*.h`) include only standard headers.

### 3. Architecture-specific atomics
xtc relies on C11 `<stdatomic.h>` which is widely supported but:
- ppc64le: gcc atomics on 64-bit doublewords need `-mcpu=power7+`.
- s390x: clang has issues with relaxed atomics in versions < 14.
- mips64: lacks 64-bit lock-free atomics on some kernels.

Action items:
- [ ] Add a build-time check: each `__os_atomic_*` operation has a
  corresponding compile-time test (currently in `pbt_atomic.c`; we
  rely on gcc to optimise correctly).  Add ARCH-conditional tests for
  weak-atomic platforms.
- [ ] Add a runtime PBT property: `__os_atomic_load_i64` on a value
  written by another thread always observes a complete write (no
  torn reads).  Run this PBT under all the supported memory models.

### 4. ABI / packaging
PG vendors xtc as either:
- `--with-bundled-xtc` (in-tree, statically linked). Most likely.
- `--with-system-xtc` (external pkg-config installation). Less likely
  but supported.

For packaging:
- [ ] `pkg-config` `xtc.pc` file (currently NOT shipped -- add).
- [ ] CMake config `xtc-config.cmake` for downstream finders (currently
  NOT shipped -- defer; pkg-config is sufficient).
- [ ] SemVer policy in `docs/abi-stability.md` (already shipped).
- [ ] Symbol versioning via `libxtc.map` -- currently NOT used because
  we ship a static lib only.  When we ship `libxtc.so`, add a version
  script.

### 5. Test discipline
- [ ] Every test must complete in < 30s (current `make check` runs
  in ~4s on Linux floki); add a per-test timeout to munit harness.
- [ ] No test depends on hostname, /proc, /sys, or `/tmp` being
  writable.  (`test_oos_enforced.sh` currently uses `/tmp` -- verify
  it falls back to `$TMPDIR`.)
- [ ] No test requires root.
- [ ] No test connects to an external host or DNS.
- [ ] Bench programs (bench_disk, bench_net, bench_uring_disk,
  bench_million_tasks) MUST NOT be invoked from `make check`; they're
  for hand-run perf evaluation.

### 6. Locale and encoding
PG buildfarm runs with various LC_ALL / LANG settings.  xtc tests
must not depend on locale.  Audit: search for `setlocale`, `strcoll`,
`towlower` -- none should be in src/.

### 7. Runtime feature detection
- xtc auto-detects the L1 backend at configure time (epoll / kqueue /
  uring / iocp / solaris / aix).  This works for the buildfarm's
  default configurations.
- If liburing is missing on a Linux animal, we fall back to epoll.
  Verified.  Document explicitly.

### 8. Time and clock
xtc uses `CLOCK_MONOTONIC` for time and `CLOCK_REALTIME` for
condvar deadlines.  `pthread_cond_clockwait` is preferable on glibc
2.30+ but not portable.  Audit: do we consistently use the right
clock for each path?  (Currently `__os_clock_real` and
`__os_clock_mono` are in `os_time.c`; usage is reviewed.)

### 9. Thread sanitizer
Run TSan in CI weekly:
- All M9 sync primitives must pass (notify, sem, abort_source,
  amutex, rwlock, barrier, gate).
- M13c lockmgr should pass (the deadlock detector grabs partition
  locks in a fixed order).
- xtc_proc + xtc_send/recv path under TSan is the most complex; spot
  check.

## Concrete CI plan to satisfy all of the above

See `docs/M_CI.md` for the tiered approach.  Specifically for
buildfarm readiness:

**Phase A** (immediate):
- Add GitHub Actions workflow `.github/workflows/buildfarm.yml`:
  - matrix: ubuntu-22.04, ubuntu-24.04, fedora-40, alpine-edge,
    macos-13, macos-14, windows-2022.
  - run: `./dist/configure && make check`.
  - one TSan job (Ubuntu): `CFLAGS="-fsanitize=thread,undefined"`.
  - one ASan job (Ubuntu): `CFLAGS="-fsanitize=address,undefined"`.

**Phase B** (after Phase A green for 2 weeks):
- Add scheduled QEMU matrix run: aarch64-linux, ppc64le-linux,
  s390x-linux, freebsd-15-amd64, openindiana-amd64 -- using cached
  S3 images.
- Run nightly; alert on any red.

**Phase C** (after macOS/AIX hosts available):
- Add macOS arm64, AIX 7.3 to the matrix.
- Cache base images in S3.

**Phase D** (when xtc is ready for PG submission):
- Each PR comment can request `/test-buildfarm` to run the full
  matrix.  Self-hosted EC2 burns ~$0.05 per run.
- Pre-merge gate: PR must have a green Phase A + a green Phase B
  within the last 24 hours.

## Known animals that will need special handling

| Animal | Why it'll be tricky |
|---|---|
| `florican` (HP-UX 11i ia64) | GCC ancient, ucontext quirks, no io_uring/kqueue/iocp. Must use poll backend. |
| `mantid` (NetBSD-current sparc64) | Big-endian + 32-bit pointers. Audit alignment of all packed structs. |
| `taipan` (Windows MSVC arm64) | We support MinGW x64 but not yet MSVC arm64.  Need round-3+ Windows asm + MSVC toolchain in CI. |
| `loach` (Solaris 11 SPARC) | Same as illumos OpenIndiana; should "just work" once the `__EXTENSIONS__` shim is reviewed for Solaris-vs-illumos differences. |
| `hornet` (AIX 7.2 power9 xlc) | xlc has different intrinsics for atomics; we use C11 `<stdatomic.h>` which xlc 16+ supports.  Older xlc would fail. |

## Pre-submission checklist (before PG hackers list)

- [ ] All tier-A tier-B platforms green for 14 consecutive days.
- [ ] TSan + ASan + UBSan: zero warnings on the full test suite.
- [ ] Coverage > 90% line, > 80% branch (currently ~80% line per
  audit; need a test-writing push).
- [ ] No `XTC_E_NOSYS` callers in production code paths (currently
  only in `io_aix.c` while AIX awaits a host; this is acceptable).
- [ ] Documentation parity: `make check` passes including all
  `test/dist/test_*.sh` shell tests; mandoc lint clean.
- [ ] `pkg-config` `xtc.pc` shipped.
- [ ] All ABI-stable APIs documented in `man3/`.
- [ ] PLAN.md, ARCHITECTURE.md, M_PORT.md all match reality (verified
  by the audit agent -- documented in `docs/audit-log.md`).
