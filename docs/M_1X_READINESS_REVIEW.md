# libxtc 1.x Production-Readiness Review

Date: 2026-06-25.  Reviewed at v0.6.0 (HEAD de23560 + the IO kqueue fix).
Method: full source read of src/ (os/io/evt/ptc/orc), the IO/AIO layer in
depth, the public-header/install surface, the test tree, the man pages,
and the docs -- cross-checked against what the code actually does and
what `make install` actually ships.  Findings independently verified,
not taken on faith.

This review answers four questions: (1) is the AIO/IO substrate portable
across every supported platform; (2) what gaps/opportunities exist; (3)
what is not 100% finished; (4) do docs/comments/tests match the code --
and concludes with a 1.x go/no-go.


## TL;DR

The CODE is in good shape: the architecture is sound, the POSIX runtime
is well-tested, the AIO portability story is genuinely complete, and the
event/orchestration layers are production-grade.  The BLOCKERS for a 1.x
"production" stamp are not in the algorithms -- they are in (a) the
install/packaging surface, which is broken for the traditional build,
(b) three Tier-1 platforms claimed "Done" that have never run, and (c) a
real cross-loop data race in proc links/monitors.  None is deep; all are
fixable.

## STATUS UPDATE (2026-06-25): all four review items addressed

  - B1 (install headers): FIXED.  dist/Makefile.in + meson.build now
    install the full public header set; a new regression test
    (test/dist/test_install_headers.sh, wired into the build-and-test
    CI job for gcc and clang) compiles + links a consumer against the
    INSTALLED headers.  Verified: the README example compiles post-install.
  - B3 (proc link/monitor cross-loop race): FIXED.  Peer-list pushes go
    through __peer_push_link / __peer_push_monitored_by under the peer's
    table lock with a slot/gen liveness re-check; __notify_links_and_monitors
    detaches the peer-visible lists AND releases the slot atomically under
    that lock.  New regression test test/m8/test_proc_link_race.c (cross-
    loop link+monitor racing peer exit) passes under AddressSanitizer x3.
  - Doc refresh: DONE.  README test count corrected (412 munit + 23
    hegel-c, OTP suite named); getting-started TODO removed;
    illumos/FreeBSD/Windows status reconciled to "prior runs / not in
    per-commit CI" across README + KNOWN_ISSUES; the L0/L1/TLS status
    table made honest about runtime-verification.  Platform-gated
    XTC_E_NOSYS now documented in each header (xtc_pkey/slab/proc already
    had it; added the note to xtc_osproc.h).
  - B2 (platform claims): ADDRESSED by downgrade.  Windows/illumos/AIX
    are now consistently described as compiled / prior-run-verified /
    not-in-per-commit-CI rather than "Done"; a dedicated status section
    was added to KNOWN_ISSUES.md.  (Runtime verification on real hosts
    remains future work, but the CLAIMS are now honest.)
  - SHOULD-FIX: the mpsc capacity-overflow guard and the lrlock
    slot-exhaustion NULL-return doc were added.  The mpsc-recv backoff
    (cooperative model: empty window) remains a NICE-TO-HAVE.


## 1. AIO portability -- the specific question

VERDICT: the portable-AIO promise holds.  `xtc_aio_pread/pwrite/fsync/
fdatasync` work on every platform libxtc builds on, with no platform
where they fail outright.  The API parks the fiber and keeps the loop
live in all cases.

  Backend     Native async file I/O?         Fallback
  --------    ----------------------         --------
  io_uring    YES (IORING_OP_READ/WRITE/FSYNC)  n/a
  IOCP        YES (ReadFile/WriteFile; fsync offloads)  blocking pool
  kqueue      YES on FreeBSD/DragonFly (POSIX AIO + EVFILT_AIO)  blocking pool
  kqueue      macOS -> blocking pool (26 SDK dropped sigev_notify_kqueue)
  epoll       no native -> blocking pool
  poll        no native -> blocking pool
  select      no native -> blocking pool
  solaris     no native -> blocking pool
  aix         no native -> blocking pool

