# Security / CVE audit -- 2026-07 (pre-1.21.0)

Scope: the libxtc LIBRARY (src/), prioritizing network-facing parse
paths (the real remote attack surface), then the send-path, allocator
size arithmetic, and Windows/IOCP/signal paths.  Examples (examples/*)
are consumer code, out of scope for the library audit.

Method: source review of every path where untrusted or config-supplied
bytes drive a length/size, cross-checked with the security-sweep gate
(test/security/test_alloc_overflow_sweep.sh) and an ASan+UBSan run over
the network + new-code tests (test_net / test_net_frame / test_tnt /
test_cskip / test_chash / test_svr / test_xproc -- all clean).

## Headline

NO remote CVE-class bug found.  The library's actual wire-parse
surfaces are sound: the framed transport bounds the claimed length, the
SChannel decrypt path guards its consume-underflow, and TLS record
parsing is delegated to the TLS library (not hand-rolled).  Two
defense-in-depth fixes landed for latent int-cast / config-overflow
hazards on non-remote paths.

## Findings

### F1 (FIXED) -- int-cast truncation in the framed transport I/O loops
src/io/io_net.c: xtc_net_recv_frame / xtc_net_send_frame looped
    recv/send(fd, p + off, (int)(len - off), 0)
where a frame length is a size_t (up to ~4 GiB from the 4-byte wire
prefix).  For a single outstanding span above INT_MAX (2 GiB), the
(int) cast sign-overflows to a negative value handed to the kernel --
a short/failed I/O or implementation-defined behavior.  Reachable on
the paths that pass max_len == 0 ("no cap"): the cross-fork/subprocess
CONTROL channels (osproc.c, xproc.c), which read from a child the
library itself spawned -- semi-trusted, not a remote peer, so not
remote-CVE-class, but a real latent bug.
Fix: XTC_NET_IO_CHUNK (1 GiB) clamp via __net_io_chunk() on both
payload loops, so each syscall's byte count always fits a positive int
and the loop makes progress across chunks.  Root-cause guard in the
shared function -- protects every caller at once.  test_net_frame green.

Note: the frame LENGTH computation itself is safe (byte-by-byte
size_t shifts, no overflow), and the max_len guard correctly rejects
oversized frames with XTC_E_RANGE when a cap is set.  The "max_len 0 =
unlimited" convention on the internal control channels is a documented
footgun, not a fix target (those peers are library-spawned children).

### F2 (FIXED) -- config-overflow in tnt arena sizing
src/orc/tnt.c: arena_init computed
    slot_count * stride, slot_count * sizeof(slot_t),
    slot_count * sizeof(uint32_t)
from the Isolate TYPE registration (slot_count uint32_t, stride
size_t).  Both are caller-supplied SETUP config (not wire input), so a
misconfiguration guard, not a remote CVE.  A pathological type could
wrap one of these products, under-allocate the arena, and turn every
slot index into an out-of-bounds write.
Fix: reject slot_count == 0 and any slot_count*stride /
slot_count*sizeof(slot_t) that would exceed SIZE_MAX, at the top of
arena_init (one guard covers all three products).  test_tnt +
test_sim_tnt (24-seed DST) green.

### Reviewed and found SOUND (no change)

- src/io/tls_schannel.c enc_consume(): the memmove(enc, enc+consumed,
  enc_len-consumed) is guarded by `if (consumed >= enc_len) {enc_len=0;
  return;}` above it -- the size_t subtraction cannot underflow.  This
  IS a remote decrypt path; the dangerous case is already handled.
  (Windows-only, not built/tested on this host; not blind-edited.)
- src/io/tls_openssl.c: the BoringSSL SSL_read/write shim clamps
  `len > INT_MAX` before the (int) cast; the main path uses the
  size_t-native _ex functions; ALPN is a bounded strlen+memcpy of
  caller config.  Sound.  TLS record parsing itself is delegated to the
  TLS library -- libxtc does not hand-parse records.
- src/orc/svr.c reply-strip: `*out_size = size - 4` is guarded by
  `if (size < 4) return XTC_E_INVAL` -- no underflow; malloc+memcpy
  bounded.
- src/orc/stream.c: combinators over user next() functions, no
  wire-byte parsing, no size arithmetic.  (Deeply-nested destroy
  recurses, but the chain is caller-built, not remote.)
- src/os/os_file.c xtc_fs_tmpdir: `if (n + 1 > cap) return E_RANGE`
  before memcpy+NUL -- bounded; reads env, not wire.
- io_poll.c / io_select.c / io_sim.c / pg.c capacity-doubling reallocs:
  `sizeof(*x) * new_cap` where new_cap is a small internal counter that
  doubles (not attacker-controlled); overflow needs ~2^60 elements,
  unreachable.

## Also fixed this arc (found by testing, not this review)

- src/ptc/rcu.c: a genuine data race (double-checked locking on the
  lazy slab-pool init) surfaced by the cskip TSan stress; both slab
  pointers made _Atomic (acquire/release).  Benign-in-practice but a
  real race every RCU consumer hit.  Commit e7908e9.

## Residual / accepted

- The "max_len 0 = unlimited" convention on library-internal control
  channels is retained (the peer is a library-spawned child); documented
  in xtc_net.h.
- tls_schannel.c send_all has the same (int)(len-off) pattern as F1 but
  on library-produced ciphertext bounded well under INT_MAX; left
  unedited (Windows-only, untestable here, not remote-reachable).  If a
  future arc touches that file, apply the same clamp for consistency.

## Verification

security-sweep gate OK; ASan+UBSan clean on all network + new-code
tests; full DST suite (53 tests) green; -Werror gcc clean.
