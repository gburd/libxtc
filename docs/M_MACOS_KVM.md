# macOS via KVM (osx-kvm) -- buildfarm integration

This document describes how to run an xtc CI build on macOS without a
physical Mac, using the OSX-KVM project on a Linux/AMD64 host.

## Why

xtc has Tier-1 platform support for macOS (per PLAN.md (S)3), but no
physical Mac in the current buildfarm.  KVM virtualization gives us
a host-agnostic CI path until we can dedicate a Mac mini.

## Reference

- Upstream: https://github.com/kholia/osx-kvm
- Apple's licensing forbids running macOS on non-Apple hardware in
  production; this approach is acceptable for **personal CI/dev use**.
  For a commercial buildfarm, use a Mac mini.

## Prerequisites

A Linux host with:

- KVM (`/dev/kvm` exists), QEMU 8+, and a CPU that supports SSE 4.2
  + AVX2 (Haswell or newer Intel; modern AMD CPUs lack some macOS-
  required SSE extensions and need extra coaxing -- Intel preferred).
- ~80 GB free disk (macOS image + work disks).
- 8+ GB RAM the VM can claim.
- Network: NAT bridge or `virbr0` for outbound.

## Setup steps

```bash
# As root or with sudo:
git clone --depth 1 https://github.com/kholia/OSX-KVM.git
cd OSX-KVM
# 1. Pick a macOS version (Sonoma 14 / Sequoia 15 are current).
./fetch-macOS-v2.py     # interactive: choose Sonoma
./create-image.sh       # writes BaseSystem.img -> mac_hdd_ng.img

# 2. Adjust OpenCore-Boot.sh for your host:
#    - CPUID set; many modern Intel CPUs already pass.
#    - Add -smp 4,sockets=1,cores=2,threads=2 to taste.
#    - Set memory to e.g. 8192M.

# 3. First boot: install macOS into the disk.
./OpenCore-Boot.sh
# In the Apple installer:
#  - Open Disk Utility, erase mac_hdd_ng.img to APFS
#  - Install macOS to it (~25 minutes)
#  - Reboot, complete setup, enable SSH (System Settings > Sharing).

# 4. Subsequent boots:
./OpenCore-Boot.sh &     # boots headless if VNC viewer unused
ssh -p 2222 user@localhost   # default port-forward 2222->22
```

## xtc-specific provisioning

After macOS is up and SSH works, install developer tooling:

```bash
# On the macOS guest (as the user):
xcode-select --install            # CLT for clang/make/ld
# Optionally Homebrew, but xtc only needs autoconf, automake,
# autoreconf:
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
brew install autoconf automake libtool

# Verify:
clang --version          # 16+ on Sonoma
autoreconf --version     # 2.71+
make --version           # GNU make 3.81 baseline
```

## buildfarm integration

Add to `/etc/hosts` on the buildfarm coordinator:

```
192.168.122.NN  macos-vm
```

(Use `arp -a` post-boot to find the NAT IP, or pin via static DHCP
in the host's libvirt config.)

Then provision an SSH config alias and use the same pattern as
the existing `nuc`, `sun`, `santorini` aliases:

```
# ~/.ssh/config
Host macos
  HostName    192.168.122.NN
  Port        2222            # if using user-mode networking
  User        builder
  IdentityFile ~/.ssh/buildfarm_key
```

xtc's tarball-push pattern (`scp /tmp/xtc.tgz macos:ws/`) then works
identically to the other hosts.

## CI hook

Add to `docs/M_BUILDFARM.md` row, then in the CI driver:

```sh
# In the cross-platform smoke loop:
for host in linux nuc sun santorini macos; do
    scp -q /tmp/xtc.tgz $host:ws/xtc.tgz
    ssh $host "cd ~/ws && find xtc -mindepth 1 -delete; \
               mkdir -p xtc && cd xtc && tar xzf ../xtc.tgz && \
               cd dist && autoreconf -i && cd .. && \
               find build_unix -mindepth 1 -delete; \
               mkdir -p build_unix && cd build_unix && \
               ../dist/configure && make -j4 -s && make -s check"
done
```

## Quirks observed across xtc

When the macOS path is wired up, expect these adjustments (informed
by what we already saw on FreeBSD/illumos):

1. **`<sys/mman.h>`** present -- slab.c and lock_lr.c are fine.
2. **`<execinfo.h>`** present -- `XTC_HAS_EXECINFO` detection should
   probably be widened from `__GLIBC__` to "glibc OR Darwin" in
   `src/ptc/slab.c`.  Trivial follow-up.
3. **`SO_REUSEPORT`** present.  **`SO_PEERCRED`** absent; `LOCAL_PEERCRED`
   present -- the existing `_BSD_VISIBLE` branch in `xtc_net_unix_recv_creds`
   handles it.
4. **kqueue** is the auto-detected backend; verify the existing
   `io_kqueue.c` round-3 dedup logic still passes on macOS. Our
   FreeBSD coverage is the closest analog.
5. **Apple `clang`** lacks gcc-15's `__attribute__((musttail))` as a
   statement attribute (matches the gcc 14 / clang 19 fleet); the
   `XTC_MUSTTAIL` no-op fallback already covers this.
6. **fctx assembler**: `fctx_x86_64_sysv.S` works on Intel macs.
   For Apple Silicon (M1/M2 in a hypothetical bare-metal Mac mini),
   we'll need an `fctx_aarch64_aapcs.S` port -- out of scope for KVM
   since KVM macOS only runs on Intel.

## Cost / time

- Image build: ~60 minutes interactive
- Per-CI run: ~2 minutes for build + check on a 4-core VM
- Disk: ~40 GB image, can be snapshotted and reverted