The readiness-only backends return XTC_E_NOSYS from xtc_io_aio_submit and
ptc/aio.c offloads to the blocking thread pool (or runs inline off a
loop).  The offload path is exercised in CI on Linux by a forced-offload
test (test/m4/test_aio.c) that proves it is byte-for-byte identical to
the native path -- so the "write once, runs as AIO everywhere" claim is
tested where the tests run, not just asserted.  The header docs
(xtc_aio.h, xtc_io.h) accurately describe this.  No correctness gap was
found in the buffer-lifetime-across-park handling (the on-stack xtc_aio_t
is only touched by this op's completion).

OPPORTUNITY (not a blocker): no native async file I/O on Solaris event
ports (could use a worker-thread + port_send completion), and macOS now
always offloads (Apple's POSIX AIO never reliably did SIGEV_KEVENT on
regular files anyway).  Both are acceptable -- the offload is correct and
non-blocking -- but a native Solaris path would close the last
"everything offloads here" gap on a Tier-1 platform.


## 2. BLOCKERS for a 1.x "production" release

### B1. The traditional `make install` ships an unusable header set.

`make install` (autoconf) and `meson install` both install exactly two
headers: xtc.h and xtc_runtime.h.  xtc.h is a STUB -- it declares only
the version macros, the XTC_E_* error enum, xtc_version_string,
xtc_version_components, and xtc_strerror.  It does NOT include or pull in
the functional API.

Consequence: after `./dist/configure --prefix=PFX && make && make
install`, a consumer cannot compile against the library.  The README's
own 30-second example does `#include <xtc_loop.h>` / `<xtc_proc.h>`;
neither header is installed.  Verified by doing a clean install into a
temp prefix and compiling the example: "fatal error: xtc_loop.h: No such
file or directory".

The ~48 functional headers (xtc_loop.h, xtc_proc.h, xtc_chan.h,
xtc_net.h, xtc_aio.h, xtc_sync.h, xtc_lrlock.h, xtc_lwlock.h,
xtc_lockmgr.h, xtc_slab.h, xtc_res.h, xtc_app.h, xtc_svr.h,
xtc_supervisor/orc.h, ...) are NOT installed.  The examples build only
because they use -I../../src/inc against the in-tree headers, which masks
the problem.

The single-file amalgamation (dist/mkamalgamation.py) DOES fold the full
surface into one xtc.h and works -- but that is a separate path the
traditional package recipes (debian/, rpm spec, pkg-config) do not use.

Fix options (pick one):
  - Install all public headers (the mkamalgamation public_headers() list:
    every src/inc/xtc_*.h except xtc_int.h/xtc_ext.h, plus xtc_int.h's
    os_* helpers) into includedir, and keep xtc.h as the umbrella that
    includes them; OR
  - Make `make install` emit and install the amalgamated single-file
    xtc.h (run mkamalgamation at install time).
The pkg-config file, debian/, and rpm recipes then need to match.

This is THE blocker: a library that cannot be linked against after its
documented install is not 1.x-shippable.

### B2. Three Tier-1 platforms are "Done" in docs but have never run.

README's status table marks L0/L1 "Done" on Windows, illumos, and (with
"untested") AIX, and the prose claims illumos "verified against the
current tree -- 283/283".  The source tells a more honest story:

  - io_iocp.c:49  "STATUS: COMPILED, NOT RUNTIME-VERIFIED ... has NOT run
    on a Windows host."  The Windows CI job builds xtc.lib + a smoke test
    only; the full munit runtime never runs on Windows.  The AFD-poll
    socket path and the file-AIO cancel/re-arm lifetime are unproven.
  - io_aix.c       reviewed-but-untested pollset backend; README itself
    says "AIX pollset (untested)".
  - io_solaris.c   event-ports backend, code complete; the one-shot
    re-arm logic has no runtime proof on the current tree (README's
    "283/283 verified" refers to a PRIOR run, which KNOWN_ISSUES.md
    correctly calls "re-verify pending" -- the two docs contradict).

For 1.x the honest move is to either (a) run these on real hosts/CI and
make the claim true, or (b) downgrade them to "compiles; runtime
verification pending" in the README status table and CHANGELOG.  Shipping
"Done" for a backend that never executed is the kind of claim a 1.0 must
not make.

### B3. proc links/monitors mutate a peer's list with no lock (cross-loop UAF).

xtc_link(other) appends to BOTH self->links and peer->links:
    pe->next = peer->links; peer->links = pe;   (src/ptc/proc.c, xtc_link)
