# AIX via KVM -- buildfarm integration

This document describes how to run xtc CI builds on AIX without
physical Power hardware, using AIX-on-x86-KVM-via-QEMU.

## Why

xtc has Tier-2 platform support for AIX (per PLAN.md (S)3 and the
existing `src/io/io_aix.c` pollset backend), code-complete but
runtime-unverified.  A KVM-based AIX VM gives us a verification
path until we can secure access to physical Power hardware.

## Reference

- IBM blog post (Hugo B., Jan 2024):
  https://community.ibm.com/community/user/blogs/hugo-b/2024/01/17/aix-virtualization-x86-kvm-qemu
- IBM officially supports running AIX 7.3 on KVM via QEMU's PowerPC
  CPU emulation (TCG), with PowerVM emulation under the hood.

## Caveats

- This is **CPU-emulated**, not native; expect ~5-10x slowdown vs
  Power hardware.  Acceptable for compile + test on a small CI
  workload; not acceptable for benchmarking.
- AIX licensing: IBM offers a **30-day evaluation ISO** for AIX 7.3
  via the IBM Developer portal; for ongoing CI use, get an entitled
  AIX media kit through Passport Advantage.

## Prerequisites

A Linux x86_64 host with:

- QEMU 7.2+ with `qemu-system-ppc64` (`qemu-system-ppc64-spapr`).
  On Debian/Ubuntu: `apt install qemu-system-ppc`.
  On Nix: `nix-shell -p qemu` (the qemu package includes ppc64).
- 80 GB disk for the AIX image + work disks.
- 8+ GB RAM (AIX 7.3 wants 4 GB minimum).
- ~12 hours patience for the install (it's emulated PowerPC).

## Setup steps

```bash
# 1. Acquire the AIX 7.3 install media.
#    - From IBM Passport Advantage (entitled), or
#    - 30-day evaluation ISO (download via IBM Developer portal).
#    Place the ISO at /var/lib/libvirt/images/aix73-install.iso.

# 2. Create the disk image:
qemu-img create -f qcow2 /var/lib/libvirt/images/aix73.qcow2 60G

# 3. Boot for installation.  AIX requires the IBM PowerVM-emulated
#    pSeries machine type (`pseries-2.12+`):
qemu-system-ppc64 \
    -M pseries-7.2 -cpu POWER9 \
    -smp 2,cores=2,threads=1,sockets=1 \
    -m 8192 \
    -drive file=/var/lib/libvirt/images/aix73.qcow2,format=qcow2,if=none,id=hd0 \
    -device virtio-blk-pci,drive=hd0,bootindex=1 \
    -drive file=/var/lib/libvirt/images/aix73-install.iso,if=none,id=cd0,media=cdrom \
    -device scsi-cd,drive=cd0,bootindex=2 \
    -netdev user,id=n0,hostfwd=tcp::2223-:22 \
    -device virtio-net-pci,netdev=n0 \
    -nographic \
    -serial mon:stdio

# Inside the AIX installer (BIST > diagnostics > install):
#  - Default disk, language, locale.
#  - Install BOS: takes 4-8 hours on first boot.
#  - Reboot when prompted.
```

## Post-install provisioning

```bash
# Boot from disk (drop -drive cd0 + -device scsi-cd lines):
qemu-system-ppc64 \
    -M pseries-7.2 -cpu POWER9 -smp 2 -m 8192 \
    -drive file=/var/lib/libvirt/images/aix73.qcow2,format=qcow2,if=virtio \
    -netdev user,id=n0,hostfwd=tcp::2223-:22 \
    -device virtio-net-pci,netdev=n0 \
    -nographic -serial mon:stdio
```

Once booted to the AIX login:

```bash
# As root:
mkdir -p /usr/local
# AIX Toolbox provides gcc, make, autoreconf:
# https://www.ibm.com/support/pages/aix-toolbox-open-source-software
# Direct install via dnf-aix:
yum install -y gcc make autoconf automake libtool tar gzip openssh

# Add a builder user:
mkuser builder
passwd builder

# Enable SSH:
startsrc -s sshd
```

## buildfarm integration

```
# ~/.ssh/config
Host aix
  HostName    192.168.122.NN     # if using libvirt bridge
  Port        2223               # for user-mode networking
  User        builder
  IdentityFile ~/.ssh/buildfarm_key
```

xtc's tarball pattern then works identically.

## xtc-specific provisioning

```bash
# On the AIX guest, as builder:
mkdir -p ~/ws && cd ~/ws
# Receive xtc.tgz...
tar xzf xtc.tgz -C xtc
cd xtc/dist
autoreconf -i
cd ..
mkdir -p build_unix && cd build_unix
../dist/configure   # auto-detects --with-io-backend=aix
make -j2 -s
make -s check
```

## Quirks observed / expected on AIX

xtc's existing `io_aix.c` uses the `pollset_*` API.  Things to
verify when the AIX path comes online:

1. **`pollset_create / pollset_ctl / pollset_poll`** present in
   AIX 7.x; matches the existing `io_aix.c`.  Expect no surprises.
2. **`_Atomic`** support: AIX `xlc` doesn't ship it; we use gcc
   from the AIX Toolbox.  Configure already detects this.
3. **`pthread_mutex_t` alignment** -- AIX requires explicit alignment
   for pthread structs in 64-bit mode.  The existing
   `_Alignas(long long)` shim in `xtc_int.h` (added for illumos)
   should already cover this.
4. **`<execinfo.h>`** absent.  `XTC_SLAB_BACKTRACE` flag will fall
   through to no-op via the `__GLIBC__` gate.
5. **`SO_PEERCRED`** absent.  AIX has `getpeereid(3)`; the xtc_net
   credential path will return uid=0,gid=0 -- acceptable degradation.
6. **fctx assembler**: We have `fctx_x86_64_sysv.S` and
   `fctx_x86_64_ms_pe.S`.  AIX is PowerPC.  The KVM emulated AIX
   needs an `fctx_ppc64_elf_v2.S` port.  Out of scope for this
   document; tracked for the future Power-bare-metal CI path.
   **Until then, the M4 fiber tests will fail on AIX.**  Other
   tests should still pass (loop + io + chan + sync don't use
   fcontext on the read path).

## Cost / time

- Install: 1x one-time, 4-8 hours on emulation.
- Per-CI run: ~10-15 minutes for build + check (vs 2 minutes on
  native x86_64) due to PowerPC TCG emulation.
- Disk: ~40 GB image; snapshotted and reverted between CI runs.

## Alternative: skip AIX KVM

If the emulation overhead is unacceptable, the alternative is:

1. Code-complete check via review (already done -- `io_aix.c` exists).
2. Document `make check` as runtime-unverified on AIX in
   docs/KNOWN_ISSUES.md and PLAN.md (S)3.
3. Wait for either an IBM partnership or an unused Power 8/9 box
   we can rack.

This document keeps the door open; the actual host setup is
optional.