with no lock on peer.  __notify_links_and_monitors (proc.c) walks and
frees p->links as p exits.  In the single-loop model this is safe
(cooperative, no preemption).  But xtc explicitly supports a multi-loop
executor with cross-loop spawn (proc.c documents "cross-loop spawn ...
the target loop may already be running"), so a proc on loop A linking to
a peer on loop B, concurrently with that peer exiting on loop B, is a
genuine data race and potential use-after-free on the link list.  Sends
to the peer are mbox-locked and safe; the LINK-LIST mutation is not.
Not documented, not asserted, not in KNOWN_ISSUES.

Fix: protect each proc's links/monitors lists with that proc's mbox_lock
(already held on the send path), or restrict link/monitor to same-loop
peers and assert it.


## 3. Not-100%-finished features (acceptable for 1.x IF documented)

These are real declines that are fine to ship as "optional / platform-
gated" provided the headers/docs say so -- several currently do not.

  - xtc_pkey_* (os_pkey.c): returns XTC_E_NOSYS where PKU is absent or
    WRPKRU traps.  Optional perf feature; nothing depends on it.  OK.
  - xtc_osproc_* (orc/osproc.c): all return XTC_E_NOSYS on _WIN32; the
    POSIX implementation is complete.  Mark "POSIX-only".
  - xtc_crash_handler_install (ptc/dump.c): XTC_E_NOSYS on Windows; the
    header should say fault containment is POSIX-only.
  - xtc_slab_pressure_* (ptc/slab.c): XTC_E_NOSYS off Linux (PSI is a
    Linux feature); the header does not state this -- it should.
  - TLS: openssl/mbedtls/gnutls/wolfssl/boringssl all build and pass the
    m18 suite in CI (good).  SChannel (tls_schannel.c) is compile-only;
    tls_none.c is the stub.  KNOWN_ISSUES is honest here.
  - L5 PG adapter: designed, not implemented -- correctly stated.

SHOULD-FIX: every PUBLIC function that returns XTC_E_NOSYS on a platform
should say so in its header doc-comment, so a consumer knows it is
platform-gated rather than broken.


## 4. Lower-severity correctness items (SHOULD-FIX, not blockers)

  - chan.c xtc_chan_mpsc_try_recv spins unbounded waiting for a
    producer's slot store to become visible.  In the cooperative model a
    producer fiber cannot be preempted between reserve and store, so the
    window is empty; across OS threads (true MPSC) a preempted producer
    makes the consumer burn a core until it resumes.  Add a bounded
    backoff/yield.  (Liveness, narrow.)
  - chan.c xtc_chan_mpsc_create rounds capacity up to a power of two with
    no overflow guard; a capacity > SIZE_MAX/2 wraps to 0.  Unreachable
    in practice (the alloc would fail anyway) but the project bans
    unguarded size arithmetic -- add the cap.
  - lock_lr.c reader-slot pool is fixed at 4096 global slots;
    xtc_lrlock_read_begin returns NULL on exhaustion and the header does
    not document the NULL return.  On a many-hundred-thread host this is
    reachable.  Document, and consider a larger/configurable cap.
  - registry (orc/reg.c) is an O(n) linear scan -- fine at small N, a
    1.1 item for large registries.
  - timer cancellation is lazy (cancelled timers stay in the heap until
    they pop); documented trade-off, fine for 1.x.


## 5. Docs / comments / tests vs code

Mostly good, with concrete drift to fix:

  - README test count "280 munit + 23 hegel-c" is STALE.  Actual: ~411
    munit test cases wired into `make check` (including the 35-case OTP
    suite, which the README omits entirely) + 11 PBT files.  Update the
    headline.
  - README says docs/getting-started.md is a "TODO ... fragments only" --
    but the file exists and is a complete 432-line guide.  Remove the
    TODO.
  - README claims illumos/FreeBSD "verified against the current tree";
    KNOWN_ISSUES.md says re-verify is pending.  Reconcile (B2).
  - xtc.h's own comment "including this header alone is sufficient to use
    every M0-public API" is true only for the M0 version/error API, yet
    reads as if it covers the library.  Clarify, and resolve against B1.
  - man-page coverage: test_man_coverage.sh enforces a page for every
    function in xtc.h -- but xtc.h is the stub, so "100% coverage" covers
    ~5 functions.  The functional APIs (xtc_aio_*, xtc_blocking_*,
    xtc_dio_sched_*, xtc_pkey_*, xtc_iosched_*) have no man pages and are
    not caught by the coverage gate.  MAN_TODO.md does not list them.
  - PUBLIC: signature comments that were spot-checked all matched their
    definitions.  No renamed/removed-API drift found.
  - KNOWN_ISSUES.md is otherwise comprehensive and honest.


## 6. Recommended path to 1.0

Must-do (blockers):
  1. Fix the install header surface (B1) and align pkg-config/debian/rpm;
     add a test that compiles a consumer against the INSTALLED headers
     (not the in-tree ones) so this never regresses.
  2. Either runtime-verify Windows/illumos/AIX or downgrade their status
     to "compiles; runtime-pending" everywhere (B2).
  3. Lock the proc link/monitor lists (or restrict + assert same-loop) (B3).

Should-do (1.0 quality):
  4. Document every platform-gated XTC_E_NOSYS in its header.
  5. Refresh the README test count and remove the getting-started TODO.
  6. Add the mpsc backoff + the capacity overflow guard.
  7. Document the lrlock slot-exhaustion NULL return.
  8. Write the missing man pages (or mark them deferred) and fix the
     coverage gate to scan the real public surface.

Nice-to-have (1.1):
  - native Solaris AIO, registry hashing, timer-heap compaction,
    gen_statem.

Bottom line: the engine is 1.x-quality on POSIX; the PACKAGING and the
PLATFORM CLAIMS are what stand between it and an honest 1.0.
